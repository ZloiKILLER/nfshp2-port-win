// High-level emulation of the IOP side, at the EE SDK boundary.
//
// The generated code calls HLE_<name> instead of the recompiled body for every
// function listed in hle_names.txt. Arguments follow the EE EABI: a0-a3, t0-t3
// in r4-r11, further ones on the caller's stack; the result goes to v0.
//
// The game addresses its data by sector (sceCdRead of ZZDATA.BIN); the sectors
// come from the virtual disc over the game folder (disc.h).
#include "ps2.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "disc.h"
#include "funcs.h"
#include "iop.h"
#include "spu2.h"

extern FILE *g_hle_log;

#define A0 c.r[4].w[0]
#define A1 c.r[5].w[0]
#define A2 c.r[6].w[0]
#define A3 c.r[7].w[0]
#define T0 c.r[8].w[0]
#define T1 c.r[9].w[0]
#define T2 c.r[10].w[0]
#define T3 c.r[11].w[0]
#define RET(v) (c.r[2].d[0] = sx32((u32)(v)))
#define LOG(...) fprintf(g_hle_log, __VA_ARGS__)

static std::string guest_str(u32 a)
{
    std::string s;
    for (u8 ch; a && (ch = LB(a)) != 0 && s.size() < 256; a++)
        s += (char)ch;
    return s;
}

// Run guest code (a callback) from host code without disturbing the caller.
static void call_guest(Ctx &c, u32 fn, u32 a0, u32 a1 = 0)
{
    if (!fn)
        return;
    Ctx saved = c;
    c.r[4].d[0] = sx32(a0);
    c.r[5].d[0] = sx32(a1);
    c.r[29].d[0] -= 0x100;
    ps2_call(c, fn);
    c = saved;
}

// ---- sound driver trace (HP2_SNDLOG=<file>) ----------------------------------------
// The EA sound library talks to MultiStream 5.2 (STREAM.IRX, RPC server 0x12345). An
// RPC with fno 0 carries a packet of u16: count, then count x {cmd, nargs, args[nargs]};
// IOP side: AdpcmSetParam in STREAM.IRX. Names below are the IOP handlers.
static const u32 SOUND_SID = 0x00012345;

static const char *snd_cmd_name(u32 cmd)
{
    static const char *const names[80] = {
        "PlaySFX", "PlayStream", "SetChannelVolume", "SetChannelPitch", "KeyOff",
        "StopStream", nullptr, "InitSPU", nullptr, "GetStatus",
        "Pause", "Resume", "InitStreamData", "AllocateStreamBuffer", "SetStreamMemoryEnd",
        "CloseStreamBuffer", "SetMaxStreams", "CheckStereo", "CancelCheckStereo", "EnableEffects",
        "DisableEffects", "SetEffectMasterVolume", "AllChannelEffectsOn", "AllChannelEffectsOff", "SetChannelEffectOn",
        "SetChannelEffectOff", nullptr, "ChainStreams", "PatchSFX", "GetFileInfo",
        "SetFileInfo", "CreateFileInfo", "FreeFileID", "FreeAllFileIDs", "StoreHOST0FileName",
        "AllocateFileMemory", "FreeFileMemory", "AllocateSpotFXMemory", "FreeSpotFXMemory", "FreeAllSpotFXIDs",
        "FreeSpotFXID", "SetFileOffsetAndPlaySize", "InitCD", "SetStreamParent", "SetStreamChild",
        "GetMaxIOPRAM", "StreamAllowKeyOn", "StreamDisableKeyOn", "SetStreamTrack", "SetEEWriteAddress",
        "SetIOPWriteAddress", "GetFileIDInfo", "TransIOPToSPU", "RestartFromCDError", "EXTERNAL_CDErrorPause",
        "CheckCDError", "EXTERNAL_CDErrorRestart", "PlaySFX57", "DisableCDCallback", "DisableSPUCallback",
        "EnableSPUCallback", "SetDTSMode", "SFXAllowKeyOn", "KeyOnChannel", "StreamAddSPUChannel",
        "SetOutput", "ModifyEETransferAddress", "AppendStream", "SetStreamLoadSource", "InitPCMPlayback",
        "SetPCMType", "ResizeStreamBuffer", "SetPCMVolume", "SetWaitEEAddress", "SetStreamStartPosition",
        "PCMStart", "PCMStop", "InitPCMPlaybackNoStream", nullptr, "ClosePCMPlayback",
    };
    switch (cmd) {
    case 200: return "SdRemote";
    case 201: return "SdEffect";
    case 202: return "SetLoadMethod";
    case 203: return "SetSmoothChannelVolume";
    }
    return cmd < 80 && names[cmd] ? names[cmd] : "?";
}

static FILE *snd_log()
{
    static FILE *f = [] {
        const char *path = getenv("HP2_SNDLOG");
        return path ? fopen(path, "w") : nullptr;
    }();
    return f;
}

