// The platform side of running as a game of its own (host.h).
#include "host.h"

#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

static bool g_windowed;                 // nobody sees stderr: the player gets a message box
static bool g_log_to_file;              // and speed.log has what was written

std::string host_exe_dir()
{
#ifdef _WIN32
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH)) {
        std::string p = exe;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }
#endif
    return "";
}

#ifdef _WIN32
// ---- speed.log, never larger than 4 MB -------------------------------------------------------------
// What the program writes to stderr goes through a pipe to a thread here. The first 2 MB go into the file as they come, as before: a run
// that goes well never writes more than a few kilobytes, and its lines are in the file at once. Past that the thread keeps the last 2 MB
// in memory and puts them after the first 2, with a line between that says how much was left out; twice a second while more comes, and
// when the program ends or stops on an error. So a run that floods the log (a fault repeated every frame) leaves the start, where the
// graphics card and the settings are named, and the end, where it went wrong, and no more.
namespace {

const DWORD kHead = 2u << 20, kTail = 2u << 20;
HANDLE g_file = INVALID_HANDLE_VALUE, g_pipe_read, g_pipe_write;
std::mutex g_log_lock;
std::vector<char> g_ring;
unsigned long long g_total;             // bytes that came
DWORD g_ring_at;                        // where the next byte goes in the ring
bool g_tail_dirty;

void put(const void *p, DWORD n)
{
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD done;
    WriteFile(g_file, p, n, &done, nullptr);
}

// the last 2 MB into the file, in order, after the first 2
void write_tail()
{
    if (!g_tail_dirty || g_file == INVALID_HANDLE_VALUE) return;
    g_tail_dirty = false;
    const unsigned long long past = g_total - kHead;
    const DWORD have = past < kTail ? (DWORD)past : kTail, first = past < kTail ? 0 : g_ring_at;
    SetFilePointer(g_file, (LONG)kHead, nullptr, FILE_BEGIN);
    if (past > kTail) {
        char note[160];
        const int len = snprintf(note, sizeof note, "\r\n[speed.log keeps its first and its last 2 MB: %llu bytes between them were left out]\r\n", past - kTail);
        put(note, (DWORD)len);
    }
    put(g_ring.data() + first, have - first < have ? kTail - first : have);
    if (past >= kTail && first) put(g_ring.data(), first);
    SetEndOfFile(g_file);
}

void take(const char *p, DWORD n)
{
    std::lock_guard<std::mutex> hold(g_log_lock);
    while (n) {
        if (g_total < kHead) {
            const DWORD part = (DWORD)(kHead - g_total) < n ? (DWORD)(kHead - g_total) : n;
            put(p, part);
            g_total += part, p += part, n -= part;
            continue;
        }
        if (g_ring.empty()) g_ring.resize(kTail);
        const DWORD part = kTail - g_ring_at < n ? kTail - g_ring_at : n;
        memcpy(g_ring.data() + g_ring_at, p, part);
        g_ring_at = (g_ring_at + part) % kTail;
        g_total += part, p += part, n -= part;
        g_tail_dirty = true;
    }
}

void log_thread()
{
    char buf[16384];
    DWORD n;
    while (ReadFile(g_pipe_read, buf, sizeof buf, &n, nullptr) && n) take(buf, n);
}

void tail_thread()
{
    for (;;) {
        Sleep(500);
        std::lock_guard<std::mutex> hold(g_log_lock);
        write_tail();
    }
}

}   // namespace
#endif

#ifdef _WIN32
void host_log_flush();
// A fault nobody caught: what it was and where, as an offset into speed.exe (speed.map names the function), into the log before Windows
// closes the program. Without this such an end leaves nothing: the log just stops.
#ifdef HP2_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
#endif

