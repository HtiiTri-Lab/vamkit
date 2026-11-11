// VaM GC Tuning 1.2.0 (2026-11-05): Mono's collector (Unity 2018.1 legacy Mono, Boehm GC 6.x, mono.dll).
// 1.0.0: the collection threshold. VaM's mono.dll collects once the memory allocated since the last collection
//   reaches (heap in use + large free blocks / 4) / GC_free_space_divisor; each collection stops VaM ~0.95 s on a
//   heavy scene (docs/knowledge/gc.md). Boehm's default divisor 3 collects once about half the live
//   heap has been allocated again; 2 waits for about the live heap (collections about half as often, the heap levels
//   off 2-3 GB higher once); 1 would never collect by allocation, so it is not offered. Written once at startup, at
//   the address min_words_allocd's divide reads (checked first).
// 1.1.0: VaMGcTuningNative.bin (native\gctuning.cpp):
//   - parallel marking: 91% of a collection is GC_mark_from, Boehm's single-threaded marker; it now runs on the
//     collecting thread plus helper threads (same result: every reachable object marked, nothing else);
//   - 4 GB boundaries: this libgc compares block-header pointers with 32 bits (Win64 `unsigned long`), so a header
//     placed right after a 4 GB boundary is misread (endless loop, or live objects left unmarked). The first 64 KB
//     after each boundary are kept reserved so the collector can never put a header there.
// 1.1.1: 1.1.0 reserved those 64 KB after every boundary from 4 GB to 16 TB at startup, which left CUDA no address range
//   longer than 4 GB: DLSS SR / RR / NR / DLSS-G (NvAPI_D3D12_CreateCuModule) all failed, and path tracing with them
//  . Now mono.dll's VirtualAlloc import is wrapped and only a boundary that
//   one of mono's own new regions covers is reserved (then the region is taken again elsewhere).
// 1.2.0: when an allocation finds no free block, mono collects instead of growing the heap if 500 more objects were
//   registered for finalization since that test last collected (Boehm 6.x). A load that creates thousands of them
//   (UI text, coroutines) on a heap of several GB then pays a full collection per 500: the first load of
//   a stage scene of about 400 atoms in a fresh VaM collected 14-56 times (docs/knowledge/gc.md). That test is now the
//   one bdwgc 8 uses: collect only if the new ones are more than one per AllocatedBytesPerFinalizer bytes allocated
//   since the last collection (10000, bdwgc's default).
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using BepInEx;
using BepInEx.Configuration;

