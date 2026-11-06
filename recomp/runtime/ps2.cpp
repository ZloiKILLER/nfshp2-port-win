// Host side of the recompiled EE: memory, ELF loading, BIOS syscalls,
// hardware register access and the indirect-call dispatcher.
#include "ps2.h"
#include "hw.h"
#include "pcconfig.h"
#include "disc.h"
#include "setup_iso.h"
#include "setup.h"
#include "host.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <algorithm>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

alignas(64) u8 g_ram[0x02000000 + 64];
alignas(64) u8 g_spr[0x4000 + 64];
alignas(64) u8 g_vumem[0x10000 + 64];

static FILE *g_log = stderr;
FILE *g_hle_log = stderr;
static Ctx *g_ctx;
static std::map<u32, int> g_io_seen;

void profile_start();                       // profile.cpp

[[noreturn]] static void die(const char *what, Ctx &c, u32 pc)
{
    char msg[128];
    snprintf(msg, sizeof msg, "FATAL: %s at pc=%08x ra=%08x sp=%08x", what, pc, c.r[31].w[0], c.r[29].w[0]);
    host_fatal(msg);
    exit(1);
}

// ---- dispatcher -----------------------------------------------------------------
void ps2_call(Ctx &c, u32 addr)
{
    int lo = 0, hi = g_functable_size - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        u32 a = g_functable[mid].addr;
        if (a == addr) {
            g_functable[mid].fn(c);
            return;
        }
        if (a < addr) lo = mid + 1;
        else hi = mid - 1;
    }
    char msg[64];
    snprintf(msg, sizeof msg, "indirect call to non-function %08x", addr);
    die(msg, c, addr);
}

// ---- interrupts ---------------------------------------------------------------------
// Only what the game needs so far: INTC handlers (VBlank) fired by a host timer.
// They run at the next function entry with the whole CPU state saved around them.
volatile u32 g_irq_pending;

struct IrqHandler {
    u32 fn, arg;
};
static std::vector<IrqHandler> g_intc[16], g_dmac[16];
static u32 g_intc_enabled, g_dmac_enabled;
static bool g_in_irq;

// bit 31 of g_irq_pending stands for "some DMAC channel finished" (g_dmac_pending)
static const u32 IRQ_DMAC = 1u << 31;

// raised from the DMA, VBlank and EE threads alike
// the game's thread asleep in ps2_idle: what wakes it
static std::mutex g_idle_lock;
static std::condition_variable g_idle_wake;
static std::atomic<bool> g_idling;

static void wake_idle()
{
    if (!g_idling.load()) return;
    std::lock_guard<std::mutex> lock(g_idle_lock);
    g_idle_wake.notify_one();
}

void raise_intc(int cause)
{
    __atomic_fetch_or(&g_irq_pending, 1u << cause, __ATOMIC_SEQ_CST);
    wake_idle();
}

void ps2_idle(Ctx &c)
{
    if (!g_irq_pending) {
        std::unique_lock<std::mutex> lock(g_idle_lock);
        g_idling = true;
        // the time is only a guard: the loop looks at its variable again in any case
        g_idle_wake.wait_for(lock, std::chrono::milliseconds(20), [] { return g_irq_pending != 0; });
        g_idling = false;
    }
    if (g_irq_pending) ps2_take_interrupts(c);
}

bool g_eie = true;
void op_ei(Ctx &c)
{
    g_eie = true;
    if (g_irq_pending) ps2_take_interrupts(c);
}

static void run_handler(Ctx &c, u32 fn, u32 cause, u32 arg)
{
    Ctx saved = c;
    bool eie = g_eie;                   // as the handler's own DI/EI leave it is not ours to keep
    c.r[4].d[0] = cause;
    c.r[5].d[0] = sx32(arg);
    // on the kernel's stack, as on the PS2: the game's stack below sp keeps what the
    // game left there (elSetupLightContext reads two light directions it does not
    // always write; a handler's frames there lit cars black)
    static const u32 irq_sp = getenv("HP2_IRQSTACK") ? (u32)strtoul(getenv("HP2_IRQSTACK"), nullptr, 16) : 1;   // TEMP: 1 = the game stack (sp - 0x400), else a kernel stack address, e.g. 80070000
    if (irq_sp == 1) c.r[29].d[0] -= 0x400;
    else c.r[29].d[0] = (u64)(s64)(s32)irq_sp;
    ps2_call(c, fn);
    c = saved;
    g_eie = eie;
}

