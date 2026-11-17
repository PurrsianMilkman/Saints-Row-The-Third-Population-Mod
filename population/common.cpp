#include "common.h"

Ctx   g_ctx;
FILE* g_log;

void L(const char* f, ...) {
    if (!g_log) return;
    va_list a; va_start(a, f); vfprintf(g_log, f, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

bool patch(uintptr_t addr, const unsigned char* expect, const unsigned char* repl, size_t n, const char* what) {
    if (memcmp((void*)addr, expect, n)) { L("SKIP %-28s @%08x: bytes differ from expected", what, (unsigned)addr); return false; }
    DWORD old; VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)addr, repl, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    L("OK   %-28s @%08x", what, (unsigned)addr);
    return true;
}

struct Region { unsigned base, size; const char* name; };
static Region g_regions[256];
static volatile LONG g_nregions;
void regRegion(const void* base, size_t size, const char* name) {
    LONG i = InterlockedIncrement(&g_nregions) - 1;
    if (i < 256) { g_regions[i].base = (unsigned)(uintptr_t)base; g_regions[i].size = (unsigned)size; g_regions[i].name = name; }
}
void* modAlloc(size_t n, const char* name) {
    void* p = VirtualAlloc(0, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p) regRegion(p, n, name);
    return p;
}
bool findRegion(unsigned a, const char** name, unsigned* off) {
    LONG n = g_nregions; if (n > 256) n = 256;
    for (LONG i = 0; i < n; i++) if (a - g_regions[i].base < g_regions[i].size) { *name = g_regions[i].name; *off = a - g_regions[i].base; return true; }
    return false;
}

unsigned char* allocExec(size_t n) {
    return (unsigned char*)VirtualAlloc(0, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

void put32(unsigned char*& p, uintptr_t v) { memcpy(p, &v, 4); p += 4; }

void putRel32(unsigned char*& p, uintptr_t target) {
    int rel = (int)(target - ((uintptr_t)p + 4));
    memcpy(p, &rel, 4); p += 4;
}

int iniInt(const char* key, int def, int lo, int hi) {
    char d[32], b[64]; snprintf(d, sizeof d, "%d", def);
    GetPrivateProfileStringA("Population", key, d, b, sizeof b, g_ctx.ini);
    int v = atoi(b); if (v < lo) v = lo; if (v > hi) v = hi; return v;
}

double iniDouble(const char* key, double def, double lo, double hi) {
    char d[32], b[64]; snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA("Population", key, d, b, sizeof b, g_ctx.ini);
    double v = atof(b); if (v < lo) v = lo; if (v > hi) v = hi; return v;
}