static double snd_time()
{
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static void snd_log_packet(u32 fno, u32 send, u32 ssize, u32 ra)
{
    FILE *f = snd_log();
    if (!f) return;
    if (fno != 0 || !send) {
        fprintf(f, "%9.3f rpc fn %u send %08x/%u (ra %08x)\n", snd_time(), fno, send, ssize, ra);
        fflush(f);
        return;
    }
    u32 count = LH(send), p = send + 2;
    // a packet with nothing but the status poll FlushIOPCommand appends is counted, not printed
    if (count == 1 && LH(p) == 9) {
        static u32 polls;
        if (++polls % 600 == 0) fprintf(f, "%9.3f (%u status polls)\n", snd_time(), polls);
        return;
    }
    fprintf(f, "%9.3f packet %u cmds\n", snd_time(), count);
    for (u32 i = 0; i < count && p < send + ssize; i++) {
        u32 cmd = LH(p), n = LH(p + 2);
        p += 4;
        fprintf(f, "    %3u %-24s", cmd, snd_cmd_name(cmd));
        for (u32 k = 0; k < n; k++) fprintf(f, " %04x", LH(p + 2 * k));
        // string commands carry chars packed as u16
        if (cmd == 34 || cmd == 31) {
            fprintf(f, "  \"");
            for (u32 k = 0; k < n; k++) {
                u16 v = LH(p + 2 * k);
                for (int b = 0; b < 2; b++) {
                    u8 ch = (u8)(v >> (8 * b));
                    if (ch >= 0x20 && ch < 0x7f) fputc(ch, f);
                }
            }
            fprintf(f, "\"");
        }
        fprintf(f, "\n");
        p += 2 * n;
    }
    fflush(f);
}

// ---- SIF / IOP ----------------------------------------------------------------------
// sceSifClientData: rpcd(16) command buff cbuff func para serve
static const u32 CD_SERVE = 36;
static std::map<u32, u32> g_rpc_sid;        // client data -> server id
static int g_next_module = 1;

void HLE_sceSifInitCmd(Ctx &c) { RET(0); }
void HLE_sceSifExitCmd(Ctx &c) { RET(0); }
void HLE_sceSifInitRpc(Ctx &c) { RET(0); }
void HLE_sceSifExitRpc(Ctx &c) { RET(0); }
void HLE_sceSifSetDma(Ctx &c)
{
    // (sceSifDmaData *sdd, int len): {data (EE), addr (IOP), size, mode}
    if (FILE *f = snd_log()) {
        for (u32 i = 0; i < A1; i++) {
            u32 d = A0 + 16 * i, src = LW(d), dst = LW(d + 4), size = LW(d + 8);
            fprintf(f, "%9.3f sif dma EE %08x -> IOP %08x size %u mode %u:", snd_time(), src, dst,
                    size, LW(d + 12));
            for (u32 k = 0; k < 16 && k < size; k++) fprintf(f, " %02x", LB(src + k));
            fprintf(f, " (ra %08x)\n", c.r[31].w[0]);
            // HP2_SNDDUMP=<dir>: keep every transfer as <dir>/sifdma_NNNN.bin
            static const char *dir = getenv("HP2_SNDDUMP");
            static int seq;
            if (dir && size < 0x400000) {
                char name[512];
                snprintf(name, sizeof name, "%s/sifdma_%04d.bin", dir, seq++);
                if (FILE *o = fopen(name, "wb")) {
                    std::vector<u8> tmp(size);
                    for (u32 k = 0; k < size; k++) tmp[k] = LB(src + k);
                    fwrite(tmp.data(), 1, size, o);
                    fclose(o);
                }
            }
        }
        fflush(f);
    }
    if (iop_active())
        for (u32 i = 0; i < A1; i++) {
            u32 d = A0 + 16 * i;
            iop_sif_from_ee(LW(d + 4), LW(d), LW(d + 8));
        }
    RET(1);
}
void HLE_sceSifDmaStat(Ctx &c) { RET(-1); }                 // transfer finished
void HLE_sceSifSendCmd(Ctx &c) { RET(1); }
void HLE_sceSifAddCmdHandler(Ctx &c) { RET(0); }
void HLE_sceSifSetCmdBuffer(Ctx &c) { RET(0); }
void HLE_sceSifGetReg(Ctx &c) { RET(A0 == 4 ? 0x70000 : 0); }  // SMFLAG: IOP up
void HLE_sceSifSetReg(Ctx &c) { RET(0); }
void HLE_sceSifGetSreg(Ctx &c) { RET(0); }
void HLE_sceSifSetSreg(Ctx &c) { RET(0); }
void HLE_sceSifLoadFileReset(Ctx &c) { RET(0); }
void HLE_sceSifResetIop(Ctx &c) { LOG("IOP reset: %s\n", guest_str(A0).c_str()); RET(1); }
void HLE_sceSifRebootIop(Ctx &c) { LOG("IOP reboot: %s\n", guest_str(A0).c_str()); RET(1); }
void HLE_sceSifSyncIop(Ctx &c) { RET(1); }
void HLE_sceSifIsAliveIop(Ctx &c) { RET(1); }

// The sound driver runs for real on the IOP interpreter (iop.h); HP2_NOSOUND keeps it
// at the old zero-answer HLE.
static bool runs_on_iop(const std::string &path)
{
    return path.find("STREAM.IRX") != std::string::npos && !getenv("HP2_NOSOUND");
}

static int start_on_iop(const std::string &path)
{
    DiscFile e;
    if (!disc_find(path, e)) return -1;
    std::vector<u8> irx(((size_t)e.size + 2047) & ~(size_t)2047);
    if (!disc_read(e.lsn, (u32)irx.size() / 2048, irx.data())) return -1;
    irx.resize(e.size);
    int r = iop_start_module(path, std::move(irx));
    spu2_start_output();
    return r;
}

void HLE_sceSifLoadModule(Ctx &c)
{
    std::string path = guest_str(A0);
    bool iop = runs_on_iop(path);
    int r = iop ? start_on_iop(path) : 0;
    LOG("IOP module %s -> id %d%s\n", path.c_str(), g_next_module, iop ? " (interpreted)" : "");
    if (r < 0) LOG("IOP module %s failed to start\n", path.c_str());
    RET(g_next_module++);
}
void HLE_sceSifLoadStartModule(Ctx &c)
{
    std::string path = guest_str(A0);
    bool iop = runs_on_iop(path);
    int r = iop ? start_on_iop(path) : 0;
    LOG("IOP module %s (start) -> id %d%s\n", path.c_str(), g_next_module, iop ? " (interpreted)" : "");
    // int *result is the 4th argument
    if (A3) SW(A3, (u32)r);
    RET(g_next_module++);
}

void HLE_sceSifBindRpc(Ctx &c)
{
    u32 cd = A0, sid = A1;
    g_rpc_sid[cd] = sid;
    // a server of an interpreted module answers once it has registered itself
    bool found = !(sid == SOUND_SID && iop_active()) || iop_has_server(sid);
    SW(cd + CD_SERVE, found ? 0x00000001 : 0);  // any non-zero: server found
    LOG("RPC bind %08x -> server %08x\n", cd, sid);
    RET(0);
}

void HLE_sceSifCallRpc(Ctx &c)
{
    // (cd, fno, mode, send, ssize, receive, rsize, end_func, end_param)
    u32 cd = A0, fno = A1, recv = T1, rsize = T2, endf = T3;
    u32 endp = LW(c.r[29].w[0]);
    u32 sid = g_rpc_sid.count(cd) ? g_rpc_sid[cd] : 0;
    static std::map<u64, int> seen;
    if (seen[((u64)sid << 32) | fno]++ < 3)
        LOG("RPC call server %08x fn %u send %08x/%u recv %08x/%u (ra %08x)\n", sid, fno,
            T0 ? A3 : 0, T0, recv, rsize, c.r[31].w[0]);
    if (sid == SOUND_SID) snd_log_packet(fno, A3, T0, c.r[31].w[0]);
    if (iop_has_server(sid)) {
        iop_call_rpc(sid, fno, A3, T0, recv, rsize);
        // HP2_SNDLOG: the driver's answer too (its first 256 bytes: the channels' and streams' status), the words that changed since the last
        FILE *f = sid == SOUND_SID && recv && rsize ? snd_log() : nullptr;
        if (f) {
            static u16 last[128];
            const u32 n = std::min<u32>(rsize / 2, 128);
            bool any = false;
            for (u32 i = 0; i < n; i++) {
                const u16 w = LH(recv + 2 * i);
                if (w == last[i]) continue;
                if (!any) fprintf(f, "%9.3f reply:", snd_time());
                any = true;
                fprintf(f, " [%u]=%04x", i, w);
                last[i] = w;
            }
            if (any) fprintf(f, "\n");
        }
    } else if (recv && rsize && rsize < 0x100000) {
        // no server runs on our side: answer with zeros (success for most of them)
        for (u32 i = 0; i < rsize; i++) SB(recv + i, 0);
    }
    call_guest(c, endf, endp);
    RET(0);
}
void HLE_sceSifCheckStatRpc(Ctx &c) { RET(0); }              // not busy

// ---- CD/DVD ----------------------------------------------------------------------------
static u32 g_cd_error;

void HLE_sceCdInit(Ctx &c) { RET(1); }
void HLE_sceCdInitEeCB(Ctx &c) { RET(1); }
void HLE_sceCdMmode(Ctx &c) { RET(1); }
void HLE_sceCdDiskReady(Ctx &c) { RET(2); }                 // SCECdComplete
void HLE_sceCdNcmdDiskReady(Ctx &c) { RET(2); }
void HLE_sceCdGetDiskType(Ctx &c) { RET(0x14); }            // SCECdPS2DVD
void HLE_sceCdGetError(Ctx &c) { RET(g_cd_error); }
void HLE_sceCdSync(Ctx &c) { RET(0); }                      // done
void HLE_sceCdSyncS(Ctx &c) { RET(0); }
// sceCdCLOCK: stat, then BCD second, minute, hour, a pad, day, month, year. The one caller, the NIS overlay's camcorder date, turns it
// to local time with the call below. The PC's clock is local already, so this gives that and the call leaves it alone.
void HLE_sceCdReadClock(Ctx &c)
{
    auto bcd = [](int v) { return (u8)(v / 10 << 4 | v % 10); };
    time_t now = time(nullptr);
    struct tm t = *localtime(&now);
    SB(A0, 0);
    SB(A0 + 1, bcd(t.tm_sec));
    SB(A0 + 2, bcd(t.tm_min));
    SB(A0 + 3, bcd(t.tm_hour));
    SB(A0 + 4, 0);
    SB(A0 + 5, bcd(t.tm_mday));
    SB(A0 + 6, bcd(t.tm_mon + 1));
    SB(A0 + 7, bcd(t.tm_year % 100));
    RET(1);
}

void HLE_sceScfGetLocalTimefromRTC(Ctx &c) { RET(0); }

// sceCdlFILE: lsn, size, name[16], date[8]
void HLE_sceCdSearchFile(Ctx &c)
{
    std::string name = guest_str(A1);
    DiscFile e;
    bool ok = disc_find(name, e);
    LOG("sceCdSearchFile %s -> %s lsn %u size %u\n", name.c_str(), ok ? "found" : "MISSING",
        ok ? e.lsn : 0, ok ? e.size : 0);
    if (ok) {
        SW(A0, e.lsn);
        SW(A0 + 4, e.size);
    }
    RET(ok ? 1 : 0);
}

void HLE_sceCdRead(Ctx &c)
{
    u32 lsn = A0, sectors = A1, buf = A2;
    static int n;
    if (n++ < 8) LOG("sceCdRead lsn %u x%u -> %08x\n", lsn, sectors, buf);
    std::vector<u8> tmp((size_t)sectors * 2048);
    bool ok = disc_read(lsn, sectors, tmp.data());
    for (size_t i = 0; i < tmp.size(); i++) SB(buf + (u32)i, tmp[i]);
    g_cd_error = ok ? 0 : 0x32;
    RET(ok ? 1 : 0);
}

// sceCdRChain entries {lbn, sectors, buffer}, terminated by lbn 0xFFFFFFFF
void HLE_sceCdReadChain(Ctx &c)
{
    static int n;
    bool ok = true;
    for (u32 tag = A0;; tag += 12) {
        u32 lsn = LW(tag), sectors = LW(tag + 4), buf = LW(tag + 8);
        if (lsn == 0xFFFFFFFF || sectors == 0xFFFFFFFF) break;
        if (n++ < 8) LOG("sceCdReadChain lsn %u x%u -> %08x\n", lsn, sectors, buf);
        std::vector<u8> tmp((size_t)sectors * 2048);
        ok &= disc_read(lsn, sectors, tmp.data());
        for (size_t i = 0; i < tmp.size(); i++) SB(buf + (u32)i, tmp[i]);
    }
    g_cd_error = ok ? 0 : 0x32;
    RET(ok ? 1 : 0);
}

// ---- IOP file I/O ------------------------------------------------------------------
struct HostFile {
    DiscFile e;
    u32 pos;
};
static std::map<int, HostFile> g_files;
static int g_next_fd = 3;

void HLE_sceFsInit(Ctx &c) { RET(0); }
void HLE_sceFsReset(Ctx &c) { RET(0); }
void HLE_sceOpen(Ctx &c)
{
    std::string name = guest_str(A0);
    DiscFile e;
    if (!disc_find(name, e)) {
        LOG("sceOpen %s -> not found\n", name.c_str());
        RET(-2);                            // ENOENT
        return;
    }
    int fd = g_next_fd++;
    g_files[fd] = {e, 0};
    LOG("sceOpen %s -> fd %d\n", name.c_str(), fd);
    RET(fd);
}
void HLE_sceClose(Ctx &c) { g_files.erase((int)A0); RET(0); }
void HLE_sceRead(Ctx &c)
{
    auto it = g_files.find((int)A0);
    if (it == g_files.end()) { RET(-9); return; }
    HostFile &f = it->second;
    u32 n = A2;
    if (f.pos + n > f.e.size) n = f.e.size - f.pos;
    std::vector<u8> tmp(n);
    if (n) disc_read_file(f.e, f.pos, n, tmp.data());
    for (u32 i = 0; i < n; i++) SB(A1 + i, tmp[i]);
    f.pos += n;
    RET(n);
}
void HLE_sceLseek(Ctx &c)
{
    auto it = g_files.find((int)A0);
    if (it == g_files.end()) { RET(-9); return; }
    HostFile &f = it->second;
    s32 off = (s32)A1;
    f.pos = A2 == 0 ? off : A2 == 1 ? f.pos + off : f.e.size + off;
    RET(f.pos);
}
void HLE_sceWrite(Ctx &c) { RET(A2); }
void HLE_sceIoctl(Ctx &c) { RET(0); }
void HLE_sceDevctl(Ctx &c) { RET(0); }

// ---- pad -----------------------------------------------------------------------------
void HLE_scePadInit(Ctx &c) { RET(1); }
void HLE_scePadInit2(Ctx &c) { RET(1); }
void HLE_scePadEnd(Ctx &c) { RET(1); }
// Both ports always have a pad: the second player can always have the keyboard (window_pad). HP2_PADSCRIPT2 plays port 1.
static bool pad_port_used(u32 port) { return port < 2; }
void HLE_scePadPortOpen(Ctx &c) { RET(pad_port_used(A0) ? 1 : 0); }
void HLE_scePadPortClose(Ctx &c) { RET(1); }
void HLE_scePadGetState(Ctx &c) { RET(pad_port_used(A0) ? 6 : 0); }  // scePadStateStable / Disconnect
void HLE_scePadGetReqState(Ctx &c) { RET(0); }              // scePadReqStateComplete
void HLE_scePadSetReqState(Ctx &c) { RET(0); }
u16 window_pad(u8 sticks[4], u8 pressure[12]);
u16 window_pad2(u8 sticks[4], u8 pressure[12]);

// HP2_PADSCRIPT=<file>: port 0 plays a script instead of the keyboard, for repeatable runs
// through the menus. Time is counted in port-0 pad reads (one per game frame), so loading
// times do not shift it. A line is "<frame> <button>[+<button>...] [<held frames>]"; a
// frame written "+N" is N after the previous line; held frames default to 6; '#' starts a
// comment. Buttons: X O T S (cross circle triangle square) UP DOWN LEFT RIGHT START SELECT
// L1 R1 L2 R2. Every 60 frames run.log gets "pad frame N" with the host time.
template <int PORT> static bool pad_script(u16 &buttons, u8 sticks[4], u8 pressure[12])
{
    struct Step { long t, hold; u16 mask; };
    static const std::vector<Step> steps = [] {
        std::vector<Step> v;
        const char *path = getenv(PORT ? "HP2_PADSCRIPT2" : "HP2_PADSCRIPT");
        FILE *f = path ? fopen(path, "r") : nullptr;
        if (!f) return v;
        static const struct { const char *name; int bit; } names[] = {
            {"SELECT", 0}, {"START", 3}, {"UP", 4}, {"RIGHT", 5}, {"DOWN", 6}, {"LEFT", 7},
            {"L2", 8}, {"R2", 9}, {"L1", 10}, {"R1", 11}, {"T", 12}, {"O", 13}, {"X", 14}, {"S", 15}};
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (char *h = strchr(line, '#')) *h = 0;
            char when[32], keys[128];
            Step s = {0, 6, 0};
            int n = sscanf(line, "%31s %127s %ld", when, keys, &s.hold);
            if (n < 2) continue;
            s.t = atol(when + (when[0] == '+')) + (when[0] == '+' && !v.empty() ? v.back().t : 0);
            for (char *k = strtok(keys, "+"); k; k = strtok(nullptr, "+"))
                for (auto &nm : names)
                    if (!strcmp(k, nm.name)) s.mask |= (u16)(1u << nm.bit);
            v.push_back(s);
        }
        fclose(f);
        LOG("pad script %s: %zu steps\n", path, v.size());
        return v;
    }();
    if (steps.empty()) return false;
    static long t = -1;
    t++;
    if (t % 60 == 0) {
        time_t now = time(nullptr);
        struct tm tmv = *localtime(&now);
        LOG("pad frame %ld at %02d:%02d:%02d\n", t, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        fflush(g_hle_log);
    }
    // pressure order: right left up down triangle circle cross square L1 R1 L2 R2
    static const int press_of_bit[16] = {-1, -1, -1, -1, 2, 0, 3, 1, 10, 11, 8, 9, 4, 5, 6, 7};
    buttons = 0xFFFF;
    static size_t logged;
    while (logged < steps.size() && t >= steps[logged].t)
        LOG("pad script: step %zu at frame %ld (buttons %04x)\n", logged, t, steps[logged].mask), logged++;
    for (auto &s : steps)
        if (t >= s.t && t < s.t + s.hold)
            for (int b = 0; b < 16; b++)
                if (s.mask & (1u << b)) {
                    buttons &= (u16)~(1u << b);
                    if (press_of_bit[b] >= 0) pressure[press_of_bit[b]] = 0xFF;
                }
    // the left stick follows the D-pad, as with the keyboard
    if (!(buttons & 0x80)) sticks[2] = 0x00;
    if (!(buttons & 0x20)) sticks[2] = 0xFF;
    if (!(buttons & 0x10)) sticks[3] = 0x00;
    if (!(buttons & 0x40)) sticks[3] = 0xFF;
    return true;
}

void HLE_scePadRead(Ctx &c)
{
    u32 buf = A2;
    u8 sticks[4], pressure[12];
    u16 buttons = 0xFFFF;
    if (A0 == 0) {
        buttons = window_pad(sticks, pressure);
        pad_script<0>(buttons, sticks, pressure);
    } else {
        memset(sticks, 0x80, 4);
        memset(pressure, 0, 12);
        if (A0 == 1) {
            buttons = window_pad2(sticks, pressure);
            pad_script<1>(buttons, sticks, pressure);
        }
    }
    for (int i = 0; i < 32; i++) SB(buf + i, 0);
    SB(buf + 0, 0x00);                      // success
    SB(buf + 1, 0x79);                      // DualShock 2 in analog mode, pressure data follows
    SB(buf + 2, (u8)buttons);               // buttons, active low
    SB(buf + 3, (u8)(buttons >> 8));
    for (int i = 0; i < 4; i++) SB(buf + 4 + i, sticks[i]);
    for (int i = 0; i < 12; i++) SB(buf + 8 + i, pressure[i]);
    RET(pad_port_used(A0) ? 32 : 0);
}
void HLE_scePadInfoMode(Ctx &c)
{
    // (port, slot, term, offs): current ID 7 (analog), 2 modes, current 1
    switch ((s32)A2) {
    case 1: RET(7); break;                  // InfoModeCurID
    case 2: RET(1); break;                  // InfoModeCurExID
    case 3: RET(1); break;                  // InfoModeCurExOffs
    case 4: RET(A3 == (u32)-1 ? 2 : A3 == 0 ? 4 : 7); break;  // InfoModeIdTable
    default: RET(0);
    }
}
void HLE_scePadSetMainMode(Ctx &c) { RET(1); }
void HLE_scePadInfoAct(Ctx &c) { RET(A3 == (u32)-1 ? 2 : 1); }
void HLE_scePadSetActAlign(Ctx &c) { RET(1); }
void window_rumble_port(int port, int small, int large);
// (port, slot, char data[6]): the align the game sets is the DualShock's, small motor then big
void HLE_scePadSetActDirect(Ctx &c)
{
    if (A0 < 2 && A2) window_rumble_port(A0, LB(A2), LB(A2 + 1));
    RET(1);
}
void HLE_scePadInfoPressMode(Ctx &c) { RET(1); }
void HLE_scePadEnterPressMode(Ctx &c) { RET(1); }
void HLE_scePadExitPressMode(Ctx &c) { RET(1); }
void HLE_scePadInfoComb(Ctx &c) { RET(0); }
void HLE_scePadGetPortMax(Ctx &c) { RET(2); }
void HLE_scePadGetSlotMax(Ctx &c) { RET(1); }
void HLE_scePadGetModVersion(Ctx &c) { RET(0x0400); }
void HLE_scePadSetButtonInfo(Ctx &c) { RET(1); }
void HLE_scePadGetButtonMask(Ctx &c) { RET(0xFFFF); }
void HLE_scePadSetVrefParam(Ctx &c) { RET(1); }
void HLE_scePadSetWarningLevel(Ctx &c) { RET(1); }

// ---- memory card ---------------------------------------------------------------------
// libmc at its API: every call is a request that cMC finishes with sceMcSync, like the
// RPC to MCSERV on the PS2. The card of port 0 is the folder saves/ next to the exe (or
// HP2_MEMCARD), always formatted and known; port 1 has none. The game's save directory is that
// folder itself and its save file saves/save.dat; the card's icons are taken and thrown away.
// HP2_MEMCARD=none: no card. Details: work/memcard/README.md.
namespace {

// libmc result codes (sceMcRes*) and command numbers (sceMcFunc*)
enum { MC_CHANGED = -1, MC_NOFORMAT = -2, MC_FULL = -3, MC_NOENTRY = -4, MC_DENIED = -5,
       MC_NOTEMPTY = -6, MC_UPLIMIT = -7, MC_NOCARD = -11 };
enum { MCF_GETINFO = 1, MCF_OPEN = 2, MCF_CLOSE = 3, MCF_READ = 5, MCF_WRITE = 6, MCF_MKDIR = 11,
       MCF_GETDIR = 13, MCF_DELETE = 15, MCF_FORMAT = 16, MCF_UNFORMAT = 17 };
// sceMcOpen mode bits
enum { MC_RDONLY = 1, MC_WRONLY = 2, MC_RDWR = 3, MC_CREAT = 0x200 };
// mcfs entry attributes as a PS2 card reports them
const u16 ATTR_DIR = 0x8427, ATTR_FILE = 0x8497;
const int MC_CLUSTERS = 8000;               // free 1 KB clusters of an empty 8 MB card
const int MC_MAX_FILES = 3;                 // mcman's open file limit

struct McFile {
    bool used = false;
    int port = 0;
    std::string host;                       // host path
    bool dir = false, writable = false, sink = false;  // sink: a file the PC does not keep
    u32 pos = 0;
};

struct McState {
    bool init = false;
    std::string root;                       // empty: no cards at all
    McFile files[MC_MAX_FILES];
    // the request that sceMcSync finishes
    bool pending = false;
    int cmd = 0;
    s32 result = 0;
    int busy = 0;                           // non-blocking polls still answered "running"
    int busy_polls = 1;
    // sceMcGetDir continues a listing with mode 1
    std::vector<std::string> dir_names;
    std::string dir_host;
    size_t dir_next = 0;
} g_mc;

FILE *mc_log()
{
    static FILE *f = [] {
        const char *path = getenv("HP2_MCLOG");
        return path ? fopen(path, "w") : nullptr;
    }();
    return f;
}

#define MCLOG(...) do { LOG(__VA_ARGS__); if (FILE *f_ = mc_log()) { fprintf(f_, __VA_ARGS__); fflush(f_); } } while (0)

void mc_setup()
{
    if (g_mc.init) return;
    g_mc.init = true;
    if (const char *b = getenv("HP2_MCBUSY")) g_mc.busy_polls = atoi(b);
    const char *env = getenv("HP2_MEMCARD");
    if (env && !strcmp(env, "none")) {
        MCLOG("memory card: none (HP2_MEMCARD=none)\n");
        return;
    }
    std::filesystem::path root;
    if (env) {
        root = env;
    } else {
#ifdef _WIN32
        char *exe = nullptr;
        _get_pgmptr(&exe);
        root = std::filesystem::path(exe ? exe : "").parent_path() / "saves";
#else
        root = "saves";
#endif
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    g_mc.root = root.string();
    MCLOG("saves in %s%s\n", g_mc.root.c_str(), ec ? " (cannot create it)" : "");
}

// Profiles: saves/<name>/, each with its own save.dat, which is that profile's card. HP2_MEMCARD (the tests) keeps one card and no profiles.
}   // namespace

static std::string g_mc_base;

bool mc_profiles_enabled()
{
    mc_setup();
    return !getenv("HP2_MEMCARD") && !g_mc.root.empty();
}

std::string mc_profile_dir(const std::string &name)
{
    mc_setup();
    if (g_mc_base.empty()) g_mc_base = g_mc.root;
    return g_mc_base + "/" + name;
}

// the names; a save from before profiles becomes the first
std::vector<std::string> mc_profiles()
{
    mc_setup();
    if (g_mc_base.empty()) g_mc_base = g_mc.root;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(g_mc_base + "/save.dat", ec)) {
        fs::create_directories(g_mc_base + "/Player1", ec);
        fs::rename(g_mc_base + "/save.dat", g_mc_base + "/Player1/save.dat", ec);
    }
    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(g_mc_base, ec))
        if (e.is_directory(ec)) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

void mc_profile_use(const std::string &name)
{
    const std::string dir = mc_profile_dir(name);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g_mc.root = dir;
    MCLOG("profile %s: saves in %s\n", name.c_str(), dir.c_str());
}

bool mc_profile_delete(const std::string &name)
{
    if (name.empty() || name == "." || name == "..") return false;
    std::error_code ec;
    return std::filesystem::remove_all(mc_profile_dir(name), ec) > 0 && !ec;
}

namespace {

std::string mc_card(int port)
{
    if (g_mc.root.empty() || port != 0) return {};
    std::error_code ec;
    return std::filesystem::is_directory(g_mc.root, ec) ? g_mc.root : std::string();
}

bool mc_formatted(const std::string &card)
{
    std::error_code ec;
    return !std::filesystem::exists(card + "/.unformatted", ec);
}

// Card path ("/DIR/FILE", "DIR", ...) -> host path; false for names a card or the host
// cannot hold. The current directory is always the root (the game never calls sceMcChdir).
bool mc_host_path(const std::string &card, const std::string &name, std::string &out,
                  std::string *last = nullptr)
{
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= name.size()) {
        size_t next = name.find('/', pos);
        if (next == std::string::npos) next = name.size();
        std::string part = name.substr(pos, next - pos);
        pos = next + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        if (part.size() > 31) return false;
        for (char ch : part)
            if ((u8)ch < 0x20 || strchr("<>:\"\\|", ch)) return false;
        parts.push_back(part);
    }
    if (last) *last = parts.empty() ? std::string() : parts.back();
    // the game's own directory is the folder, its save save.dat; out empty: the icons, not kept
    static const char SAVE_DIR[] = "BASLUS-20362HP2";
    if (!parts.empty() && parts[0] == SAVE_DIR) {
        bool wild = parts.back().find_first_of("*?") != std::string::npos;
        out = parts.size() == 1 ? card : parts.size() == 2 && parts[1] == SAVE_DIR ? card + "/save.dat"
              : parts.size() == 2 && wild ? card + "/" + parts[1] : std::string();
        return true;
    }
    out = card;
    for (auto &p : parts) out += "/" + p;
    return true;
}

bool mc_wild(const std::string &name)
{
    return name.find_first_of("*?") != std::string::npos;
}

bool mc_match(const char *pat, const char *s)
{
    if (!*pat) return !*s;
    if (*pat == '*') return mc_match(pat + 1, s) || (*s && mc_match(pat, s + 1));
    return *s && (*pat == '?' || *pat == *s) && mc_match(pat + 1, s + 1);
}

int mc_clusters(u64 bytes) { return (int)((bytes + 1023) / 1024); }

// Clusters in use: every file by its size, every directory by its 512-byte entries
// ("." and ".." included, two to a cluster).
int mc_used(const std::string &dir, bool root = true)
{
    int used = 0, entries = 2;
    std::error_code ec;
    for (auto &e : std::filesystem::directory_iterator(dir, ec)) {
        std::string n = e.path().filename().string();
        if (n[0] == '.') continue;
        entries++;
        if (e.is_directory(ec)) used += mc_used(e.path().string(), false);
        else used += mc_clusters(e.file_size(ec));
    }
    return root ? used : used + (entries + 1) / 2;
}

// sceMcStDateTime {resv, sec, min, hour, day, month, year:u16}: the PS2 clock runs in JST
void mc_put_time(u32 a, time_t t)
{
    t += 9 * 3600;
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    SB(a, 0);
    SB(a + 1, (u8)tmv.tm_sec);
    SB(a + 2, (u8)tmv.tm_min);
    SB(a + 3, (u8)tmv.tm_hour);
    SB(a + 4, (u8)tmv.tm_mday);
    SB(a + 5, (u8)(tmv.tm_mon + 1));
    SH(a + 6, (u16)(tmv.tm_year + 1900));
}

time_t mc_mtime(const std::string &host)
{
    struct stat st;
    return stat(host.c_str(), &st) == 0 ? st.st_mtime : time(nullptr);
}

void mc_request(Ctx &c, int cmd, s32 result)
{
    if (g_mc.pending)
        MCLOG("mc: command %d issued before command %d was synced\n", cmd, g_mc.cmd);
    g_mc.pending = true;
    g_mc.cmd = cmd;
    g_mc.result = result;
    g_mc.busy = g_mc.busy_polls;
    RET(0);                                 // request accepted
}

McFile *mc_fd(s32 fd)
{
    return fd >= 0 && fd < MC_MAX_FILES && g_mc.files[fd].used ? &g_mc.files[fd] : nullptr;
}

// the error a call on this port gets before it reaches the file system, or 0
s32 mc_card_error(int port, std::string &card)
{
    card = mc_card(port);
    if (card.empty()) return MC_NOCARD;
    if (!mc_formatted(card)) return MC_NOFORMAT;
    return 0;
}

s32 mc_open(int port, const std::string &name, u32 mode, bool mkdir)
{
    std::string card, host, last;
    if (s32 e = mc_card_error(port, card)) return e;
    if (!mc_host_path(card, name, host, &last) || last.empty() || mc_wild(last)) return MC_NOENTRY;
    if (host.empty()) {
        if (mkdir || !(mode & MC_WRONLY)) return MC_NOENTRY;
        for (int fd = 0; fd < MC_MAX_FILES; fd++) {
            McFile &f = g_mc.files[fd];
            if (f.used) continue;
            f = McFile();
            f.used = f.sink = f.writable = true;
            f.port = port;
            return fd;
        }
        return MC_UPLIMIT;
    }
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(host).parent_path();
    if (!std::filesystem::is_directory(parent, ec)) return MC_NOENTRY;
    bool exists = std::filesystem::exists(host, ec);
    bool is_dir = exists && std::filesystem::is_directory(host, ec);
    if (mkdir) {
        if (exists) return MC_NOENTRY;
        if (mc_used(card) + 1 > MC_CLUSTERS) return MC_FULL;
        std::filesystem::create_directory(host, ec);
        return ec ? MC_NOENTRY : 0;
    }
    if (!exists) {
        if (!(mode & MC_CREAT)) return MC_NOENTRY;
        if (FILE *f = fopen(host.c_str(), "wb")) fclose(f);
        else return MC_NOENTRY;
    } else if (is_dir && (mode & MC_RDWR) != MC_RDONLY) {
        return MC_DENIED;
    }
    for (int fd = 0; fd < MC_MAX_FILES; fd++) {
        McFile &f = g_mc.files[fd];
        if (f.used) continue;
        f = McFile();
        f.used = true;
        f.port = port;
        f.host = host;
        f.dir = is_dir;
        f.writable = (mode & MC_WRONLY) != 0;
        return fd;
    }
    return MC_UPLIMIT;
}

s32 mc_rw(Ctx &c, s32 fd, u32 buf, u32 size, bool write)
{
    McFile *f = mc_fd(fd);
    if (!f) return MC_NOENTRY;
    if (f->dir || (write && !f->writable)) return MC_DENIED;
    if (f->sink) return write ? (s32)size : 0;
    std::string card;
    if (s32 e = mc_card_error(f->port, card)) return e;
    std::vector<u8> tmp(size);
    FILE *h = fopen(f->host.c_str(), write ? "r+b" : "rb");
    if (!h) return MC_NOENTRY;
    s32 n;
    if (write) {
        std::error_code ec;
        u64 old = std::filesystem::file_size(f->host, ec);
        u64 grown = std::max<u64>(old, (u64)f->pos + size);
        if (mc_used(card) + mc_clusters(grown) - mc_clusters(old) > MC_CLUSTERS) {
            fclose(h);
            return MC_FULL;
        }
        for (u32 i = 0; i < size; i++) tmp[i] = LB(buf + i);
        fseek(h, (long)f->pos, SEEK_SET);
        n = (s32)fwrite(tmp.data(), 1, size, h);
    } else {
        fseek(h, (long)f->pos, SEEK_SET);
        n = (s32)fread(tmp.data(), 1, size, h);
        for (s32 i = 0; i < n; i++) SB(buf + i, tmp[i]);
    }
    fclose(h);
    f->pos += n;
    return n;
}

s32 mc_delete(int port, const std::string &name)
{
    std::string card, host, last;
    if (s32 e = mc_card_error(port, card)) return e;
    if (!mc_host_path(card, name, host, &last) || last.empty() || host.empty()) return MC_NOENTRY;
    std::error_code ec;
    if (!std::filesystem::exists(host, ec)) return MC_NOENTRY;
    for (auto &f : g_mc.files)
        if (f.used && f.host == host) return MC_DENIED;
    if (std::filesystem::is_directory(host, ec) && !std::filesystem::is_empty(host, ec))
        return MC_NOTEMPTY;
    return std::filesystem::remove(host, ec) ? 0 : MC_DENIED;
}

// sceMcTblGetDir: create(8) modify(8) FileSizeByte AttrFile:u16 Reserve1:u16 Reserve2
// PdaAplNo EntryName[32]
s32 mc_getdir(Ctx &c, int port, const std::string &name, u32 mode, s32 maxent, u32 table)
{
    if (mode == 0) {
        std::string card, host, pattern;
        if (s32 e = mc_card_error(port, card)) return e;
        if (!mc_host_path(card, name, host, &pattern)) return MC_NOENTRY;
        std::error_code ec;
        std::string dir = host;
        if (pattern.empty()) {
            pattern = "*";
        } else {
            dir = std::filesystem::path(host).parent_path().string();
            if (!mc_wild(pattern) && !std::filesystem::exists(host, ec)) return 0;
        }
        if (!std::filesystem::is_directory(dir, ec)) return MC_NOENTRY;
        std::vector<std::string> names;
        if (dir != card) names = {".", ".."};
        std::vector<std::string> files;
        for (auto &e : std::filesystem::directory_iterator(dir, ec)) {
            std::string n = e.path().filename().string();
            if (n[0] != '.') files.push_back(n);
        }
        std::sort(files.begin(), files.end());
        names.insert(names.end(), files.begin(), files.end());
        g_mc.dir_names.clear();
        for (auto &n : names)
            if (mc_match(pattern.c_str(), n.c_str())) g_mc.dir_names.push_back(n);
        g_mc.dir_host = dir;
        g_mc.dir_next = 0;
    }
    s32 count = 0;
    for (; count < maxent && g_mc.dir_next < g_mc.dir_names.size(); count++) {
        const std::string &n = g_mc.dir_names[g_mc.dir_next++];
        std::string host = n == "." ? g_mc.dir_host
                         : n == ".." ? std::filesystem::path(g_mc.dir_host).parent_path().string()
                                     : g_mc.dir_host + "/" + n;
        std::error_code ec;
        bool is_dir = std::filesystem::is_directory(host, ec);
        u32 size = 0;
        if (!is_dir) {
            size = (u32)std::filesystem::file_size(host, ec);
        } else if (n == ".") {
            // a directory's length is its number of entries
            size = 2;
            for (auto &e : std::filesystem::directory_iterator(host, ec))
                if (e.path().filename().string()[0] != '.') size++;
        }
        u32 t = table + 64 * count;
        for (u32 i = 0; i < 64; i++) SB(t + i, 0);
        time_t mt = mc_mtime(host);
        mc_put_time(t, mt);
        mc_put_time(t + 8, mt);
        SW(t + 16, size);
        SH(t + 20, is_dir ? ATTR_DIR : ATTR_FILE);
        for (size_t i = 0; i < n.size() && i < 31; i++) SB(t + 32 + (u32)i, (u8)n[i]);
    }
    return count;
}

} // namespace

