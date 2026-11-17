// Diagnostics (DX11 only). Nothing here changes game behaviour; it counts what the engine does and appends it to SR3Population.log every 4 s:
//
//   DIAG    spawn passes, candidates per pass, attempts / successes on the ring path (A) and the queued-vehicle path (B)
//   GATES   how often each pre-spawn check is evaluated and how often it returned non-zero
//   REMOVE  top callers of the vehicle-release and ped-delete functions (which code is deleting entities)
//   POOLS   every engine pool with entries:  index(vtable low 16 bits):used/capacity
//   BUDGET  raw dump of the two spawn-budget structures
//
// Counters live in g_cnt; tiny hand-assembled caves increment them with `inc dword [g_cnt+n]`.
#include "common.h"
#include <math.h>

static volatile LONG g_exit[9];   // spawn-pass early exits to 0x90E522, per jcc site (see kExit)
static const uintptr_t kExit[9] = { 0x90E310, 0x90E324, 0x90E33D, 0x90E35B, 0x90E368, 0x90E38E, 0x90E396, 0x90E3A3, 0x90E3CB };
static volatile LONG g_cnt[32];   // 0 passes, 1 candidate sum, 2 last candidate count, 3 attempts A, 4 ok A, 5 attempts B, 6 ok B,
                                  // 8+2k / 9+2k = calls / non-zero returns of gate k (k = 0..4)

// ---- removal histogram: which code address deletes vehicles / peds ----
struct RmTab { unsigned key[64]; volatile LONG n[64]; };
static RmTab g_rm[2];   // 0 = vehicle release, 1 = ped delete (a generic destroy routine - not ped-specific)

static void __cdecl recCaller(unsigned ret, int w) {
    RmTab& t = g_rm[w & 1]; unsigned h = (ret >> 2) & 63;
    for (int i = 0; i < 64; i++) {
        unsigned idx = (h + i) & 63;
        if (t.key[idx] == ret) { InterlockedIncrement(&t.n[idx]); return; }
        if (t.key[idx] == 0)   { t.key[idx] = ret; InterlockedIncrement(&t.n[idx]); return; }
    }
}

static void installRemoveHooks() {
    struct { uintptr_t site; unsigned char disp[6]; int len; int w; const char* nm; } hk[2] = {
        { 0x9047A0, { 0x39,0x05,0x2C,0x09,0xA1,0x05 }, 6, 0, "diag: vehicle release hook" },
        { 0x852DF0, { 0x56,0x8B,0x74,0x24,0x08,0 },    5, 1, "diag: ped delete hook" } };
    unsigned char* mem = allocExec(256);
    if (!mem) return;
    for (int k = 0; k < 2; k++) {
        unsigned char* cave = mem + k * 128; unsigned char* p = cave;
        *p++ = 0x60; *p++ = 0x9C;                                          // pushad / pushfd
        *p++ = 0x68; memcpy(p, &hk[k].w, 4); p += 4;                       // push w
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x28;                // push [esp+0x28]  (return address of the hooked call)
        *p++ = 0xB8; put32(p, (uintptr_t)&recCaller);                      // mov eax, recCaller
        *p++ = 0xFF; *p++ = 0xD0;                                          // call eax
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                             // add esp,8
        *p++ = 0x9D; *p++ = 0x61;                                          // popfd / popad
        memcpy(p, hk[k].disp, hk[k].len); p += hk[k].len;                  // displaced original instruction(s)
        *p++ = 0xE9; putRel32(p, hk[k].site + hk[k].len);
        unsigned char jm[6] = { 0xE9,0,0,0,0,0x90 };
        { int rel = (int)((uintptr_t)cave - (hk[k].site + 5)); memcpy(jm + 1, &rel, 4); }
        patch(hk[k].site, hk[k].disp, jm, hk[k].len, hk[k].nm);
    }
}

