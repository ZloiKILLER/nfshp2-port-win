#pragma once
// The stall catcher (profile.cpp). A race's picture now and then stands still for half a second or a second, rarely and not to be had
// by a script: so the game watches itself. A thread sees the frames go by; when none has come for 150 ms in a race where they were
// coming, it writes into the log where the game's thread and the renderer's thread are, a few times while the picture stands, as
// "stall:" lines: the module and the offset in it, and the return addresses into the program found up the stack (for
// tools/recomp/stallmap.py and the speed.map of that very build). It costs nothing while the frames come.
// Beside it the frame's own account, "native: long frame" (native_vk.cpp), is written for every frame of more than 100 ms, with the
// time the frame spent reading the disc and waiting for the sound processor.
#include <atomic>

void stall_watch_start();           // on the game's thread, once
void stall_render_thread();         // on the renderer's thread, once
void stall_frame();                 // a frame has been handed to the screen
// The world's clock, for the catcher: false when the world has a reason to stand (not a race, a menu, a replay, not the mode of
// driving). The user's first catch (2026-11-13) had neither a long frame nor a thread that stood: the frames went on, the sound went
// on, and the world stood for about a second and then went on from where it was. So the catcher also sees the world's clock stand
// while frames are shown, and writes down where the threads are then ("stall: the world stands").
bool stall_world(unsigned &clock_bits, unsigned &paused, unsigned &slow);
void stall_test();                  // on the game's thread each frame: HP2_STALLTEST=<ms> stands still that long once, 10 s into a race
extern std::atomic<int> g_stall_cd_us, g_stall_rpc_us;      // this frame's microseconds in the disc's reads and in the calls to the IOP

// the microseconds from its making to its end, added to one of those
struct StallTimer {
    std::atomic<int> &to;
    long long from;
    explicit StallTimer(std::atomic<int> &t);
    ~StallTimer();
};
