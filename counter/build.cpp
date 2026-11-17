// Per-executable address tables for the counter (DX9 exe = 17e4d85b..., DX11 exe = 66d3ba3a...).
#include "counter.h"

static const Build DX9 = {
    0x907530, 0x2609E38, 0x2CC991C, 0x2CC9988, 0x9086D0, 0,        0x130AF7C, 0x910940,
    0x2609120, 0x2609E34, 0x1308DA8, 0x13089A8, 0x3171A64, 0x907970 };
static const Build DX11 = {
    0x906510, 0x5A113B8, 0x61EE7FC, 0x61EE868, 0x907690, 0x908AB0, 0x130EF8C, 0x90F900,
    0x5A106A0, 0x5A113B4, 0x130CDB8, 0x130C9B8, 0x65382B4, 0x906930 };

Build B;

void selectBuild() {
    char exe[MAX_PATH]; GetModuleFileNameA(0, exe, MAX_PATH);
    B = strstr(exe, "DX11") ? DX11 : DX9;
}

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery(p, &m, sizeof m) && m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           (char*)p + n <= (char*)m.BaseAddress + m.RegionSize;
}
