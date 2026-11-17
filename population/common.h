// Shared state and helpers for the SR3 Population plugin (SR3Population.asi).
// Nothing here knows about a specific game feature - see counts.cpp, distance.cpp, spawn.cpp, parked.cpp, pools.cpp, diag.cpp.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>

// Resolved once at startup (main.cpp) and read by every feature.
struct Ctx {
    char  ini[MAX_PATH];      // SR3Population.ini next to the .asi
    char  logPath[MAX_PATH];  // SR3Population.log next to the .asi
    bool  dx11;               // running inside SaintsRowTheThird_DX11.exe (else the DX9 exe)
    int   cars;               // ambient vehicle base count   = 12 * VehicleMultiplier
    int   peds;               // ambient pedestrian base count = 40 * PedestrianMultiplier
    int   parkedMax;          // ParkedMax (also used by the enter/exit-vehicle handlers)
    float globalScale;        // GlobalDistanceScale: the engine-wide distance scale (also drives LOD - keep 1.0)
    float popDist;            // DistanceMultiplier: vehicle despawn radius + ped radii only
};
extern Ctx   g_ctx;
extern FILE* g_log;

// Logging. Every patch writes one "OK" or "SKIP" line.
void L(const char* fmt, ...);

// Write `n` bytes at `addr` only if the current bytes equal `expect`. Logs OK/SKIP. Returns true when written.
bool patch(uintptr_t addr, const unsigned char* expect, const unsigned char* repl, size_t n, const char* what);

// Zeroed read/write memory for relocated engine tables, registered under `name` in the crash reporter's memory map (never freed).
void* modAlloc(size_t n, const char* name);
// Register any other range (e.g. a buffer the engine owns) for the crash reporter. Names must be string literals or otherwise permanent.
void regRegion(const void* base, size_t size, const char* name);
// Describe an address for crash reports: which module/section, mod-allocated table, mempool, entity pool slot, heap or stack it is in.
void describeAddress(unsigned a, char* out, size_t n);

// Executable scratch memory for code caves (never freed).
unsigned char* allocExec(size_t n);

// Little-endian emitters for building caves.
void put32(unsigned char*& p, uintptr_t v);
void putRel32(unsigned char*& p, uintptr_t target);   // writes target - (p + 4) and advances p

// Read an integer / floating setting from [Population] in SR3Population.ini, clamped to [lo, hi].
int    iniInt(const char* key, int def, int lo, int hi);
double iniDouble(const char* key, double def, double lo, double hi);

// Each feature file exposes one entry point, called in this order from main.cpp.
void apply_counts();     // ped/car base counts, ambient cap, density override, human cap, enter/exit-vehicle handlers
void apply_distance();   // global scale detours (off by default), vehicle despawn radius, ped radii, spawn ring
void apply_spawn();      // spawn timer, per-model + per-segment caps, model-cap bypass, entity budgets
void apply_parked();     // parked-car maximum
void apply_pools();      // vehicle / NPC / effect / mover / debris pool capacities and the heap sizes behind them
void installDiag();      // DX11 only: spawn-pass counters, gate wrappers, removal histogram, periodic log thread
void installCrashReporter();
void crash_tick();             // re-asserts the last-chance filter (diag monitor thread)   // crash.cpp: registers + stack + minidump on the first game-code fault
void apply_autotest();   // unattended test runs: auto-press CONTINUE (AutoContinueSeconds, off by default)
void apply_limits();     // DX11 only: spawn tracker, Havok jobs, anim registries, wheel/tyre pools, seat registry (limits.cpp)
void apply_render();     // DX11 only: renderlib instance caps, vehicle customisation slots 48 -> N, Havok TOI queue (render.cpp)
void apply_audit();      // DX11 only: mempool registry + population-linked mempool scaling + engine failure signals (audit.cpp)
void audit_report(FILE* f, int tick);   // called by the diag monitor thread every 4 s
void audit_startup_log(FILE* f);
