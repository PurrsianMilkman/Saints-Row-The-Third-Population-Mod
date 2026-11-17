// Render-library instance caps, vehicle customisation slots, Havok TOI queue (DX11 only). See audit/engine-limits.md.
//
// renderlib keeps one fixed-capacity class per instance type (descriptor: cap @+0x14, in use @+0x18, peak @+0x1C). Allocation returns NULL
// when a class is exhausted - silently. The "rl factory" mempool is sized at init as sum(cap * sizeof), so raising a cap before init is all it
// takes; each cap is read only by its own class init (free-list build), verified for every class below.
//
// Every live vehicle also owns a "vehicle custom instance" plus one slot in three parallel static arrays, all fixed at 48. The 49th vehicle got
// NULL and 0xA789E4 passes it unchecked into 0xA99640 (write to [0x3C]). The arrays are relocated to N slots: list capacity (static
// initializer), owner flags (4 refs), the two buffers (2 refs), the two arrays of 0x50-byte mempool objects (init loop disp32s, allocator,
// free, the CRT vector ctor/dtor functions) and the three loop bounds (two of them imm8 -> small caves).
#include "common.h"

static void p32(uintptr_t a, unsigned oldv, unsigned newv, const char* what) {
    patch(a, (const unsigned char*)&oldv, (const unsigned char*)&newv, 4, what);
}

// ---------------------------------------------------------------- renderlib class caps
static void renderCaps() {
    struct Cap { uintptr_t a; unsigned oldv, newv; const char* nm; };
    static const Cap caps[] = {
        // immediates in the PC setup function 0x5DF6C0 (runs before renderlib init)
        { 0x5DF6DA, 0x200,  4096,  "rl_skin_instance + light volume" },
        { 0x5DF6F3, 0x600,  8192,  "rl_dynamic_material" },
        { 0x5DF71B, 0x7D0,  4000,  "rl_light" },
        { 0x5DF749, 0x400,  4096,  "rl_zbias_decal" },
        { 0x5DF786, 0x1000, 16384, "rl_mesh_instance" },
        { 0x5DF711, 0x1400, 20000, "rl_multi_mesh_instance" },
        { 0x5DF7A0, 0x12C,  1200,  "rl_pc_morph_instance" },
        // static caps in .data
        { 0x1312A70, 0xC2,  1024,  "vehicle_deformable_mesh_instance" },
        { 0x475A730, 0x400, 4096,  "rl_pc_clone_mesh_instance" },
        { 0x474A64C, 0x2BC, 4096,  "rl_corona_instance" },
        { 0x475A6E4, 0xC8,  800,   "rl_pc_morph_container" },
        { 0x4759C08, 0x64,  400,   "rl_pc_dynamic_morph_instance" },
    };
    for (const Cap& c : caps) {
        char w[64]; snprintf(w, sizeof w, "rl cap %s", c.nm);
        if (patch(c.a, (const unsigned char*)&c.oldv, (const unsigned char*)&c.newv, 4, w)) L("  %s %u -> %u", c.nm, c.oldv, c.newv);
    }
}

// ---------------------------------------------------------------- vehicle customisation slots
static unsigned char* emit(unsigned char* p, const void* b, size_t n) { memcpy(p, b, n); return p + n; }
static unsigned char* emitRel(unsigned char* p, unsigned char op, uintptr_t at, uintptr_t target) {   // op rel32, instruction at `at`
    *p++ = op; int rel = (int)(target - (at + 5)); memcpy(p, &rel, 4); return p + 4;
}

