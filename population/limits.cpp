// Engine structures sized for the vanilla human / vehicle counts (DX11 only). See audit/npc-limits.md.
//
// 1. Spawn-reservation tracker 0x8EFF50: every live human / vehicle holds one 0x28-byte node. Node arrays are static: humans 150 @0x59F9558,
//    vehicles 48 @0x59F8DD8 - the REAL ceiling on live humans and cars (the "kind 1 free capacity" in BUDGET lines is this free count).
//    Relocated to N nodes per kind. Handles are ((gen*2|kind) << 8) | index with an 8-bit index, so they are widened to a 16-bit index:
//    builder 0x8EF8B6, decoder 0x8F03D6/D9/DF + 0x8F0405, and the release path 0x8F0266 that re-derives the kind from a node's handle.
// 2. Havok hk_custom_character job table 0x79A160: 150 x 64-byte jobs per frame @0x56421D0 - proxy 151+ is not stepped that frame.
// 3. 0x8BE050 (per-frame action-node pass): 150-pointer stack array that ends at the return address (151st distinct target = smashed stack).
// 4. Co-op variant of the human/driver budget (0x8EFEEE push 150; single player 75 is patched in spawn.cpp).
#include "common.h"

static bool same(uintptr_t a, const unsigned char* b, size_t n) { return !memcmp((void*)a, b, n); }
static void p32(uintptr_t a, unsigned oldv, unsigned newv, const char* what) {
    patch(a, (const unsigned char*)&oldv, (const unsigned char*)&newv, 4, what);
}

static void spawnTracker(unsigned nh, unsigned nv) {
    struct S { uintptr_t a; unsigned v; };
    static const S chk[] = { { 0x8EF898, 0x59F9558 }, { 0x8F01E2, 0x59F9558 }, { 0x8F03F8, 0x59F9558 }, { 0x8F01E7, 0x96 },
                             { 0x8EF891, 0x59F8DD8 }, { 0x8F01D6, 0x59F8DD8 }, { 0x8F03F1, 0x59F8DD8 }, { 0x8F01DB, 0x30 } };
    for (const S& s : chk) if (*(unsigned*)s.a != s.v) { L("SKIP spawn tracker: bytes differ at %08x", (unsigned)s.a); return; }
    static const unsigned char shl8[] = { 0xC1, 0xE3, 0x08 }, mzx[] = { 0x0F, 0xB6, 0xD0 }, shrA[] = { 0xC1, 0xE8, 0x08 },
                               shrC[] = { 0xC1, 0xE9, 0x09 }, shrD[] = { 0xC1, 0xEA, 0x09 };
    if (!same(0x8EF8B6, shl8, 3) || !same(0x8F03D6, mzx, 3) || !same(0x8F03D9, shrA, 3) || !same(0x8F03DF, shrC, 3) ||
        !same(0x8F0405, shrD, 3) || !same(0x8F0266, shrA, 3)) { L("SKIP spawn tracker: handle code differs"); return; }

    unsigned char* h = (unsigned char*)modAlloc(nh * 0x28 + nv * 0x28 + 64, "spawn tracker nodes (humans, then vehicles)");
    if (!h) { L("SKIP spawn tracker: VirtualAlloc failed"); return; }
    unsigned hb = (unsigned)(uintptr_t)h, vb = hb + ((nh * 0x28 + 15) & ~15u);

    if (nh > 255 || nv > 255) {   // 16-bit node index (instruction lengths unchanged)
        static const unsigned char n1[] = { 0xC1, 0xE3, 0x10 }, n2[] = { 0x0F, 0xB7, 0xD0 }, n3[] = { 0xC1, 0xE8, 0x10 },
                                   n4[] = { 0xC1, 0xE9, 0x11 }, n5[] = { 0xC1, 0xEA, 0x11 };
        patch(0x8EF8B6, shl8, n1, 3, "tracker handle: index << 16");
        patch(0x8F03D6, mzx,  n2, 3, "tracker decode: 16-bit index");
        patch(0x8F03D9, shrA, n3, 3, "tracker decode: kind bit 16");
        patch(0x8F03DF, shrC, n4, 3, "tracker decode: generation >> 17");
        patch(0x8F0405, shrD, n5, 3, "tracker decode: node gen >> 17");
        patch(0x8F0266, shrA, n3, 3, "tracker release: kind bit 16");
    }
    p32(0x8EF898, 0x59F9558, hb, "tracker human nodes (build)");
    p32(0x8F01E2, 0x59F9558, hb, "tracker human nodes (init)");
    p32(0x8F03F8, 0x59F9558, hb, "tracker human nodes (lookup)");
    p32(0x8F01E7, 0x96, nh, "tracker human node count 150");
    p32(0x8EF891, 0x59F8DD8, vb, "tracker vehicle nodes (build)");
    p32(0x8F01D6, 0x59F8DD8, vb, "tracker vehicle nodes (init)");
    p32(0x8F03F1, 0x59F8DD8, vb, "tracker vehicle nodes (lookup)");
    p32(0x8F01DB, 0x30, nv, "tracker vehicle node count 48");
    L("spawn tracker: humans 150 -> %u, vehicles 48 -> %u", nh, nv);
}

