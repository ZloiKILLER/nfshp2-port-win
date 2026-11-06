// Minimal IOP kernel for modules run by the R3000 interpreter (iop_cpu.cpp).
//
// Everything on the IOP runs on one host thread ("IOP thread"). Threads are register
// sets switched by priority like the IOP kernel does: a thread runs until it blocks
// or a higher priority one becomes ready. Interrupt handlers (hard timer, SPU DMA end,
// CD callback) run between slices on their own stack while interrupts are enabled.
// Time is host time.
//
// HP2_IOPLOG=<file>: kernel events and the modules' printf (default: the HLE log).
#include "iop.h"
#include "disc.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

extern FILE *g_hle_log;
extern u32 g_iop_hle_index;

u8 *g_iop_ram;

FILE *iop_log()
{
    static FILE *f = [] {
        const char *path = getenv("HP2_IOPLOG");
        FILE *o = path ? fopen(path, "w") : nullptr;
        return o ? o : g_hle_log;
    }();
    return f;
}
#define KLOG(...) (fprintf(iop_log(), __VA_ARGS__), fflush(iop_log()))

#define A0 c.r[4]
#define A1 c.r[5]
#define A2 c.r[6]
#define A3 c.r[7]
#define V0 c.r[2]
#define ARG(n) ((n) < 4 ? c.r[4 + (n)] : iop_lw(c.r[29] + 4 * (n)))

static u64 now_us()
{
    static const auto t0 = std::chrono::steady_clock::now();
    return (u64)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
}

// IOP system clock: 36.864 MHz
static const double SYSCLK_PER_US = 36.864;

// ---- HLE table --------------------------------------------------------------------
static std::map<std::string, IopHle> &hle_table()
{
    static std::map<std::string, IopHle> t;
    return t;
}
void iop_add_hle(const char *name, IopHle fn) { hle_table()[name] = fn; }

static void hle_missing(IopRegs &c)
{
    KLOG("IOP: unimplemented import %s (ra %08x)\n", iop_hle_name(g_iop_hle_index), c.r[31]);
    V0 = 0;
}

// ---- sysmem ----------------------------------------------------------------------
static const u32 MODULE_BASE = 0x60000;
static u32 g_heap_lo, g_heap_hi = 0x1F0000;
static std::map<u32, u32> g_blocks;     // start -> size

static u32 heap_alloc(u32 type, u32 size, u32 want)
{
    size = (size + 255) & ~255u;
    if (type == 2) {
        for (auto &b : g_blocks)
            if (want < b.first + b.second && b.first < want + size) return 0;
        g_blocks[want] = size;
        return want;
    }
    u32 best = 0;
    bool found = false;
    u32 prev = g_heap_lo;
    for (auto it = g_blocks.begin();; ++it) {
        u32 next = it == g_blocks.end() ? g_heap_hi : it->first;
        if (next >= prev + size) {
            u32 at = type == 1 ? next - size : prev;
            if (!found || type == 1) best = at;
            found = true;
            if (type != 1) break;
        }
        if (it == g_blocks.end()) break;
        prev = it->first + it->second;
    }
    if (!found) return 0;
    g_blocks[best] = size;
    return best;
}

u32 iop_alloc(u32 size) { return heap_alloc(1, size, 0); }

static void AllocSysMemory(IopRegs &c)
{
    V0 = heap_alloc(A0, A1, A2);
    KLOG("IOP: AllocSysMemory(%u, %u) -> %06x\n", A0, A1, V0);
}
static void FreeSysMemory(IopRegs &c) { V0 = g_blocks.erase(A0) ? 0 : (u32)-1; }
static void QueryMaxFreeMemSize(IopRegs &c)
{
    u32 best = 0, prev = g_heap_lo;
    for (auto &b : g_blocks) {
        if (b.first > prev && b.first - prev > best) best = b.first - prev;
        prev = b.first + b.second;
    }
    if (g_heap_hi - prev > best) best = g_heap_hi - prev;
    V0 = best;
}

// ---- threads -----------------------------------------------------------------------
enum ThState { TH_DORMANT, TH_READY, TH_WAIT_SEMA, TH_WAIT_DELAY, TH_WAIT_RPC };

