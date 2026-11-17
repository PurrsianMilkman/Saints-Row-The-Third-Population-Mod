// Fixed-size entity pools (DX11 only).
//
// The engine allocates one pool per object type at startup (vanilla: 48 vehicles, 150 NPCs, 1230 effect objects ...). EVERYTHING of that
// type draws from it - ambient, parked, police, gang, mission, player. Pools are carved from the "objects" heap, which is sized as the SUM of
// every pool class's size method + 0xC800, rounded to 64 KB, and was already 99.97% full in vanilla. So raising a capacity also needs its size
// method patched, or a later pool allocation returns NULL and the engine crashes in pool init.
//
//   size = 20 + capacity * (slot stride + 4)          (the extra 4 bytes per slot are the free-list entry)
//   capacity  = immediate in the pool's allocate wrapper (`push cap`), size = `mov eax,imm32` in vtable[4]
#include "common.h"

// vtable[2] of every concrete_instantiator is get_capacity(): `mov eax,<vanilla cap> / ret`. The object system (0x47E030 / 0x47E560) sums
// these per type to size each type's object list, the objects heap, and the 16-bit object handle ranges. A pool enlarged without this
// getter keeps a 48-entry vehicle list / handle range while slots up to the new capacity exist: objects past the vanilla count fall out of
// the list or alias another type's handles. Some getters are shared code (0xF6BC30 also serves other classes), so the vtable slot is
// redirected to a private `mov eax,cap / ret` instead of patching the function. Vanilla sum of all capacities 27894, engine limit 65535.
static void capGetter(uintptr_t vtable, uintptr_t oldFn, unsigned cap, const char* nm) {
    if (!iniInt("FixCapGetters", 1, 0, 1)) return;
    unsigned char* cv = allocExec(16);
    cv[0] = 0xB8; memcpy(cv + 1, &cap, 4); cv[5] = 0xC3;
    unsigned char e[4], r[4]; memcpy(e, &oldFn, 4); uintptr_t c = (uintptr_t)cv; memcpy(r, &c, 4);
    char w[48]; snprintf(w, sizeof w, "%s get_capacity", nm);
    if (patch(vtable + 8, e, r, 4, w)) L("%s get_capacity -> %u", nm, cap);
}

