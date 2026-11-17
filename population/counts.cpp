// How many ambient cars / pedestrians the engine WANTS, and the ceilings in front of that number.
// Every address has a DX11 and a DX9 value (the two executables differ); patch() byte-checks each site before writing.
#include "common.h"

void apply_counts() {
    const bool dx11 = g_ctx.dx11;
    const int cars = g_ctx.cars, peds = g_ctx.peds, pmx = g_ctx.parkedMax;

    // ---- pedestrian base count (vanilla 40 = 0x28): two `mov eax,imm32` + two `add eax,imm8` (the in-vehicle bonus path adds to the same base)
    uintptr_t pedMov1 = dx11 ? 0x9064B3 : 0x9074D3, pedMov2 = dx11 ? 0x9064F6 : 0x907516;
    uintptr_t pedAdd1 = dx11 ? 0x9064B0 : 0x9074D0, pedAdd2 = dx11 ? 0x9064E0 : 0x9074FA;
    unsigned char e40[4] = { 0x28,0,0,0 }, r40[4] = { (unsigned char)peds,0,0,0 };
    patch(pedMov1, e40, r40, 4, "ped base (mov, no bonus)");
    patch(pedMov2, e40, r40, 4, "ped base (mov, density)");
    unsigned char e1[1] = { 0x28 }, r1[1] = { (unsigned char)peds };
    patch(pedAdd1, e1, r1, 1, "ped base (add, bonus)");
    patch(pedAdd2, e1, r1, 1, "ped base (add, bonus+dens)");

    // ---- ambient car base count (vanilla 12): one global int
    uintptr_t carGlobal = dx11 ? 0x130C9B8 : 0x13089A8;
    unsigned char eg[4] = { 12,0,0,0 }, rg[4]; memcpy(rg, &cars, 4);
    patch(carGlobal, eg, rg, 4, "car base global");

    // ---- hard ceiling on live ambient vehicles (`mov esi,0x30` before `cmp [liveCount],esi`): follow the car base
    int cap = cars * 4 / 3; if (cap < 48) cap = 48; if (cap > 1500) cap = 1500;
    unsigned char e48[4] = { 0x30,0,0,0 }, r48[4]; memcpy(r48, &cap, 4);
    patch(dx11 ? 0x90DE03 : 0x90EE43, e48, r48, 4, "ambient vehicle cap 48");
    L("ambient vehicle cap -> %d", cap);

    // ---- vehicle density override slot: when > 0 the engine uses it INSTEAD of (hood density x wanted-level scale).
    // Vanilla stores -1.0 ("off"); writing 1.0 makes the target the full base count everywhere, day and night.
    if (iniInt("ForceFullDensity", 1, 0, 1)) {
        unsigned char off[4] = { 0,0,0x80,0xBF }, one[4] = { 0,0,0x80,0x3F };
        patch(dx11 ? 0x130CB04 : 0x1308AF4, off, one, 4, "vehicle density override = 1.0");
        // the pedestrian target (0x90F680) has its own override slot just before it; without it the ped base is scaled by the hood density
        // and the wanted-level table 0x130CB08 (1, 1, .75, .5, .25, .1) - that is what held ambient peds at exactly 40 of 120
        if (dx11) {
            patch(0x130CB00, off, one, 4, "pedestrian density override = 1.0");
            // scripts lower it during missions/activities through the setter 0x90F870 and restore it with 0x90F890, which reloads -1.0
            // from a shared constant (0x12A6B9C): point that reset at a private 1.0 so full density comes back after every mission
            float* pv = (float*)modAlloc(16, "ped density reset value");
            if (pv) {
                *pv = 1.0f; unsigned na = (unsigned)(uintptr_t)pv, oa = 0x12A6B9C;
                patch(0x90F894, (const unsigned char*)&oa, (const unsigned char*)&na, 4, "pedestrian density reset -> 1.0");
            }
        }
    }

    // ---- human cap: the spawn pass only runs while (150 - humans counted) > 0; every ambient driver and ped counts
    int hcap = iniInt("HumanCap", 450, 150, 2000);
    unsigned char e150[4] = { 0x96,0,0,0 }, r150[4] = { (unsigned char)hcap,(unsigned char)(hcap >> 8),0,0 };
    patch(dx11 ? 0x9076F0 : 0x908730, e150, r150, 4, "human cap 150");
    L("human cap -> %d", hcap);

    // ---- car-base override branch: movzx esi,[flag] / neg / sbb / and esi,6 / add esi,0xC  =>  12 (+6 when the flag is set), 17 bytes.
    // Those immediates are imm8, which capped the target at 100/150 whenever the branch ran. Rebuilt with imm32 in a cave.
    {
        uintptr_t bs = dx11 ? 0x906564 : 0x907584, flagG = dx11 ? 0x5A10938 : 0x26093B8;
        unsigned char ob[17] = { 0x0F,0xB6,0x35, (unsigned char)flagG,(unsigned char)(flagG >> 8),(unsigned char)(flagG >> 16),(unsigned char)(flagG >> 24),
                                 0xF7,0xDE,0x1B,0xF6,0x83,0xE6,0x06,0x83,0xC6,0x0C };
        unsigned char* cv = allocExec(64);
        if (cv && !memcmp((void*)bs, ob, 17)) {
            unsigned char* q = cv; int half = cars / 2;
            memcpy(q, ob, 11); q += 11;                                            // movzx / neg / sbb  (unchanged)
            *q++ = 0x81; *q++ = 0xE6; memcpy(q, &half, 4); q += 4;                 // and esi, imm32
            *q++ = 0x81; *q++ = 0xC6; memcpy(q, &cars, 4); q += 4;                 // add esi, imm32
            *q++ = 0xE9; putRel32(q, bs + 17);                                     // back to the instruction after the block
            unsigned char nj[17] = { 0xE9,0,0,0,0 }; memset(nj + 5, 0x90, 12);
            { int rel = (int)((uintptr_t)cv - (bs + 5)); memcpy(nj + 1, &rel, 4); }
            patch(bs, ob, nj, 17, "car base override branch (imm32)");
        } else L("SKIP car base override branch: bytes differ");
    }

    // ---- the player's ENTER / EXIT-vehicle handlers rewrite the car-base global to 18 (driving) / 12 (on foot) and clamp parked max to 18,
    // undoing everything above each time you get in or out of a car. Point their immediates at our values.
    {
        uintptr_t b18[2] = { dx11 ? 0x9063DF : 0x9073FF, dx11 ? 0x9066F4 : 0x907714 };
        uintptr_t b12[2] = { dx11 ? 0x906441 : 0x907461, dx11 ? 0x906746 : 0x907766 };
        unsigned char e18[4] = { 0x12,0,0,0 }, e12[4] = { 0x0C,0,0,0 };
        int drv = cars + cars / 2; unsigned char r18[4], r12[4]; memcpy(r18, &drv, 4); memcpy(r12, &cars, 4);
        for (int k = 0; k < 2; k++) {
            patch(b18[k], e18, r18, 4, "enter-vehicle car base (18)");
            patch(b12[k], e12, r12, 4, "exit-vehicle car base (12)");
        }
        uintptr_t cmpA[2]  = { dx11 ? 0x906465 : 0x907485, dx11 ? 0x90676A : 0x90778A };
        uintptr_t pushA[2] = { dx11 ? 0x906475 : 0x907495, dx11 ? 0x906778 : 0x907798 };
        unsigned char ee[1] = { 0x12 }, rr[1] = { (unsigned char)pmx };
        for (int k = 0; k < 2; k++) {
            patch(cmpA[k],  ee, rr, 1, "enter-vehicle parked clamp cmp");
            patch(pushA[k], ee, rr, 1, "enter-vehicle parked clamp push");
        }
    }
}