void HLE_sceMcInit(Ctx &c) { mc_setup(); RET(0); }
void HLE_sceMcEnd(Ctx &c) { RET(0); }
void HLE_sceMcUdCheckNewCard(Ctx &c) { RET(0); }

// (port, slot, int *type, int *free, int *format)
void HLE_sceMcGetInfo(Ctx &c)
{
    mc_setup();
    int port = (int)A0;
    u32 type = A2, free = A3, format = T0;
    std::string card = mc_card(port);
    s32 r;
    if (card.empty()) {
        // cMC::MCGetInfoResult maps results below -10 to eMC_NO_CARD
        r = MC_NOCARD;
    } else {
        bool fmt = mc_formatted(card);
        if (type) SW(type, 2);              // PS2 card
        if (free) SW(free, (u32)(fmt ? std::max(0, MC_CLUSTERS - mc_used(card)) : 0));
        if (format) SW(format, fmt ? 1 : 0);
        r = fmt ? 0 : MC_NOFORMAT;
    }
    static s32 last_logged = 1;
    if (r != last_logged) MCLOG("sceMcGetInfo port %d -> %d\n", port, r);
    last_logged = r;
    mc_request(c, MCF_GETINFO, r);
}

void HLE_sceMcOpen(Ctx &c)
{
    std::string name = guest_str(A2);
    s32 r = mc_open((int)A0, name, A3, false);
    MCLOG("sceMcOpen port %d %s mode %#x -> %d\n", (int)A0, name.c_str(), A3, r);
    mc_request(c, MCF_OPEN, r);
}

