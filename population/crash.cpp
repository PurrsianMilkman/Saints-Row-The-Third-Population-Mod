// Crash reporter. Two layers:
//  - a vectored handler sees every access violation / illegal instruction raised by game code (first chance - the game handles some of
//    these itself, e.g. a null player read at 0x9E7817 during loading). Each distinct fault site is logged once as "FAULT #n".
//  - a last-chance (unhandled) filter, re-asserted every monitor tick so the game cannot silently replace it, logs the fault that actually
//    kills the process as "FATAL" and writes SR3Population.dmp.
// Every report classifies the faulting address and each register: exe section, a table the mod allocated/relocated (modAlloc), an engine
// mempool (audit registry), an entity pool slot, the thread's stack, other modules, heap, or unmapped. tools\crash_report.py turns the log
// into function names + disassembly offline.
#include "common.h"
#include <dbghelp.h>

bool findRegion(unsigned a, const char** name, unsigned* off);   // common.cpp
bool auditFindPool(unsigned a, char* out, size_t n);              // audit.cpp

static volatile LONG g_faults;
static DWORD g_seen[32];

static bool inExe(DWORD a) { return a >= 0x401000 && a < 0x101F000; }

void describeAddress(unsigned a, char* out, size_t n) {
    const char* rn; unsigned ro;
    if (a < 0x10000) { snprintf(out, n, "NULL page (null pointer + 0x%x)", a); return; }
    if (findRegion(a, &rn, &ro)) { snprintf(out, n, "mod table \"%s\" +0x%x", rn, ro); return; }
    if (a >= 0x401000 && a < 0x101F000) { snprintf(out, n, "exe .text"); return; }
    if (a >= 0x101F000 && a < 0x12E5000) { snprintf(out, n, "exe .rdata"); return; }
    if (a >= 0x12E5000 && a < 0x475E600) { snprintf(out, n, "exe .data (initialised)"); return; }
    if (a >= 0x475E600 && a < 0x699F000) { snprintf(out, n, "exe .bss (static zero-init data)"); return; }
    for (int i = 0; i < 96; i++) {                          // entity pools: items @+0x44, end @+0x48, capacity u16 @+0x3A
        unsigned* pp = (unsigned*)(0x61EE7E0 + 4 * i); MEMORY_BASIC_INFORMATION mb;
        if (!VirtualQuery(pp, &mb, sizeof mb) || mb.State != MEM_COMMIT) break;
        unsigned char* o = (unsigned char*)*pp; if (!o || (uintptr_t)o < 0x10000) continue;
        if (!VirtualQuery(o, &mb, sizeof mb) || mb.State != MEM_COMMIT) continue;
        if (*(unsigned*)o == 0x111D2FC) continue;              // abstract object_instantiator: no item storage
        unsigned base = *(unsigned*)(o + 0x44), end = *(unsigned*)(o + 0x48), cap = *(unsigned short*)(o + 0x3A);
        if (base && end > base && end - base < 0x8000000 && cap && a - base < end - base) {
            unsigned stride = (end - base) / cap;
            snprintf(out, n, "entity pool #%d (vtable %06x) slot %u +0x%x (cap %u, stride ~0x%x)", i, *(unsigned*)o & 0xFFFFFF,
                     stride ? (a - base) / stride : 0, stride ? (a - base) % stride : 0, cap, stride);
            return;
        }
    }
    if (auditFindPool(a, out, n)) return;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    if ((void*)a >= tib->StackLimit && (void*)a < tib->StackBase) { snprintf(out, n, "this thread's stack"); return; }
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery((void*)a, &mb, sizeof mb)) { snprintf(out, n, "?"); return; }
    if (mb.State == MEM_FREE) { snprintf(out, n, "UNMAPPED (free address space)"); return; }
    if (mb.State == MEM_RESERVE) { snprintf(out, n, "reserved but not committed (alloc base %08x)", (unsigned)(uintptr_t)mb.AllocationBase); return; }
    if (mb.Type == MEM_IMAGE) {
        char m[MAX_PATH] = "?"; GetModuleFileNameA((HMODULE)mb.AllocationBase, m, MAX_PATH);
        const char* s = strrchr(m, '\\');
        snprintf(out, n, "module %s +0x%x", s ? s + 1 : m, a - (unsigned)(uintptr_t)mb.AllocationBase); return;
    }
    snprintf(out, n, "%s memory, alloc base %08x +0x%x%s", mb.Type == MEM_MAPPED ? "mapped" : "private/heap",
             (unsigned)(uintptr_t)mb.AllocationBase, a - (unsigned)(uintptr_t)mb.AllocationBase,
             (mb.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ? " (no access / guard page)" : "");
}