void ps2_take_interrupts(Ctx &c)
{
    if (g_in_irq || !g_eie) return;
    g_in_irq = true;
    u32 pending = __atomic_exchange_n(&g_irq_pending, 0u, __ATOMIC_SEQ_CST);
    for (u32 cause = 0; cause < 16; cause++) {
        if (!(pending & (1u << cause)) || !(g_intc_enabled & (1u << cause))) continue;
        for (auto &h : g_intc[cause]) run_handler(c, h.fn, cause, h.arg);
    }
    if (pending & IRQ_DMAC) {
        u32 chans = __atomic_exchange_n(&g_dmac_pending, 0u, __ATOMIC_SEQ_CST);
        for (u32 ch = 0; ch < 16; ch++) {
            if (!(chans & (1u << ch)) || !(g_dmac_enabled & (1u << ch))) continue;
            for (auto &h : g_dmac[ch]) run_handler(c, h.fn, ch, h.arg);
        }
    }
    g_in_irq = false;
}

// HP2_PEEK=<addr>[,<addr>...]: once a second, the guest words at those addresses;
// an address is hex, or gp+N / gp-N (decimal) for small data
static void peek()
{
    static const char *e = getenv("HP2_PEEK");
    static int n;
    if (!e || !g_ctx || ++n % 60) return;
    fprintf(g_log, "peek:");
    for (const char *p = e; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : nullptr) {
        u32 a = !strncmp(p, "gp", 2) ? g_ctx->r[28].w[0] + (u32)atoi(p + 2) : (u32)strtoul(p, nullptr, 16);
        u32 v;
        memcpy(&v, g_ram + (a & 0x1FFFFFC), 4);
        fprintf(g_log, " %08x=%d", a, (int)v);
    }
    fprintf(g_log, "\n");
}

static void start_vblank_timer()
{
    std::thread([] {
        auto next = std::chrono::steady_clock::now();
        for (;;) {
            next += std::chrono::microseconds(16683);       // NTSC field rate
            std::this_thread::sleep_until(next);
            peek();
            gs_vblank();
            __atomic_fetch_or(&g_irq_pending, (1u << INTC_VBLANK_START) | (1u << INTC_VBLANK_END), __ATOMIC_SEQ_CST);
            wake_idle();
        }
    }).detach();
}

// ---- BIOS syscalls (HLE) ------------------------------------------------------------
static u32 g_heap_end;
static u32 g_stack_bottom = 0x01FFC000;
static u32 g_user_syscalls[256];
// kernel RAM (below 0x80000) is not used by the game itself
static const u32 KERNEL_SYSCALL_TABLE = 0x80010000;