// ---- periodic log writer ----
static DWORD WINAPI monitor(LPVOID) {
    LONG prev[32] = {};
    for (int tick = 0;; tick++) {
        Sleep(4000);
        LONG c[32]; for (int i = 0; i < 32; i++) c[i] = g_cnt[i];
        FILE* f = fopen(g_ctx.logPath, "a");
        if (f) {
            LONG dp = c[0] - prev[0];
            fprintf(f, "DIAG +%ld passes (avg %.1f candidates, last %ld) | pathA(ring) attempts +%ld ok +%ld | pathB(queued) attempts +%ld ok +%ld\n",
                    dp, dp ? (double)(c[1] - prev[1]) / dp : 0.0, c[2], c[3] - prev[3], c[4] - prev[4], c[5] - prev[5], c[6] - prev[6]);
            { static LONG pe[9]; fprintf(f, "EXITS (spawn pass bail-outs per jcc site):");
              for (int k = 0; k < 9; k++) { LONG v = g_exit[k]; fprintf(f, " %06x:+%ld", (unsigned)kExit[k] & 0xFFFFFF, v - pe[k]); pe[k] = v; } fprintf(f, "\n"); }
            fprintf(f, "GATES (calls/blocked): flag1 %ld/%ld flag2 %ld/%ld activity %ld/%ld eligibility %ld/%ld g904890 %ld/%ld\n",
                    c[8] - prev[8], c[9] - prev[9], c[10] - prev[10], c[11] - prev[11], c[12] - prev[12], c[13] - prev[13],
                    c[14] - prev[14], c[15] - prev[15], c[16] - prev[16], c[17] - prev[17]);

            { static RmTab prevRm[2]; const char* nm[2] = { "vehicle", "ped" };
              for (int w = 0; w < 2; w++) {
                struct { unsigned key; LONG d; } top[6] = {};
                for (int i = 0; i < 64; i++) {
                    unsigned k = g_rm[w].key[i]; if (!k) continue;
                    LONG dl = g_rm[w].n[i] - prevRm[w].n[i];
                    for (int j = 0; j < 6; j++) if (dl > top[j].d) { for (int m = 5; m > j; m--) top[m] = top[m - 1]; top[j].key = k; top[j].d = dl; break; }
                    prevRm[w].n[i] = g_rm[w].n[i]; prevRm[w].key[i] = k;
                }
                fprintf(f, "REMOVE %s:", nm[w]);
                for (int j = 0; j < 6; j++) if (top[j].d > 0) fprintf(f, " %08x x%ld", top[j].key, top[j].d);
                fprintf(f, "\n");
              } }

            {   // every pool with entries: pointer array of pool objects at 0x61EE7E0; capacity u16 @+0x3A, free u16 @+0x3E, "initialised" byte @+0x38
                fprintf(f, "POOLS:");
                for (int i = 0; i < 96; i++) {
                    unsigned* pp = (unsigned*)(0x61EE7E0 + 4 * i); MEMORY_BASIC_INFORMATION mb;
                    if (!VirtualQuery(pp, &mb, sizeof mb) || mb.State != MEM_COMMIT) break;
                    unsigned char* o = (unsigned char*)*pp; if (!o || (uintptr_t)o < 0x10000) continue;
                    if (!VirtualQuery(o, &mb, sizeof mb) || mb.State != MEM_COMMIT || (o + 0x44) > (unsigned char*)mb.BaseAddress + mb.RegionSize) continue;
                    unsigned cap = *(unsigned short*)(o + 0x3A), fr = *(unsigned short*)(o + 0x3E);
                    if (cap == 0 || cap > 20000 || fr > cap || o[0x38] != 1 || fr == cap) continue;
                    fprintf(f, " %d(%04x):%u/%u", i, (*(unsigned*)o) & 0xFFFF, cap - fr, cap);
                }
                fprintf(f, "\n");
            }

            {   // render/sim desync: vehicle pool (index 7), items at pool+0x44, stride 0x1E10, vtable at +0, sim pos +0x40, LOD pos +0x1DDC
                unsigned char* o = (unsigned char*)*(unsigned*)(0x61EE7E0 + 4 * 7); MEMORY_BASIC_INFORMATION mb;
                if (o && VirtualQuery(o, &mb, sizeof mb) && mb.State == MEM_COMMIT && (o + 0x50) <= (unsigned char*)mb.BaseAddress + mb.RegionSize) {
                    unsigned cap = *(unsigned short*)(o + 0x3A); unsigned char* base = (unsigned char*)*(unsigned*)(o + 0x44);
                    int live = 0, bad = 0; float worst = 0; int worstSlot = -1, ex[3] = { -1, -1, -1 }; float exd[3] = {};
                    if (cap && cap <= 2000 && base) for (unsigned s = 0; s < cap; s++) {
                        unsigned char* p = base + (size_t)s * 0x1E10;
                        if (!VirtualQuery(p, &mb, sizeof mb) || mb.State != MEM_COMMIT || (p + 0x1E10) > (unsigned char*)mb.BaseAddress + mb.RegionSize) continue;
                        unsigned vt = *(unsigned*)p; if (vt < 0x1000000 || vt >= 0x1300000) continue;
                        float* a = (float*)(p + 0x40); float* b = (float*)(p + 0x1DDC); live++;
                        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
                        if (!(d < 5.0f)) { if (bad < 3) { ex[bad] = (int)s; exd[bad] = d; } bad++; }
                        if (d > worst || d != d) { worst = d; worstSlot = (int)s; }
                    }
                    fprintf(f, "DESYNC vehicles live %d, |sim-render|>5m: %d, worst %.1f m (slot %d) eg:", live, bad, worst, worstSlot);
                    for (int j = 0; j < 3 && ex[j] >= 0; j++) fprintf(f, " s%d=%.1f", ex[j], exd[j]);
                    fprintf(f, "\n");
                }
            }

            fprintf(f, "LIMITS (live spawn budget table 0x130C304, class 0..7):");   // what the gate 0x8F0080 actually compares against
            for (int k = 0; k < 2; k++) { fprintf(f, " kind%d", k); for (int i = 0; i < 8; i++) fprintf(f, " %u", *(unsigned*)(0x130C304 + 12 * k + 28 * i)); }
            fprintf(f, "\n");
            for (int k = 0; k < 2; k++) {   // spawn-budget structures, 0x90 bytes each
                unsigned* b = (unsigned*)(0x59FACC8 + 0x90 * k);
                MEMORY_BASIC_INFORMATION mb; if (!VirtualQuery(b, &mb, sizeof mb) || mb.State != MEM_COMMIT) continue;
                fprintf(f, "BUDGET kind%d:", k); for (int i = 0; i < 36; i++) fprintf(f, " %u", b[i]); fprintf(f, "\n");
            }
            if (tick == 0) audit_startup_log(f);
            audit_report(f, tick);
            crash_tick();
            fclose(f);
        }
        for (int i = 0; i < 32; i++) prev[i] = c[i];
    }
}

