// VaM GC Tuning native library (VaMGcTuningNative.bin), 2026-11-04.
// VaM's mono.dll (Unity 2018.1 legacy Mono, Boehm GC 6.x, RVAs below verified against the loaded code first):
//
// 1. Parallel marking. One collection stops VaM ~0.95 s on a heavy scene, 91% of it in GC_mark_from (0x15a598), the
//    single-threaded marker (timed per phase of a collection). GC_mark_from(top, bottom, limit) is entered with a jump to
//    MarkFrom below, which computes the same thing - every object reachable from the entries is marked and nothing
//    else - on the calling thread plus helper threads, and returns an empty stack. Ports Boehm's loop exactly
//    (header lookup via GC_top_index, hb_map displacement, mark bits, length / bitmap / per-object descriptors,
//    black lists); mark bits and black-list bits are set with atomic operations. Mark procedures (GC_DS_PROC) are
//    mono.dll's own and set bits non-atomically, so they run on the calling thread between parallel rounds. If a
//    local stack ever runs out of reserved space the entry is dropped and Boehm's overflow state is set (it then
//    rescans from the marked objects, as after its own mark stack overflows). A fault while scanning is re-raised on
//    the calling thread, where GC_mark_some's handler recovers as it does for its own marker.
//    Verify mode runs Boehm's own marker first, then ours from the same entries and mark bits, and compares all mark
//    bits and black lists (keeps Boehm's result on any difference).
// 2. 4 GB boundaries. IS_FORWARDING_ADDR_OR_NIL is `(unsigned long)hhdr <= 0xfff`, 32 bits on Win64 (`cmp e?x,0xfff`
//    in the binary): a block header whose address has low 32 bits <= 0xfff is taken for a forwarding count (0 =
//    endless loop in GC_find_start / GC_base, else a wrong block and live objects left unmarked). Headers come from
//    64 KB scratch chunks (VirtualAlloc(NULL, 64 KB + 1)), so this happens when a chunk starts exactly on a 4 GB
//    boundary. GT_GuardBoundaries wraps mono.dll's VirtualAlloc import: a new region (address NULL, MEM_RESERVE)
//    that covers a 4 GB boundary is released, the first 64 KB after that boundary reserved, and the call repeated,
//    so nothing of mono's ever sits on a boundary.
//    1.1.0 reserved those 64 KB after every boundary from 4 GB to 16 TB up front instead. That left no free range
//    longer than 4 GB below 16 TB, and CUDA (which DLSS SR / RR / NR / DLSS-G use through NvAPI_D3D12_CreateCuModule)
//    could not reserve its address range: every NGX feature failed with 0xBAD00002 (docs/knowledge/gc.md).
//    Now only the boundaries mono itself reaches are taken (a few per session).
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uintptr_t word;
struct Mse { word start; word descr; };
typedef Mse* (*MarkFromFn)(Mse* top, Mse* bottom, Mse* limit);
typedef Mse* (*MarkProcFn)(word* addr, Mse* msp, Mse* msl, word env);

// ---- mono.dll layout ---------------------------------------------------------------------------------------------
enum : uint32_t {
    RVA_MARK_FROM = 0x15a598, RVA_MARK_FROM_END = 0x15ac2c,
    RVA_ARRAYS = 0x27c7e0,            // GC_arrays (lea r10 at 0x15a620)
    OFS_TOP_INDEX = 0x414f0,          // GC_top_index[2048] in GC_arrays
    RVA_ALL_NILS_PTR = 0x2bdcc8,      // bottom_index* GC_all_nils
    OFS_MARK_PROCS = 0x88,            // GC_mark_procs[64] in GC_arrays
    RVA_LEAST_HA = 0x265968, RVA_GREATEST_HA = 0x269bd0,
    RVA_INVALID_HEADER = 0x26c5c0,    // hdr* GC_invalid_header
    RVA_ALL_INTERIOR = 0x269c3c,      // int GC_all_interior_pointers
    RVA_OBJECTS_ARE_MARKED = 0x269c90,
    RVA_MARK_STATE = 0x269c88, RVA_STACK_TOO_SMALL = 0x269c8c,
    RVA_MODWS_VALID = 0x281ab9,       // char GC_modws_valid_offsets[8]
    RVA_OLD_NORMAL_BL = 0x26c448, RVA_INCOMPLETE_NORMAL_BL = 0x26c450,
    RVA_OLD_STACK_BL = 0x26c458, RVA_INCOMPLETE_STACK_BL = 0x26c460,
    RVA_ALLOC_ML = 0x2c1ce0,          // CRITICAL_SECTION GC_allocate_ml
};
// hdr (struct hblkhdr): hb_sz +0 (words), hb_descr +0x18, hb_map +0x20, hb_flags +0x29 (1 = IGNORE_OFF_PAGE),
// hb_marks[8] +0x30. bottom_index: index[1024] +0, key +0x2010, hash_link +0x2018.
struct Hdr { word sz; word next, prev; word descr; uint8_t* map; uint8_t kind, flags; uint16_t lastReclaimed; uint32_t pad; volatile LONG64 marks[8]; };
static_assert(offsetof(Hdr, descr) == 0x18 && offsetof(Hdr, map) == 0x20 && offsetof(Hdr, flags) == 0x29 && offsetof(Hdr, marks) == 0x30, "hdr");
struct Bi { Hdr* index[1024]; Bi* asc; Bi* desc; word key; Bi* hashLink; };
static_assert(offsetof(Bi, key) == 0x2010 && offsetof(Bi, hashLink) == 0x2018, "bi");

static uint8_t* gMono;
static Bi** gTopIndex;
static Bi* gAllNils;
static MarkProcFn* gMarkProcs;
static word gLeast, gGreatest;
static bool gAllInterior;
static const uint8_t* gModwsValid;
static volatile LONG64 *gOldNormalBl, *gIncNormalBl, *gOldStackBl, *gIncStackBl;
static int* gMarkState; static int* gStackTooSmall; static int* gObjectsAreMarked;
static char gStatus[512] = "not initialised";

static word RipTarget(uint32_t rva, int dispAt, int len) { return rva + len + *(int32_t*)(gMono + rva + dispAt); }