void ps2_syscall(Ctx &c, u32 pc)
{
    s32 num = (s32)c.r[3].w[0];
    u32 a0 = c.r[4].w[0], a1 = c.r[5].w[0], a2 = c.r[6].w[0];
    u32 ret = 0;
    // handlers the game installed itself with SetSyscall run as ordinary code
    // (handlers copied into kernel RAM, e.g. by InitAlarm, were never recompiled:
    // those syscalls fall through to the HLE below)
    u32 user = g_user_syscalls[(num < 0 ? -num : num) & 0xFF];
    if (user && user < 0x80000000) {
        ps2_call(c, user);
        return;
    }
    switch (num < 0 ? -num : num) {
    case 0x74:                      // SetSyscall(number, handler)
        // libkernel's _setup locates the BIOS syscall table by searching kernel
        // memory for these handler addresses, so keep a table where it looks
        g_user_syscalls[a0 & 0xFF] = a1;
        SW(KERNEL_SYSCALL_TABLE + (a0 & 0xFF) * 4, a1);
        break;
    case 0x3C: {                    // SetupThread(gp, stack, stack_size, args, root)
        u32 top = a1 == 0xFFFFFFFF ? 0x02000000 : a1 + a2;
        g_stack_bottom = top - a2;
        ret = top - 0x10;
        break;
    }
    case 0x3D:                      // SetupHeap(start, size): -1 runs up to the main stack
        g_heap_end = a1 == 0xFFFFFFFF ? g_stack_bottom : a0 + a1;
        ret = a0;
        break;
    case 0x64:                      // FlushCache
    case 0x68:
        break;
    case 0x3E:                      // EndOfHeap
        ret = g_heap_end;
        break;
    case 0x2F:                      // GetThreadId
        ret = 1;
        break;
    case 0x7C:                      // Deci2Call: no debugger attached on a retail unit
        ret = (u32)-1;
        break;
    case 0x7A:                      // SifGetReg
        // SMFLAG: IOP booted, SIF commands up, boot finished (the sceSif* HLE
        // in hle_iop.cpp covers the library; this is the raw syscall)
        ret = a0 == 4 ? 0x70000 : 0;
        break;
    case 0x79:                      // SifSetReg
        break;
    case 0x10:                      // AddIntcHandler(cause, handler, next, arg)
        g_intc[a0 & 15].push_back({a1, c.r[7].w[0]});
        ret = (u32)g_intc[a0 & 15].size();
        break;
    case 0x12:                      // AddDmacHandler(channel, handler, next, arg)
        g_dmac[a0 & 15].push_back({a1, c.r[7].w[0]});
        ret = (u32)g_dmac[a0 & 15].size();
        break;
    case 0x14: case 0x1A:           // (i)EnableIntc
        g_intc_enabled |= 1u << (a0 & 15);
        ret = 1;
        break;
    case 0x15: case 0x1B:           // (i)DisableIntc
        g_intc_enabled &= ~(1u << (a0 & 15));
        ret = 1;
        break;
    case 0x16: case 0x1C:           // (i)EnableDmac
        g_dmac_enabled |= 1u << (a0 & 15);
        ret = 1;
        break;
    case 0x17: case 0x1D:           // (i)DisableDmac
        g_dmac_enabled &= ~(1u << (a0 & 15));
        ret = 1;
        break;
    default: {
        static int seen[256];
        if (seen[(num < 0 ? -num : num) & 0xFF]++ < 4)
            fprintf(g_log, "syscall %d (0x%x) a0=%08x a1=%08x a2=%08x at %08x\n",
                    num, num < 0 ? -num : num, a0, a1, a2, pc);
        break;
    }
    }
    c.r[2].d[0] = sx32(ret);
}

void ps2_break(Ctx &c, u32 pc) { die("break", c, pc); }

void ps2_unimpl(Ctx &c, u32 pc, u32 word)
{
    char msg[64];
    snprintf(msg, sizeof msg, "unimplemented instruction %08x", word);
    die(msg, c, pc);
}

// ---- COP0 -----------------------------------------------------------------------------
static auto g_t0 = std::chrono::steady_clock::now();

u32 op_mfc0(Ctx &, int rd)
{
    if (rd == 9) {                  // Count: 294.912 MHz
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - g_t0).count();
        return (u32)(ns * 294912 / 1000000);
    }
    if (rd == 12) return g_eie ? 0x10000 : 0;   // Status: EIE
    if (rd == 15) return 0x2E20;    // PRId: EE
    return 0;
}
void op_mtc0(Ctx &, int, u32) {}

// ---- VU0 control registers ---------------------------------------------------------
static u32 g_cmsar0;

u32 op_cfc2(Ctx &c, int rd)
{
    if (rd < 16) return c.vi[rd];
    switch (rd) {
    case 16: return c.vstatus;
    case 17: return c.vmac;
    case 18: return c.vclip;
    case 21: { FPR f; f.f = c.vi_i; return f.u; }
    case 22: { FPR f; f.f = c.vq; return f.u; }
    case 27: return g_cmsar0;
    case 29: return 0;              // VPU-STAT: VU0/VU1 idle
    }
    return 0;
}
void op_ctc2(Ctx &c, int rd, u32 v)
{
    if (rd > 0 && rd < 16) { c.vi[rd] = (u16)v; return; }
    switch (rd) {
    case 16: c.vstatus = v; break;
    case 18: c.vclip = v; break;
    case 21: { FPR f; f.u = v; c.vi_i = f.f; break; }
    case 22: { FPR f; f.u = v; c.vq = f.f; break; }
    case 27: g_cmsar0 = v; break;
    }
}

void vu0_callms(Ctx &c, u32 addr)
{
    static int n;
    if (n++ < 4)
        fprintf(g_log, "VU0 microprogram call at %04x ignored (ra=%08x)\n", addr, c.r[31].w[0]);
}

// ---- hardware registers ------------------------------------------------------------
static void io_note(const char *kind, u32 addr, u64 v)
{
    int &n = g_io_seen[addr];
    if (n++ < 2)
        fprintf(g_log, "%s %08x = %llx  (ra=%08x)\n", kind, addr, (unsigned long long)v,
                g_ctx ? g_ctx->r[31].w[0] : 0);
}

