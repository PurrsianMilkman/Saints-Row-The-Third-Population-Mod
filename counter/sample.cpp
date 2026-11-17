// Reads the engine's counters and formats the overlay text. Engine functions are called inside __try guards and report -1 on a fault.
#include "counter.h"

char g_txt[256] = "SR3 counter starting...";

typedef int (__cdecl *fn_v)(void);
typedef int (__cdecl *fn_p)(void*);
static DWORD g_exc;

static int callV(uintptr_t f)    { if (!f) return -9; __try { return ((fn_v)f)(); } __except (g_exc = GetExceptionCode(), 1) { return -1; } }
static int callTarget()          { __try { return ((fn_p)B.targetFn)(0); } __except (g_exc = GetExceptionCode(), 1) { return -1; } }

// Walk a parked-vehicle list: next pointer at +0xC8; a node equal to the head ends it (matches the engine's own loop).
static int countList(uintptr_t headAddr) {
    if (!readable((void*)headAddr, 4)) return -1;
    char* head = *(char**)headAddr; int n = 0;
    for (char* p = head; p && n < 512; ) {
        if (!readable(p + 0xC8, 4)) return -2;
        n++; char* nx = *(char**)(p + 0xC8); if (nx == head) break; p = nx;
    }
    return n;
}

static void logPathNextToDll(char* out, void* anyFn) {
    HMODULE me; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)anyFn, &me);
    GetModuleFileNameA(me, out, MAX_PATH); strcpy(strrchr(out, '.'), ".log");
}

// Append the overlay text as one line every 4 s (newlines become '|').
static DWORD g_lastLog;
static void logSample(const char* t) {
    if (GetTickCount() - g_lastLog < 4000) return; g_lastLog = GetTickCount();
    char lp[MAX_PATH]; logPathNextToDll(lp, (void*)&logSample);
    FILE* f = fopen(lp, "a"); if (!f) return;
    char b[512]; strncpy(b, t, 511); b[511] = 0; for (char* p = b; *p; p++) if (*p == 10) *p = '|';
    fprintf(f, "%u %s", GetTickCount() / 1000, b); fputc(10, f); fclose(f);
}

// One-shot dump of the vehicle and NPC pool objects, written the first time the world is ready (rewrites the log file).
static bool g_dumped;
static void dumpPools() {
    if (g_dumped) return; g_dumped = true;
    char lp[MAX_PATH]; logPathNextToDll(lp, (void*)&dumpPools);
    FILE* f = fopen(lp, "w"); if (!f) return;
    uintptr_t ptrs[2] = { B.poolPtr, B.npcPoolPtr }; const char* nm[2] = { "vehicle pool", "npc pool" };
    for (int k = 0; k < 2; k++) {
        fprintf(f, "== %s ptr@%08x\n", nm[k], (unsigned)ptrs[k]);
        if (!readable((void*)ptrs[k], 4)) { fprintf(f, " unreadable\n"); continue; }
        char* o = *(char**)ptrs[k]; fprintf(f, " object %p\n", o);
        if (!o || !readable(o, 0x80)) continue;
        for (int i = 0; i < 32; i++) fprintf(f, " +%02x: %08x%s", i * 4, *(unsigned*)(o + i * 4), (i % 4 == 3) ? "\n" : "");
        unsigned* vt = *(unsigned**)o; fprintf(f, " vtable %p:", vt);
        if (readable(vt, 64)) for (int i = 0; i < 16; i++) fprintf(f, " %08x", vt[i]);
        fprintf(f, "\n");
    }
    fclose(f);
}

void sample() {
    int veh = -1, parked = -1, pmax = -1, peds = -1;
    if (readable((void*)B.liveVeh, 4))   veh  = *(int*)B.liveVeh;
    if (readable((void*)B.parkedMax, 4)) pmax = *(int*)B.parkedMax;
    int carBase = readable((void*)B.carBase, 4) ? *(int*)B.carBase : -1;
    bool world = readable((void*)B.world, 4) && *(void**)B.world;

    parked = countList(B.parkedHead);                       // first list ...
    int pk2 = countList(B.parked2);                         // ... plus the second; the overlay shows the sum
    if (parked >= 0 && pk2 > 0) parked += pk2;

    int target = -1, head = -1, pool = -1, bfn = -1;
    float sc = readable((void*)B.scaleG, 4) ? *(float*)B.scaleG : -1.f;
    if (world) { dumpPools(); peds = callV(B.pedFn); target = callTarget(); head = callV(B.headFn); pool = callV(B.poolFn); bfn = callV(B.baseFn); }

    snprintf(g_txt, sizeof g_txt,
             "Vehicles (ambient): %d  target %d  [car base %d]\nParked vehicles: %d / %d\nPedestrians (ambient): %d%s\n"
             "Human headroom: %d   Vehicle pool free: %d   Distance scale: %.2f\nCar base fn: %d   Parked lists: %d + %d",
             veh, target, carBase, parked, pmax, peds, world ? "" : "  (world not ready)", head, pool, sc, bfn, parked, pk2);
    logSample(g_txt);
}