void HLE_sceMcMkdir(Ctx &c)
{
    std::string name = guest_str(A2);
    s32 r = mc_open((int)A0, name, 0, true);
    MCLOG("sceMcMkdir port %d %s -> %d\n", (int)A0, name.c_str(), r);
    mc_request(c, MCF_MKDIR, r);
}

void HLE_sceMcClose(Ctx &c)
{
    McFile *f = mc_fd((s32)A0);
    if (f) *f = McFile();
    MCLOG("sceMcClose %d -> %d\n", (s32)A0, f ? 0 : MC_NOENTRY);
    mc_request(c, MCF_CLOSE, f ? 0 : MC_NOENTRY);
}

void HLE_sceMcRead(Ctx &c)
{
    s32 r = mc_rw(c, (s32)A0, A1, A2, false);
    MCLOG("sceMcRead %d %u bytes -> %d\n", (s32)A0, A2, r);
    mc_request(c, MCF_READ, r);
}

void HLE_sceMcWrite(Ctx &c)
{
    s32 r = mc_rw(c, (s32)A0, A1, A2, true);
    MCLOG("sceMcWrite %d %u bytes -> %d\n", (s32)A0, A2, r);
    mc_request(c, MCF_WRITE, r);
}

// (port, slot, name, mode, maxent, sceMcTblGetDir *table)
void HLE_sceMcGetDir(Ctx &c)
{
    std::string name = guest_str(A2);
    s32 r = mc_getdir(c, (int)A0, name, A3, (s32)T0, T1);
    MCLOG("sceMcGetDir port %d %s mode %u -> %d\n", (int)A0, name.c_str(), A3, r);
    mc_request(c, MCF_GETDIR, r);
}

