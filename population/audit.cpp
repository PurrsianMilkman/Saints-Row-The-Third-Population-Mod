// Engine resource audit + resource scaling (DX11 only).
//
// The population boost puts far more vehicles and humans into the world than the engine was tuned for. Besides the entity pools (pools.cpp)
// every character / vehicle pulls memory out of many named engine mempools (skin bone matrices, anim pages, shader constants, render heap...).
// This file:
//   1. registers EVERY mempool the engine constructs (hooks on the four mempool constructors) so the log can show each one's fill level,
//   2. enlarges the population-linked mempools at construction time: the constructor's (buffer, size) arguments are replaced with a larger
//      VirtualAlloc'd block. Verified per pool that no code references the original static buffer except the constructor push,
//   3. logs every failure signal the engine has: block-pool "can allocate" refusals (0xDB99D0), mempool "not large enough" errors (0xDC8EE0)
//      and the engine's compiled-out warning function (0x778910, 1386 call sites incl. "NPC limit reached"), with caller address and text.
//
// Mempool objects: name char[32] @+0x10, buffer @+0x38, size @+0x3C. Block pools (vtable 0x124DB74): blocks used @+0x44, block size @+0x4C,
// block count @+0x50. Linear mempools (vtable 0x1120ACC): bytes used @+0x44, high-water @+0x4C.
#include "common.h"
#include <intrin.h>

// ---------------------------------------------------------------- scaling table
struct Scale { const char* name; double factor; };   // factor < 0 means "ResourceScale * -factor"
static const Scale kScale[] = {
    { "RL_skin large bone pool",        -1.0 },   // 180 x 0xC00 skin matrix blocks (characters + vehicles over 40 bones); 71 used with 70 npcs
    { "RL_skin small bone pool",        -1.0 },   // 2504 x 0x60 blocks; vehicle parts
    { "shader_constant_cache",          -2.0 },   // only 63 blocks; 29 used with 33 cars
    { "renderlib heap",                 -0.5 },   // 10 MB render heap, 62% used with 33 cars
    { "renderlib dynamic materials",    -1.0 },
    { "renderlib material block map",   -1.0 },
    { "Deformation weights",            -1.0 },   // vehicle deformation
    { "cloth sim runtime cpu",          -1.0 },
    { "v_anim data pool",               -1.0 },
    { "Animlib large bone pool",        -1.0 },
    { "Animlib small bone pool",        -1.0 },
    { "Animlib parent aim bone pool",   -1.0 },   // 79 / 127 used
    { "Anim network page pool",         -1.0 },
    { "synced_resource",                -1.0 },   // 103 / 116 used
    { "prop block pool",                -1.0 },
    { "char_info data pool",             4.0 },   // 90% used
    { "pf",                              4.0 },   // 99% used (pathfinding)
    { "Value override mempool",          4.0 },   // 86% used
    { "human drop",                      4.0 },
    { "permanent",                       1.5 },   // 11.4 MB, 93% used; also feeds the vehicle customisation record pools (render.cpp)
    { "stream2 shared prim pp",          2.0 },   // 80% used
};
static double g_resScale = 8.0;

static double factorFor(const char* nm) {
    for (const Scale& s : kScale)
        if (!strcmp(nm, s.name)) return s.factor < 0 ? g_resScale * -s.factor : s.factor;
    return 1.0;
}

// ---------------------------------------------------------------- registry (filled during engine start-up, after our log is closed)
struct PoolRec { void* obj; unsigned origSize, newSize; char name[32]; unsigned hw; };
static PoolRec g_pool[768];
static volatile LONG g_npool;

static bool onStack(void* p) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    return p >= tib->StackLimit && p < tib->StackBase;
}