// effect_object capacity is ALSO baked into the effect module (audit/effect-pool.md):
//  - a static per-slot table "G" at 0x48781B0: groups of 4 slots x 96 bytes (x[4] y[4] z[4] lodA[4] lodB[4] flags[4]), 308 groups = 1232 slots,
//    indexed by (obj - pool base) / 656. Slots past 1231 wrote over the node-pool object at 0x487F540 (the 5000-slot crash). Relocated to a
//    VirtualAlloc'd table of N/4 groups: 17 absolute references (15 x base, base+0x10, base+0x50). 0x5C3ECC also points into this area but is a
//    static destructor of the array BELOW G that never reaches it.
//  - the per-frame update 0x5C6EE0 has two 1232-entry stack arrays (dist floats @+0xA0, state ints after them) and the /GS cookie above them:
//    frame size, cookie offsets, state offsets, group count 308 and slot bound 1232 are rescaled (N multiple of 4 keeps the 16-byte alignment).
//  - the 0x4CE immediates in 0x5C4xxx-0x5C8xxx belong to str_data_hash_table<int,1230,...> (a name registry used only by a dead function), not the pool.
// Every original byte is checked before anything is written; on any mismatch the pool stays at 1230.
static bool effectSupport(unsigned n) {
    struct Imm { uintptr_t a; unsigned oldv, newv; };
    unsigned char* g = (unsigned char*)modAlloc(n / 4 * 96 + 64, "effect slot table G");
    if (!g) { L("SKIP effect table relocation: VirtualAlloc failed"); return false; }
    unsigned gb = (unsigned)(uintptr_t)g;
    unsigned st = 0xA0 + n * 4, ck = st + n * 4;            // state array base, /GS cookie slot
    const Imm im[] = {
        { 0x5C5A48, 0x48781B0, gb }, { 0x5C5A8C, 0x48781B0, gb }, { 0x5C5ACD, 0x48781B0, gb }, { 0x5C6006, 0x48781B0, gb },
        { 0x5C6082, 0x48781B0, gb }, { 0x5C63FB, 0x48781B0, gb }, { 0x5C69A4, 0x48781B0, gb }, { 0x5C6A1F, 0x48781B0, gb },
        { 0x5C6B1C, 0x48781B0, gb }, { 0x5C6B88, 0x48781B0, gb }, { 0x5C6C23, 0x48781B0, gb }, { 0x5C75D1, 0x48781B0, gb },
        { 0x5C8561, 0x48781B0, gb }, { 0x5C90DC, 0x48781B0, gb }, { 0x5CB283, 0x48781B0, gb },
        { 0x5C5B75, 0x4878200, gb + 0x50 }, { 0x5C6FA7, 0x48781C0, gb + 0x10 },
        { 0x5C6EE7, 0x2724, ck + 4 },  { 0x5C6EFA, 0x2720, ck },     { 0x5C6FC0, 0x134, n / 4 },
        { 0x5C709C, 0x13E8, st + 8 },  { 0x5C70C3, 0x13E0, st },     { 0x5C71EC, 0x4D0, n },  { 0x5C71F9, 0x272C, ck + 0xC } };
    for (const Imm& i : im)
        if (memcmp((void*)i.a, &i.oldv, 4)) { L("SKIP effect pool support: bytes differ at %08x (pool stays 1230)", (unsigned)i.a); VirtualFree(g, 0, MEM_RELEASE); return false; }
    for (const Imm& i : im) {
        char w[48]; snprintf(w, sizeof w, "effect module %08x -> %x", (unsigned)i.oldv, i.newv);
        patch(i.a, (const unsigned char*)&i.oldv, (const unsigned char*)&i.newv, 4, w);
    }
    L("effect slot table relocated to %08x (%u groups), update-frame stack %u bytes", gb, n / 4, ck + 4);
    return true;
}

// Every renderable object type has a dirty_update_impl<T> container (registered in the per-type table 0x5901B20) holding a bitset of slots
// whose visual/transform needs updating this frame. The bitset is EMBEDDED in the static container, sized for the vanilla pool:
//   vehicle 2 dwords (64 slots), object_debris 2 (64), effect_object 39 (1248, four containers), general_mover 165 (5280).
// With bigger pools a slot past that sets a bit beyond the bitset (corrupting the next static object - the general_mover crash in the
// render walk 0x5DDCB0) and is never walked, so the object's rendered model stops following it: the frozen-car bug.
// Mark (0x8CB5F0), clear (0x8CB650) and the walk all obtain the bitset through vtable[2] get_bits(&ptr) -> dword count, and the container
// vtables have only three entries, so redirecting vtable[2] to a private, properly sized bitset fixes every user. 0x8F1570 is shared by
// several classes, so the vtable slot is redirected, not the function.
static void dirtyBits(uintptr_t vtable, uintptr_t oldFn, unsigned slots, const char* nm) {
    if (!iniInt("FixDirtyBits", 1, 0, 1)) return;
    unsigned dw = (slots + 31) / 32 + 1;
    void* bits = modAlloc(dw * 4, nm);
    unsigned char* cv = allocExec(32);
    if (!bits || !cv) { L("SKIP %s: alloc failed", nm); return; }
    unsigned b = (unsigned)(uintptr_t)bits, c = (unsigned)(uintptr_t)cv;
    unsigned char* q = cv;
    *q++ = 0x8B; *q++ = 0x44; *q++ = 0x24; *q++ = 0x04;      // mov eax,[esp+4]
    *q++ = 0xC7; *q++ = 0x00; memcpy(q, &b, 4); q += 4;        // mov dword ptr [eax], bits
    *q++ = 0xB8; memcpy(q, &dw, 4); q += 4;                    // mov eax, dword count
    *q++ = 0xC2; *q++ = 0x04; *q++ = 0x00;                     // ret 4
    char w[64]; snprintf(w, sizeof w, "%s get_bits", nm);
    if (patch(vtable + 8, (const unsigned char*)&oldFn, (const unsigned char*)&c, 4, w)) L("%s: %u slots (%u dwords) at %08x", nm, slots, dw, b);
}