// Each (site, bytes, rip target) the code above relies on; plus the stolen prologue.
static bool CheckLayout() {
    struct Site { uint32_t rva; const char* bytes; int dispAt, len; uint32_t target; };
    static const Site sites[] = {
        { 0x15a5c2, "\x4c\x8b\x25", 3, 7, RVA_GREATEST_HA },
        { 0x15a5c9, "\x48\x8b\x05", 3, 7, RVA_LEAST_HA },
        { 0x15a5fe, "\xc7\x05", 2, 10, RVA_OBJECTS_ARE_MARKED },
        { 0x15a620, "\x4c\x8d\x15", 3, 7, RVA_ARRAYS },
        { 0x15a79b, "\x4c\x3b\x0d", 3, 7, RVA_ALL_NILS_PTR },
        { 0x15a7c9, "\x48\x8b\x05", 3, 7, RVA_INVALID_HEADER },
        { 0x15a83b, "\x83\x3d", 2, 7, RVA_ALL_INTERIOR },
        { 0x15a54b, "\xc7\x05", 2, 10, RVA_MARK_STATE },
        { 0x15a55b, "\xc7\x05", 2, 10, RVA_STACK_TOO_SMALL },
        { 0x1617d9, "\x48\x8d\x15", 3, 7, RVA_MODWS_VALID },
        { 0x161806, "\x48\x8b\x05", 3, 7, RVA_OLD_NORMAL_BL },
        { 0x161825, "\x48\x8b\x05", 3, 7, RVA_INCOMPLETE_NORMAL_BL },
        { 0x16186b, "\x48\x8b\x05", 3, 7, RVA_OLD_STACK_BL },
        { 0x16188a, "\x48\x8b\x05", 3, 7, RVA_INCOMPLETE_STACK_BL },
        { 0x15871d, "\x48\x8d\x0d", 3, 7, RVA_ALLOC_ML },
    };
    for (const Site& s : sites) {
        if (memcmp(gMono + s.rva, s.bytes, strlen(s.bytes)) != 0 || RipTarget(s.rva, s.dispAt, s.len) != s.target) {
            sprintf_s(gStatus, "mono.dll differs at %x", s.rva);
            return false;
        }
    }
    // Constants inlined in GC_mark_from: top index offset/mask, bottom index key/link, hdr fields, mark proc table,
    // PER_OBJECT bias, the map's invalid / too-big codes, block word count.
    struct Bytes { uint32_t rva; const char* bytes; size_t n; };
    static const Bytes code[] = {
        { 0x15a598, "\x48\x89\x5c\x24\x10\x55\x56\x57\x41\x54\x41\x55\x41\x56", 14 },  // stolen prologue
        { 0x15a785, "\x49\xc1\xe8\x16\x49\x8b\xc0\x25\xff\x07\x00\x00\x4d\x8b\x8c\xc2\xf0\x14\x04\x00", 20 },
        { 0x15a7a4, "\x4d\x8b\x89\x18\x20\x00\x00\x4d\x39\x81\x10\x20\x00\x00", 14 },
        { 0x15a7f9, "\x49\x8b\x41\x20", 4 },
        { 0x15a812, "\x81\xfa\xfd\x00\x00\x00", 6 },
        { 0x15a833, "\x48\x3d\x00\x02\x00\x00", 6 },
        { 0x15a871, "\x4d\x8b\x54\xd1\x30", 5 },
        { 0x15a883, "\x4d\x8b\x61\x18", 4 },
        { 0x15a717, "\x41\xff\x94\xc2\x88\x00\x00\x00", 8 },
        { 0x15a658, "\x4a\x8b\x5c\x33\xfd", 5 },
        { 0x15a66e, "\x48\x8b\x58\xf3", 4 },
        { 0x15a4e9, "\xf6\x40\x29\x01", 4 },
    };
    for (const Bytes& b : code) {
        if (memcmp(gMono + b.rva, b.bytes, b.n) != 0) { sprintf_s(gStatus, "mono.dll code differs at %x", b.rva); return false; }
    }
    return true;
}

static void Bind() {
    word* arrays = (word*)(gMono + RVA_ARRAYS);
    gTopIndex = (Bi**)((uint8_t*)arrays + OFS_TOP_INDEX);
    gMarkProcs = (MarkProcFn*)((uint8_t*)arrays + OFS_MARK_PROCS);
    gModwsValid = gMono + RVA_MODWS_VALID;
    gMarkState = (int*)(gMono + RVA_MARK_STATE);
    gStackTooSmall = (int*)(gMono + RVA_STACK_TOO_SMALL);
    gObjectsAreMarked = (int*)(gMono + RVA_OBJECTS_ARE_MARKED);
}

// Per collection-call snapshot of globals that are fixed while marking.
static void ReadGlobals() {
    gAllNils = *(Bi**)(gMono + RVA_ALL_NILS_PTR);
    gLeast = *(word*)(gMono + RVA_LEAST_HA);
    gGreatest = *(word*)(gMono + RVA_GREATEST_HA);
    gAllInterior = *(int*)(gMono + RVA_ALL_INTERIOR) != 0;
    gOldNormalBl = *(volatile LONG64**)(gMono + RVA_OLD_NORMAL_BL);
    gIncNormalBl = *(volatile LONG64**)(gMono + RVA_INCOMPLETE_NORMAL_BL);
    gOldStackBl = *(volatile LONG64**)(gMono + RVA_OLD_STACK_BL);
    gIncStackBl = *(volatile LONG64**)(gMono + RVA_INCOMPLETE_STACK_BL);
}

// GC_find_header: the bottom-index entry for p (0, a forwarding count <= 0xfff, or a header).
static inline Hdr* FindHeader(word p) {
    word hi = p >> 22;
    Bi* bi = gTopIndex[hi & 0x7ff];
    while (bi->key != hi) {
        if (bi == gAllNils) break;
        bi = bi->hashLink;
    }
    return bi->index[(p >> 12) & 0x3ff];
}
static inline bool IsForwardingOrNil(Hdr* h) { return (word)h <= 0xfff; }  // all 64 bits (Boehm compares 32)

static inline void BlackList(word p) {
    word idx = (p >> 12) & 0xfffff;
    volatile LONG64 *oldBl, *incBl;
    if (gAllInterior) { oldBl = gOldStackBl; incBl = gIncStackBl; }
    else {
        if (!gModwsValid[p & 7]) return;
        oldBl = gOldNormalBl; incBl = gIncNormalBl;
    }
    if (FindHeader(p) == 0 || ((oldBl[idx >> 6] >> (idx & 63)) & 1)) {
        LONG64 bit = (LONG64)1 << (idx & 63);
        if (!(incBl[idx >> 6] & bit)) _InterlockedOr64(&incBl[idx >> 6], bit);
    }
}