static void havokJobs(unsigned n) {
    if (*(unsigned*)0x79A19C != 0x96 || *(unsigned*)0x79A1AD != 0x56421D0) { L("SKIP havok character jobs: bytes differ"); return; }
    void* t = modAlloc(n * 64 + 64, "havok character job table");   // page aligned (movaps at +0x10)
    if (!t) { L("SKIP havok character jobs: VirtualAlloc failed"); return; }
    p32(0x79A1AD, 0x56421D0, (unsigned)(uintptr_t)t, "havok character job table base");
    p32(0x79A19C, 0x96, n, "havok character jobs per frame 150");
}

static void actionNodeStack(unsigned n) {
    unsigned frame = n * 4 + 0xC;                       // array at [esp+0xC], frame stays below 4 KB (no stack probe in this function)
    if (frame >= 0x1000) { n = (0x1000 - 0x10) / 4; frame = n * 4 + 0xC; }
    struct S { uintptr_t a; unsigned oldv, newv; };
    const S s[] = { { 0x8BE052, 0x264, frame }, { 0x8BE057, 0x258, n * 4 }, { 0x8BE197, 0x96, n }, { 0x8BE1EE, 0x264, frame } };
    for (const S& x : s) if (*(unsigned*)x.a != x.oldv) { L("SKIP action-node target array: bytes differ at %08x", (unsigned)x.a); return; }
    for (const S& x : s) p32(x.a, x.oldv, x.newv, "action-node target array 150");
}