void HLE_sceMcDelete(Ctx &c)
{
    std::string name = guest_str(A2);
    s32 r = mc_delete((int)A0, name);
    MCLOG("sceMcDelete port %d %s -> %d\n", (int)A0, name.c_str(), r);
    mc_request(c, MCF_DELETE, r);
}

// Format empties the card and marks it formatted; Unformat empties it and marks it not
static s32 mc_wipe(int port, bool formatted)
{
    std::string card = mc_card(port);
    if (card.empty()) return MC_NOCARD;
    for (auto &f : g_mc.files)
        if (f.used && f.port == port) f = McFile();
    std::error_code ec;
    for (auto &e : std::filesystem::directory_iterator(card, ec))
        std::filesystem::remove_all(e.path(), ec);
    if (!formatted)
        if (FILE *f = fopen((card + "/.unformatted").c_str(), "wb")) fclose(f);
    return 0;
}

void HLE_sceMcFormat(Ctx &c)
{
    s32 r = mc_wipe((int)A0, true);
    MCLOG("sceMcFormat port %d -> %d\n", (int)A0, r);
    mc_request(c, MCF_FORMAT, r);
}

void HLE_sceMcUnformat(Ctx &c)
{
    s32 r = mc_wipe((int)A0, false);
    MCLOG("sceMcUnformat port %d -> %d\n", (int)A0, r);
    mc_request(c, MCF_UNFORMAT, r);
}

// (mode, int *cmd, int *result): 1 finished, 0 still running (mode 1 only), -1 idle
void HLE_sceMcSync(Ctx &c)
{
    if (!g_mc.pending) {
        RET(-1);
        return;
    }
    if (A0 == 1 && g_mc.busy > 0) {
        g_mc.busy--;
        RET(0);
        return;
    }
    g_mc.pending = false;
    if (A1) SW(A1, (u32)g_mc.cmd);
    if (A2) SW(A2, (u32)g_mc.result);
    RET(1);
}