static LONG WINAPI crash_to_log(EXCEPTION_POINTERS *e)
{
    static LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr), at = (uintptr_t)r->ExceptionAddress;
    fprintf(stderr, "crash: exception %08lx at %p (speed.exe+%llx), thread %lu\n", r->ExceptionCode, r->ExceptionAddress,
            (unsigned long long)(at - base), GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        fprintf(stderr, "crash: %s address %p\n", r->ExceptionInformation[0] == 1 ? "writing" : r->ExceptionInformation[0] == 8 ? "running" : "reading",
                (void *)r->ExceptionInformation[1]);
    void *frames[24];
    const USHORT n = CaptureStackBackTrace(0, 24, frames, nullptr);
    fprintf(stderr, "crash: called from");
    for (USHORT i = 0; i < n; i++) fprintf(stderr, " +%llx", (unsigned long long)((uintptr_t)frames[i] - base));
    fprintf(stderr, "\n");
    host_log_flush();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void host_log_start(bool wanted)
{
#ifdef HP2_PGO_GEN
    // The build that counts what it runs (CMake's HP2_PGO=GEN): its counts are written when the program ends by itself, and a test run
    // is ended by the clock. HP2_PGO_SECONDS=<n>: written n seconds in (to LLVM_PROFILE_FILE), and the run ends there.
    if (const char *secs = getenv("HP2_PGO_SECONDS"))
        std::thread([n = atoi(secs)] {
            Sleep((DWORD)n * 1000);
            const int failed = __llvm_profile_write_file();
            fprintf(stderr, "pgo: the counts are written (%d)%c", failed, 10);
            fflush(stderr);
            Sleep(500);                     // the log's own thread takes the line
            TerminateProcess(GetCurrentProcess(), 0);
        }).detach();
#endif
#ifdef _WIN32
    // a GUI program has no standard handles unless whoever started it redirected them: then the log is theirs, as it comes
    // (and one that is a real file, pipe or console: a process started by another without handles of its own can be handed that one's
    // numbers, which are nothing here)
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) return;
    g_windowed = true;
    if (!wanted) {                      // log=off in hp2.ini: nothing is written anywhere
        if (freopen("NUL", "w", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
        return;
    }
    // the run before this one keeps its log, as speed_prev.log: what ended a game is asked about after it has been started again
    MoveFileExA((host_exe_dir() + "speed.log").c_str(), (host_exe_dir() + "speed_prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    SetUnhandledExceptionFilter(crash_to_log);
    if (getenv("HP2_CRASHTEST"))        // the record of a fault, tried: one made on purpose three seconds in
        std::thread([] { Sleep(3000); *(volatile int *)nullptr = 1; }).detach();
    g_file = CreateFileA((host_exe_dir() + "speed.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE || !CreatePipe(&g_pipe_read, &g_pipe_write, nullptr, 1 << 16)) {
        // no pipe: the file itself, with no limit, as it used to be
        if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
        if (freopen((host_exe_dir() + "speed.log").c_str(), "w", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
        g_log_to_file = true;
        return;
    }
    const int fd = _open_osfhandle((intptr_t)g_pipe_write, _O_TEXT);
    if (freopen("NUL", "w", stderr)) {      // stderr has no descriptor in a GUI program until it is opened on something
        _dup2(fd, _fileno(stderr));
        _setmode(_fileno(stderr), _O_TEXT);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    SetStdHandle(STD_ERROR_HANDLE, g_pipe_write);
    g_log_to_file = true;
    std::thread(log_thread).detach();
    std::thread(tail_thread).detach();
#else
    (void)wanted;
#endif
}

void host_log_flush()
{
#ifdef _WIN32
    if (g_file == INVALID_HANDLE_VALUE || !g_pipe_write) return;
    fflush(stderr);
    FlushFileBuffers(g_pipe_write);     // until the thread has read what was written
    Sleep(30);                          // and taken it
    std::lock_guard<std::mutex> hold(g_log_lock);
    write_tail();
    FlushFileBuffers(g_file);
#endif
}

void host_log_close()
{
#ifdef _WIN32
    if (g_file == INVALID_HANDLE_VALUE) return;
    host_log_flush();
    std::lock_guard<std::mutex> hold(g_log_lock);
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;      // what is written from now on is read and dropped
#endif
}

void host_fatal(const std::string &msg)
{
    fprintf(stderr, "%s\n", msg.c_str());
    fflush(stderr);
    host_log_flush();
#ifdef _WIN32
    if (g_windowed)
        MessageBoxA(nullptr, (msg + (g_log_to_file ? "\n\nDetails are in speed.log next to the game." : "\n\nThe log is off (log=off in hp2.ini).")).c_str(),
                    "Need for Speed: Hot Pursuit 2", MB_OK | MB_ICONERROR | MB_TOPMOST);
#endif
}
