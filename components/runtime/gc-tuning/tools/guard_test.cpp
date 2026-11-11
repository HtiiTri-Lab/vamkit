// Offline check of the 1.1.1 boundary guard (native\gctuning.cpp, GuardedVirtualAlloc / GT_GuardBoundaries).
// Build + run: tools\guard_test.cmd. Uses VaM's mono.dll from the install (loaded, not initialised).
#include "../native/gctuning.cpp"
#include <stdio.h>

static int gFails;
#define CHECK(c, ...) do { if (!(c)) { gFails++; printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static word FreeBoundary(SIZE_T before, SIZE_T after) {  // a 4 GB boundary with [b - before, b + after) all free
    for (word b = (word)0x300 << 32; b < ((word)0x700 << 32); b += kFourGb) {
        void* p = VirtualAlloc((void*)(b - before), before + after, MEM_RESERVE, PAGE_NOACCESS);
        if (p == (void*)(b - before)) { VirtualFree(p, 0, MEM_RELEASE); return b; }
        if (p) VirtualFree(p, 0, MEM_RELEASE);
    }
    return 0;
}

static word gForceAt; static SIZE_T gForceSize; static int gCalls;
static LPVOID WINAPI ForcingAlloc(LPVOID a, SIZE_T n, DWORD t, DWORD pr) {  // first NULL call lands on gForceAt
    gCalls++;
    if (!a && gForceAt) { word at = gForceAt; gForceAt = 0; return VirtualAlloc((void*)at, n, t, pr); }
    return VirtualAlloc(a, n, t, pr);
}

static DWORD StateAt(word p) { MEMORY_BASIC_INFORMATION mb; VirtualQuery((void*)p, &mb, sizeof(mb)); return mb.State; }

int wmain(int argc, wchar_t** argv) {
    // 1. Wrapper logic with an allocator that puts the first region on a boundary.
    gRealVirtualAlloc = ForcingAlloc;
    const SIZE_T chunk = 65536 + 1;  // GC_win32_get_mem's scratch chunk
    word b = FreeBoundary(0, 1 << 20);
    gForceAt = b;
    word r = (word)GuardedVirtualAlloc(NULL, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(r && !BoundaryIn(r, chunk), "starts on boundary %llx -> %llx", b, r);
    CHECK(StateAt(b) == MEM_RESERVE, "boundary %llx not kept reserved (state %lx)", b, StateAt(b));
    CHECK(gBoundaryHits == 1 && gBoundaryPlugs == 1, "hits %ld plugs %ld", gBoundaryHits, gBoundaryPlugs);
    *(volatile char*)r = 1;  // committed and writable
    printf("on-boundary chunk %llx moved to %llx, boundary reserved\n", b, r);

    word b2 = FreeBoundary(1 << 20, 1 << 20);  // a 1 MB region straddling a boundary
    gForceAt = b2 - 65536;
    word r2 = (word)GuardedVirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(r2 && !BoundaryIn(r2, 1 << 20), "straddling %llx -> %llx", b2, r2);
    CHECK(StateAt(b2) == MEM_RESERVE, "boundary %llx not reserved", b2);
    CHECK(StateAt(b2 - 65536) == MEM_FREE, "released region %llx still held", b2 - 65536);
    printf("straddling region at %llx moved to %llx, boundary reserved, old region released\n", b2 - 65536, r2);

    int calls = gCalls;  // explicit address and commit-only calls pass straight through
    word fixed = FreeBoundary(0, 1 << 16);
    void* f = GuardedVirtualAlloc((void*)fixed, 65536, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(f == (void*)fixed && gCalls == calls + 1, "explicit address not passed through");
    void* c = GuardedVirtualAlloc((void*)fixed, 4096, MEM_COMMIT, PAGE_READWRITE);
    CHECK(c == (void*)fixed, "commit not passed through");
    CHECK(gBoundaryHits == 2, "explicit-address call counted as hit");
    VirtualFree(f, 0, MEM_RELEASE);

    word plain = (word)GuardedVirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(plain && gBoundaryHits == 2, "ordinary region touched");

    // 2. The real mono.dll: the import slot is found, holds the system VirtualAlloc, and is redirected.
    gRealVirtualAlloc = NULL;
    HMODULE m = argc > 1 ? LoadLibraryW(argv[1]) : NULL;  // <VaM>/Mono/EmbedRuntime/mono.dll
    CHECK(m, "mono.dll not loaded (%lu)", GetLastError());
    if (m) {
        CHECK(GT_Init() == 1, "GT_Init: %s", gStatus);
        void** slot = FindImport((uint8_t*)m, "KERNEL32.dll", "VirtualAlloc");
        CHECK(slot && (uint8_t*)slot - (uint8_t*)m == 0x1901d8, "slot rva %llx", slot ? (word)((uint8_t*)slot - (uint8_t*)m) : 0);
        int bad = -1;
        CHECK(GT_GuardBoundaries(&bad) == 1 && bad == 0, "guard bad %d", bad);
        CHECK(*slot == (void*)&GuardedVirtualAlloc, "slot not redirected");
        CHECK(GT_GuardBoundaries(&bad) == 1 && gRealVirtualAlloc != (VirtualAllocFn)&GuardedVirtualAlloc, "second call wraps itself");
        LPVOID viaMono = ((VirtualAllocFn)*slot)(NULL, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        CHECK(viaMono != NULL, "call through mono's slot failed");
        char st[2048]; GT_Status(st, sizeof st);
        printf("mono.dll slot rva %llx redirected; status: ...%s\n", (word)((uint8_t*)slot - (uint8_t*)m), strstr(st, "boundary guard"));
    }
    printf(gFails ? "%d FAILED\n" : "all passed\n", gFails);
    return gFails ? 1 : 0;
}
