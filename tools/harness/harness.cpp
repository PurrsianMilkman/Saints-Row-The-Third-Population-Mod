// Test harness: maps the real SaintsRowTheThird_DX11.exe image at its fixed base (0x400000) inside this process, loads a plugin .asi into it,
// then dumps the patched image. Lets us diff two plugin builds (old monolith vs new modules) byte-for-byte without launching the game.
// Build as an x86 exe linked at /BASE:0x10000 /FIXED so 0x400000.. is free. The exe must be named *DX11* (plugins pick their address tables by name).
#include <windows.h>
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 4) { printf("usage: harness <exe> <plugin.asi> <image.out>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb"); if (!f) { printf("cannot open exe\n"); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> file(sz); fread(file.data(), 1, sz, f); fclose(f);
    auto dos = (IMAGE_DOS_HEADER*)file.data(); auto nt = (IMAGE_NT_HEADERS32*)(file.data() + dos->e_lfanew);
    unsigned char* base = (unsigned char*)VirtualAlloc((void*)nt->OptionalHeader.ImageBase, nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (base != (unsigned char*)nt->OptionalHeader.ImageBase) {
        printf("could not map image at %p (err %lu)\n", (void*)nt->OptionalHeader.ImageBase, GetLastError());
        MEMORY_BASIC_INFORMATION mb; unsigned char* p = (unsigned char*)nt->OptionalHeader.ImageBase; unsigned char* end = p + nt->OptionalHeader.SizeOfImage;
        while (p < end && VirtualQuery(p, &mb, sizeof mb)) {
            if (mb.State != MEM_FREE) printf("  blocker: %p size %lx state %lx type %lx\n", mb.BaseAddress, (unsigned long)mb.RegionSize, (unsigned long)mb.State, (unsigned long)mb.Type);
            p = (unsigned char*)mb.BaseAddress + mb.RegionSize;
        }
        return 3;
    }
    memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize));
    std::vector<unsigned char> before(base, base + nt->OptionalHeader.SizeOfImage);
    HMODULE h = LoadLibraryA(argv[2]);
    printf("plugin %s -> %p\n", argv[2], h);
    if (!h) return 4;
    FILE* o = fopen(argv[3], "wb"); fwrite(base, 1, nt->OptionalHeader.SizeOfImage, o); fclose(o);
    int diff = 0; for (size_t i = 0; i < before.size(); i++) if (before[i] != base[i]) diff++;
    printf("bytes changed in image: %d\n", diff);
    return 0;
}
