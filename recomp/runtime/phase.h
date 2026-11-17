#pragma once
// HP2_PHASES=1: where the game's thread spends a frame, by what it is doing, and what every thread of the process costs (phase.cpp).
// For the work on more than one core (2026-11-13): what can be given to another core is decided by these numbers, not guessed.
// The game's thread is in one phase at a time, the innermost of those entered; the display's phases are also split by the picture
// (the render target) the game is building then. Written to the log every 15 seconds and when a race begins or ends, apart for the
// race and for all else (the loading, the menus). Off, a phase costs a test of a flag.
enum {
    PH_OTHER,           // the game's code with no phase of its own
    PH_WAIT_TICK,       // World::GetTimestep: till the step is due
    PH_WAIT_VSYNC,      // WaitUntilVSync, eWaitUntilRenderingDone, the idle wait for an interrupt
    PH_WAIT_RENDER,     // for the renderer's thread to take the frame before
    PH_AI,              // Car::Think, GlobalCopThink
    PH_PHYSICS,         // the cars' forces and movement
    PH_COLLISIONS,      // World::DoTimestepCollisions
    PH_PARTICLES,       // AcidEffectSystem::DoTimeStep
    PH_SOUND,           // the game's own sound code: the events, the engines, sound::Update
    PH_WORLD_REST,      // the rest of World::DoTimestep
    PH_RPC,             // sceSifCallRpc: a call to the IOP (the sound driver) and the wait for its answer
    PH_DISC,            // sceCdRead
    PH_FILEQ,           // ServiceQueuedFiles: the game's queue of files
    PH_LOADING,         // ResourceFile::LoadResourceIfFileTransferFinished: what a file that has come is turned into
    PH_GAMEFLOW,        // ServiceGameFlow
    PH_MENU,            // MenuSystemTick
    PH_DISPLAY,         // eDisplayFrame with no part of its own below
    PH_CULL,            // GrandSceneryCullInfo::DoCulling
    PH_SCENERY,         // GrandSceneryCullInfo::StuffScenery
    PH_CARS,            // RenderCars, Player::RenderPlayers
    PH_WORLD_OBJECTS,   // RenderWorldObjects
    PH_EFFECTS,         // AcidEffectSystem::RenderEffects
    PH_POLYS,           // AttachSolidPolys, AttachAlphaPolys
    PH_HUD,             // FlailerRenderManager::RenderLayer
    PH_VISIBILITY,      // eModel::DetermineVisibleStrips: which of a model's strips are in sight
    PH_STRIPS,          // eModel::RenderStrips: a model's strips into the frame's lists
    PH_LISTS,           // the port's own: a list of the game's read into the frame
    PH_TEXTURES,        // the port's own: a texture decoded
    PH_COUNT
};
extern bool g_phases_on;
void phase_enter(int phase);
void phase_leave();
void phase_view(int target);        // the picture the display works on now (a render target's number), -1: none
void phase_frame();                 // the game's thread, once a frame
void phase_thread(const char *name);    // any thread, once: its name in the account of the threads
struct Phase {
    bool on;
    explicit Phase(int phase) : on(g_phases_on) { if (on) phase_enter(phase); }
    ~Phase() { if (on) phase_leave(); }
};