// rewrite a CRT vector ctor/dtor helper (`push dtor; [push ctor;] push 0x30; push 0x50; push base; call fn; ...`) with an imm32 count
static void rewriteVec(uintptr_t at, const unsigned char* orig, size_t origLen, size_t room, bool ctor, unsigned base, unsigned n,
                       uintptr_t vecFn, uintptr_t atexitArg, const char* what) {
    unsigned char code[64]; memset(code, 0xCC, sizeof code);
    unsigned char* p = code;
    *p++ = 0x68; unsigned v = 0x43B000; p = emit(p, &v, 4);                  // push element dtor
    if (ctor) { *p++ = 0x68; v = 0x43B270; p = emit(p, &v, 4); }            // push element ctor
    *p++ = 0x68; p = emit(p, &n, 4);                                        // push count (imm32)
    *p++ = 0x6A; *p++ = 0x50;                                               // push element size 0x50
    *p++ = 0x68; p = emit(p, &base, 4);                                     // push array base
    p = emitRel(p, 0xE8, at + (p - code), vecFn);                           // call vector ctor / dtor
    if (ctor) {
        *p++ = 0x68; v = (unsigned)atexitArg; p = emit(p, &v, 4);            // push dtor helper
        p = emitRel(p, 0xE8, at + (p - code), 0xEA8561);                    // call atexit
        *p++ = 0x59;                                                        // pop ecx
    }
    *p++ = 0xC3;
    size_t len = p - code;
    if (len > room) { L("SKIP %s: rewrite does not fit", what); return; }
    unsigned char expect[64]; memcpy(expect, orig, origLen); memset(expect + origLen, 0xCC, room - origLen);
    patch(at, expect, code, room, what);
}