struct ModuleJob;
struct Server;
struct Thread {
    IopRegs regs;
    int prio;
    ThState state = TH_DORMANT;
    u32 entry, stack, stack_size;
    u32 wait_sema = 0;
    u64 wake = 0;
    u64 ready_seq = 0;
    ModuleJob *job = nullptr;          // module start thread: report v0 at exit
    Server *serving = nullptr;         // RPC call in progress
    u32 rpc_sp = 0;
};
static std::map<int, Thread> g_threads;
static int g_next_thread = 1;
static Thread *g_cur;                  // null while an interrupt handler runs
static u64 g_ready_seq;
static u32 g_gp;
static bool g_intr_off;

static void make_ready(Thread &t)
{
    t.state = TH_READY;
    t.ready_seq = ++g_ready_seq;
}

// a woken thread may outrank the running one: end the slice so the scheduler looks
static void preempt_check(Thread &woken)
{
    if (g_cur && woken.prio < g_cur->prio) iop_stop();
}

static u32 g_magic_thread_exit, g_magic_intr_ret, g_magic_rpc_ret;

static int create_thread(u32 entry, u32 stack_size, int prio)
{
    int id = g_next_thread++;
    Thread &t = g_threads[id];
    t.entry = entry;
    t.stack_size = (stack_size + 255) & ~255u;
    t.stack = iop_alloc(t.stack_size);
    t.prio = prio;
    return id;
}

static void start_thread(Thread &t, u32 arg)
{
    memset(&t.regs, 0, sizeof t.regs);
    t.regs.pc = t.entry;
    t.regs.r[4] = arg;
    t.regs.r[28] = g_gp;
    t.regs.r[29] = t.stack + t.stack_size - 16;
    t.regs.r[31] = g_magic_thread_exit;
    make_ready(t);
}

// ThreadParam: attr, option, entry, stackSize, initPriority
static void CreateThread(IopRegs &c)
{
    u32 p = A0;
    int id = create_thread(iop_lw(p + 8), iop_lw(p + 12), (int)iop_lw(p + 16));
    KLOG("IOP: CreateThread entry %08x stack %u prio %d -> %d\n", iop_lw(p + 8), iop_lw(p + 12),
         (int)iop_lw(p + 16), id);
    V0 = id;
}
static void StartThread(IopRegs &c)
{
    auto it = g_threads.find((int)A0);
    if (it == g_threads.end()) { V0 = (u32)-407; return; }
    start_thread(it->second, A1);
    preempt_check(it->second);
    V0 = 0;
}
static void GetThreadId(IopRegs &c)
{
    V0 = 0;
    for (auto &t : g_threads)
        if (&t.second == g_cur) V0 = t.first;
}
static void DelayThread(IopRegs &c)
{
    V0 = 0;
    if (!g_cur) return;
    g_cur->state = TH_WAIT_DELAY;
    g_cur->wake = now_us() + A0;
    iop_stop();
}
static void USec2SysClock(IopRegs &c)
{
    u64 clk = (u64)((double)A0 * SYSCLK_PER_US);
    iop_sw(A1, (u32)clk);
    iop_sw(A1 + 4, (u32)(clk >> 32));
    V0 = 0;
}

static void thread_exit(IopRegs &c);

// ---- semaphores -------------------------------------------------------------------
struct Sema {
    int count, max;
};
static std::map<int, Sema> g_semas;
static int g_next_sema = 1;

static void CreateSema(IopRegs &c)
{
    int id = g_next_sema++;
    g_semas[id] = {(int)iop_lw(A0 + 8), (int)iop_lw(A0 + 12)};
    V0 = id;
}
static void signal_sema(IopRegs &c)
{
    auto it = g_semas.find((int)A0);
    if (it == g_semas.end()) { V0 = (u32)-408; return; }
    Thread *best = nullptr;
    for (auto &t : g_threads)
        if (t.second.state == TH_WAIT_SEMA && t.second.wait_sema == A0 &&
            (!best || t.second.prio < best->prio))
            best = &t.second;
    if (best) {
        best->regs.r[2] = 0;
        make_ready(*best);
        preempt_check(*best);
    } else if (it->second.count < it->second.max) {
        it->second.count++;
    }
    V0 = 0;
}
static void WaitSema(IopRegs &c)
{
    auto it = g_semas.find((int)A0);
    if (it == g_semas.end()) { V0 = (u32)-408; return; }
    V0 = 0;
    if (it->second.count > 0) { it->second.count--; return; }
    if (!g_cur) { KLOG("IOP: WaitSema in interrupt\n"); return; }
    g_cur->state = TH_WAIT_SEMA;
    g_cur->wait_sema = A0;
    iop_stop();
}

