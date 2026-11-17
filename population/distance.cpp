// Spawn / despawn distances.
//
// The engine keeps ONE global distance scale (normally 1.0). The same value also drives vehicle and human LOD switching, so scaling it makes
// cars render in their lowest-detail mesh from a few metres away. Therefore:
//   * the global scale is left alone by default (GlobalDistanceScale=1.0); the detours below exist only for experiments, and
//   * the boost is applied inside the population code: vehicle despawn radius, pedestrian radii, and the vehicle spawn ring.
#include "common.h"

void apply_distance() {
    const bool dx11 = g_ctx.dx11;
    const float dm = g_ctx.globalScale, popdm = g_ctx.popDist;
    const uintptr_t scaleG = dx11 ? 0x130EF8C : 0x130AF7C;   // the global distance scale (float)

    // ---- optional: multiply the global scale itself (breaks LOD - keep GlobalDistanceScale at 1.0) ----
    if (dm > 1.001f) {
        // computed branch: movss [scale],xmm0 / cvtps2pd xmm0,xmm0   (11 bytes)
        uintptr_t site = dx11 ? 0x9D95C3 : 0x9DA0E3;
        unsigned char orig[11] = { 0xF3,0x0F,0x11,0x05, (unsigned char)scaleG,(unsigned char)(scaleG >> 8),(unsigned char)(scaleG >> 16),(unsigned char)(scaleG >> 24), 0x0F,0x5A,0xC0 };
        if (memcmp((void*)site, orig, 11)) L("SKIP distance scale detour @%08x: bytes differ", (unsigned)site);
        else {
            unsigned char* cave = allocExec(64);
            if (!cave) L("SKIP distance scale detour: VirtualAlloc failed");
            else {
                memcpy(cave + 48, &dm, 4);
                unsigned char* p = cave;
                *p++ = 0x0F; *p++ = 0x28; *p++ = 0xD8;                                              // movaps xmm3,xmm0
                *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x59; *p++ = 0x1D; put32(p, (uintptr_t)(cave + 48)); // mulss xmm3,[mult]
                *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x11; *p++ = 0x1D; put32(p, scaleG);               // movss [scale],xmm3
                *p++ = 0x0F; *p++ = 0x5A; *p++ = 0xC0;                                              // cvtps2pd xmm0,xmm0 (displaced original)
                *p++ = 0xE9; putRel32(p, site + 11);                                                // jmp back
                unsigned char jmp[11] = { 0xE9,0,0,0,0, 0x90,0x90,0x90,0x90,0x90,0x90 };
                { int rel = (int)((uintptr_t)cave - (site + 5)); memcpy(jmp + 1, &rel, 4); }
                patch(site, orig, jmp, 11, "distance scale detour");
                L("distance multiplier x%.2f", dm);
            }
        }
        // early branch: at normal FOV the engine stores 1.0 here instead (8-byte store, nothing displaced)
        uintptr_t site2 = dx11 ? 0x9D94F3 : 0x9DA013;
        unsigned char orig2[8] = { 0xF3,0x0F,0x11,0x05, (unsigned char)scaleG,(unsigned char)(scaleG >> 8),(unsigned char)(scaleG >> 16),(unsigned char)(scaleG >> 24) };
        if (memcmp((void*)site2, orig2, 8)) L("SKIP distance scale detour (early) @%08x: bytes differ", (unsigned)site2);
        else {
            unsigned char* cave2 = allocExec(64);
            if (cave2) {
                memcpy(cave2 + 48, &dm, 4);
                unsigned char* p = cave2;
                *p++ = 0x0F; *p++ = 0x28; *p++ = 0xD8;                                              // movaps xmm3,xmm0
                *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x59; *p++ = 0x1D; put32(p, (uintptr_t)(cave2 + 48)); // mulss xmm3,[mult]
                *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x11; *p++ = 0x1D; put32(p, scaleG);               // movss [scale],xmm3
                *p++ = 0xE9; putRel32(p, site2 + 8);                                                // jmp back
                unsigned char jmp2[8] = { 0xE9,0,0,0,0, 0x90,0x90,0x90 };
                { int rel = (int)((uintptr_t)cave2 - (site2 + 5)); memcpy(jmp2 + 1, &rel, 4); }
                patch(site2, orig2, jmp2, 8, "distance scale detour (early)");
            }
        }
    }

    // ---- vehicle despawn radius: r^2 = f32(D^2) * s^2 where s is read from the scale global at ONE instruction. Multiply s there only.
    if (popdm > 1.001f) {
        uintptr_t dsite = dx11 ? 0x9E9C8F : 0x9EA76F;
        unsigned char od[10] = { 0xF3,0x0F,0x10,0x05, (unsigned char)scaleG,(unsigned char)(scaleG >> 8),(unsigned char)(scaleG >> 16),(unsigned char)(scaleG >> 24), 0xEB,0x08 };
        unsigned char* cd = allocExec(64);
        if (cd) {
            memcpy(cd + 48, &popdm, 4);
            unsigned char* p = cd;
            *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x10; *p++ = 0x05; put32(p, scaleG);                    // movss xmm0,[scale]
            *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x59; *p++ = 0x05; put32(p, (uintptr_t)(cd + 48));     // mulss xmm0,[mult]
            *p++ = 0xE9; putRel32(p, dsite + 18);                                                    // where the original short jmp landed
            unsigned char nj[10] = { 0xE9,0,0,0,0, 0x90,0x90,0x90,0x90,0x90 };
            { int rel = (int)((uintptr_t)cd - (dsite + 5)); memcpy(nj + 1, &rel, 4); }
            patch(dsite, od, nj, 10, "vehicle despawn radius multiplier");
            L("vehicle despawn radii x%.2f (140 -> %.0f m)", popdm, 140.0f * popdm);
        }
    }

    // ---- pedestrian radii: doubles 70.0 (candidate edges) and 90.0 (accept band / despawn) read by four instructions.
    // Point those instructions at private copies scaled by DistanceMultiplier x PedDistanceExtra.
    double pe = iniDouble("PedDistanceExtra", 2.0, 1.0, 6.0);
    if (pe * popdm > 1.001) {
        double* priv = (double*)VirtualAlloc(0, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (priv) {
            priv[0] = 70.0 * pe * popdm; priv[1] = 90.0 * pe * popdm;
            uintptr_t c70 = dx11 ? 0x1172378 : 0x116EF78, c90 = dx11 ? 0x116D848 : 0x116A3E8;
            uintptr_t s70[2] = { dx11 ? 0x906F6A : 0x907FAA, dx11 ? 0x907630 : 0x908670 };
            uintptr_t s90[2] = { dx11 ? 0x90AB02 : 0x90BB42, dx11 ? 0xB8FA2F : 0xB90BEF };
            for (int k = 0; k < 4; k++) {
                uintptr_t site = k < 2 ? s70[k] : s90[k - 2], oldc = k < 2 ? c70 : c90, nw = (uintptr_t)(k < 2 ? &priv[0] : &priv[1]);
                char nm[40]; snprintf(nm, sizeof nm, "ped radius %s site #%d", k < 2 ? "70m" : "90m", k % 2 + 1);
                patch(site, (unsigned char*)&oldc, (unsigned char*)&nw, 4, nm);
            }
            L("ped radii x%.2f (70 -> %.0f m, 90 -> %.0f m)", pe * popdm, priv[0], priv[1]);
        }
    }

    // ---- vehicle spawn ring: new cars appear where a road crosses a SPHERE of radius (130 m x scale) around the camera, and that point must
    // lie in a loaded zone. A very large ring runs out of valid spawn points, so it gets its own distance instead of following the scale.
    double ring = iniDouble("SpawnRingMeters", 200.0, 0.0, 1e9);
    if (ring >= 30.0 && ring <= 2000.0) {
        double* pr = (double*)VirtualAlloc(0, 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (pr) {
            pr[0] = ring / (dm > 1.0f ? dm : 1.0f);
            uintptr_t oldc = dx11 ? 0x1172370 : 0x116EF70, nw = (uintptr_t)pr;
            patch(dx11 ? 0x90D2C9 : 0x90E309, (unsigned char*)&oldc, (unsigned char*)&nw, 4, "vehicle spawn ring distance");
            L("spawn ring -> %.0f m (scale %.2f x %.1f m); despawn radii keep the x%.2f scale", ring, dm, pr[0], dm);
        }
    }
}