// ---------------------------------------------------------------- vehicle-side structures (audit/vehicle-limits.md)
// Animation registries shared by vehicles and humans: R1 anim instances (400 x 0x3A0), R2 anim controllers (400 x 0x480), both carved out of a
// temporary "object pool" mempool with hardcoded sizes, and R3, a ring of 375 controller-state nodes (75 blocks x 5). The dump had R1 219/400 with
// 33 cars + 70 npcs. When one runs dry the vehicle/human gets no anim instance and its skeleton pose stops updating while physics continues.
static void animRegistries(unsigned n) {
    struct S { uintptr_t a; unsigned oldv, newv; const char* w; };
    unsigned k = (n + 4) / 5;
    unsigned char* r1 = (unsigned char*)modAlloc(n * 0x3A0 + n * 4 + 64, "anim instance registry");
    unsigned char* r2 = (unsigned char*)modAlloc(n * 0x480 + n * 4 + 64, "anim controller registry");
    unsigned char* r3 = (unsigned char*)modAlloc(k * 0x870 + 64, "anim state ring");
    if (!r1 || !r2 || !r3) { L("SKIP anim registries: VirtualAlloc failed"); return; }
    const S s[] = {
        { 0x4B3CB3, 0x5B040,   n * 0x3A0 + n * 4,       "anim instance pool size" },
        { 0x4B3CB8, 0x66E9F10, (unsigned)(uintptr_t)r1, "anim instance pool buffer" },
        { 0x4B3E6F, 0x5AA00,   n * 0x3A0,               "anim instance array bytes" },
        { 0x4B3E88, 0x640,     n * 4,                   "anim instance free list bytes" },
        { 0x4B3E96, 0x190,     n,                       "anim instance count 400" },
        { 0x4B3BD3, 0x70E40,   n * 0x480 + n * 4,       "anim controller pool size" },
        { 0x4B3BD8, 0x6744FD0, (unsigned)(uintptr_t)r2, "anim controller pool buffer" },
        { 0x4B3D7F, 0x70800,   n * 0x480,               "anim controller array bytes" },
        { 0x4B3D98, 0x640,     n * 4,                   "anim controller free list bytes" },
        { 0x4B3DA6, 0x190,     n,                       "anim controller count 400" },
        { 0x49D286, 0x66C24F0, (unsigned)(uintptr_t)r3, "anim state ring base" },
        { 0x49D369, 0x66E9DC0, (unsigned)(uintptr_t)r3 + k * 0x870, "anim state ring end" } };
    for (const S& x : s) if (*(unsigned*)x.a != x.oldv) { L("SKIP anim registries: bytes differ at %08x", (unsigned)x.a); return; }
    for (const S& x : s) p32(x.a, x.oldv, x.newv, x.w);
    L("anim registries: instances/controllers 400 -> %u, state ring 375 -> %u", n, k * 5);
}

