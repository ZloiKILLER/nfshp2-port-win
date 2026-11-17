#pragma once
// The processor's cores and who runs where (cores.cpp). 2026-11-14, the plan for more than one core, its stage 0.
//
// A processor's cores are not always alike: Intel's from the 12th generation have fast (P) and slow (E) ones, the phones' ARM
// processors big and little ones. The port asks the system which are which and says of each of its threads what kind it is; where the
// cores differ, the game's and the renderer's threads keep to the fast ones, the background's work (the log, the loading) to the
// slow ones, and the workers are spread over what is left, fast first. Nothing is pinned to one core: a thread is given a set of
// cores and the system moves it among them. Where the cores are alike nothing is restricted at all.
//
// The workers are a pool for work that splits into pieces: jobs_run gives the pieces out to them and to the calling thread and
// comes back when all are done. With one core (or HP2_THREADS=0) there are none and jobs_run does the pieces in order on the
// caller: the old path, to the bit.
//
// HP2_CORES=<fast>,<slow>: as on a processor of that many fast and slow cores, for a machine whose cores are alike (the cores are
// only chosen, not slowed down). HP2_THREADS=<n>: that many workers, 0 none. The layout is written to the log, "cores: ...".
enum CoreKind {
    CORE_GAME,          // the game's thread
    CORE_RENDER,        // the renderer's
    CORE_SOUND,         // the sound: the mixer, the sound driver. Short and on time
    CORE_BACKGROUND,    // what may be late: the log, the pads' polling
};
void cores_init();                      // once, before the other threads
void cores_thread(CoreKind kind);       // the calling thread, once
int cores_workers();                    // the pool's threads; 0: none
// fn(ctx, index, slot) for every index of [0, count): on the workers and on the caller, in no order, each index once. slot says who
// runs it, 0 the caller, 1.. a worker: for buffers of one's own. Not to be called from inside a piece.
void jobs_run(int count, void (*fn)(void *ctx, int index, int slot), void *ctx);
template <typename F> inline void jobs_each(int count, F &&f)
{
    jobs_run(count, [](void *ctx, int index, int slot) { (*(F *)ctx)(index, slot); }, (void *)&f);
}