// ---- workers -----------------------------------------------------------------------------------------------------
struct Stack {  // local mark stack: reserved address space, committed as it grows; entries in [bot, top)
    Mse* base; Mse* bot; Mse* top; Mse* committed; Mse* end;
};
static const size_t kStackReserve = (size_t)1 << 29;   // 512 MB = 32M entries each
static const size_t kCommitStep = (size_t)1 << 20;

static bool StackInit(Stack& s, size_t reserve) {
    s.base = (Mse*)VirtualAlloc(NULL, reserve, MEM_RESERVE, PAGE_READWRITE);
    if (!s.base) return false;
    s.bot = s.top = s.committed = s.base;
    s.end = (Mse*)((uint8_t*)s.base + reserve);
    return true;
}
static bool StackGrow(Stack& s, size_t n) {  // room for n more entries at top
    if (s.top + n <= s.committed) return true;
    if (s.bot > s.base) {  // compact first
        size_t used = s.top - s.bot;
        memmove(s.base, s.bot, used * sizeof(Mse));
        s.bot = s.base; s.top = s.base + used;
        if (s.top + n <= s.committed) return true;
    }
    size_t need = (uint8_t*)(s.top + n) - (uint8_t*)s.committed;
    size_t step = (need + kCommitStep - 1) / kCommitStep * kCommitStep;
    if ((uint8_t*)s.committed + step > (uint8_t*)s.end) return false;
    if (!VirtualAlloc(s.committed, step, MEM_COMMIT, PAGE_READWRITE)) return false;
    s.committed = (Mse*)((uint8_t*)s.committed + step);
    return true;
}

enum { kMaxWorkers = 32 };
struct alignas(64) Worker {
    Stack stack;
    Stack procs;            // GC_DS_PROC entries, run on the calling thread between rounds
    word hcBlock[8]; Hdr* hcHdr[8];
    uint64_t entries, words;
    bool overflow, fault;
    HANDLE wake;
    int index;
};
static Worker gW[kMaxWorkers];
static int gThreads = 1;        // participants including the calling thread
static int gHelpersStarted;

// Shared pool (entries handed between workers), guarded by gPoolLock; gBusy = workers holding work.
static SRWLOCK gPoolLock = SRWLOCK_INIT;
static Stack gPool;
static volatile LONG gPoolCount, gBusy, gHungry, gDone, gFinished, gRoundHelpers, gAbort;
static const int kChunk = 128;

static inline void Push(Worker& w, word start, word descr) {
    Stack& s = w.stack;
    if (s.top >= s.committed && !StackGrow(s, 1)) { w.overflow = true; return; }  // dropped: Boehm rescans
    s.top->start = start; s.top->descr = descr; s.top++;
}

// HC_PUSH_CONTENTS for a candidate already inside [least, greatest).
static inline void MarkCandidate(Worker& w, word p) {
    word blk = p >> 12;
    int ci = (int)(blk & 7);
    Hdr* h;
    if (w.hcBlock[ci] == blk) h = w.hcHdr[ci];
    else {
        h = FindHeader(p);
        if (IsForwardingOrNil(h)) {
            // GC_find_start
            if (!gAllInterior || h == 0) { BlackList(p); return; }
            word q = p & ~(word)0xfff;
            do {
                q -= (word)h << 12;
                h = FindHeader(q);
            } while (IsForwardingOrNil(h) && h != 0);
            if (h == 0 || (h->flags & 1) || (word)((p - q) >> 3) >= h->sz) { BlackList(p); return; }
            p = q;
        } else {
            w.hcBlock[ci] = blk; w.hcHdr[ci] = h;
        }
    }
    unsigned byteDispl = (unsigned)(p & 0xfff);
    int displ = (int)(byteDispl >> 3);
    unsigned me = h->map[byteDispl];
    if (me > 0xfd) {
        if (me != 0xfe) { BlackList(p); return; }
        word sz = h->sz;
        displ -= (int)((word)displ % sz);
        if ((word)displ + sz > 0x200) { BlackList(p); return; }
    } else displ -= (int)me;
    volatile LONG64* mw = &h->marks[displ >> 6];
    LONG64 bit = (LONG64)1 << (displ & 63);
    if (*mw & bit) return;
    if (_interlockedbittestandset64(mw, displ & 63)) return;
    word d = h->descr;
    if (d) Push(w, (p & ~(word)0xfff) + (word)displ * 8, d);
}

static inline void ScanRange(Worker& w, word* cur, word* last) {  // inclusive
    word least = gLeast, greatest = gGreatest;
    w.words += (uint64_t)(last - cur + 1);
    for (; cur <= last; cur++) {
        word v = *cur;
        if (v >= least && v < greatest) MarkCandidate(w, v);
    }
}

// One mark stack entry (the body of GC_mark_from's loop).
static void Process(Worker& w, word start, word descr) {
    for (;;) {
        if (descr & ~(word)0x3fc) {  // (~(1024 - 1)) | GC_DS_TAGS: not a small length
            switch (descr & 3) {
            case 0: {  // GC_DS_LENGTH, large: 127 words now, the rest back on the stack
                Push(w, start + 0x3f8, descr - 0x3f8);
                ScanRange(w, (word*)start, (word*)(start + 0x3f0));
                return;
            }
            case 1: {  // GC_DS_BITMAP
                word* cur = (word*)start;
                word least = gLeast, greatest = gGreatest;
                for (descr &= ~(word)3; descr; descr <<= 1, cur++) {
                    if ((intptr_t)descr < 0) {
                        word v = *cur;
                        if (v >= least && v < greatest) MarkCandidate(w, v);
                    }
                }
                return;
            }
            case 2: {  // GC_DS_PROC: mono.dll's procedure, later on the calling thread
                Stack& s = w.procs;
                if (s.top >= s.committed && !StackGrow(s, 1)) { w.overflow = true; return; }
                s.top->start = start; s.top->descr = descr; s.top++;
                return;
            }
            default: {  // GC_DS_PER_OBJECT
                if ((intptr_t)descr >= 0) descr = *(word*)(start + descr - 3);
                else {
                    word td = *(word*)start;
                    if (!td) return;
                    descr = *(word*)(td - descr - 0xd);
                }
                if (!descr) return;
                continue;
            }
            }
        }
        if (descr == 0) return;
        ScanRange(w, (word*)start, (word*)(start + descr - 8));
        return;
    }
}

