// The ambient VEHICLE spawn pass: how often it runs, which candidates it accepts, and the budget gate in front of it.
//
// A pass (every 200-300 ms in vanilla) does: gates -> budget check (0x904890) -> build spawn points on the ring -> up to 3 random attempts,
// the first success ends the pass. Each stage has a hidden limit; they are lifted here.
#include "common.h"

void apply_spawn() {
    const bool dx11 = g_ctx.dx11;

    // ---- pass timer: random 200..300 ms between passes, ONE vehicle per pass. Divide the interval by SpawnSpeed.
    // The `push 300 / push 200 / mov ecx,<timer>` sequence is inlined at several call sites, so scan .text for all of them.
    float sp = (float)iniDouble("SpawnSpeed", 4.0, 1.0, 20.0);
    if (sp > 1.001f) {
        uintptr_t timerG = dx11 ? 0x130C9FC : 0x13089EC;
        unsigned char* base = (unsigned char*)GetModuleHandleA(0);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        unsigned char* t = base + sec[0].VirtualAddress; size_t tl = sec[0].Misc.VirtualSize;
        int lo = (int)(200 / sp); if (lo < 10) lo = 10;
        int hi = (int)(300 / sp); if (hi < lo + 5) hi = lo + 5;
        int hits = 0;
        for (size_t i = 0; i + 16 < tl; i++) {
            if (t[i] == 0x68 && *(int*)(t + i + 1) == 300 && t[i + 5] == 0x68 && *(int*)(t + i + 6) == 200 &&
                t[i + 10] == 0xB9 && *(uintptr_t*)(t + i + 11) == timerG) {
                unsigned char ex[10] = { 0x68,0x2C,1,0,0,0x68,0xC8,0,0,0 };
                unsigned char rp[10] = { 0x68,(unsigned char)hi,(unsigned char)(hi >> 8),0,0,0x68,(unsigned char)lo,(unsigned char)(lo >> 8),0,0 };
                char nm[48]; snprintf(nm, sizeof nm, "spawn pass timer #%d", ++hits);
                patch((uintptr_t)(t + i), ex, rp, 10, nm);
            }
        }
        L("spawn pass timer: %d sites, %d..%d ms", hits, lo, hi);
    }

    // ---- per-model nearby cap: the model picker excludes a model that already has >= 4 live copies nearby (7 with its caller flag).
    // With only a few vehicle models streamed that caps nearby traffic at ~4 x models.  (`and esi,3 / add esi,4`)
    int mcap = iniInt("ModelNearbyCap", 40, 4, 120);
    unsigned char e4[1] = { 4 }, r4[1] = { (unsigned char)mcap };
    patch(dx11 ? 0x9164D1 : 0x9174C1, e4, r4, 1, "per-model nearby cap 4");
    L("per-model nearby cap -> %d", mcap);

    // ---- road-segment occupancy: a segment already holding 3 live cars (5 on highways) is skipped as a spawn point
    int scap = iniInt("SegmentCap", 12, 3, 60);
    unsigned char e3[1] = { 3 }, r3[1] = { (unsigned char)scap }, e5[1] = { 5 }, r5[1] = { (unsigned char)(scap + scap / 2) };
    patch(dx11 ? 0x90D023 : 0x90E063, e3, r3, 1, "segment occupancy cap 3");
    patch(dx11 ? 0x90D028 : 0x90E068, e5, r5, 1, "highway segment cap 5");
    L("segment cap -> %d (highway %d)", scap, scap + scap / 2);

    // ---- per-model LIVE cap: the candidate filter rejects a streamed model once its live count reaches the group's VehicleDayCap/NightCap
    // (`cmp esi,eax / jb`). Vanilla caps are 1-6, so a handful of models bounded the whole ambient population. Make the jump unconditional.
    if (iniInt("BypassModelCap", 1, 0, 1)) {
        unsigned char ejb[2] = { 0x72,0x09 }, rjmp[2] = { 0xEB,0x09 };
        patch(dx11 ? 0x915B7F : 0x916B6F, ejb, rjmp, 2, "per-model live cap bypass");
    }

    // ---- spawn BUDGET: the pass gate (0x904890) is budget(kind,class,1,1) = (live[kind][class] + 1 <= limit) AND free-capacity(kind) >= 1.
    // (a) free-capacity (0x8EFEC0) hard-codes a TOTAL of 75 humans+drivers: ebx = 0x4B and again +0x4B in a second term,
    //     free = min(75 - used, field20 - used + 75).  Raise the first, neutralise the second.
    int bt = iniInt("BudgetTotal", 3000, 75, 100000);
    if (dx11 && bt > 75) {
        unsigned char e1b[4] = { 0x4B,0,0,0 }, r1b[4]; memcpy(r1b, &bt, 4);
        patch(0x8EFEDA, e1b, r1b, 4, "entity budget total 75");
        unsigned char e2b[7] = { 0x2B,0xC6,0x8B,0xCB,0x83,0xC0,0x4B }, r2b[7] = { 0xB8,0xFF,0xFF,0xFF,0x7F,0x8B,0xCB };
        patch(0x8EFF29, e2b, r2b, 7, "entity budget second term");
        L("entity budget total -> %d", bt);
    }
    // (b) the per-(kind,class) limit table at [0x130C304 + 12*kind + 28*class]. Vanilla: kind1/class1 = 18 (the ambient-car ceiling),
    //     kind0/class1 = 30, kind0/class2 = 150 (humans), kind1/class2 = 48 (vehicles) ... Scale every non-zero limit.
    int bm = iniInt("BudgetMultiplier", 20, 1, 100);
    if (dx11 && bm > 1) {
        int n = 0;
        for (int k = 0; k < 2; k++) for (int i = 0; i < 8; i++) {
            uintptr_t a = 0x130C304 + 12 * k + 28 * i; unsigned v = *(unsigned*)a;
            if (v == 0 || v > 1000) continue;
            unsigned nv = v * bm; unsigned char ex[4], rp[4]; memcpy(ex, &v, 4); memcpy(rp, &nv, 4);
            char nm[48]; snprintf(nm, sizeof nm, "spawn budget kind%d/class%d %u", k, i, v);
            if (patch(a, ex, rp, 4, nm)) n++;
        }
        L("spawn budgets scaled x%d (%d limits)", bm, n);
    } else if (!dx11) L("spawn budget patch is DX11-only");

    // ---- crowding throttle: after the gates the pass asks 0xB2CA10 whether >= 4 vehicles are already in the query area around the
    // player (push 4 at 0x90E3B0); if so it only proceeds when timer 0x130CA04 expired and re-arms it for 4-6 s. With dense traffic that
    // is always true, which is why only 0-1 passes per 4 s reached the candidate stage. Scale the threshold with the car multiplier.
    if (dx11) {
        int thr = 4 * g_ctx.cars / 12; if (thr < 4) thr = 4; if (thr > 127) thr = 127;
        unsigned char e2[2] = { 0x6A, 0x04 }, r2[2] = { 0x6A, (unsigned char)thr };
        if (patch(0x90E3B0, e2, r2, 2, "spawn crowding threshold 4")) L("spawn crowding threshold 4 -> %d vehicles", thr);
    }
}