// ---- spawn-pass hooks ----
// Wrap a `call target` at `site` so that `inc [cntCalls]` runs on every call and `inc [cntHits]` when the result is non-zero. testOp is the
// 2-byte `test` that decides non-zero: 0x85 0xC0 = test eax,eax ; 0x84 0xC0 = test al,al.
static void wrapCall(unsigned char* cave, uintptr_t site, uintptr_t target, uintptr_t cntCalls, uintptr_t cntHits, unsigned char testOp, const char* nm) {
    unsigned char* q = cave;
    *q++ = 0xE8; putRel32(q, target);
    *q++ = 0xFF; *q++ = 0x05; put32(q, cntCalls);
    *q++ = testOp; *q++ = 0xC0; *q++ = 0x74; *q++ = 0x06;
    *q++ = 0xFF; *q++ = 0x05; put32(q, cntHits);
    *q++ = 0xC3;
    unsigned char orig[5] = { 0xE8,0,0,0,0 }, nw[5] = { 0xE8,0,0,0,0 };
    { int rel = (int)(target - (site + 5)); memcpy(orig + 1, &rel, 4); }
    { int rel = (int)((uintptr_t)cave - (site + 5)); memcpy(nw + 1, &rel, 4); }
    patch(site, orig, nw, 5, nm);
}

