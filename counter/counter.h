// Shared declarations for the SR3 Population Counter plugin (SR3PopCounter.asi). Read-only: it never writes to the game.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Addresses of the engine counters we read. DX9 and DX11 executables differ (build.cpp holds both tables).
struct Build {
    uintptr_t baseFn;      // car-base function (returns the current base count)
    uintptr_t parked2;     // head of the second parked-vehicle list
    uintptr_t poolPtr;     // pointer to the vehicle pool object
    uintptr_t npcPoolPtr;  // pointer to the NPC pool object
    uintptr_t headFn;      // human headroom (150 - humans counted)
    uintptr_t poolFn;      // vehicle pool free slots (0 = not available in this build)
    uintptr_t scaleG;      // global distance scale (float)
    uintptr_t targetFn;    // vehicle target count (takes a hood record or 0)
    uintptr_t liveVeh;     // live ambient vehicle counter
    uintptr_t parkedHead;  // head of the first parked-vehicle list
    uintptr_t parkedMax;   // parked-car maximum
    uintptr_t carBase;     // car-base global
    uintptr_t world;       // pointer to the world object (null before the world loads)
    uintptr_t pedFn;       // live ambient pedestrian count
};
extern Build B;
void selectBuild();        // picks the DX9 or DX11 table from the running exe's name

bool readable(const void* p, size_t n);   // VirtualQuery-based guard before every raw read

extern char g_txt[256];    // text shown by the overlay; written by sample()
void sample();             // reads the engine counters, formats g_txt, appends it to the log every 4 s

DWORD WINAPI overlayThread(LPVOID);   // creates the click-through window and refreshes it