static void Donate(Worker& w) {  // the oldest half (up to kChunk) of the local stack to the pool
    Stack& s = w.stack;
    size_t have = s.top - s.bot;
    size_t n = have / 2; if (n > kChunk) n = kChunk;
    if (n == 0) return;
    AcquireSRWLockExclusive(&gPoolLock);
    if (gPool.top + n > gPool.committed && !StackGrow(gPool, n)) { ReleaseSRWLockExclusive(&gPoolLock); return; }
    memcpy(gPool.top, s.bot, n * sizeof(Mse));
    gPool.top += n;
    gPoolCount = (LONG)(gPool.top - gPool.bot);
    ReleaseSRWLockExclusive(&gPoolLock);
    s.bot += n;
    if (s.bot == s.top) s.bot = s.top = s.base;
}

static bool Take(Worker& w, bool& busy) {  // false = round finished
    for (;;) {
        AcquireSRWLockExclusive(&gPoolLock);
        size_t have = gPool.top - gPool.bot;
        if (have) {
            size_t n = have < (size_t)kChunk ? have : (size_t)kChunk;
            gPool.top -= n;
            if (StackGrow(w.stack, n)) {
                memcpy(w.stack.top, gPool.top, n * sizeof(Mse));
                w.stack.top += n;
            } else w.overflow = true;  // dropped (already marked): Boehm rescans
            if (gPool.top == gPool.bot) gPool.top = gPool.bot = gPool.base;
            gPoolCount = (LONG)(gPool.top - gPool.bot);
            if (!busy) { busy = true; gBusy++; }
            ReleaseSRWLockExclusive(&gPoolLock);
            return true;
        }
        if (busy) { busy = false; gBusy--; }
        if (gBusy == 0 || gAbort) { gDone = 1; ReleaseSRWLockExclusive(&gPoolLock); return false; }
        ReleaseSRWLockExclusive(&gPoolLock);
        _InterlockedIncrement(&gHungry);
        while (!gDone && gPoolCount == 0) _mm_pause();
        _InterlockedDecrement(&gHungry);
        if (gDone) return false;
    }
}

static void RunWorker(Worker& w, bool seeded) {
    memset(w.hcBlock, 0xff, sizeof(w.hcBlock));
    bool busy = seeded;  // the calling thread starts with the entries (counted in gBusy by the caller)
    __try {
        for (;;) {
            Stack& s = w.stack;
            uint32_t n = 0;
            while (s.top > s.bot) {
                s.top--;
                word start = s.top->start, descr = s.top->descr;
                if (s.top == s.bot) s.top = s.bot = s.base;
                Process(w, start, descr);
                w.entries++;
                if ((++n & 31) == 0 && gHungry && (size_t)(s.top - s.bot) > 4) Donate(w);
                if (gAbort) break;
            }
            if (!Take(w, busy)) break;
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        w.fault = true;
        _InterlockedExchange(&gAbort, 1);
        AcquireSRWLockExclusive(&gPoolLock);
        if (busy) gBusy--;
        gDone = 1;
        ReleaseSRWLockExclusive(&gPoolLock);
    }
    w.stack.bot = w.stack.top = w.stack.base;
}

static DWORD WINAPI Helper(void* arg) {
    Worker& w = *(Worker*)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        WaitForSingleObject(w.wake, INFINITE);
        RunWorker(w, false);
        _InterlockedIncrement(&gFinished);
    }
}

// ---- MarkFrom (replaces GC_mark_from) ----------------------------------------------------------------------------
struct Stats { uint64_t calls, parallelRounds, entriesIn, entries, words, procs, overflows, faults, ticks; } gStats;
static LARGE_INTEGER gFreq;
static const size_t kWakeThreshold = 256;  // local entries before helpers are woken

// Runs the calling thread alone until it holds enough work, then wakes the helpers for the rest of the round.
static void Round(Worker& m) {
    memset(m.hcBlock, 0xff, sizeof(m.hcBlock));
    bool helpersOn = false;
    __try {
        Stack& s = m.stack;
        while (s.top > s.bot) {
            s.top--;
            word start = s.top->start, descr = s.top->descr;
            if (s.top == s.bot) s.top = s.bot = s.base;
            Process(m, start, descr);
            m.entries++;
            if (gThreads > 1 && (size_t)(s.top - s.bot) >= kWakeThreshold) { helpersOn = true; break; }
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        m.fault = true;
        m.stack.bot = m.stack.top = m.stack.base;
    }
    if (!helpersOn) return;
    gStats.parallelRounds++;
    gDone = 0; gAbort = 0; gHungry = 0; gFinished = 0;
    gBusy = 1;  // the calling thread
    gPool.bot = gPool.top = gPool.base; gPoolCount = 0;
    int helpers = gThreads - 1 < gHelpersStarted ? gThreads - 1 : gHelpersStarted;
    gRoundHelpers = helpers;
    for (int i = 1; i <= helpers; i++) SetEvent(gW[i].wake);
    RunWorker(m, true);
    while (gFinished < helpers) _mm_pause();
}

static Mse* MarkFromImpl(Mse* top, Mse* bottom) {
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    *gObjectsAreMarked = 1;
    ReadGlobals();
    gStats.calls++;
    Worker& m = gW[0];
    size_t n = top >= bottom ? (size_t)(top - bottom + 1) : 0;
    gStats.entriesIn += n;
    for (int i = 0; i < gThreads; i++) { gW[i].entries = gW[i].words = 0; gW[i].overflow = gW[i].fault = false; }
    if (n && StackGrow(m.stack, n)) { memcpy(m.stack.top, bottom, n * sizeof(Mse)); m.stack.top += n; }
    else if (n) m.overflow = true;
    for (;;) {
        Round(m);
        // mark procedures, on this thread (mono.dll's code, non-atomic), with the helpers idle
        bool any = false, fault = false;
        for (int i = 0; i < gThreads; i++) fault |= gW[i].fault;
        if (fault) break;
        for (int i = 0; i < gThreads; i++) {
            Stack& p = gW[i].procs;
            while (p.top > p.bot) {
                p.top--;
                Mse e = *p.top;
                gStats.procs++;
                if (!StackGrow(m.stack, 65536)) { m.overflow = true; continue; }
                Mse* msp = m.stack.top - 1;
                Mse* msl = m.stack.top + 65536;
                msp = gMarkProcs[(e.descr >> 2) & 0x3f]((word*)e.start, msp, msl, e.descr >> 8);
                m.stack.top = msp + 1;
                any = true;
            }
            p.bot = p.top = p.base;
        }
        if (!any || m.stack.top == m.stack.bot) break;
    }
    bool overflow = false, fault = false;
    for (int i = 0; i < gThreads; i++) {
        overflow |= gW[i].overflow; fault |= gW[i].fault;
        gStats.entries += gW[i].entries; gStats.words += gW[i].words;
    }
    m.stack.bot = m.stack.top = m.stack.base;
    if (overflow) { gStats.overflows++; *gMarkState = 5; *gStackTooSmall = 1; }  // GC_signal_mark_stack_overflow
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    gStats.ticks += t1.QuadPart - t0.QuadPart;
    if (fault) {
        gStats.faults++;
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);  // GC_mark_some's handler restarts marking
    }
    return bottom - 1;
}