void installDiag() {
    unsigned char* cave = allocExec(512);
    if (!cave) return;
    const uintptr_t cnt = (uintptr_t)g_cnt;

    // hook 1 @0x90E451: `mov ebp,eax / lea ebx,[esp+0x64]` right after the ring generator returns its candidate count in eax
    unsigned char o1[6] = { 0x8B,0xE8,0x8D,0x5C,0x24,0x64 };
    unsigned char* p = cave;
    *p++ = 0x8B; *p++ = 0xE8; *p++ = 0x9C;                  // mov ebp,eax / pushfd
    *p++ = 0xFF; *p++ = 0x05; put32(p, cnt + 0);            // inc passes
    *p++ = 0x01; *p++ = 0x05; put32(p, cnt + 4);            // add candSum, eax
    *p++ = 0xA3; put32(p, cnt + 8);                         // mov last, eax
    *p++ = 0x9D; *p++ = 0x8D; *p++ = 0x5C; *p++ = 0x24; *p++ = 0x64;   // popfd / lea ebx,[esp+0x64]
    *p++ = 0xE9; putRel32(p, 0x90E457);
    unsigned char j1[6] = { 0xE9,0,0,0,0,0x90 };
    { int rel = (int)((uintptr_t)cave - (0x90E451 + 5)); memcpy(j1 + 1, &rel, 4); }
    patch(0x90E451, o1, j1, 6, "diag: candidate count hook");

    // hooks 2/3: the two spawn calls - path A (ring candidates) and path B (queued vehicle)
    wrapCall(cave + 64,  0x90E4A1, 0x90CD00, cnt + 4 * 3, cnt + 4 * 4, 0x85, "diag: path A wrapper");
    wrapCall(cave + 112, 0x90E4E4, 0x90C110, cnt + 4 * 5, cnt + 4 * 6, 0x85, "diag: path B wrapper");

    // gate wrappers: the five checks at the top of the pass; a non-zero `al` blocks it (except 0x904890, where non-zero means "go")
    struct { uintptr_t site, target; const char* nm; } gt[5] = {
        { 0x90E2C2, 0x92A4E0, "diag: gate flag1" },       { 0x90E2CF, 0x92A4F0, "diag: gate flag2" },
        { 0x90E306, 0xAD2250, "diag: gate activity" },    { 0x90E31D, 0x90DDD0, "diag: gate eligibility" },
        { 0x90E336, 0x904890, "diag: gate 904890" } };
    for (int k = 0; k < 5; k++)
        wrapCall(cave + 160 + k * 32, gt[k].site, gt[k].target, cnt + 4 * (8 + 2 * k), cnt + 4 * (9 + 2 * k), 0x84, gt[k].nm);

    // exit counters: every `jcc 0x90E522` between the gates and the ring generator is pointed at a cave that counts and continues
    {
        unsigned char* ec = allocExec(9 * 16);
        for (int k = 0; k < 9 && ec; k++) {
            uintptr_t site = kExit[k];
            unsigned char* jc = (unsigned char*)site;
            if (jc[0] != 0x0F || (jc[1] & 0xF0) != 0x80 || site + 6 + *(int*)(jc + 2) != 0x90E522) { L("SKIP diag exit counter %08x", (unsigned)site); continue; }
            unsigned char* c = ec + k * 16, *q = c;
            *q++ = 0xF0; *q++ = 0xFF; *q++ = 0x05; put32(q, (uintptr_t)&g_exit[k]);   // lock inc [g_exit+k]  (flags not needed after a taken jcc)
            *q++ = 0xE9; putRel32(q, 0x90E522);
            unsigned char nj[6]; memcpy(nj, jc, 2); int rel = (int)((uintptr_t)c - (site + 6)); memcpy(nj + 2, &rel, 4);
            patch(site, jc, nj, 6, "diag: spawn exit counter");
        }
    }

    installRemoveHooks();
    CreateThread(0, 0, monitor, 0, 0, 0);
}
