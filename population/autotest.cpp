// Unattended testing support (DX11). The user does not drive test runs, and the main menu ignores synthetic input, so the plugin itself
// presses CONTINUE: the per-frame call at 0x5D24E8 (main loop -> 0x5CFEE0) is routed through a thunk; once AutoContinueSeconds have passed
// and there is still no local player, it calls 0x7ACC20 - the C side of the menu's Lua "main_menu_continue" - on the main thread.
// Off unless AutoContinueSeconds > 0 (tools\launch_test.ps1 -Continue sets it).
#include "common.h"

static DWORD g_start, g_delayMs, g_lastTry;
static int g_tries;

static void __cdecl onFrame() {
    if (g_tries >= 3) return;
    DWORD now = GetTickCount();
    if (now - g_start < g_delayMs || now - g_lastTry < 10000) return;
    typedef void* (__cdecl* getPlayer)();
    if (((getPlayer)0x9D99C0)()) { g_tries = 3; return; }        // already in the world
    g_lastTry = now; g_tries++;
    ((void(__cdecl*)())0x7ACC20)();                            // main_menu_continue
    if (FILE* f = fopen(g_ctx.logPath, "a")) { fprintf(f, "AUTOTEST: main_menu_continue called (try %d)\n", g_tries); fclose(f); }
}

__declspec(naked) static void frameThunk() {
    __asm {
        pushad
        pushfd
        call onFrame
        popfd
        popad
        mov eax, 0x5CFEE0
        jmp eax
    }
}

void apply_autotest() {
    if (!g_ctx.dx11) return;
    int s = iniInt("AutoContinueSeconds", 0, 0, 3600);
    if (!s) return;
    g_start = GetTickCount(); g_delayMs = (DWORD)s * 1000;
    unsigned char e[5] = { 0xE8 }, r[5] = { 0xE8 };
    int oldRel = (int)(0x5CFEE0 - (0x5D24E8 + 5)); memcpy(e + 1, &oldRel, 4);
    int rel = (int)((uintptr_t)frameThunk - (0x5D24E8 + 5)); memcpy(r + 1, &rel, 4);
    if (patch(0x5D24E8, e, r, 5, "autotest frame hook")) L("autotest: CONTINUE after %d s", s);
}