u64 io_read(u32 addr, int size)
{
    (void)size;
    u64 v;
    if (dma_io_read(addr, v) || vif_io_read(addr, v) || gs_io_read(addr, v)) return v;
    io_note("io read ", addr, 0);
    return 0;
}
void io_write(u32 addr, u64 value, int size)
{
    (void)size;
    if (dma_io_write(addr, value)) return;
    if (vif_io_write(addr, value) || gs_io_write(addr, value)) return;
    io_note("io write", addr, value);
}
u128 io_read128(u32 addr)
{
    io_note("io read128 ", addr, 0);
    return 0;
}
void io_write128(u32 addr, u128 value)
{
    if (addr == 0x10005000) { vif1_scan((const u32 *)&value, 4); return; }   // VIF1 FIFO
    if (addr == 0x10006000) return;                                          // GIF FIFO
    io_note("io write128", addr, (u64)value);
}

// ---- ELF loading ------------------------------------------------------------------------
// The game's program from the player's disc, data\SLUS_203.62 (the first start copies it there), checked by its SHA-1: the code is recompiled,
// its data sections and the VU microcode it carries go into guest memory
static std::vector<u8> g_elf;

static bool load_elf(u32 &entry)
{
    const char *env = getenv("HP2_DATA");
    const std::string path = (env ? std::string(env) + "/" : host_exe_dir()) + "data/SLUS_203.62";
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    g_elf.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    const bool read = fread(g_elf.data(), 1, g_elf.size(), f) == g_elf.size();
    fclose(f);
    bool known = false;
    const std::string sha = sha1_hex(g_elf.data(), g_elf.size());
    for (const KnownDisc &k : kKnownDiscs) known |= sha == k.elf_sha1;
    if (!read || !known || g_elf.size() < 0x34) return false;
    const u8 *d = g_elf.data();
    entry = *(u32 *)&d[0x18];
    u32 phoff = *(u32 *)&d[0x1C];
    u16 phnum = *(u16 *)&d[0x2C];
    for (int i = 0; i < phnum; i++) {
        const u32 *ph = (const u32 *)&d[phoff + i * 32];
        if (ph[0] != 1) continue;           // PT_LOAD
        u32 off = ph[1], vaddr = ph[2], filesz = ph[4], memsz = ph[5];
        memcpy(g_ram + (vaddr & 0x01FFFFFF), &d[off], filesz);
        memset(g_ram + (vaddr & 0x01FFFFFF) + filesz, 0, memsz - filesz);
    }
    return true;
}

void F_00100008(Ctx &c);

// The EE's FPU and VU0 round toward zero and take a denormal for zero. The game's code counts on it: DriveTarget::Resample steps along a path while
// the sum is within its length, which rounding to nearest can pass by a bit before the last point. That point is then whatever was on the stack, a racer's
// belt goes through it, the car is thrown a billion metres, and the way back over the finish line counts as a lap.
static void ee_float_mode()
{
    if (getenv("HP2_NOEEFLOAT")) return;        // the host's rounding, for comparing
#if defined(__x86_64__)
    __builtin_ia32_ldmxcsr(__builtin_ia32_stmxcsr() | 0x6000 | 0x8000 | 0x40);      // RC toward zero, flush to zero, denormals are zero
#elif defined(__aarch64__)
    unsigned long fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= 3ul << 22 | 1ul << 24;                                                  // RMode toward zero, FZ
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

int main()
{
    // the first start, or the game installed beside this exe: the game runs from its own folder
    if (setup_start() != SETUP_PLAY) return 0;
    host_log_start();
    if (!disc_open()) {
        host_fatal("The game's data was not found: the data folder (with data\\IRX253\\STREAM.IRX) must be next to the game.");
        return 1;
    }
    // slack past the end: 64-bit and 128-bit accesses at the last address stay in bounds
    u32 entry;
    if (!load_elf(entry)) {
        host_fatal("The game's program, data\\SLUS_203.62, is missing or is not one the port knows. Install the game again from the disc image.");
        return 1;
    }
    fprintf(g_log, "entry %08x\n", entry);
    profile_start();
    start_vblank_timer();
    pc_config_init();
    if (!getenv("HP2_NOWINDOW")) window_start();
    static Ctx c;
    g_ctx = &c;
    c.r[29].d[0] = 0x01FFFF00;
    c.vf[0].f[3] = 1.0f;
    ee_float_mode();
    F_00100008(c);
    fprintf(g_log, "_start returned\n");
    return 0;
}