static void vehicleCustSlots(unsigned n) {
    if (*(volatile unsigned*)0x1311688 != 0) { L("SKIP vehicle customisation slots: static initializers already ran"); return; }

    // verify every site first
    struct S { uintptr_t a; unsigned v; };
    static const S chk[] = {
        { 0x100C9C6, 0x30 }, { 0x100CA76, 0x690 }, { 0x100CB26, 0xD20 }, { 0xA94D5B, 0x5B940A0 }, { 0xA9672A, 0x5B940A0 }, { 0xA96753, 0x5B940A0 }, { 0xA98773, 0x5B940A0 },
        { 0xA94C7B, 0x5B94160 }, { 0xA94C80, 0x5BAC160 }, { 0xA96740, 0x5BB8BC0 }, { 0xA96746, 0x5BB9AC0 }, { 0xA98750, 0x5BB8BC0 },
        { 0xA94D73, 0xF00 } };
    for (const S& s : chk) if (*(unsigned*)s.a != s.v) { L("SKIP vehicle customisation slots: bytes differ at %08x", (unsigned)s.a); return; }
    static const unsigned char loopA[] = { 0x83, 0xF8, 0x30, 0x7C, 0xF1 };   // 0xA96731 cmp eax,0x30 / jl 0xA96727
    static const unsigned char loopB[] = { 0x83, 0xF8, 0x2F, 0x77, 0x07 };   // 0xA9876B cmp eax,0x2F / ja 0xA98777
    if (memcmp((void*)0xA96731, loopA, 5) || memcmp((void*)0xA9876B, loopB, 5)) { L("SKIP vehicle customisation slots: loop bytes differ"); return; }
    // init-loop disp32s into the two object arrays (0x5BB8BC0.. "vcust cb", 0x5BB9AC0.. "ext vcust cb"), each object 0x50 bytes
    static const uintptr_t vcDisp[]  = { 0xA94C9F, 0xA94CAB, 0xA94CB1, 0xA94CB7, 0xA94CBD, 0xA94CC3, 0xA94CC9, 0xA94CD3, 0xA94CDE, 0xA94CED, 0xA94CF3 };
    static const uintptr_t extDisp[] = { 0xA94D06, 0xA94D16, 0xA94D1C, 0xA94D22, 0xA94D28, 0xA94D2E, 0xA94D34, 0xA94D3E, 0xA94D48, 0xA94D4E, 0xA94D54 };
    for (uintptr_t a : vcDisp)  { unsigned v = *(unsigned*)a; if (v < 0x5BB8BC0 || v >= 0x5BB8C10) { L("SKIP vehicle customisation slots: disp at %08x", (unsigned)a); return; } }
    for (uintptr_t a : extDisp) { unsigned v = *(unsigned*)a; if (v < 0x5BB9AC0 || v >= 0x5BB9B10) { L("SKIP vehicle customisation slots: disp at %08x", (unsigned)a); return; } }
    static const unsigned char ctorA[] = { 0x68,0x00,0xB0,0x43,0x00, 0x68,0x70,0xB2,0x43,0x00, 0x6A,0x30, 0x6A,0x50, 0x68,0xC0,0x8B,0xBB,0x05, 0xE8,0xD0,0xB9,0xE9,0xFF,
                                           0x68,0xF0,0xCC,0x01,0x01, 0xE8,0xAF,0xB9,0xE9,0xFF, 0x59, 0xC3 };
    static const unsigned char ctorB[] = { 0x68,0x00,0xB0,0x43,0x00, 0x68,0x70,0xB2,0x43,0x00, 0x6A,0x30, 0x6A,0x50, 0x68,0xC0,0x9A,0xBB,0x05, 0xE8,0xA0,0xB9,0xE9,0xFF,
                                           0x68,0x10,0xCD,0x01,0x01, 0xE8,0x7F,0xB9,0xE9,0xFF, 0x59, 0xC3 };
    static const unsigned char dtorA[] = { 0x68,0x00,0xB0,0x43,0x00, 0x6A,0x30, 0x6A,0x50, 0x68,0xC0,0x8B,0xBB,0x05, 0xE8,0x38,0xB9,0xE8,0xFF, 0xC3 };
    static const unsigned char dtorB[] = { 0x68,0x00,0xB0,0x43,0x00, 0x6A,0x30, 0x6A,0x50, 0x68,0xC0,0x9A,0xBB,0x05, 0xE8,0x18,0xB9,0xE8,0xFF, 0xC3 };
    if (memcmp((void*)0x100CB90, ctorA, sizeof ctorA) || memcmp((void*)0x100CBC0, ctorB, sizeof ctorB) ||
        memcmp((void*)0x101CCF0, dtorA, sizeof dtorA) || memcmp((void*)0x101CD10, dtorB, sizeof dtorB)) { L("SKIP vehicle customisation slots: vector ctor/dtor bytes differ"); return; }

    // new storage (zeroed; objects 16-byte aligned like the originals)
    unsigned flagsSz = n * 4, vcObjSz = n * 0x50, extObjSz = n * 0x50, vcBufSz = n * 0x400, extBufSz = n * 0x800;
    unsigned char* m = (unsigned char*)modAlloc(flagsSz + vcObjSz + extObjSz + vcBufSz + extBufSz + 256, "vehicle customisation slots");
    if (!m) { L("SKIP vehicle customisation slots: VirtualAlloc failed"); return; }
    unsigned flags = (unsigned)(uintptr_t)m, vcObj = (flags + flagsSz + 15) & ~15u, extObj = vcObj + vcObjSz, vcBuf = extObj + extObjSz, extBuf = vcBuf + vcBufSz;

    p32(0x100C9C6, 0x30, n, "vcust list capacity 48");
    p32(0x100CA76, 0x690, 35 * n, "vcust component records 1680");   // vanilla 48 x 35, 24-byte records from the "permanent" mempool
    p32(0x100CB26, 0xD20, 70 * n, "vcust colour records 3360");      // vanilla 48 x 70 (audit.cpp enlarges "permanent" x1.5)
    static const uintptr_t flagRefs[] = { 0xA94D5B, 0xA9672A, 0xA96753, 0xA98773 };
    for (uintptr_t a : flagRefs) p32(a, 0x5B940A0, flags, "vcust owner flags base");
    p32(0xA94C7B, 0x5B94160, extBuf, "ext vcust buffer base");
    p32(0xA94C80, 0x5BAC160, vcBuf, "vcust buffer base");
    for (uintptr_t a : vcDisp)  { unsigned v = *(unsigned*)a; p32(a, v, vcObj + (v - 0x5BB8BC0), "vcust cb field disp"); }
    for (uintptr_t a : extDisp) { unsigned v = *(unsigned*)a; p32(a, v, extObj + (v - 0x5BB9AC0), "ext vcust cb field disp"); }
    p32(0xA96740, 0x5BB8BC0, vcObj, "vcust alloc: cb base");
    p32(0xA96746, 0x5BB9AC0, extObj, "vcust alloc: ext cb base");
    p32(0xA98750, 0x5BB8BC0, vcObj, "vcust free: cb base");
    p32(0xA94D73, 0xF00, n * 0x50, "vcust init loop bound");

    {   // 0xA96731: cmp eax,n / jl 0xA96727 / jmp 0xA96736
        unsigned char* c = allocExec(32), *p = c;
        *p++ = 0x3D; p = emit(p, &n, 4);
        *p++ = 0x0F; *p++ = 0x8C; { int rel = (int)(0xA96727 - ((uintptr_t)p + 4)); p = emit(p, &rel, 4); }
        *p++ = 0xE9; putRel32(p, 0xA96736);
        unsigned char j[5]; j[0] = 0xE9; int rel = (int)((uintptr_t)c - (0xA96731 + 5)); memcpy(j + 1, &rel, 4);
        patch(0xA96731, loopA, j, 5, "vcust alloc slot scan bound");
    }
    {   // 0xA9876B: cmp eax,n-1 / ja 0xA98777 / jmp 0xA98770
        unsigned char* c = allocExec(32), *p = c; unsigned last = n - 1;
        *p++ = 0x3D; p = emit(p, &last, 4);
        *p++ = 0x0F; *p++ = 0x87; { int rel = (int)(0xA98777 - ((uintptr_t)p + 4)); p = emit(p, &rel, 4); }
        *p++ = 0xE9; putRel32(p, 0xA98770);
        unsigned char j[5]; j[0] = 0xE9; int rel = (int)((uintptr_t)c - (0xA9876B + 5)); memcpy(j + 1, &rel, 4);
        patch(0xA9876B, loopB, j, 5, "vcust free slot bound");
    }
    rewriteVec(0x100CB90, ctorA, sizeof ctorA, 48, true,  vcObj,  n, 0xEA8578, 0x101CCF0, "vcust cb vector ctor");
    rewriteVec(0x100CBC0, ctorB, sizeof ctorB, 48, true,  extObj, n, 0xEA8578, 0x101CD10, "ext vcust cb vector ctor");
    rewriteVec(0x101CCF0, dtorA, sizeof dtorA, 32, false, vcObj,  n, 0xEA863B, 0,         "vcust cb vector dtor");
    rewriteVec(0x101CD10, dtorB, sizeof dtorB, 32, false, extObj, n, 0xEA863B, 0,         "ext vcust cb vector dtor");
    L("vehicle customisation slots 48 -> %u (flags %08x, cb %08x, ext cb %08x)", n, flags, vcObj, extObj);
}

void apply_render() {
    if (!g_ctx.dx11) return;
    if (iniInt("FixRenderCaps", 1, 0, 1)) renderCaps();
    if (iniInt("FixToiQueue", 1, 0, 1)) p32(0xC2FD40, 0xFA, 4096, "havok TOI event queue 250");   // hkpWorldCinfo default m_sizeOfToiEventQueue (game never overrides it)
    int vp = iniInt("VehiclePoolSize", 128, 48, 1000);
    unsigned n = (unsigned)((vp + 15) & ~15); if (n < 48) n = 48;
    if (n > 48 && iniInt("FixVehicleCustSlots", 1, 0, 1)) vehicleCustSlots(n);
}