// ---- interrupts ---------------------------------------------------------------------
struct Pending {
    u32 fn, a0, a1;
};
static std::deque<Pending> g_pending;
static u32 g_intr_stack;
static bool g_intr_done;

void iop_raise(u32 fn, u32 a0, u32 a1)
{
    if (fn) g_pending.push_back({fn, a0, a1});
}

static u32 run_intr(u32 fn, u32 a0, u32 a1)
{
    IopRegs c = {};
    c.pc = fn;
    A0 = a0;
    A1 = a1;
    c.r[28] = g_gp;
    c.r[29] = g_intr_stack;
    c.r[31] = g_magic_intr_ret;
    Thread *saved = g_cur;
    g_cur = nullptr;
    g_intr_done = false;
    for (int n = 0; !g_intr_done; n++) {
        if (n == 1000) { KLOG("IOP: interrupt handler %08x does not return\n", fn); break; }
        iop_execute(c, 100000);
    }
    g_cur = saved;
    return V0;
}

static void intr_ret(IopRegs &c)
{
    g_intr_done = true;
    iop_stop();
}

static void CpuSuspendIntr(IopRegs &c)
{
    if (A0) iop_sw(A0, g_intr_off);
    V0 = g_intr_off ? (u32)-102 : 0;
    g_intr_off = true;
}
static void CpuResumeIntr(IopRegs &c)
{
    g_intr_off = A0 != 0;
    V0 = 0;
}

// ---- hard timers ----------------------------------------------------------------------
struct Timer {
    bool armed;
    u32 compare, handler, common;
    u64 next;
};
static std::map<int, Timer> g_timers;
static int g_next_timer = 1;

static void AllocHardTimer(IopRegs &c)
{
    int id = g_next_timer++;
    g_timers[id] = {};
    V0 = id;
}
static void SetTimerHandler(IopRegs &c)
{
    auto it = g_timers.find((int)A0);
    if (it == g_timers.end()) { V0 = (u32)-151; return; }
    it->second.compare = A1;
    it->second.handler = A2;
    it->second.common = A3;
    V0 = 0;
}
static void SetupHardTimer(IopRegs &c) { V0 = 0; }
static void StartHardTimer(IopRegs &c)
{
    auto it = g_timers.find((int)A0);
    if (it == g_timers.end()) { V0 = (u32)-151; return; }
    Timer &t = it->second;
    t.armed = t.compare != 0;
    t.next = now_us() + (u64)(t.compare / SYSCLK_PER_US);
    KLOG("IOP: hard timer %d every %.0f us\n", (int)A0, t.compare / SYSCLK_PER_US);
    V0 = 0;
}

// ---- SIF RPC --------------------------------------------------------------------------
struct RpcReq {
    u32 sid, fno, ee_send, ssize, ee_recv, rsize;
    bool done;
};
struct Server {
    u32 sid, func, buff, qd;
    int thread;
    std::deque<RpcReq *> pending;
};
static std::map<u32, Server> g_servers;              // by sid
static std::map<u32, int> g_queue_thread;            // qd -> thread

static std::mutex g_mtx;                             // EE <-> IOP thread
static std::condition_variable g_cv_iop, g_cv_ee;
static std::deque<RpcReq *> g_incoming;
static std::map<u32, bool> g_bound;                  // sid registered (read by EE)

