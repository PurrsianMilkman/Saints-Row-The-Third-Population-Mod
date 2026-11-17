// Parked cars. The maximum lives in ONE global (vanilla 18). It is reset to 18 in two places (world start / reset) and the Lua setter
// `parking_spot_set_max` rejects anything above 0x12. The enter/exit-vehicle handlers also clamp it - those are patched in counts.cpp.
#include "common.h"

void apply_parked() {
    const bool dx11 = g_ctx.dx11;
    const int pmx = g_ctx.parkedMax;
    unsigned char e18d[4] = { 0x12,0,0,0 }, r18d[4] = { (unsigned char)pmx,0,0,0 };
    unsigned char e18b[1] = { 0x12 }, r18b[1] = { (unsigned char)pmx };
    patch(dx11 ? 0x918E56 : 0x919E46, e18d, r18d, 4, "parked max reset #1");
    patch(dx11 ? 0x918FA7 : 0x919F97, e18d, r18d, 4, "parked max reset #2");
    patch(dx11 ? 0x918E66 : 0x919E56, e18b, r18b, 1, "parked max setter clamp");
    patch(dx11 ? 0x130CDB8 : 0x1308DA8, e18d, r18d, 4, "parked max current value");
    L("parked max -> %d", pmx);
}