// Wheel-list pool P4 (48 x 0x100) and tyre-record pool P1 (48 x 0x1D8): stack allocators with an in-use count byte and a byte id stack, so the
// limit can only go to 255 by relocation (ids are bytes). Vehicle 49 used to get NULL and 0xAD70F7 dereferenced it unchecked -> also add a
// null check after the wheel-list allocation so a full pool skips the wheel rebuild instead of crashing.
static void wheelPools(unsigned n) {
    if (n > 255) n = 255;
    unsigned char* p4 = (unsigned char*)modAlloc(n * 0x100 + 512, "wheel-list pool");
    unsigned char* p1 = (unsigned char*)modAlloc(n * 0x1D8 + 512, "tyre-record pool");
    if (!p4 || !p1) { L("SKIP wheel pools: VirtualAlloc failed"); return; }
    unsigned r4 = (unsigned)(uintptr_t)p4, c4 = r4 + n * 0x100, i4 = c4 + 1;      // records, count byte, ids
    unsigned r1 = (unsigned)(uintptr_t)p1, c1 = r1 + n * 0x1D8, i1 = c1 + 1;
    struct S { uintptr_t a; unsigned oldv, newv; };
    const S s[] = {
        { 0xAC1197, 0x5BCFCF8, c1 }, { 0xAC119E, 0x5BCFCF9, i1 }, { 0xAC2171, 0x5BCFCF8, c1 }, { 0xAC217F, 0x5BCFCF9, i1 },
        { 0xAC218E, 0x5BCFCF9, i1 }, { 0xAC2195, 0x5BCFCF8, c1 }, { 0xAC219B, 0x5BCA478, r1 }, { 0xAC2218, 0x5BCFCF8, c1 },
        { 0xAC221E, 0x5BCA478, r1 }, { 0xAC2231, 0x5BCFCF8, c1 }, { 0xAC2243, 0x5BCFCF9, i1 }, { 0x100D541, 0x5BCA478, r1 },
        { 0xAC3276, 0x2F, n - 1 },
        { 0xAC123B, 0x5C34070, c4 }, { 0xAC1242, 0x5C34071, i4 }, { 0xAC2581, 0x5C34070, c4 }, { 0xAC258F, 0x5C34071, i4 },
        { 0xAC2598, 0x5C34071, i4 }, { 0xAC259F, 0x5C34070, c4 }, { 0xAC25A8, 0x5C31070, r4 }, { 0xAC25C1, 0x5C34070, c4 },
        { 0xAC25CD, 0x5C31070, r4 }, { 0xAC25D8, 0x5C34070, c4 }, { 0xAC25DE, 0x5C34071, i4 } };
    static const uintptr_t cmp8[] = { 0xAC11A7, 0xAC124C, 0xAC2176, 0xAC2586 };   // cmp al,0x30 (init loops + alloc full test)
    for (const S& x : s) if (*(unsigned*)x.a != x.oldv) { L("SKIP wheel pools: bytes differ at %08x", (unsigned)x.a); return; }
    for (uintptr_t a : cmp8) if (*(unsigned short*)a != 0x303C) { L("SKIP wheel pools: compare differs at %08x", (unsigned)a); return; }
    static const unsigned char call5[] = { 0xE8, 0xA9, 0xB4, 0xFE, 0xFF };          // 0xAD70D2 call 0xAC2580
    if (memcmp((void*)0xAD70D2, call5, 5)) { L("SKIP wheel pools: wheel rebuild call differs"); return; }
    for (const S& x : s) p32(x.a, x.oldv, x.newv, "wheel/tyre pool reference");
    for (uintptr_t a : cmp8) { unsigned char o[2] = { 0x3C, 0x30 }, r[2] = { 0x3C, (unsigned char)n }; patch(a, o, r, 2, "wheel/tyre pool bound 48"); }
    unsigned char* c = allocExec(32), *q = c;                                        // call alloc / test eax,eax / je skip / jmp back
    *q++ = 0xE8; putRel32(q, 0xAC2580);
    *q++ = 0x85; *q++ = 0xC0;
    *q++ = 0x0F; *q++ = 0x84; putRel32(q, 0xAD71D8);
    *q++ = 0xE9; putRel32(q, 0xAD70D7);
    unsigned char j[5] = { 0xE9 }; int rel = (int)((uintptr_t)c - (0xAD70D2 + 5)); memcpy(j + 1, &rel, 4);
    patch(0xAD70D2, call5, j, 5, "wheel list null check");
    L("wheel-list / tyre-record pools 48 -> %u", n);
}

// Seat / occupancy registry: 48 records x 0x1640 @0x5C4D250. The submitter returns 0 when full and the ambient spawner ignores it -> cars past
// 48 occupied vehicles get no driver. 54 base references, 5 end compares, two imm8 count loops (caves), the CRT constructor count.
static const uintptr_t kSeatBase[] = {   // instruction addresses of mov reg,0x5C4D250 (imm32 at +1)
    0x607E8E, 0x608D41, 0xAF06B1, 0xAF06F1, 0xAF079C, 0xAF080C, 0xAF088B, 0xAF091B, 0xAF098B, 0xAF0A2B, 0xAF0A8B, 0xAF0AEB, 0xAF0B4B, 0xAF0BAB,
    0xAF0C0B, 0xAF0C6B, 0xAF0CCB, 0xAF0D1B, 0xAF0D6B, 0xAF0DBB, 0xAF0E7B, 0xAF1326, 0xAF1336, 0xAF1414, 0xAF147B, 0xAF14FC, 0xAF159B, 0xAF24AF,
    0xAF2CC8, 0xAF2D72, 0xAF2E8A, 0xAF301B, 0xAF36BA, 0xAF3D86, 0xAF5176, 0xAF7B57, 0xAF7DDC, 0xAF7EC2, 0xAF7F80, 0xAF81AD, 0xB0A1D6, 0xB0A33D,
    0xB0A3AD, 0xB0A40D, 0xB0A4F2, 0xB0A55D, 0xB0A61C, 0xB0A715, 0xB0A81D, 0xB0BA8B, 0xB0CE09, 0xB0D8EC, 0xB0DF27, 0x100D8F2 };