static void sceSifInitRpc(IopRegs &c) { V0 = 0; }
static void sceSifSetRpcQueue(IopRegs &c)
{
    g_queue_thread[A0] = (int)A1;
    V0 = 0;
}
// (sd, sid, func, buff, cfunc, cbuff, qd)
static void sceSifRegisterRpc(IopRegs &c)
{
    Server s = {A1, A2, A3, ARG(6), 0, {}};
    g_servers[s.sid] = s;
    KLOG("IOP: RPC server %08x func %08x buff %08x\n", s.sid, s.func, s.buff);
    std::lock_guard<std::mutex> lock(g_mtx);
    g_bound[s.sid] = true;
    V0 = 0;
}
static void sceSifRpcLoop(IopRegs &c)
{
    if (!g_cur) return;
    for (auto &s : g_servers)
        if (s.second.qd == A0)
            for (auto &t : g_threads)
                if (&t.second == g_cur) s.second.thread = t.first;
    g_cur->state = TH_WAIT_RPC;
    g_cur->rpc_sp = c.r[29];
    iop_stop();
}

static void ee_to_iop(u32 iop, u32 ee, u32 size)
{
    for (u32 k = 0; k < size; k++) iop_sb(iop + k, LB(ee + k));
}
static void iop_to_ee(u32 ee, u32 iop, u32 size)
{
    for (u32 k = 0; k < size; k++) SB(ee + k, iop_lb(iop + k));
}

static void dispatch_rpc()
{
    for (auto &sp : g_servers) {
        Server &s = sp.second;
        if (s.pending.empty()) continue;
        auto it = g_threads.find(s.thread);
        if (it == g_threads.end() || it->second.state != TH_WAIT_RPC) continue;
        Thread &t = it->second;
        RpcReq *r = s.pending.front();
        if (r->ssize && s.buff) ee_to_iop(s.buff, r->ee_send, r->ssize);
        t.regs.pc = s.func;
        t.regs.r[4] = r->fno;
        t.regs.r[5] = s.buff;
        t.regs.r[6] = r->ssize;
        t.regs.r[29] = t.rpc_sp;
        t.regs.r[31] = g_magic_rpc_ret;
        t.serving = &s;
        make_ready(t);
    }
}

static void rpc_ret(IopRegs &c)
{
    Thread *t = g_cur;
    if (!t || !t->serving) { iop_stop(); return; }
    Server &s = *t->serving;
    RpcReq *r = s.pending.front();
    s.pending.pop_front();
    if (r->rsize && r->ee_recv && V0) iop_to_ee(r->ee_recv, V0, r->rsize);
    t->serving = nullptr;
    t->state = TH_WAIT_RPC;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        r->done = true;
    }
    g_cv_ee.notify_all();
    iop_stop();
}

// ---- sifman: IOP -> EE ----------------------------------------------------------------
static void sceSifInit(IopRegs &c) { V0 = 0; }
static void sceSifCheckInit(IopRegs &c) { V0 = 1; }
// (sceSifDmaData *, count): data (IOP), addr (EE), size, mode
static void sceSifSetDma(IopRegs &c)
{
    for (u32 i = 0; i < A1; i++) {
        u32 d = A0 + 16 * i;
        iop_to_ee(iop_lw(d + 4), iop_lw(d), iop_lw(d + 8));
    }
    V0 = 1;
}
static void sceSifDmaStat(IopRegs &c) { V0 = (u32)-1; }

// ---- cdvdman on the virtual disc -------------------------------------------------------
static u32 g_cd_callback;

static void sceCdInit(IopRegs &c) { V0 = 1; }
static void sceCdMmode(IopRegs &c) { V0 = 1; }
static void sceCdDiskReady(IopRegs &c) { V0 = 2; }
static void sceCdSync(IopRegs &c) { V0 = 0; }
static void sceCdGetError(IopRegs &c) { V0 = 0; }
static void sceCdCallback(IopRegs &c)
{
    V0 = g_cd_callback;
    g_cd_callback = A0;
}
static void sceCdSearchFile(IopRegs &c)
{
    KLOG("IOP: sceCdSearchFile %s (not supported)\n", iop_str(A1).c_str());
    V0 = 0;
}
// (lsn, sectors, buf, mode)
static void sceCdRead(IopRegs &c)
{
    u32 lsn = A0, n = A1, buf = A2;
    std::vector<u8> tmp((size_t)n * 2048);
    bool ok = disc_read(lsn, n, tmp.data());
    u8 *dst = iop_host(buf);
    if (dst && (buf & 0x1FFFFFFF) + tmp.size() <= IOP_RAM_SIZE) memcpy(dst, tmp.data(), tmp.size());
    static int logged;
    if (logged++ < 20) KLOG("IOP: sceCdRead lsn %u x%u -> %06x\n", lsn, n, buf);
    iop_raise(g_cd_callback, 1, 0);           // SCECdFuncRead
    V0 = ok ? 1 : 0;
}
static void sceCdSeek(IopRegs &c)
{
    iop_raise(g_cd_callback, 4, 0);           // SCECdFuncSeek
    V0 = 1;
}