static void writeDump(EXCEPTION_POINTERS* ep) {
    typedef BOOL(WINAPI* MDW)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dh = LoadLibraryA("dbghelp.dll"); if (!dh) return;
    MDW w = (MDW)GetProcAddress(dh, "MiniDumpWriteDump"); if (!w) return;
    char path[MAX_PATH]; strcpy(path, g_ctx.logPath); strcpy(strrchr(path, '.'), ".dmp");
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mi = { GetCurrentThreadId(), ep, FALSE };
    w(GetCurrentProcess(), GetCurrentProcessId(), h, (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo), &mi, 0, 0);
    CloseHandle(h);
}

static void report(EXCEPTION_POINTERS* ep, const char* tag, LONG k) {
    CONTEXT* c = ep->ContextRecord; EXCEPTION_RECORD* er = ep->ExceptionRecord;
    FILE* f = fopen(g_ctx.logPath, "a"); if (!f) return;
    unsigned fa = er->NumberParameters >= 2 ? (unsigned)er->ExceptionInformation[1] : 0;
    const char* kind = er->NumberParameters >= 2 ? (er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read") : "?";
    char d[200];
    fprintf(f, "%s #%ld code %08lx at eip %08lx (thread %lu)\n", tag, k, er->ExceptionCode, c->Eip, GetCurrentThreadId());
    describeAddress(c->Eip, d, sizeof d); fprintf(f, "  eip is in: %s\n", d);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) { describeAddress(fa, d, sizeof d); fprintf(f, "  %s of %08x -> %s\n", kind, fa, d); }
    const char* rn[8] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp" };
    DWORD rv[8] = { c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp };
    for (int i = 0; i < 8; i++) { describeAddress(rv[i], d, sizeof d); fprintf(f, "  %s %08lx  %s\n", rn[i], rv[i], d); }
    fprintf(f, "  code at eip:");
    __try { for (int i = 0; i < 16; i++) fprintf(f, " %02x", ((unsigned char*)c->Eip)[i]); } __except (EXCEPTION_EXECUTE_HANDLER) { fprintf(f, " <unreadable>"); }
    fprintf(f, "\n  stack (return-address candidates into the exe, nearest first):");
    DWORD* sp = (DWORD*)c->Esp; int n = 0;
    for (int i = 0; i < 4096 && n < 48; i++) {
        DWORD v; __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (inExe(v)) { fprintf(f, " %08lx", v); n++; }
    }
    fprintf(f, "\n");
    fclose(f);
}

static LONG CALLBACK onFault(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    HMODULE me = 0; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&onFault, &me);
    MEMORY_BASIC_INFORMATION mb;
    if (VirtualQuery((void*)c->Eip, &mb, sizeof mb) && mb.AllocationBase == (void*)me) return EXCEPTION_CONTINUE_SEARCH;   // our own __try probes
    for (int i = 0; i < 32; i++) if (g_seen[i] == c->Eip) return EXCEPTION_CONTINUE_SEARCH;
    LONG k = InterlockedIncrement(&g_faults); if (k > 32) return EXCEPTION_CONTINUE_SEARCH; g_seen[k - 1] = c->Eip;
    report(ep, "FAULT (first chance, may be handled by the game)", k);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter;
static volatile LONG g_fatal;
static LONG WINAPI onUnhandled(EXCEPTION_POINTERS* ep) {
    if (!InterlockedExchange(&g_fatal, 1)) { report(ep, "FATAL (unhandled - this one kills the game)", 1); writeDump(ep); }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void crash_tick() {   // called from the monitor thread every 4 s
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(onUnhandled);
    if (cur != onUnhandled) g_prevFilter = cur;
}

void installCrashReporter() {
    AddVectoredExceptionHandler(1, onFault);
    crash_tick();
    L("crash reporter installed (FAULT = first chance, FATAL = unhandled + SR3Population.dmp)");
}