static const uintptr_t kSeatEnd[] = { 0xAF06C3, 0xAF0703, 0xAF254A, 0xAF7F50, 0xAF8300 };   // cmp reg,0x5C8FE50 (imm32 at +2)

static void seatRegistry(unsigned n) {
    for (uintptr_t a : kSeatBase) if (*(unsigned*)(a + 1) != 0x5C4D250) { L("SKIP seat registry: base differs at %08x", (unsigned)a); return; }
    for (uintptr_t a : kSeatEnd)  if (*(unsigned*)(a + 2) != 0x5C8FE50) { L("SKIP seat registry: end differs at %08x", (unsigned)a); return; }
    static const unsigned char l1[] = { 0x83, 0xF8, 0x30, 0x7C, 0xED }, l2[] = { 0x83, 0xF8, 0x30, 0x7C, 0xE7 };
    if (memcmp((void*)0xB07323, l1, 5) || memcmp((void*)0xB07364, l2, 5) || *(unsigned*)0x100D8F8 != 0x2F) { L("SKIP seat registry: loop bytes differ"); return; }
    unsigned char* m = (unsigned char*)modAlloc(n * 0x1640 + 64, "seat registry");
    if (!m) { L("SKIP seat registry: VirtualAlloc failed"); return; }
    unsigned base = (unsigned)(uintptr_t)m, end = base + n * 0x1640;
    for (uintptr_t a : kSeatBase) p32(a + 1, 0x5C4D250, base, "seat registry base");
    for (uintptr_t a : kSeatEnd)  p32(a + 2, 0x5C8FE50, end, "seat registry end");
    p32(0x100D8F8, 0x2F, n - 1, "seat registry ctor count 48");
    struct Lp { uintptr_t at; const unsigned char* orig; uintptr_t loop, back; const char* w; };
    const Lp lp[] = { { 0xB07323, l1, 0xB07315, 0xB07328, "seat find-free bound 48" }, { 0xB07364, l2, 0xB07350, 0xB07369, "seat find-by-handle bound 48" } };
    for (const Lp& x : lp) {   // cmp eax,n / jl loop / jmp back
        unsigned char* c = allocExec(32), *q = c;
        *q++ = 0x3D; memcpy(q, &n, 4); q += 4;
        *q++ = 0x0F; *q++ = 0x8C; putRel32(q, x.loop);
        *q++ = 0xE9; putRel32(q, x.back);
        unsigned char j[5] = { 0xE9 }; int rel = (int)((uintptr_t)c - (x.at + 5)); memcpy(j + 1, &rel, 4);
        patch(x.at, x.orig, j, 5, x.w);
    }
    L("seat registry 48 -> %u records", n);
}

void apply_limits() {
    if (!g_ctx.dx11) return;
    unsigned np = (unsigned)iniInt("NpcPoolSize", 400, 150, 1500), vp = (unsigned)iniInt("VehiclePoolSize", 128, 48, 1000);
    if (iniInt("FixSpawnTracker", 1, 0, 1)) spawnTracker(np, vp);
    if (iniInt("FixHavokJobs", 1, 0, 1)) havokJobs(np);
    if (iniInt("FixActionNodeStack", 1, 0, 1)) actionNodeStack(np);
    unsigned bt = (unsigned)iniInt("BudgetTotal", 3000, 75, 20000);
    p32(0x8EFEEE, 0x96, bt, "co-op human/driver budget 150");
    if (iniInt("FixAnimRegistries", 1, 0, 1)) animRegistries(2048);
    unsigned vn = (vp + 15) & ~15u;
    if (iniInt("FixWheelPools", 1, 0, 1)) wheelPools(vn);
    if (iniInt("FixSeatRegistry", 1, 0, 1)) seatRegistry(vn);
}
