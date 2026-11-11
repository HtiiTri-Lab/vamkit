// Runs inside VaM's own Mono (mono_host.exe) and writes the float results of the IL shapes DAZSkinV2.SkinMeshPart
// uses, so float_check.exe can tell which rounding rule this Mono follows. Also compares Math.Sin / Math.Cos with the
// internal-call function pointer Mono hands out for them (mono_lookup_internal_call).
//     mono_host.exe <VaM folder> MonoFloatTest.exe <out file>
using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;

internal static class MonoFloatTest
{
    [DllImport("kernel32", CharSet = CharSet.Ansi)] private static extern IntPtr GetModuleHandleA(string name);
    [DllImport("kernel32", CharSet = CharSet.Ansi)] private static extern IntPtr GetProcAddress(IntPtr module, string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr LookupFn(IntPtr method);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate double MathFn(double x);

    private const int N = 200000;
    private static uint seed = 12345;
    private static float Rnd(float scale)
    {
        seed = seed * 1664525u + 1013904223u;
        uint m = seed >> 9;                       // 23 random mantissa bits
        float f = BitConverter.ToSingle(BitConverter.GetBytes(0x3f800000u | m), 0) - 1.5f;   // [-0.5, 0.5)
        return f * scale;
    }

    private struct V3 { public float x, y, z; }

    private static int Main(string[] args)
    {
        var a = new float[N]; var b = new float[N]; var c = new float[N]; var d = new float[N];
        var m = new float[12];
        for (int i = 0; i < N; i++) { a[i] = Rnd(4f); b[i] = Rnd(3f); c[i] = Rnd(2f); d[i] = Rnd(1f); }
        for (int k = 0; k < 12; k++) m[k] = Rnd(2f);
        float m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3];

        var p1 = new float[N]; var p2 = new float[N]; var p5 = new float[N]; var p6x = new float[N]; var p6y = new float[N];
        var p7 = new Vector3[N]; var p8 = new float[N];
        var verts = new Vector3[N]; var outv = new Vector3[N];
        var sinf = new float[N]; var cosf = new float[N];
        var sind = new double[N]; var cosd = new double[N];
        for (int i = 0; i < N; i++) verts[i] = new Vector3(a[i], b[i], c[i]);

        for (int i = 0; i < N; i++)
        {
            // P1: one expression straight into an array element.
            p1[i] = a[i] * b[i] + c[i];
            // P2: product through a float local.
            float t = a[i] * b[i];
            p2[i] = t + c[i];
            // P3: SkinMeshPart's matrix row, struct copied to a local, written to a field of an array element.
            Vector3 v = verts[i];
            outv[i].x = ((v.x * m0 + v.y * m1) + v.z * m2) + m3;
            // P5: bulge x = x + x * k through a local Vector3.
            Vector3 w = verts[i];
            float k = d[i] * a[i];
            w.x = w.x + w.x * k;
            p5[i] = w.x;
            // P6: rotation with sin/cos of an angle * weight, as SkinMeshPart does.
            float ang = a[i] * d[i];
            float s = (float)Math.Sin((double)ang);
            float co = (float)Math.Cos((double)ang);
            sinf[i] = s; cosf[i] = co;
            sind[i] = Math.Sin((double)a[i]); cosd[i] = Math.Cos((double)a[i]);
            Vector3 r = verts[i];
            float nx = r.x * co - r.y * s;
            r.y = r.x * s + r.y * co;
            r.x = nx;
            p6x[i] = r.x; p6y[i] = r.y;
            // P7: Vector3 operator + (a call into UnityEngine.CoreModule).
            p7[i] = verts[i] + new Vector3(d[i], c[i], b[i]);
            // P8: chained product through a local, as the bulge factors are built.
            float q = (a[i] * b[i]) * c[i];
            p8[i] = q;
        }

        // Math.Sin / Math.Cos versus the internal-call pointers.
        IntPtr mono = GetModuleHandleA("mono.dll");
        var lookup = (LookupFn)Marshal.GetDelegateForFunctionPointer(GetProcAddress(mono, "mono_lookup_internal_call"), typeof(LookupFn));
        IntPtr sinPtr = lookup(typeof(Math).GetMethod("Sin").MethodHandle.Value);
        IntPtr cosPtr = lookup(typeof(Math).GetMethod("Cos").MethodHandle.Value);
        var isin = (MathFn)Marshal.GetDelegateForFunctionPointer(sinPtr, typeof(MathFn));
        var icos = (MathFn)Marshal.GetDelegateForFunctionPointer(cosPtr, typeof(MathFn));
        int sinDiff = 0, cosDiff = 0;
        for (int i = 0; i < N; i++)
        {
            if (BitConverter.DoubleToInt64Bits(isin((double)a[i])) != BitConverter.DoubleToInt64Bits(sind[i])) sinDiff++;
            if (BitConverter.DoubleToInt64Bits(icos((double)a[i])) != BitConverter.DoubleToInt64Bits(cosd[i])) cosDiff++;
        }
        Console.WriteLine("mono.dll " + mono.ToString("x") + " sin icall " + sinPtr.ToString("x") + " cos icall " + cosPtr.ToString("x"));
        Console.WriteLine("Math.Sin vs icall pointer: " + sinDiff + " of " + N + " differ; Math.Cos: " + cosDiff);

        using (var f = new BinaryWriter(File.Create(args[0])))
        {
            f.Write(N);
            foreach (var x in m) f.Write(x);
            for (int i = 0; i < N; i++)
            {
                f.Write(a[i]); f.Write(b[i]); f.Write(c[i]); f.Write(d[i]);
                f.Write(p1[i]); f.Write(p2[i]); f.Write(outv[i].x); f.Write(p5[i]); f.Write(p6x[i]); f.Write(p6y[i]);
                f.Write(p7[i].x); f.Write(p7[i].y); f.Write(p7[i].z); f.Write(p8[i]);
                f.Write(sinf[i]); f.Write(cosf[i]); f.Write(sind[i]); f.Write(cosd[i]);
            }
        }
        Console.WriteLine("wrote " + args[0]);
        return 0;
    }
}