// ---- verify mode -------------------------------------------------------------------------------------------------
static MarkFromFn gOriginal;     // trampoline: the stolen prologue + jmp back
static int gMode;                // 0 = Boehm's marker, 1 = parallel, 2 = verify
struct Verify { uint64_t calls, tiny, compared, mismatchWords, blMismatchWords, serialOverflows, ourOverflows, stateMismatch; char first[256]; } gVerify;
static Hdr** gHdrList; static size_t gHdrCount, gHdrCap;
static LONG64* gSnapP; static LONG64* gSnapA; static LONG64* gSnapB;
static LONG64* gBlP; static LONG64* gBlA; static LONG64* gBlB;
static const size_t kBlWords = (1 << 20) / 64;

static void ListHeaders() {
    gHdrCount = 0;
    for (int t = 0; t < 2048; t++) {
        for (Bi* bi = gTopIndex[t]; bi && bi != gAllNils; bi = bi->hashLink) {
            for (int j = 0; j < 1024; j++) {
                Hdr* h = bi->index[j];
                if ((word)h > 0xfff && gHdrCount < gHdrCap) gHdrList[gHdrCount++] = h;
            }
        }
    }
}
static void SnapMarks(LONG64* dst, LONG64* bl) {
    for (size_t i = 0; i < gHdrCount; i++) memcpy(dst + i * 8, (const void*)gHdrList[i]->marks, 64);
    memcpy(bl, (const void*)gIncNormalBl, kBlWords * 8);
    memcpy(bl + kBlWords, (const void*)gIncStackBl, kBlWords * 8);
}
static void RestoreMarks(const LONG64* src, const LONG64* bl) {
    for (size_t i = 0; i < gHdrCount; i++) memcpy((void*)gHdrList[i]->marks, src + i * 8, 64);
    memcpy((void*)gIncNormalBl, bl, kBlWords * 8);
    memcpy((void*)gIncStackBl, bl + kBlWords, kBlWords * 8);
}

// Boehm's marker is run on a private 16M-entry stack (its own stack is small and overflows on a heavy heap, after which
// Boehm rescans block by block with a mark call per block; snapshots on each of those never finish). At most
// kComparePerGc calls of >= 8 entries are compared per collection; the rest run ours unchecked (same code).
static Mse* gSerial; static const size_t kSerialEntries = (size_t)1 << 24;
static int* gGcNo; static int gLastGc = -1, gComparedThisGc;
static const int kComparePerGc = 3;