// ---- ioman (host0:) ---------------------------------------------------------------------
static void io_fail(IopRegs &c) { V0 = (u32)-1; }

// ---- sysclib / stdio ---------------------------------------------------------------------
static void k_memcpy(IopRegs &c)
{
    for (u32 k = 0; k < A2; k++) iop_sb(A0 + k, iop_lb(A1 + k));
    V0 = A0;
}
static void k_memset(IopRegs &c)
{
    for (u32 k = 0; k < A2; k++) iop_sb(A0 + k, (u8)A1);
    V0 = A0;
}
static void k_strlen(IopRegs &c) { V0 = (u32)iop_str(A0).size(); }
static void k_strcpy(IopRegs &c)
{
    std::string s = iop_str(A1);
    for (size_t k = 0; k <= s.size(); k++) iop_sb(A0 + (u32)k, k < s.size() ? s[k] : 0);
    V0 = A0;
}
static void k_strcat(IopRegs &c)
{
    u32 end = A0 + (u32)iop_str(A0).size();
    std::string s = iop_str(A1);
    for (size_t k = 0; k <= s.size(); k++) iop_sb(end + (u32)k, k < s.size() ? s[k] : 0);
    V0 = A0;
}
static void k_strncmp(IopRegs &c)
{
    s32 d = 0;
    for (u32 k = 0; k < A2; k++) {
        u8 a = iop_lb(A0 + k), b = iop_lb(A1 + k);
        if (a != b) { d = a - b; break; }
        if (!a) break;
    }
    V0 = (u32)d;
}
static void k_strstr(IopRegs &c)
{
    std::string h = iop_str(A0), n = iop_str(A1);
    size_t p = h.find(n);
    V0 = p == std::string::npos ? 0 : A0 + (u32)p;
}

static void k_printf(IopRegs &c)
{
    std::string fmt = iop_str(A0), out;
    int arg = 1;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t j = i + 1;
        while (j < fmt.size() && strchr("-+ #0123456789.lh", fmt[j])) j++;
        if (j >= fmt.size()) break;
        std::string spec = fmt.substr(i, j - i);
        spec.erase(std::remove_if(spec.begin(), spec.end(), [](char ch) { return ch == 'l' || ch == 'h'; }),
                   spec.end());
        char conv = fmt[j], buf[256];
        if (conv == '%') snprintf(buf, sizeof buf, "%%");
        else if (conv == 's') snprintf(buf, sizeof buf, (spec + "s").c_str(), iop_str(ARG(arg++)).c_str());
        else if (strchr("dicuxXop", conv)) snprintf(buf, sizeof buf, (spec + conv).c_str(), ARG(arg++));
        else snprintf(buf, sizeof buf, "%s%c", spec.c_str(), conv);
        out += buf;
        i = j;
    }
    fprintf(iop_log(), "IOP printf: %s", out.c_str());
    if (out.empty() || out.back() != '\n') fprintf(iop_log(), "\n");
    fflush(iop_log());
    V0 = (u32)out.size();
}

// ---- module loader ----------------------------------------------------------------------
struct ModuleJob {
    std::string name;
    std::vector<u8> irx;
    int result;
    bool done;
};
static std::deque<ModuleJob *> g_jobs;
static u32 g_next_base = MODULE_BASE;

static u32 rd32(const std::vector<u8> &f, size_t o) { u32 v; memcpy(&v, &f[o], 4); return v; }
static u16 rd16(const std::vector<u8> &f, size_t o) { u16 v; memcpy(&v, &f[o], 2); return v; }

