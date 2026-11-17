// SR3 Population Counter (SR3PopCounter.asi) - separate, read-only plugin. See counter.h, build.cpp, sample.cpp, overlay.cpp.
#include "counter.h"

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { HANDLE t = CreateThread(0, 0, overlayThread, 0, 0, 0); if (t) CloseHandle(t); }
    return TRUE;
}