[BepInPlugin("local.vam.gctuning", "VaM GC Tuning", "1.2.0")]
public sealed unsafe class VaMGcTuning : BaseUnityPlugin
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandleW(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] private static extern IntPtr GetProcAddress(IntPtr m, string name);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int SetMarkFn(int threads, int mode);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int GuardFn(out int badHeaders);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int IntFn(int n);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int StatusFn(byte[] buf, int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void VoidFn();

    private const int DivideRva = 0x15790e, DivisorRva = 0x265950;

    private static SetMarkFn setMark;
    private static IntFn setRule;
    private static StatusFn status;
    private static VoidFn resetStats;
    private static BepInEx.Logging.ManualLogSource log;

    private void Awake()
    {
        log = Logger;
        ConfigEntry<int> setting = Config.Bind("Mono", "FreeSpaceDivisor", 2, new ConfigDescription(
            "When Mono collects garbage. 3 = VaM's own (after about half the live heap is allocated again); 2 = about " +
            "half as often, the managed heap levels off 2-3 GB larger. Read at startup.", new AcceptableValueList<int>(2, 3)));
        ConfigEntry<int> threads = Config.Bind("Mono", "MarkThreads", 16, new ConfigDescription(
            "Threads that mark live objects during a collection (VaM is stopped meanwhile), the collecting thread " +
            "included. 0 = Mono's own single-threaded marker. Read at startup.", new AcceptableValueRange<int>(0, 32)));
        ConfigEntry<int> perFinalizer = Config.Bind("Mono", "AllocatedBytesPerFinalizer", 10000, new ConfigDescription(
            "When the heap is full, collect instead of growing it because of objects registered for finalization only " +
            "if there are more than one per this many bytes allocated since the last collection (bdwgc 8's test and " +
            "default). 0 = Mono's own test (every 500 new ones). Read at startup.", new AcceptableValueRange<int>(0, 1000000)));
        try
        {
            byte* mono = (byte*)GetModuleHandleW("mono.dll").ToPointer();
            if (mono == null) { Logger.LogWarning("mono.dll not loaded: collector unchanged"); return; }
            byte* ins = mono + DivideRva;
            bool layout = ins[0] == 0x48 && ins[1] == 0xF7 && ins[2] == 0x35 && DivideRva + 7 + *(int*)(ins + 3) == DivisorRva;
            long* divisor = (long*)(mono + DivisorRva);
            if (!layout || (*divisor != 3 && *divisor != setting.Value))
            {
                Logger.LogWarning("mono.dll is not the one this was made for: collector unchanged");
                return;
            }
            long old = *divisor;
            *divisor = setting.Value;
            Logger.LogInfo("Mono GC free space divisor " + old + " -> " + *divisor);
            // The setting is an upper bound: never more marking threads than the machine has logical processors to spare.
            int markThreads = Math.Min(threads.Value, Math.Max(1, Environment.ProcessorCount - 2));
            Native(markThreads, perFinalizer.Value);
        }
        catch (Exception e) { Logger.LogWarning("collector unchanged: " + e.Message); }
    }

    private void Native(int markThreads, int perFinalizer)
    {
        string path = Path.Combine(Path.GetDirectoryName(Info.Location), "VaMGcTuningNative.bin");
        IntPtr m = LoadLibraryW(path);
        if (m == IntPtr.Zero) { Logger.LogWarning("VaMGcTuningNative.bin not loaded"); return; }
        InitFn init = (InitFn)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "GT_Init"), typeof(InitFn));
        GuardFn guard = (GuardFn)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "GT_GuardBoundaries"), typeof(GuardFn));
        setMark = (SetMarkFn)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "GT_SetMark"), typeof(SetMarkFn));
        setRule = (IntFn)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "GT_SetFinalizerRule"), typeof(IntFn));
        status = (StatusFn)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "GT_Status"), typeof(StatusFn));
        resetStats = (VoidFn)Marshal.GetDelegateForFunctionPointer(GetProcAddress(m, "GT_ResetStats"), typeof(VoidFn));
        if (init() != 1) { Logger.LogWarning("collector unchanged: " + Status()); return; }
        int bad;
        int g = guard(out bad);
        Logger.LogInfo(g == 1 ? "4 GB boundaries kept free of block headers: mono's new regions on a boundary are moved, misread headers present " + bad
                              : "4 GB boundary guard not set up (" + g + ")");
        if (markThreads > 0)
        {
            int r = setMark(markThreads, 1);
            Logger.LogInfo(r == 1 ? "Mono GC parallel marking on, " + markThreads + " threads" : "parallel marking not set up (" + r + ")");
        }
        if (perFinalizer > 0)
        {
            int r = setRule(perFinalizer);
            Logger.LogInfo(r == perFinalizer ? "Mono GC collects for finalizable objects only above one per " + perFinalizer + " bytes allocated"
                                             : "finalizer test unchanged (" + r + ")");
        }
    }

    // Test agent hooks.
    public static string Status()
    {
        if (status == null) return "native not loaded";
        byte[] buf = new byte[2048];
        int n = status(buf, buf.Length);
        return Encoding.ASCII.GetString(buf, 0, Math.Max(0, n));
    }

    // mode: 0 = Mono's own marker, 1 = parallel, 2 = verify (Mono's marker, then ours from the same state, compared).
    public static string SetMark(int threads, int mode)
    {
        if (setMark == null) return "native not loaded";
        return "set " + setMark(threads, mode) + "; " + Status();
    }

    // 0 = Mono's own test (500 new finalizable objects), n > 0 = bdwgc 8's with n bytes per finalizer.
    public static string SetFinalizerRule(int n)
    {
        if (setRule == null) return "native not loaded";
        return "set " + setRule(n) + "; " + Status();
    }

    public static string ResetStats() { if (resetStats != null) resetStats(); return Status(); }
}
