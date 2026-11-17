// The on-screen counter: a layered, click-through, topmost window placed over the game window's top-left corner. It never touches D3D,
// so it does not appear in exclusive fullscreen (use borderless / windowed). F10 hides / shows it.
#include "counter.h"

static HWND g_ov;
static bool g_show = true;
static const int OV_W = 704, OV_H = 148;

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &r, b); DeleteObject(b);
        HFONT f = CreateFontA(22, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, "Consolas");
        SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(120, 255, 120));
        r.left += 8; r.top += 6; DrawTextA(dc, g_txt, -1, &r, DT_LEFT | DT_NOCLIP);
        DeleteObject(f); EndPaint(h, &ps); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

// Find the game's main window: visible, unowned, same process, wider than 300 px, and not our overlay.
struct FindCtx { DWORD pid; HWND w; };
static BOOL CALLBACK enumWindows(HWND h, LPARAM p) {
    FindCtx* c = (FindCtx*)p; DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && h != g_ov) {
        RECT r; GetClientRect(h, &r); if (r.right > 300) { c->w = h; return FALSE; }
    }
    return TRUE;
}

DWORD WINAPI overlayThread(LPVOID) {
    selectBuild();
    Sleep(8000);   // let the game create its window and load first
    WNDCLASSA wc = {}; wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "SR3PopCounter";
    RegisterClassA(&wc);
    g_ov = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           "SR3PopCounter", "", WS_POPUP, 10, 10, OV_W, OV_H, 0, 0, wc.hInstance, 0);
    SetLayeredWindowAttributes(g_ov, 0, 200, LWA_ALPHA);
    ShowWindow(g_ov, SW_SHOWNOACTIVATE);
    for (;;) {
        if (GetAsyncKeyState(VK_F10) & 1) { g_show = !g_show; ShowWindow(g_ov, g_show ? SW_SHOWNOACTIVATE : SW_HIDE); }
        FindCtx c = { GetCurrentProcessId(), 0 }; EnumWindows(enumWindows, (LPARAM)&c);
        if (c.w) { POINT p = { 0, 0 }; ClientToScreen(c.w, &p);
                   SetWindowPos(g_ov, HWND_TOPMOST, p.x + 10, p.y + 10, OV_W, OV_H, SWP_NOACTIVATE | (g_show ? SWP_SHOWWINDOW : 0)); }
        sample(); InvalidateRect(g_ov, 0, FALSE);
        MSG m; while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        Sleep(250);
    }
}