static Mse* VerifyMarkFrom(Mse* top, Mse* bottom, Mse* limit) {
    ReadGlobals();
    gVerify.calls++;
    size_t n = top >= bottom ? (size_t)(top - bottom + 1) : 0;
    if (gGcNo && *gGcNo != gLastGc) { gLastGc = *gGcNo; gComparedThisGc = 0; }
    if (n < 8 || n > kSerialEntries / 2 || gComparedThisGc >= kComparePerGc || !gSerial) {
        gVerify.tiny++;
        return MarkFromImpl(top, bottom);
    }
    gComparedThisGc++;
    static Mse* saved; static size_t savedCap;
    if (n > savedCap) {
        if (saved) VirtualFree(saved, 0, MEM_RELEASE);
        savedCap = n * 2; saved = (Mse*)VirtualAlloc(NULL, savedCap * sizeof(Mse), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    memcpy(saved, bottom, n * sizeof(Mse));
    ListHeaders();
    if (gHdrCount) {
        size_t bytes = gHdrCount * 64;
        if (!VirtualAlloc(gSnapP, bytes, MEM_COMMIT, PAGE_READWRITE) || !VirtualAlloc(gSnapA, bytes, MEM_COMMIT, PAGE_READWRITE)
            || !VirtualAlloc(gSnapB, bytes, MEM_COMMIT, PAGE_READWRITE)) {
            return MarkFromImpl(top, bottom);
        }
    }
    // GC_mark_stack_too_small is cleared around each run, so an overflow in this call shows even if one happened before.
    int state0 = *gMarkState, small0 = *gStackTooSmall;
    SnapMarks(gSnapP, gBlP);
    *gStackTooSmall = 0;
    memcpy(gSerial, saved, n * sizeof(Mse));
    Mse* t = gSerial + n - 1;
    while (t >= gSerial) t = gOriginal(t, gSerial, gSerial + kSerialEntries);
    t = bottom - 1;
    int stateA = *gMarkState, overflowA = *gStackTooSmall;
    SnapMarks(gSnapA, gBlA);
    if (overflowA) {  // Boehm's own stack overflowed: dropped entries, not comparable; keep its result
        gVerify.serialOverflows++;
        *gStackTooSmall = 1;
        return t;
    }
    RestoreMarks(gSnapP, gBlP);
    *gMarkState = state0;
    memcpy(bottom, saved, n * sizeof(Mse));
    MarkFromImpl(bottom + n - 1, bottom);
    int overflowB = *gStackTooSmall;
    *gStackTooSmall = small0;
    int smallA = small0;
    SnapMarks(gSnapB, gBlB);
    if (overflowB) gVerify.ourOverflows++;
    gVerify.compared++;
    uint64_t diff = 0, bldiff = 0;
    for (size_t i = 0; i < gHdrCount * 8; i++) {
        if (gSnapA[i] != gSnapB[i]) {
            if (!diff) sprintf_s(gVerify.first, "hdr %p word %zu: boehm %llx ours %llx", gHdrList[i / 8], i % 8,
                                 (unsigned long long)gSnapA[i], (unsigned long long)gSnapB[i]);
            diff++;
        }
    }
    for (size_t i = 0; i < 2 * kBlWords; i++) bldiff += gBlA[i] != gBlB[i];
    gVerify.mismatchWords += diff; gVerify.blMismatchWords += bldiff;
    if (*gMarkState != stateA || overflowB) gVerify.stateMismatch++;
    if (diff || bldiff || *gMarkState != stateA || overflowB) {
        RestoreMarks(gSnapA, gBlA);
        *gMarkState = stateA; *gStackTooSmall = smallA;
    }
    return bottom - 1;
}

extern "C" Mse* MarkFromHook(Mse* top, Mse* bottom, Mse* limit) {
    if (gMode == 2) return VerifyMarkFrom(top, bottom, limit);
    return MarkFromImpl(top, bottom);
}

// ---- install -----------------------------------------------------------------------------------------------------
static uint8_t gStolen[14];
static uint8_t* gTrampoline;

static bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

extern "C" __declspec(dllexport) int GT_Init() {
    if (gMono) return 1;
    uint8_t* m = (uint8_t*)GetModuleHandleW(L"mono.dll");
    if (!m) { strcpy_s(gStatus, "mono.dll not loaded"); return -1; }
    gMono = m;
    if (!CheckLayout()) { gMono = NULL; return -2; }
    Bind();
    QueryPerformanceFrequency(&gFreq);
    memcpy(gStolen, gMono + RVA_MARK_FROM, 14);
    gTrampoline = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(gTrampoline, gStolen, 14);
    uint8_t jmp[14] = { 0xff, 0x25, 0, 0, 0, 0 };
    word back = (word)(gMono + RVA_MARK_FROM + 14);
    memcpy(jmp + 6, &back, 8);
    memcpy(gTrampoline + 14, jmp, 14);
    gOriginal = (MarkFromFn)gTrampoline;
    strcpy_s(gStatus, "ok");
    return 1;
}

// threads: participants including the collecting thread (1 = our marker on that thread alone, for comparison).
// mode: 0 = Boehm's own marker, 1 = parallel, 2 = verify.
extern "C" __declspec(dllexport) int GT_SetMark(int threads, int mode) {
    if (!gMono) return -1;
    if (threads < 1) threads = 1;
    if (threads > kMaxWorkers) threads = kMaxWorkers;
    if (!gPool.base && !StackInit(gPool, kStackReserve)) return -3;
    for (int i = 0; i < threads; i++) {
        Worker& w = gW[i];
        if (!w.stack.base) {
            if (!StackInit(w.stack, kStackReserve) || !StackInit(w.procs, (size_t)1 << 26)) return -3;
            w.index = i;
        }
        if (i > 0 && !w.wake) {
            w.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
            HANDLE h = CreateThread(NULL, 256 * 1024, Helper, &w, 0, NULL);
            if (!h) return -4;
            CloseHandle(h);
            gHelpersStarted = i;
        }
    }
    if (mode == 2 && !gHdrList) {
        gHdrCap = (size_t)1 << 23;  // 8M headers = 32 GB of small-object heap; snapshots committed as needed
        gHdrList = (Hdr**)VirtualAlloc(NULL, gHdrCap * sizeof(Hdr*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        gSnapP = (LONG64*)VirtualAlloc(NULL, gHdrCap * 64, MEM_RESERVE, PAGE_READWRITE);
        gSnapA = (LONG64*)VirtualAlloc(NULL, gHdrCap * 64, MEM_RESERVE, PAGE_READWRITE);
        gSnapB = (LONG64*)VirtualAlloc(NULL, gHdrCap * 64, MEM_RESERVE, PAGE_READWRITE);
        gBlP = (LONG64*)VirtualAlloc(NULL, 6 * kBlWords * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        gSerial = (Mse*)VirtualAlloc(NULL, kSerialEntries * sizeof(Mse), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        // GC_gc_no, as mono_gc_collection_count reads it (8B 05 disp32)
        uint8_t* cc = (uint8_t*)GetProcAddress((HMODULE)gMono, "mono_gc_collection_count");
        if (cc && cc[0] == 0x8b && cc[1] == 0x05) gGcNo = (int*)(cc + 6 + *(int32_t*)(cc + 2));
        gBlA = gBlP + 2 * kBlWords; gBlB = gBlA + 2 * kBlWords;
    }
    // Swap the entry of GC_mark_from while holding the collector's lock (no collection can be running).
    CRITICAL_SECTION* lock = (CRITICAL_SECTION*)(gMono + RVA_ALLOC_ML);
    EnterCriticalSection(lock);
    gThreads = threads;
    gMode = mode;
    bool ok;
    if (mode == 0) ok = WriteCode(gMono + RVA_MARK_FROM, gStolen, 14);
    else {
        uint8_t jmp[14] = { 0xff, 0x25, 0, 0, 0, 0 };
        word to = (word)&MarkFromHook;
        memcpy(jmp + 6, &to, 8);
        ok = WriteCode(gMono + RVA_MARK_FROM, jmp, 14);
    }
    LeaveCriticalSection(lock);
    return ok ? 1 : -5;
}

// ---- 3. collecting for finalizable objects (GC_collect_or_expand) ----------------------------------------------
// An allocation that finds no free block calls GC_collect_or_expand (0x1587ac), which collects instead of growing the
// heap when (alloc.c:1105)
//     GC_dont_expand && GC_words_allocd > 0
//  || GC_fo_entries > last_fo_entries + 500 && (last_words_finalized || GC_words_finalized)
//  || GC_should_collect(),
// last_fo_entries / last_words_finalized being taken after its own previous collection. GC_fo_entries is the number
// of objects registered for finalization (Unity's TextGenerator, Coroutine, ... ), so while a load creates them
// faster than 500 per heap expansion and a heap of several GB is tight, every 500 new ones cost a full collection.
// bdwgc 8 (alloc.c, GC_collect_or_expand) replaced the fixed 500 with the allocation since the last collection:
//     GC_fo_entries > last_fo_entries && (last_bytes_finalized | GC_bytes_finalized) != 0
//  && (GC_fo_entries - last_fo_entries) * GC_allocd_bytes_per_finalizer > GC_bytes_allocd,
// GC_allocd_bytes_per_finalizer 10000 by default. GT_SetFinalizerRule(n > 0) puts that test (with n) in place of
// the middle one: the 42 bytes at 0x1587dd become a jump to a stub that tests it on the same globals and jumps back
// to the collection (0x158810) or on to the GC_should_collect test (0x158807); the stub keeps every register but
// rax (the original sequence also only uses rax). n = 0 puts mono's bytes back.
enum : uint32_t {
    RVA_FO_RULE = 0x1587dd,           // mov rax,[last_fo_entries]; add rax,500; cmp [GC_fo_entries],rax; jbe; ... (42 bytes)
    RVA_SHOULD_COLLECT_TEST = 0x158807,   // call GC_should_collect; test eax,eax; je expand
    RVA_COLLECT = 0x158810,           // lea rcx,GC_never_stop_func; call GC_try_to_collect_inner
    RVA_FO_ENTRIES = 0x269ce0, RVA_LAST_FO_ENTRIES = 0x269be8, RVA_LAST_WORDS_FINALIZED = 0x269bf0,
    RVA_WORDS_FINALIZED = 0x27c838, RVA_WORDS_ALLOCD = 0x27c828,
};
static const uint8_t kFoRule[42] = {
    0x48, 0x8b, 0x05, 0x04, 0x14, 0x11, 0x00,         // mov rax, [last_fo_entries]
    0x48, 0x05, 0xf4, 0x01, 0x00, 0x00,               // add rax, 500
    0x48, 0x39, 0x05, 0xef, 0x14, 0x11, 0x00,         // cmp [GC_fo_entries], rax
    0x76, 0x14,                                       // jbe 0x158807
    0x48, 0x83, 0x3d, 0xf5, 0x13, 0x11, 0x00, 0x00,   // cmp [last_words_finalized], 0
    0x75, 0x13,                                       // jne 0x158810
    0x48, 0x83, 0x3d, 0x33, 0x40, 0x12, 0x00, 0x00,   // cmp [GC_words_finalized], 0
    0x75, 0x09,                                       // jne 0x158810
};
static uint8_t* gFoStub;
static uint8_t* gFoImm;               // the imul's imm32 in the stub
static int gBytesPerFinalizer;        // 0 = mono's own test

static uint8_t* Put(uint8_t* p, const uint8_t* b, size_t n) { memcpy(p, b, n); return p + n; }
static uint8_t* MovRcx(uint8_t* p, const void* a) { *p++ = 0x48; *p++ = 0xb9; memcpy(p, &a, 8); return p + 8; }
static uint8_t* JmpAbs(uint8_t* p, const void* to) {
    static const uint8_t j[6] = { 0xff, 0x25, 0, 0, 0, 0 };
    p = Put(p, j, 6); memcpy(p, &to, 8); return p + 8;
}

static uint8_t* BuildFoStub(uint8_t* mono) {
    uint8_t* s = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s) return NULL;
    uint8_t* p = s;
    uint8_t* jumps[3];
    *p++ = 0x51;                                                          // push rcx
    p = MovRcx(p, mono + RVA_FO_ENTRIES);      p = Put(p, (const uint8_t*)"\x48\x8b\x01", 3);   // mov rax, [rcx]
    p = MovRcx(p, mono + RVA_LAST_FO_ENTRIES); p = Put(p, (const uint8_t*)"\x48\x2b\x01", 3);   // sub rax, [rcx]
    *p++ = 0x76; jumps[0] = p++;                                          // jbe no (fo <= last)
    p = Put(p, (const uint8_t*)"\x48\x69\xc0", 3); gFoImm = p; p += 4;    // imul rax, rax, n
    p = MovRcx(p, mono + RVA_WORDS_ALLOCD);    p = Put(p, (const uint8_t*)"\x48\x8b\x09", 3);   // mov rcx, [rcx]
    p = Put(p, (const uint8_t*)"\x48\xc1\xe1\x03", 4);                    // shl rcx, 3 (bytes)
    p = Put(p, (const uint8_t*)"\x48\x39\xc8", 3);                        // cmp rax, rcx
    *p++ = 0x76; jumps[1] = p++;                                          // jbe no
    p = MovRcx(p, mono + RVA_LAST_WORDS_FINALIZED); p = Put(p, (const uint8_t*)"\x48\x8b\x01", 3);  // mov rax, [rcx]
    p = MovRcx(p, mono + RVA_WORDS_FINALIZED);      p = Put(p, (const uint8_t*)"\x48\x0b\x01", 3);  // or rax, [rcx]
    *p++ = 0x74; jumps[2] = p++;                                          // jz no
    *p++ = 0x59;                                                          // pop rcx
    p = JmpAbs(p, mono + RVA_COLLECT);
    for (uint8_t* j : jumps) *j = (uint8_t)(p - (j + 1));
    *p++ = 0x59;                                                          // no: pop rcx
    p = JmpAbs(p, mono + RVA_SHOULD_COLLECT_TEST);
    memset(gFoImm, 0, 4);
    return s;
}

// n > 0: bdwgc 8's test with GC_allocd_bytes_per_finalizer = n; 0: mono's own. Returns n, or < 0 (mono's bytes left).
extern "C" __declspec(dllexport) int GT_SetFinalizerRule(int n) {
    if (!gMono) return -1;
    if (n < 0) return -2;
    uint8_t* at = gMono + RVA_FO_RULE;
    bool stock = memcmp(at, kFoRule, sizeof kFoRule) == 0;
    bool ours = gFoStub && at[0] == 0xff && at[1] == 0x25 && *(uint32_t*)(at + 2) == 0 && *(uint8_t**)(at + 6) == gFoStub;
    if (!stock && !ours) { strcpy_s(gStatus, "mono.dll code differs at 1587dd"); return -3; }
    if (n > 0 && !gFoStub && !(gFoStub = BuildFoStub(gMono))) return -4;
    // While holding the collector's lock no thread is inside GC_collect_or_expand.
    CRITICAL_SECTION* lock = (CRITICAL_SECTION*)(gMono + RVA_ALLOC_ML);
    EnterCriticalSection(lock);
    bool ok = true;
    if (n == 0) { if (ours) ok = WriteCode(at, kFoRule, sizeof kFoRule); }
    else {
        memcpy(gFoImm, &n, 4);
        FlushInstructionCache(GetCurrentProcess(), gFoImm, 4);
        if (stock) {
            uint8_t code[sizeof kFoRule];
            memset(code, 0xcc, sizeof code);                      // the rest is never reached
            JmpAbs(code, gFoStub);
            ok = WriteCode(at, code, sizeof code);
        }
    }
    if (ok) gBytesPerFinalizer = n;
    LeaveCriticalSection(lock);
    return ok ? n : -5;
}

// Offline test (tools\fo_test.cpp): the same on an image copy whose globals the test sets.
extern "C" __declspec(dllexport) int GT_TestFinalizerRule(uint8_t* base, int n) {
    uint8_t* saved = gMono;
    gMono = base;
    int r = GT_SetFinalizerRule(n);
    gMono = saved;
    return r;
}

// ---- 4 GB boundaries: mono.dll's VirtualAlloc import ------------------------------------------------------------
typedef LPVOID (WINAPI* VirtualAllocFn)(LPVOID, SIZE_T, DWORD, DWORD);
static VirtualAllocFn gRealVirtualAlloc;
static volatile LONG gBoundaryHits, gBoundaryPlugs;
static const word kFourGb = (word)1 << 32;

// First 4 GB boundary (>= 4 GB) inside [p, p + n), or 0.
static word BoundaryIn(word p, SIZE_T n) {
    word b = (p + kFourGb - 1) & ~(kFourGb - 1);
    if (b < kFourGb) b = kFourGb;
    return b < p + n ? b : 0;
}

static LPVOID WINAPI GuardedVirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
    LPVOID r = gRealVirtualAlloc(addr, size, type, protect);
    if (!r || addr || !(type & MEM_RESERVE)) return r;
    for (int tries = 0; tries < 8; tries++) {
        word b = BoundaryIn((word)r, size);
        if (!b) return r;
        InterlockedIncrement(&gBoundaryHits);
        VirtualFree(r, 0, MEM_RELEASE);
        for (; b < (word)r + size; b += kFourGb)
            if (gRealVirtualAlloc((void*)b, 65536, MEM_RESERVE, PAGE_NOACCESS)) InterlockedIncrement(&gBoundaryPlugs);
        r = gRealVirtualAlloc(NULL, size, type, protect);
        if (!r) return r;
    }
    return r;  // a region larger than 4 GB always covers one; same as without the guard
}

// The IAT slot mono.dll calls VirtualAlloc through (GC_win32_get_mem: call [rip+...] at 0x15ea86).
static void** FindImport(uint8_t* base, const char* dll, const char* fn) {
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char*)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA64* names = (IMAGE_THUNK_DATA64*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        void** slots = (void**)(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; i++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)) continue;
            if (!strcmp(((IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData))->Name, fn)) return &slots[i];
        }
    }
    return NULL;
}

// Wraps mono.dll's VirtualAlloc import (see 2. at the top). Returns 1, or < 0 if the import is not the system one.
// *badHeaders = headers already present that the 32-bit check misreads (low 32 bits <= 0xfff).
extern "C" __declspec(dllexport) int GT_GuardBoundaries(int* badHeaders) {
    *badHeaders = 0;
    if (!gMono) return -1;
    void** slot = FindImport(gMono, "KERNEL32.dll", "VirtualAlloc");
    if (!slot) return -2;
    void* cur = *slot;
    if (cur != (void*)&GuardedVirtualAlloc) {
        void* k32 = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VirtualAlloc");
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        void* kbase = kb ? (void*)GetProcAddress(kb, "VirtualAlloc") : NULL;
        if (cur != k32 && cur != kbase) return -3;
        gRealVirtualAlloc = (VirtualAllocFn)cur;
        DWORD old;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return -4;
        InterlockedExchangePointer(slot, (void*)&GuardedVirtualAlloc);
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
    int bad = 0;
    ReadGlobals();
    for (int t = 0; t < 2048; t++)
        for (Bi* bi = gTopIndex[t]; bi && bi != gAllNils; bi = bi->hashLink)
            for (int j = 0; j < 1024; j++) {
                word h = (word)bi->index[j];
                if (h > 0xfff && (h & 0xffffffff) <= 0xfff) bad++;
            }
    *badHeaders = bad;
    return 1;
}

extern "C" __declspec(dllexport) int GT_Status(char* buf, int cap) {
    double ms = gFreq.QuadPart ? gStats.ticks * 1000.0 / gFreq.QuadPart : 0;
    int n = sprintf_s(buf, cap,
        "status %s; mode %d threads %d helpers %d; calls %llu rounds %llu entries_in %llu entries %llu words %llu "
        "procs %llu overflows %llu faults %llu ms %.1f; verify calls %llu small %llu compared %llu mismatch_words %llu "
        "bl_mismatch_words %llu state_mismatch %llu serial_overflows %llu our_overflows %llu headers %zu %s; "
        "boundary guard %s hits %ld plugs %ld; finalizer rule %d",
        gStatus, gMode, gThreads, gHelpersStarted, gStats.calls, gStats.parallelRounds, gStats.entriesIn, gStats.entries,
        gStats.words, gStats.procs, gStats.overflows, gStats.faults, ms, gVerify.calls, gVerify.tiny, gVerify.compared,
        gVerify.mismatchWords, gVerify.blMismatchWords, gVerify.stateMismatch, gVerify.serialOverflows, gVerify.ourOverflows, gHdrCount, gVerify.first,
        gRealVirtualAlloc ? "on" : "off", gBoundaryHits, gBoundaryPlugs, gBytesPerFinalizer);
    return n;
}

extern "C" __declspec(dllexport) void GT_ResetStats() { memset(&gStats, 0, sizeof(gStats)); }

// Offline: the layout checks against a mono.dll image mapped with DONT_RESOLVE_DLL_REFERENCES (nothing hooked).
extern "C" __declspec(dllexport) int GT_CheckImage(uint8_t* base, char* buf, int cap) {
    uint8_t* saved = gMono;
    gMono = base;
    bool ok = CheckLayout();
    gMono = saved;
    return sprintf_s(buf, cap, "%s", ok ? "layout ok" : gStatus) > 0 && ok ? 1 : 0;
}
