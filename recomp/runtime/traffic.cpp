// Options > Extras > Playable Traffic Cars: "Playable traffic cars" v1.4.1, a mod by 379Felipe (thanks there to Xanvier and nlgzrgn),
// as a switch (the user, 2026-11-18). The mod is an xdelta patch of the disc image. Decoded (tools/recomp/make_traffic_patch.py) it
// changes two things on a disc, and both are done here on the unchanged game while the switch is on:
//
//  - The cars' table, the 64 CarTypeInfo of 1376 bytes in the chunk 0x34600 of GLOBAL\GLOBALB.BUN: 846 bytes in 308 runs (the
//    fifteen traffic cars made selectable, their cameras, gauges and handling, the unused skins of five cars and of the cop cars,
//    the bus's behaviour, what the unused ambulance, bus and fire truck need to turn up as traffic). The game keeps the table as it
//    was loaded (gp-20616 points at its first record), so the runs are written straight into it, and back again when the switch
//    goes off. A run is written only where the table has what the disc had (or what the mod puts): all of them or none. The USA and
//    the European disc have the same table and the same runs; a disc with another table is left alone, and the log says so.
//
//  - Five places of the program, here as hooks on the four functions they are in:
//      CarTypeInfo::GetNumAvailableSkins (0x1884b8) and GetAvailableSkin (0x1884d8): a cop car (+1336 is 1) had one skin whatever
//        its record says; the branches for it are taken out, and every cop car has the skins of both discs;
//      BodyDamage::GetVisibleTotalDamage (0x1b50c8): returns as with damage off (the flag at 0x31a8d8 clear). The mod's fix for a
//        crash after finishing an event in a traffic car; it takes the dents of every car away with it;
//      BeACopChallengeSetup's destructor (0x1b6e30): the mode it leaves behind (0x30c77c) is 3 and not 7, which lets You're the Cop
//        take the point-to-point tracks; the mod's own note: car selection entered from that Setup goes to Single Race instead;
//      and two file names swapped so that the disc's uncompressed GLOBALB.BUN is the one loaded: the port has that file alone.
//
// Nothing of the mod's is in the game's files: with the switch off the game is as it was.
//
// Taken out on the user's word on 2026-11-18 ("I do not like how it behaves") and put back on 2026-11-24 as it was ("let us turn
// this mod on once more: a switch in Extras with the mod's name, and the game warns that it is about to restart"). The switch asks,
// then the game is started again with the setting turned and comes back to Extras (pcsettings.cpp): the table is written at the
// first frame of the menus of that start, before a car is made of it.
#include "pcconfig.h"
#include "pcmenu.h"
#include "ps2.h"
#include "traffic_patch.h"

#include <cstdio>
#include <cstring>

static const u32 kCarTable = 0x3358f0 - 20616;          // gp-20616: the first CarTypeInfo
static const u32 kCarTableBytes = 64 * 1376;
static int g_traffic_in = -1;                           // what the table has: 0 the disc's, 1 the mod's, -1 not looked at yet
static bool g_traffic_refused;                          // the table is not one the runs are for: not asked again

static bool traffic_on() { return g_cfg.traffic_cars != 0 && !g_traffic_refused; }

// the table to the mod's bytes or back to the disc's; false: it is not there yet, or not the table the runs are for
static bool traffic_table(bool on)
{
    const u32 table = LW(kCarTable);
    if (table < 0x100000 || table > 0x2000000 - kCarTableBytes) return false;
    for (const auto &set : kTrafficSets) {
        // every run has the disc's bytes or the mod's: this is the table they were made for
        bool fits = true;
        for (int i = 0; i < set.n && fits; i++) {
            const TrafficRun &r = set.runs[i];
            const u8 *at = host(table + r.at);
            fits = r.at + r.n <= kCarTableBytes && (!memcmp(at, r.was, r.n) || !memcmp(at, r.now, r.n));
        }
        if (!fits) continue;
        for (int i = 0; i < set.n; i++) {
            const TrafficRun &r = set.runs[i];
            memcpy(host(table + r.at), on ? r.now : r.was, r.n);
        }
        fprintf(stderr, "traffic cars: the cars' table has the %s bytes now (%d runs, the %s disc's set)\n", on ? "mod's" : "disc's", set.n, set.disc);
        g_traffic_in = on ? 1 : 0;
        return true;
    }
    if (!g_traffic_refused) fprintf(stderr, "traffic cars: this disc's cars' table is not one the mod's runs are for: left as it is\n");
    g_traffic_refused = true;
    return false;
}

// Once a frame of the menus (pcsettings.cpp), and when the switch is turned: the table follows the setting. Not in a race: the cars
// there were made from the table as it was.
void traffic_frame()
{
    if (g_traffic_refused || pc_racing()) return;
    const int want = g_cfg.traffic_cars ? 1 : 0;
    if (g_traffic_in == want) return;
    if (g_traffic_in < 0 && !want) {            // never touched and not wanted: nothing to do
        g_traffic_in = 0;
        return;
    }
    traffic_table(want != 0);
}

// false: the disc's table is another, the switch does nothing (for the menu's hint)
bool traffic_available() { return !g_traffic_refused; }

void HOOK_GetNumAvailableSkins__11CarTypeInfo(Ctx &c, void (*orig)(Ctx &))
{
    const u32 info = c.r[4].w[0];
    if (!traffic_on() || g_traffic_in != 1 || LW(info + 1336) != 1) {
        orig(c);
        return;
    }
    const u32 was = LW(info + 1336);            // as for any other car: the record's own count
    SW(info + 1336, 0);
    orig(c);
    SW(info + 1336, was);
}

void HOOK_GetAvailableSkin__11CarTypeInfoi(Ctx &c, void (*orig)(Ctx &))
{
    const u32 info = c.r[4].w[0];
    if (!traffic_on() || g_traffic_in != 1 || LW(info + 1336) != 1) {
        orig(c);
        return;
    }
    const u32 was = LW(info + 1336);
    SW(info + 1336, 0);
    orig(c);
    SW(info + 1336, was);
}

void HOOK_GetVisibleTotalDamage__10BodyDamage(Ctx &c, void (*orig)(Ctx &))
{
    if (!traffic_on() || g_traffic_in != 1) {
        orig(c);
        return;
    }
    const u32 kDamageOn = 0x320000 - 22312;     // what the function asks first: clear, it returns as with damage off
    const u32 was = LW(kDamageOn);
    SW(kDamageOn, 0);
    orig(c);
    SW(kDamageOn, was);
}

// (the destructor's start is 0x1b6e30: the symbols have its name eight bytes early, on the end of the function before it)
void HOOK_BeACopChallengeSetupLeave(Ctx &c, void (*orig)(Ctx &))
{
    orig(c);
    const u32 kModeLeft = 0x300000 - 16304 + 0x10000 + 1836;
    if (traffic_on() && g_traffic_in == 1 && LW(kModeLeft) == 7) SW(kModeLeft, 3);
}