static bool safeName(const char* s, char* out) {
    __try { for (int i = 0; i < 31; i++) { out[i] = s[i]; if (!s[i]) return i > 0; } out[31] = 0; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

static void reg(void* obj, const char* nm, unsigned osz, unsigned nsz) {
    if (onStack(obj)) return;                                   // temporary mempools living on a stack frame
    LONG i = InterlockedIncrement(&g_npool) - 1;
    if (i >= (LONG)(sizeof g_pool / sizeof g_pool[0])) { InterlockedDecrement(&g_npool); return; }
    g_pool[i].obj = obj; g_pool[i].origSize = osz; g_pool[i].newSize = nsz; g_pool[i].hw = 0;
    if (!nm || !safeName(nm, g_pool[i].name)) g_pool[i].name[0] = 0;
}

// a[0] = buffer, a[1] = size, name at a[nameIdx]
static void scaleArgs(void* self, unsigned* a, int nameIdx) {
    char nm[32]; const char* src = (const char*)a[nameIdx];
    if (!safeName(src, nm)) { reg(self, 0, a[1], a[1]); return; }
    double f = factorFor(nm);
    unsigned osz = a[1];
    if (f > 1.0 && osz) {
        unsigned nsz = (unsigned)(osz * f);
        void* mem = VirtualAlloc(0, nsz + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   // registered below via reg() (mempool registry)
        if (mem) { a[0] = (unsigned)mem; a[1] = nsz; reg(self, nm, osz, nsz); return; }
    }
    reg(self, nm, osz, osz);
}

static void __cdecl onBlockCtor(void* self, unsigned* a)  { scaleArgs(self, a, 3); }   // 0xDB9950 (buf, size, elem, name, align, flag)
static void __cdecl onLinearCtor(void* self, unsigned* a) { scaleArgs(self, a, 2); }   // 0xDB1400 (buf, size, name, align, flag)
static void __cdecl onBaseCtor(void* self, unsigned* a, int hasName) { reg(self, hasName ? (const char*)a[0] : 0, 0, 0); }   // 0xDB3180 (name, x) / 0xDB3150 ()

static DWORD g_backBlock = 0xDB9955, g_backLinear = 0xDB1405, g_backBase0 = 0xDB3155, g_backBase1 = 0xDB3185;

__declspec(naked) static void thunkBlock() {
    __asm {
        pushad
        pushfd
        lea eax, [esp + 40]
        push eax
        push ecx
        call onBlockCtor
        add esp, 8
        popfd
        popad
        push ebx                    // displaced: push ebx / mov ebx,[esp+0Ch]
        mov ebx, [esp + 0Ch]
        jmp [g_backBlock]
    }
}
__declspec(naked) static void thunkLinear() {
    __asm {
        pushad
        pushfd
        lea eax, [esp + 40]
        push eax
        push ecx
        call onLinearCtor
        add esp, 8
        popfd
        popad
        mov eax, [esp + 14h]        // displaced: mov eax,[esp+14h] / push esi
        push esi
        jmp [g_backLinear]
    }
}
__declspec(naked) static void thunkBase0() {
    __asm {
        pushad
        pushfd
        push 0
        lea eax, [esp + 44]
        push eax
        push ecx
        call onBaseCtor
        add esp, 12
        popfd
        popad
        push esi                    // displaced: push esi / mov esi,ecx / push 20h
        mov esi, ecx
        push 20h
        jmp [g_backBase0]
    }
}
__declspec(naked) static void thunkBase1() {
    __asm {
        pushad
        pushfd
        push 1
        lea eax, [esp + 44]
        push eax
        push ecx
        call onBaseCtor
        add esp, 12
        popfd
        popad
        mov eax, [esp + 4]          // displaced: mov eax,[esp+4] / push esi
        push esi
        jmp [g_backBase1]
    }
}

// ---------------------------------------------------------------- failure signals
struct Sig { volatile LONG key; volatile LONG n; LONG shown; char text[100]; };
static Sig g_warn[1024];       // keyed by caller of the warning function
static Sig g_refuse[128];      // keyed by pool object (block pool refused an allocation)
static char g_memErr[16][120]; static volatile LONG g_memErrN;

static Sig* sigSlot(Sig* t, int n, LONG key) {
    unsigned h = ((unsigned)key * 2654435761u) % (unsigned)n;
    for (int k = 0; k < n; k++) {
        Sig* s = &t[(h + k) % n];
        if (s->key == key) return s;
        if (s->key == 0 && InterlockedCompareExchange(&s->key, key, 0) == 0) return s;
        if (s->key == key) return s;
    }
    return 0;
}

static void __cdecl onWarn(unsigned caller, const char* msg) {
    Sig* s = sigSlot(g_warn, 1024, (LONG)caller); if (!s) return;
    if (InterlockedIncrement(&s->n) == 1) {
        __try { int i = 0; for (; i < 99 && msg && msg[i]; i++) s->text[i] = (msg[i] >= 32 && msg[i] < 127) ? msg[i] : '.'; s->text[i] = 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { strcpy(s->text, "<bad ptr>"); }
    }
}
static void __cdecl onRefuse(void* pool) {
    Sig* s = sigSlot(g_refuse, 128, (LONG)pool); if (!s) return;
    if (InterlockedIncrement(&s->n) == 1) safeName((const char*)pool + 0x10, s->text);
}
static void __cdecl onMemErr() {
    LONG i = InterlockedIncrement(&g_memErrN) - 1;
    char* d = g_memErr[i & 15];
    __try { strncpy(d, (const char*)0x5F10050, 119); d[119] = 0; } __except (EXCEPTION_EXECUTE_HANDLER) { d[0] = 0; }
}

__declspec(naked) static void thunkWarn() {     // replaces the `ret` of the empty warning function 0x778910 (msg, flag)
    __asm {
        pushad
        pushfd
        push dword ptr [esp + 40]   // msg
        push dword ptr [esp + 40]   // caller (return address; +4 because of the first push)
        call onWarn
        add esp, 8
        popfd
        popad
        ret
    }
}

static unsigned char* g_canAllocTramp;        // original first 7 bytes of 0xDB99D0 + jmp back
__declspec(naked) static void thunkCanAlloc() { // bool __thiscall pool::can_alloc(size, align)  (ret 8)
    __asm {
        push ecx                    // [esp]=self [esp+4]=ret [esp+8]=a1 [esp+0Ch]=a2
        push dword ptr [esp + 0Ch]
        push dword ptr [esp + 0Ch]
        mov ecx, [esp + 8]
        call [g_canAllocTramp]
        test al, al
        jnz ok
        pushad
        pushfd
        push dword ptr [esp + 36]   // self
        call onRefuse
        add esp, 4
        popfd
        popad
    ok:
        add esp, 4
        ret 8
    }
}

__declspec(naked) static void thunkMemErr() {   // replaces `add esp,0Ch / xor al,al / ret` at 0xDC8EF4 (after the message was formatted)
    __asm {
        add esp, 0Ch
        pushad
        pushfd
        call onMemErr
        popfd
        popad
        xor al, al
        ret
    }
}

// ---------------------------------------------------------------- periodic report (called from diag.cpp's monitor thread)
static bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery(p, &mb, sizeof mb) || mb.State != MEM_COMMIT || (mb.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (const char*)p + n <= (const char*)mb.BaseAddress + mb.RegionSize;
}

// fill of a pool in per-mille, plus a text "used/cap"; returns -1 when unknown
static int poolFill(const PoolRec& r, char* txt, size_t n) {
    const unsigned char* o = (const unsigned char*)r.obj;
    if (!readable(o, 0x58)) { snprintf(txt, n, "gone"); return -1; }
    unsigned vt = *(const unsigned*)o;
    if (vt == 0x124DB74) {
        unsigned used = *(const unsigned*)(o + 0x44), bs = *(const unsigned*)(o + 0x4C), cnt = *(const unsigned*)(o + 0x50);
        if (!cnt || used > cnt) { snprintf(txt, n, "?"); return -1; }
        snprintf(txt, n, "%u/%u x%u", used, cnt, bs); return (int)(1000.0 * used / cnt);
    }
    if (vt == 0x1120ACC || vt == 0x11837F4) {
        unsigned size = *(const unsigned*)(o + 0x3C), used = *(const unsigned*)(o + 0x44);
        if (!size || used > size) { snprintf(txt, n, "?"); return -1; }
        snprintf(txt, n, "%u/%u B", used, size); return (int)(1000.0 * used / size);
    }
    snprintf(txt, n, "vt %06x", vt & 0xFFFFFF); return -1;
}

void audit_report(FILE* f, int tick) {
    // pools: full table every 15th tick (60 s), otherwise only pools >= 75% full
    LONG np = g_npool; if (np > (LONG)(sizeof g_pool / sizeof g_pool[0])) np = sizeof g_pool / sizeof g_pool[0];
    bool full = (tick % 15) == 3;
    int shown = 0;
    static LONG deduped;                                         // the same object can be registered by its base ctor and its own ctor
    for (; deduped < np; deduped++)
        for (LONG j = 0; j < deduped; j++)
            if (g_pool[j].obj == g_pool[deduped].obj) {
                if (g_pool[deduped].newSize != g_pool[deduped].origSize) { g_pool[j].origSize = g_pool[deduped].origSize; g_pool[j].newSize = g_pool[deduped].newSize; }
                g_pool[deduped].obj = 0; break;
            }
    for (LONG i = 0; i < np; i++) {
        PoolRec& r = g_pool[i];
        if (!r.obj) continue;
        if (readable((const char*)r.obj + 0x10, 32)) safeName((const char*)r.obj + 0x10, r.name);
        char t[64]; int fill = poolFill(r, t, sizeof t);
        if (fill > (int)r.hw) r.hw = fill;
        if (full ? (fill >= 0 || r.newSize != r.origSize) : fill >= 750) {
            if (!shown++) fprintf(f, full ? "MEMPOOLS (all, fill now / high-water):\n" : "MEMPOOLS >=75%%:\n");
            fprintf(f, "  %-34s %5.1f%% (hw %5.1f%%) %s%s\n", r.name[0] ? r.name : "?", fill / 10.0, r.hw / 10.0, t,
                    r.newSize != r.origSize ? "  [enlarged]" : "");
        }
    }
    // failure signals since last report
    for (int i = 0; i < 1024; i++) {
        Sig& s = g_warn[i]; LONG n = s.n;
        if (s.key && n != s.shown) { fprintf(f, "WARN  caller %08x  +%ld (total %ld)  \"%s\"\n", (unsigned)s.key, n - s.shown, n, s.text); s.shown = n; }
    }
    for (int i = 0; i < 128; i++) {
        Sig& s = g_refuse[i]; LONG n = s.n;
        if (s.key && n != s.shown) { fprintf(f, "REFUSED  pool \"%s\" refused %ld allocations (total %ld)\n", s.text, n - s.shown, n); s.shown = n; }
    }
    static LONG memShown;
    LONG me = g_memErrN;
    for (LONG k = memShown; k < me; k++) if (me - k <= 16) fprintf(f, "MEMPOOL ERROR: %s\n", g_memErr[k & 15]);
    memShown = me;
}

void audit_startup_log(FILE* f) {
    LONG np = g_npool;
    fprintf(f, "AUDIT: %ld mempools registered (static initializers included if > 30)\n", np);
    for (LONG i = 0; i < np && i < (LONG)(sizeof g_pool / sizeof g_pool[0]); i++)
        if (g_pool[i].newSize != g_pool[i].origSize)
            fprintf(f, "  enlarged %-34s %u -> %u bytes\n", g_pool[i].name, g_pool[i].origSize, g_pool[i].newSize);
}

// ---------------------------------------------------------------- install
static void jmpTo(uintptr_t site, void* target, const unsigned char* expect, size_t n, const char* what) {
    unsigned char r[16]; memset(r, 0x90, sizeof r);
    r[0] = 0xE9; int rel = (int)((uintptr_t)target - (site + 5)); memcpy(r + 1, &rel, 4);
    patch(site, expect, r, n, what);
}

void apply_audit() {
    if (!g_ctx.dx11) return;
    g_resScale = iniDouble("ResourceScale", 8.0, 1.0, 32.0);
    if (!iniInt("AuditHooks", 1, 0, 1)) { L("audit hooks off"); return; }
    L("audit: ResourceScale %.1f", g_resScale);

    static const unsigned char eBlock[]  = { 0x53, 0x8B, 0x5C, 0x24, 0x0C };
    static const unsigned char eLinear[] = { 0x8B, 0x44, 0x24, 0x14, 0x56 };
    static const unsigned char eBase0[]  = { 0x56, 0x8B, 0xF1, 0x6A, 0x20 };
    static const unsigned char eBase1[]  = { 0x8B, 0x44, 0x24, 0x04, 0x56 };
    jmpTo(0xDB9950, thunkBlock,  eBlock,  5, "audit: block mempool ctor");
    jmpTo(0xDB1400, thunkLinear, eLinear, 5, "audit: linear mempool ctor");
    jmpTo(0xDB3150, thunkBase0,  eBase0,  5, "audit: mempool base ctor 0");
    jmpTo(0xDB3180, thunkBase1,  eBase1,  5, "audit: mempool base ctor 1");

    static const unsigned char eWarn[] = { 0xC3, 0xCC, 0xCC, 0xCC, 0xCC };
    jmpTo(0x778910, thunkWarn, eWarn, 5, "audit: engine warning hook");

    static const unsigned char eCan[] = { 0x51, 0x57, 0x8B, 0xF9, 0x8A, 0x47, 0x04 };
    if (!memcmp((void*)0xDB99D0, eCan, 7)) {
        g_canAllocTramp = allocExec(32);
        unsigned char* p = g_canAllocTramp; memcpy(p, eCan, 7); p += 7; *p++ = 0xE9; putRel32(p, 0xDB99D7);
        jmpTo(0xDB99D0, thunkCanAlloc, eCan, 7, "audit: block can-alloc hook");
    } else L("SKIP audit: block can-alloc hook: bytes differ");

    static const unsigned char eErr[] = { 0x83, 0xC4, 0x0C, 0x32, 0xC0, 0xC3 };
    jmpTo(0xDC8EF4, thunkMemErr, eErr, 6, "audit: mempool error hook");
}

// crash reporter helper: is `a` inside a registered engine mempool's buffer or object?
bool auditFindPool(unsigned a, char* out, size_t n) {
    LONG np = g_npool; if (np > (LONG)(sizeof g_pool / sizeof g_pool[0])) np = sizeof g_pool / sizeof g_pool[0];
    for (LONG i = 0; i < np; i++) {
        const unsigned char* o = (const unsigned char*)g_pool[i].obj;
        if (!o || !readable(o, 0x58)) continue;
        unsigned buf = *(const unsigned*)(o + 0x38), size = *(const unsigned*)(o + 0x3C);
        char nm[32]; if (!safeName((const char*)o + 0x10, nm)) strcpy(nm, "?");
        if (a - (unsigned)(uintptr_t)o < 0x58) { snprintf(out, n, "mempool object \"%s\" +0x%x", nm, a - (unsigned)(uintptr_t)o); return true; }
        if (buf && size && a - buf < size) {
            unsigned used = *(const unsigned*)(o + 0x44);
            snprintf(out, n, "mempool \"%s\" buffer +0x%x of 0x%x (used field 0x%x)%s", nm, a - buf, size, used,
                     g_pool[i].newSize != g_pool[i].origSize ? " [enlarged by mod]" : "");
            return true;
        }
    }
    return false;
}