static bool load_irx(ModuleJob &job, u32 &entry)
{
    const std::vector<u8> &f = job.irx;
    if (f.size() < 52 || rd32(f, 0) != 0x464C457F || rd16(f, 16) != 0xFF80) {
        KLOG("IOP: %s is not an IRX\n", job.name.c_str());
        return false;
    }
    u32 phoff = rd32(f, 28), shoff = rd32(f, 32);
    u16 phnum = rd16(f, 44), shnum = rd16(f, 48);
    u32 base = g_next_base, iopmod = 0, memsz = 0;
    for (u16 i = 0; i < phnum; i++) {
        size_t ph = phoff + 32 * i;
        u32 type = rd32(f, ph), off = rd32(f, ph + 4), vaddr = rd32(f, ph + 8);
        u32 filesz = rd32(f, ph + 16);
        if (type == 0x70000080) iopmod = off;
        if (type == 1) {
            memsz = rd32(f, ph + 20);
            memset(g_iop_ram + base + vaddr, 0, memsz);
            memcpy(g_iop_ram + base + vaddr, &f[off], filesz);
        }
    }
    if (!iopmod || !memsz) return false;
    // relocations (no symbols: everything is relative to the load address)
    std::vector<u32> hi;
    for (u16 i = 0; i < shnum; i++) {
        size_t sh = shoff + 40 * i;
        if (rd32(f, sh + 4) != 9) continue;              // SHT_REL
        u32 off = rd32(f, sh + 16), size = rd32(f, sh + 20);
        for (u32 k = 0; k < size; k += 8) {
            u32 at = base + rd32(f, off + k), type = rd32(f, off + k + 4) & 0xFF;
            u32 w = iop_lw(at);
            switch (type) {
            case 2: iop_sw(at, w + base); break;
            case 4: iop_sw(at, (w & 0xFC000000) | ((w + (base >> 2)) & 0x03FFFFFF)); break;
            case 5: hi.push_back(at); break;
            case 6: {
                s32 lo = (s16)(w & 0xFFFF);
                for (u32 h : hi) {
                    u32 hw = iop_lw(h);
                    u32 full = ((hw & 0xFFFF) << 16) + lo + base;
                    iop_sw(h, (hw & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF));
                }
                hi.clear();
                iop_sw(at, (w & 0xFFFF0000) | ((w + base) & 0xFFFF));
                break;
            }
            case 0: case 7: break;                       // none, GPREL16
            default: KLOG("IOP: relocation type %u at %06x\n", type, at);
            }
        }
    }
    // names of the import stubs from the symbol table
    std::map<u32, std::string> syms;
    for (u16 i = 0; i < shnum; i++) {
        size_t sh = shoff + 40 * i;
        if (rd32(f, sh + 4) != 2) continue;              // SHT_SYMTAB
        u32 off = rd32(f, sh + 16), size = rd32(f, sh + 20), link = rd32(f, sh + 24);
        u32 stroff = rd32(f, shoff + 40 * link + 16);
        for (u32 k = 0; k < size; k += 16) {
            u32 name = rd32(f, off + k), value = rd32(f, off + k + 4);
            if (name) syms[value] = (const char *)&f[stroff + name];
        }
    }
    u32 text_size = rd32(f, iopmod + 12);
    int stubs = 0, missing = 0;
    for (u32 a = base; a < base + text_size; a += 4) {
        if (iop_lw(a) != 0x41E00000) continue;
        char lib[9] = {};
        memcpy(lib, g_iop_ram + a + 12, 8);
        for (u32 s = a + 20; iop_lw(s) == 0x03E00008; s += 8) {
            u32 ord = iop_lw(s + 4) & 0xFFFF;
            auto sym = syms.find(s - base);
            std::string name = sym != syms.end() ? sym->second : std::string(lib) + "_" + std::to_string(ord);
            auto h = hle_table().find(name);
            if (h == hle_table().end()) {
                KLOG("IOP: %s import %s (%s #%u) not implemented\n", job.name.c_str(), name.c_str(), lib, ord);
                missing++;
            }
            iop_sw(s, iop_hle_opcode(h != hle_table().end() ? h->second : hle_missing, name.c_str()));
            stubs++;
        }
    }
    entry = base + rd32(f, iopmod + 4);
    g_gp = base + rd32(f, iopmod + 8);
    g_next_base = (base + memsz + 0xFFF) & ~0xFFFu;
    g_heap_lo = g_next_base;
    KLOG("IOP: loaded %s at %06x-%06x, entry %06x gp %06x, %d imports (%d missing)\n",
         job.name.c_str(), base, base + memsz, entry, g_gp, stubs, missing);
    return true;
}

