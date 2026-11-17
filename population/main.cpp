// SR3 Population Boost - runtime ASI (SR3Population.asi).
// Raises the limits on ambient traffic and pedestrians by writing the running process's memory. It never touches the exe on disk, and every
// patch site is compared against its expected original bytes first (see common.cpp: patch()).
//
// Layout:  common.*   log, patch(), ini helpers, shared Ctx
//          counts.cpp   base counts, ambient ceiling, density override, human cap, enter/exit-vehicle handlers
//          distance.cpp spawn ring, vehicle despawn radius, ped radii (+ the global scale detours, off by default)
//          spawn.cpp    pass timer, per-model / per-segment caps, spawn budgets
//          parked.cpp   parked-car maximum
//          pools.cpp    entity pool capacities + the heap sizes behind them
//          diag.cpp     DX11 counters / hooks / periodic log lines
//          limits.cpp   DX11 engine structures sized for vanilla human/vehicle counts
//          render.cpp   DX11 renderlib instance caps, vehicle customisation slots, Havok TOI queue
//          audit.cpp    DX11 mempool registry, population-linked mempool scaling, engine failure signals
#include "common.h"

static void init() {
    // paths: <asi dir>\SR3Population.ini / .log
    char path[MAX_PATH]; HMODULE me;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&init, &me);
    GetModuleFileNameA(me, path, MAX_PATH);
    strcpy(g_ctx.ini, path);     strcpy(strrchr(g_ctx.ini, '.'), ".ini");
    strcpy(g_ctx.logPath, path); strcpy(strrchr(g_ctx.logPath, '.'), ".log");
    g_log = fopen(g_ctx.logPath, "w");

    // settings every feature shares
    double cm = iniDouble("VehicleMultiplier", 2.0, 0.0, 1e9), pm = iniDouble("PedestrianMultiplier", 2.0, 0.0, 1e9);
    g_ctx.cars = (int)lround(12 * cm); if (g_ctx.cars < 1) g_ctx.cars = 1; if (g_ctx.cars > 800) g_ctx.cars = 800;
    g_ctx.peds = (int)lround(40 * pm); if (g_ctx.peds < 1) g_ctx.peds = 1; if (g_ctx.peds > 127) g_ctx.peds = 127;   // ped sites include imm8 operands
    L("SR3 population: cars 12 -> %d, peds 40 -> %d (ini: %s)", g_ctx.cars, g_ctx.peds, g_ctx.ini);

    char exe[MAX_PATH]; GetModuleFileNameA(0, exe, MAX_PATH);
    g_ctx.dx11 = strstr(exe, "DX11") != 0;
    L("build: %s", g_ctx.dx11 ? "DX11" : "DX9");

    g_ctx.parkedMax   = iniInt("ParkedMax", 60, 18, 120);
    g_ctx.globalScale = (float)iniDouble("GlobalDistanceScale", 1.0, 1.0, 6.0);
    g_ctx.popDist     = (float)iniDouble("DistanceMultiplier", 4.0, 1.0, 8.0);

    installCrashReporter();
    apply_counts();
    apply_distance();
    apply_spawn();
    apply_parked();
    apply_pools();
    apply_limits();
    apply_autotest();
    apply_render();
    apply_audit();
    if (g_ctx.dx11) installDiag();

    if (g_log) fclose(g_log);
    g_log = 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) init();   // synchronous: pool/heap patches must land before the engine sizes its heaps
    return TRUE;
}
