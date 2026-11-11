// Offline test of GT_SetFinalizerRule (native\gctuning.cpp, section 3), 2026-11-05. Runs mono.dll's own 42 bytes of
// GC_collect_or_expand's finalizable-object test, and the replacement, for real: on a copy of the image layout whose
// globals (GC_fo_entries, last_fo_entries, last_words_finalized, GC_words_finalized, GC_words_allocd) the test sets,
// with `mov eax, 0 / 1; ret` at the two places the test jumps to (0x158807 = go on to GC_should_collect, 0x158810 =
// collect). Compares with the C expressions of libgc 6 (alloc.c:1107) and bdwgc 8, and checks that n = 0 puts the
// original bytes back. Writes the stub's bytes to fo_stub.bin (disassembled by fo_test.py).
//     fo_test.exe <VaMGcTuningNative.bin> <mono.dll> <out dir>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*SetFn)(uint8_t* base, int n);
typedef int (*TestFn)();

static uint64_t gRng = 0x9e3779b97f4a7c15ull;
static uint64_t Rnd() { gRng ^= gRng << 13; gRng ^= gRng >> 7; gRng ^= gRng << 17; return gRng; }

enum : uint32_t { FO = 0x269ce0, LAST_FO = 0x269be8, LAST_WF = 0x269bf0, WF = 0x27c838, ALLOCD = 0x27c828,
                  RULE = 0x1587dd, NEXT = 0x158807, COLLECT = 0x158810, LOCK = 0x2c1ce0, IMG_SIZE = 0x2c3000 };

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: fo_test <native.bin> <mono.dll> <outdir>\n"); return 2; }
    HMODULE nat = LoadLibraryW(argv[1]);
    HMODULE mono = LoadLibraryExW(argv[2], NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!nat || !mono) { fprintf(stderr, "load failed %lu\n", GetLastError()); return 2; }
    SetFn set = (SetFn)GetProcAddress(nat, "GT_TestFinalizerRule");
    uint8_t* img = (uint8_t*)VirtualAlloc(NULL, IMG_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(img + RULE, (uint8_t*)mono + RULE, 42);
    uint8_t orig[42]; memcpy(orig, img + RULE, 42);
    static const uint8_t ret0[6] = { 0xb8, 0, 0, 0, 0, 0xc3 }, ret1[6] = { 0xb8, 1, 0, 0, 0, 0xc3 };
    memcpy(img + NEXT, ret0, 6);
    memcpy(img + COLLECT, ret1, 6);
    InitializeCriticalSection((CRITICAL_SECTION*)(img + LOCK));
    TestFn test = (TestFn)(img + RULE);
    uint64_t* fo = (uint64_t*)(img + FO); uint64_t* last = (uint64_t*)(img + LAST_FO);
    uint64_t* lwf = (uint64_t*)(img + LAST_WF); uint64_t* wf = (uint64_t*)(img + WF); uint64_t* allocd = (uint64_t*)(img + ALLOCD);

    int fails = 0;
    long cases = 0;
    // n = 0 -> mono's test; n > 0 -> bdwgc 8's with that n
    int ns[] = { 0, 10000, 3, 10000, 0 };
    for (int n : ns) {
        int r = set(img, n);
        if (r != n) { printf("set %d returned %d\n", n, r); fails++; continue; }
        if (n == 0 && memcmp(img + RULE, orig, 42) != 0) { printf("n 0: bytes not restored\n"); fails++; }
        if (n > 0 && (img[RULE] != 0xff || img[RULE + 1] != 0x25)) { printf("n %d: no jump\n", n); fails++; }
        int mism = 0;
        for (int i = 0; i < 2000000; i++) {
            uint64_t L = Rnd() % 2000000;
            uint64_t F;
            switch (Rnd() % 4) {
            case 0: F = L + 500 + (Rnd() % 5) - 2; break;           // stock boundary
            case 1: F = L + (Rnd() % 3) - 1; break;                 // fo == last +-1
            default: F = Rnd() % 2000000;
            }
            uint64_t A;
            uint64_t d = F > L ? F - L : 0;
            if (n > 0 && d && Rnd() % 2) { uint64_t bytes = d * (uint64_t)n; A = bytes / 8 + (Rnd() % 3) - 1; }   // bdwgc boundary
            else A = Rnd() % 400000000;
            uint64_t a = Rnd() % 3 == 0 ? 0 : Rnd() % 100000, b = Rnd() % 3 == 0 ? 0 : Rnd() % 100000;
            *fo = F; *last = L; *lwf = a; *wf = b; *allocd = A;
            int want = n == 0 ? (F > L + 500 && (a || b))
                              : (F > L && (a | b) != 0 && (F - L) * (uint64_t)n > A * 8);
            int got = test();
            cases++;
            if (got != want && mism++ < 5) printf("n %d: fo %llu last %llu lastWf %llu wf %llu allocd %llu: got %d want %d\n",
                                                n, F, L, a, b, A, got, want);
        }
        if (mism) fails++;
        printf("n %5d: %s\n", n, mism ? "MISMATCH" : "ok");
    }
    // the stub, for disassembly
    set(img, 10000);
    uint8_t* stub = *(uint8_t**)(img + RULE + 6);
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\fo_stub.bin", argv[3]);
    FILE* f = _wfopen(path, L"wb"); fwrite(stub, 1, 160, f); fclose(f);
    uint64_t base = (uint64_t)img, at = (uint64_t)stub;
    swprintf_s(path, L"%s\\fo_stub.txt", argv[3]);
    f = _wfopen(path, L"w"); fprintf(f, "%llu %llu\n", base, at); fclose(f);
    set(img, 0);
    printf("%ld cases, %s\n", cases, fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