static void thread_exit(IopRegs &c)
{
    if (!g_cur) { iop_stop(); return; }
    if (g_cur->job) {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_cur->job->result = (int)V0;
        g_cur->job->done = true;
        g_cur->job = nullptr;
    }
    g_cv_ee.notify_all();
    g_cur->state = TH_DORMANT;
    iop_stop();
}

// ---- scheduler ----------------------------------------------------------------------------
static void register_kernel()
{
    static const struct { const char *name; IopHle fn; } k[] = {
        {"AllocSysMemory", AllocSysMemory}, {"FreeSysMemory", FreeSysMemory},
        {"QueryMaxFreeMemSize", QueryMaxFreeMemSize},
        {"CpuSuspendIntr", CpuSuspendIntr}, {"CpuResumeIntr", CpuResumeIntr},
        {"open", io_fail}, {"close", io_fail}, {"read", io_fail}, {"lseek", io_fail},
        {"sceSifInitRpc", sceSifInitRpc}, {"sceSifRegisterRpc", sceSifRegisterRpc},
        {"sceSifSetRpcQueue", sceSifSetRpcQueue}, {"sceSifRpcLoop", sceSifRpcLoop},
        {"sceSifInit", sceSifInit}, {"sceSifSetDma", sceSifSetDma}, {"sceSifDmaStat", sceSifDmaStat},
        {"sceSifCheckInit", sceSifCheckInit},
        {"printf", k_printf},
        {"memcpy", k_memcpy}, {"memset", k_memset}, {"strcat", k_strcat}, {"strcpy", k_strcpy},
        {"strlen", k_strlen}, {"strncmp", k_strncmp}, {"strstr", k_strstr},
        {"CreateThread", CreateThread}, {"StartThread", StartThread}, {"GetThreadId", GetThreadId},
        {"DelayThread", DelayThread}, {"USec2SysClock", USec2SysClock},
        {"CreateSema", CreateSema}, {"SignalSema", signal_sema}, {"iSignalSema", signal_sema},
        {"WaitSema", WaitSema},
        {"AllocHardTimer", AllocHardTimer}, {"SetTimerHandler", SetTimerHandler},
        {"SetupHardTimer", SetupHardTimer}, {"StartHardTimer", StartHardTimer},
        {"sceCdInit", sceCdInit}, {"sceCdRead", sceCdRead}, {"sceCdSeek", sceCdSeek},
        {"sceCdGetError", sceCdGetError}, {"sceCdSearchFile", sceCdSearchFile}, {"sceCdSync", sceCdSync},
        {"sceCdDiskReady", sceCdDiskReady}, {"sceCdCallback", sceCdCallback}, {"sceCdMmode", sceCdMmode},
    };
    for (auto &e : k) iop_add_hle(e.name, e.fn);
    iop_libsd_register();
}

static Thread *pick()
{
    Thread *best = nullptr;
    for (auto &tp : g_threads) {
        Thread &t = tp.second;
        if (t.state != TH_READY) continue;
        if (!best || t.prio < best->prio ||
            (t.prio == best->prio && (&t == g_cur || (best != g_cur && t.ready_seq < best->ready_seq))))
            best = &t;
    }
    return best;
}