void apply_pools() {
    if (!g_ctx.dx11) { L("pool patches are DX11-only"); return; }

    // ---- "permanent" mempool (0x489CA20): 11.4 MB, ~93% used in vanilla. It is built by hand at 0x5D3210 (not through a mempool ctor, so
    // audit.cpp never sees it) and every later sub-pool carved from it (e.g. the 1 MB pool at 0x70A790) gets a NULL buffer once it is full:
    // the first allocations then return small offsets from address 0 and a memset crashes at 0xEB4F65 during loading. The bigger entity
    // pools and the vehicle customisation records draw on it, so it grows with the population settings.
    {
        unsigned mb = (unsigned)iniInt("PermanentHeapMB", 32, 11, 256), sz = mb << 20;
        if (sz < 0xAE0000) sz = 0xAE0000;
        unsigned o = 0xAE0000;
        patch(0x5D3216, (const unsigned char*)&o, (const unsigned char*)&sz, 4, "permanent mempool alloc size");
        patch(0x5D325A, (const unsigned char*)&o, (const unsigned char*)&sz, 4, "permanent mempool size field");
        L("permanent mempool 11.4 MB -> %u MB", sz >> 20);
    }

    // ---- vehicle pool (capacity 48, stride 0x1E10) and NPC pool (capacity 150, stride 0x1E70)
    int vp = iniInt("VehiclePoolSize", 128, 48, 1000);
    int np = iniInt("NpcPoolSize", 400, 150, 1500);
    {   // vehicle: 0xA86070 `mov eax,[esp+4] / push 0x30 / push eax / add ecx,0x30 / call alloc / ret 4` - the push is imm8; rebuild with imm32
        unsigned char ow[18] = { 0x8B,0x44,0x24,0x04,0x6A,0x30,0x50,0x83,0xC1,0x30,0xE8,0x81,0xFF,0xFF,0xFF,0xC2,0x04,0x00 };
        if (memcmp((void*)0xA86070, ow, 18)) L("SKIP vehicle pool wrapper: bytes differ");
        else {
            unsigned char* cv = allocExec(64);
            unsigned char* q = cv;
            *q++ = 0x8B; *q++ = 0x44; *q++ = 0x24; *q++ = 0x04;            // mov eax,[esp+4]
            *q++ = 0x68; memcpy(q, &vp, 4); q += 4;                        // push imm32 capacity
            *q++ = 0x50;                                                   // push eax
            *q++ = 0x83; *q++ = 0xC1; *q++ = 0x30;                         // add ecx,0x30
            *q++ = 0xE8; putRel32(q, 0xA86000);                            // call the allocator
            *q++ = 0xC2; *q++ = 0x04; *q++ = 0x00;                         // ret 4
            unsigned char nj[5] = { 0xE9,0,0,0,0 };                        // jmp rel32 into the cave
            { int rel = (int)((uintptr_t)cv - (0xA86070 + 5)); memcpy(nj + 1, &rel, 4); }
            unsigned char exp5[5]; memcpy(exp5, ow, 5);
            patch(0xA86070, exp5, nj, 5, "vehicle pool capacity 48");
        }
    }
    unsigned char en[4] = { 0x96,0,0,0 }, rn[4] = { (unsigned char)np,(unsigned char)(np >> 8),0,0 };
    patch(0x9D8E65, en, rn, 4, "npc pool capacity 150");

    unsigned vsz = 20u + (unsigned)vp * (0x1E10u + 4u), nsz = 20u + (unsigned)np * (0x1E70u + 4u);
    unsigned char ovs[4] = { 0xD4,0xA3,0x05,0x00 }, nvs[4]; memcpy(nvs, &vsz, 4);
    unsigned char ons[4] = { 0x0C,0xD8,0x11,0x00 }, nns[4]; memcpy(nns, &nsz, 4);
    patch(0xA85FF1, ovs, nvs, 4, "vehicle pool heap size");
    patch(0x9D8DE1, ons, nns, 4, "npc pool heap size");
    L("pools -> vehicles %d (%u bytes), npcs %d (%u bytes)", vp, vsz, np, nsz);
    if (vp != 48)  capGetter(0x11840D4, 0xF6BC30, (unsigned)vp, "vehicle");
    if (vp > 64)   dirtyBits(0x1183F58, 0x8F1570, (unsigned)vp, "vehicle dirty-update bitset");
    if (np != 150) capGetter(0x1178DD0, 0x9267F0, (unsigned)np, "npc");

    // ---- other pools that the extra traffic fills (measured with the POOLS: log lines): vfx attached to cars/peds, movers, crash debris
    struct XP { const char* nm; uintptr_t capImm; int capBytes; unsigned oldCap; uintptr_t sizeImm; unsigned stride; const char* key; int def; int hi; uintptr_t vt, getter; };
    const XP xp[3] = {
        { "effect_object", 0x5CA645, 4, 1230, 0x5CA5C1, 656, "EffectPoolSize", 1230,  20000, 0x112682C, 0x5CA5B0 },
        { "general_mover", 0xA06C45, 4, 5280, 0xA06BE1, 376, "MoverPoolSize",  10000, 30000, 0x117B2D4, 0xA06BD0 },
        { "object_debris", 0x8F3675, 1, 50,   0x8F35F1, 960, "DebrisPoolSize", 127,   127, 0x11710E0, 0x8F35E0 } };   // debris capacity is an imm8: max 127
    for (int k = 0; k < 3; k++) {
        unsigned nc = (unsigned)iniInt(xp[k].key, xp[k].def, (int)xp[k].oldCap, xp[k].hi);
        if (nc == xp[k].oldCap) continue;
        if (k == 0) { nc = (nc + 3) & ~3u; if (!effectSupport(nc)) continue; }   // effect pool: N multiple of 4, module tables first
        unsigned osz = 20u + xp[k].oldCap * (xp[k].stride + 4u), nsz2 = 20u + nc * (xp[k].stride + 4u);
        char nm[48]; snprintf(nm, sizeof nm, "%s pool capacity %u", xp[k].nm, xp[k].oldCap);
        unsigned char oc[4], rc[4]; memcpy(oc, &xp[k].oldCap, 4); memcpy(rc, &nc, 4);
        if (patch(xp[k].capImm, oc, rc, xp[k].capBytes, nm)) {
            unsigned char os[4], rs[4]; memcpy(os, &osz, 4); memcpy(rs, &nsz2, 4);
            snprintf(nm, sizeof nm, "%s pool heap size", xp[k].nm);
            patch(xp[k].sizeImm, os, rs, 4, nm);
            L("%s pool -> %u slots (%u bytes)", xp[k].nm, nc, nsz2);
            capGetter(xp[k].vt, xp[k].getter, nc, xp[k].nm);
            if (k == 0) { static const uintptr_t evt[4] = { 0x1126738, 0x1126748, 0x1126758, 0x1126768 };
                          static const char* en[4] = { "effect dirty-update bitset 1", "effect dirty-update bitset 2", "effect dirty-update bitset 3", "effect dirty-update bitset 4" };
                          for (int i = 0; i < 4; i++) dirtyBits(evt[i], 0x5C5580, nc, en[i]); }
            if (k == 1) dirtyBits(0x117B29C, 0xA05590, nc, "mover dirty-update bitset");
            if (k == 2) dirtyBits(0x1171048, 0x8F1570, nc, "debris dirty-update bitset");
        }
    }
}