static void iop_main()
{
#ifdef _WIN32
    // The sound driver keeps its streams fed on its timer (every 16.7 ms) and sleeps between. Windows' sleeps go by the system's tick, 15.6 ms
    // unless asked: a tick late, a stream of speech could run dry and stop. A 1 ms tick for the process, and this thread ahead of the
    // game's, as the IOP is its own processor on the PS2.
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    std::unique_lock<std::mutex> lock(g_mtx);
    for (;;) {
        // requests from the EE
        while (!g_jobs.empty()) {
            ModuleJob *job = g_jobs.front();
            g_jobs.pop_front();
            lock.unlock();
            u32 entry;
            if (load_irx(*job, entry)) {
                int id = create_thread(entry, 0x4000, 8);
                Thread &t = g_threads[id];
                start_thread(t, 0);
                t.job = job;
                lock.lock();
            } else {
                lock.lock();
                job->result = -1;
                job->done = true;
                g_cv_ee.notify_all();
            }
        }
        while (!g_incoming.empty()) {
            RpcReq *r = g_incoming.front();
            g_incoming.pop_front();
            auto s = g_servers.find(r->sid);
            if (s != g_servers.end()) {
                s->second.pending.push_back(r);
            } else {
                r->done = true;
                g_cv_ee.notify_all();
            }
        }
        lock.unlock();

        u64 now = now_us();
        dispatch_rpc();
        if (!g_intr_off) {
            for (auto &tp : g_timers) {
                Timer &t = tp.second;
                if (!t.armed || t.next > now) continue;
                u32 next = run_intr(t.handler, t.common, 0);
                if (!next) { t.armed = false; continue; }
                t.compare = next;
                t.next += (u64)(next / SYSCLK_PER_US);
                if (t.next + 100000 < now) t.next = now;
            }
            while (!g_pending.empty() && !g_intr_off) {
                Pending p = g_pending.front();
                g_pending.pop_front();
                run_intr(p.fn, p.a0, p.a1);
            }
        }
        for (auto &tp : g_threads)
            if (tp.second.state == TH_WAIT_DELAY && tp.second.wake <= now) {
                tp.second.regs.r[2] = 0;
                make_ready(tp.second);
            }

        Thread *t = pick();
        if (t) {
            g_cur = t;
            iop_execute(t->regs, 20000);
            lock.lock();
            continue;
        }
        g_cur = nullptr;
        // idle: sleep until the next timer or delayed thread, or a request from the EE
        u64 wake = now + 100000;
        for (auto &tp : g_timers)
            if (tp.second.armed && tp.second.next < wake) wake = tp.second.next;
        for (auto &tp : g_threads)
            if (tp.second.state == TH_WAIT_DELAY && tp.second.wake < wake) wake = tp.second.wake;
        lock.lock();
        if (g_jobs.empty() && g_incoming.empty() && g_pending.empty() && wake > now_us())
            g_cv_iop.wait_for(lock, std::chrono::microseconds(wake - now_us()));
    }
}

static void ensure_started()
{
    static bool started;
    if (started) return;
    started = true;
    g_iop_ram = (u8 *)calloc(IOP_RAM_SIZE, 1);
    register_kernel();
    // magic return addresses below the modules
    g_magic_thread_exit = 0x1000;
    g_magic_intr_ret = 0x1008;
    g_magic_rpc_ret = 0x1010;
    iop_sw(g_magic_thread_exit, iop_hle_opcode(thread_exit, "<thread exit>"));
    iop_sw(g_magic_intr_ret, iop_hle_opcode(intr_ret, "<interrupt return>"));
    iop_sw(g_magic_rpc_ret, iop_hle_opcode(rpc_ret, "<rpc return>"));
    g_intr_stack = 0x8000;                    // 0x2000..0x8000 for interrupt handlers
    std::thread(iop_main).detach();
}

// ---- EE side ------------------------------------------------------------------------------
bool iop_active() { return g_iop_ram != nullptr; }

int iop_start_module(const std::string &name, std::vector<u8> irx)
{
    ensure_started();
    ModuleJob job = {name, std::move(irx), 0, false};
    std::unique_lock<std::mutex> lock(g_mtx);
    g_jobs.push_back(&job);
    g_cv_iop.notify_all();
    g_cv_ee.wait(lock, [&] { return job.done; });
    return job.result;
}

bool iop_has_server(u32 sid)
{
    if (!iop_active()) return false;
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_bound.count(sid) != 0;
}

void iop_call_rpc(u32 sid, u32 fno, u32 ee_send, u32 ssize, u32 ee_recv, u32 rsize)
{
    RpcReq req = {sid, fno, ee_send, ssize, ee_recv, rsize, false};
    std::unique_lock<std::mutex> lock(g_mtx);
    g_incoming.push_back(&req);
    g_cv_iop.notify_all();
    while (!g_cv_ee.wait_for(lock, std::chrono::seconds(2), [&] { return req.done; }))
        KLOG("IOP: RPC %08x fn %u has no answer after 2 s\n", sid, fno);
}

void iop_sif_from_ee(u32 iop_dst, u32 ee_src, u32 size)
{
    if (!iop_active()) return;
    ee_to_iop(iop_dst, ee_src, size);
}
