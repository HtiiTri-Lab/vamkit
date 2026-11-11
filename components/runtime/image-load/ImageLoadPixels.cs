using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using BepInEx.Configuration;
using HarmonyLib;
using MVR.FileManagement;
using UnityEngine;

public sealed unsafe partial class ImageLoadPipeline
{
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryW(string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int PixelFn(byte* data,int pixels,int channels,int normal,int invert,int alpha);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int DxtFn(byte* src,byte* dst,int width,int height,int channels,int alpha);
    static ConfigEntry<bool> pixelsEnabled;
    static PixelFn pixelFn; static DxtFn dxtFn;
    static MethodInfo originalStream;
    [ThreadStatic] static bool pixelBypass;
    static long pixelApplied, pixelFallback, pixelBump, pixelBytes, pixelErrors;
    void SetUpPixels() {
        pixelsEnabled=Config.Bind("ImageLoad","ParallelPixels",false,"Parallelize VaM's independent channel/invert/alpha pixel transformations");
        if(!pixelsEnabled.Value) { logger.LogInfo("Image load pipeline: parallel pixels off"); return; }
        try {
            IntPtr m=LoadLibraryW(Path.Combine(Path.GetDirectoryName(Info.Location),"ImageLoadWorkNative.bin"));
            if(m==IntPtr.Zero || Native<ApiFn>(m,"ILW_Api")()!=1 || Native<ApiFn>(m,"ILW_Init")()!=1) throw new Exception("native image work or CpuBoost unavailable");
            pixelFn=Native<PixelFn>(m,"ILW_Pixels"); dxtFn=Native<DxtFn>(m,"ILW_DXT");
            originalStream=typeof(ImageLoaderThreaded.QueuedImage).GetMethod("ProcessFromStream",BindingFlags.Instance|BindingFlags.NonPublic);
            if(originalStream==null)throw new MissingMethodException("QueuedImage.ProcessFromStream");
            new Harmony("local.vam.imageloadpipeline.pixels").Patch(originalStream,new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("BeforePixelProcess")),null,null,null);
            logger.LogInfo("Image load pipeline: parallel pixels on");
        } catch(Exception e) { pixelsEnabled.Value=false; logger.LogError("Image load pipeline: parallel pixels off: "+e); }
    }
    public static bool BeforePixelProcess(ImageLoaderThreaded.QueuedImage __instance, Stream st) {
        if(pixelBypass || pixelsEnabled==null || !pixelsEnabled.Value) return true;
        if(__instance.createNormalFromBump) { Interlocked.Increment(ref pixelBump); return true; }
        if(st==null || !st.CanSeek) { Interlocked.Increment(ref pixelFallback); return true; }
        long originalPosition=st.Position;
        try {
            int width=__instance.width, height=__instance.height, channels=3;
            UnityEngine.TextureFormat textureFormat=UnityEngine.TextureFormat.RGB24;
            PixelFormat format=PixelFormat.Format24bppRgb;
            byte[] raw;
            using(Bitmap bitmap=new Bitmap(st)) using(SolidBrush brush=new SolidBrush(System.Drawing.Color.White)) {
                bitmap.RotateFlip(RotateFlipType.Rotate180FlipX);
                if(!__instance.setSize) {
                    width=bitmap.Width; height=bitmap.Height;
                    if(__instance.compress) { int x=width/4,y=height/4; width=(x==0?1:x)*4; height=(y==0?1:y)*4; }
                }
                if(__instance.createAlphaFromGrayscale || __instance.isNormalMap || bitmap.PixelFormat==PixelFormat.Format32bppArgb) {
                    textureFormat=UnityEngine.TextureFormat.RGBA32; format=PixelFormat.Format32bppArgb; channels=4;
                }
                int pixels=width*height, used=pixels*channels;
                if(width<=0 || height<=0 || pixels<=0 || used<=0 || (long)width*height!=pixels || (long)pixels*channels!=used) throw new ArgumentException("invalid dimensions");
                using(Bitmap target=new Bitmap(width,height,format)) using(System.Drawing.Graphics graphics=System.Drawing.Graphics.FromImage(target)) {
                    Rectangle rect=new Rectangle(0,0,width,height);
                    if(__instance.setSize) {
                        if(__instance.fillBackground) graphics.FillRectangle(brush,rect);
                        float ratio=Mathf.Min((float)width/bitmap.Width,(float)height/bitmap.Height);
                        int w=(int)(bitmap.Width*ratio),h=(int)(bitmap.Height*ratio);
                        graphics.DrawImage(bitmap,(width-w)/2,(height-h)/2,w,h);
                    } else graphics.DrawImage(bitmap,0,0,width,height);
                    BitmapData bits=target.LockBits(rect,ImageLockMode.ReadOnly,target.PixelFormat);
                    try { raw=new byte[Mathf.CeilToInt((float)used*1.5f)]; Marshal.Copy(bits.Scan0,raw,0,used); }
                    finally { target.UnlockBits(bits); }
                }
                fixed(byte* p=raw) if(pixelFn(p,pixels,channels,__instance.isNormalMap?1:0,__instance.invert?1:0,__instance.createAlphaFromGrayscale?1:0)!=1) throw new Exception("pixel native rejected");
                __instance.width=width; __instance.height=height; __instance.textureFormat=textureFormat; __instance.raw=raw;
                Interlocked.Increment(ref pixelApplied); Interlocked.Add(ref pixelBytes,used);
            }
            return false;
        } catch {
            Interlocked.Increment(ref pixelErrors);
            try { st.Position=originalPosition; } catch { throw; }
            return true;
        }
    }
    static string PixelStats() { return " pixelApplied="+pixelApplied+" pixelBumpFallback="+pixelBump+" pixelStreamFallback="+pixelFallback+" pixelErrors="+pixelErrors+" pixelMiB="+(pixelBytes/1048576.0).ToString("F1"); }

    // Compare the candidate with VaM's own implementation and Unity's own runtime compressor.
    public static string TestPixelsAndDxt() {
        if(pixelsEnabled==null || !pixelsEnabled.Value || originalStream==null) return "not enabled";
        StringBuilder s=new StringBuilder(); int failures=0,cases=0;
        foreach(bool alphaInput in new[]{false,true}) {
            byte[] encoded;
            using(Bitmap bmp=new Bitmap(256,256,alphaInput?PixelFormat.Format32bppArgb:PixelFormat.Format24bppRgb)) {
                for(int y=0;y<256;y++) for(int x=0;x<256;x++) bmp.SetPixel(x,y,System.Drawing.Color.FromArgb(alphaInput?(x*7+y*3)%256:255,(x*19+y)%256,(y*11+x)%256,(x*7+y*13)%256));
                using(MemoryStream m=new MemoryStream()) { bmp.Save(m,ImageFormat.Png); encoded=m.ToArray(); }
            }
            for(int mask=0;mask<32;mask++) {
                var a=new ImageLoaderThreaded.QueuedImage(); var b=new ImageLoaderThreaded.QueuedImage();
                a.compress=b.compress=(mask&1)!=0; a.isNormalMap=b.isNormalMap=(mask&2)!=0; a.invert=b.invert=(mask&4)!=0; a.createAlphaFromGrayscale=b.createAlphaFromGrayscale=(mask&8)!=0;
                a.setSize=b.setSize=(mask&16)!=0; a.fillBackground=b.fillBackground=(mask&16)!=0; if(a.setSize) { a.width=b.width=93; a.height=b.height=71; }
                using(var m=new MemoryStream(encoded,false)) { pixelBypass=true; try { originalStream.Invoke(a,new object[]{m}); } finally { pixelBypass=false; } }
                using(var m=new MemoryStream(encoded,false)) originalStream.Invoke(b,new object[]{m});
                bool same=a.width==b.width && a.height==b.height && a.textureFormat==b.textureFormat && a.raw!=null && b.raw!=null && a.raw.Length==b.raw.Length;
                if(same) for(int i=0;i<a.raw.Length;i++) if(a.raw[i]!=b.raw[i]) { same=false; break; }
                cases++; if(!same) failures++;
            }
            int c=alphaInput?4:3, total=64*64*c; byte[] raw=new byte[total];
            for(int i=0;i<64*64;i++) { raw[i*c]=(byte)((i*19)%256);raw[i*c+1]=(byte)((i*7+23)%256);raw[i*c+2]=(byte)((i*13+9)%256);if(c==4)raw[i*c+3]=(byte)((i*11)%256); }
            Texture2D tex=new Texture2D(64,64,alphaInput?TextureFormat.RGBA32:TextureFormat.RGB24,false,false);
            try {
                tex.LoadRawTextureData(raw);tex.Apply(false);tex.Compress(true);
                byte[] unity=tex.GetRawTextureData(), candidate=new byte[(64/4)*(64/4)*(alphaInput?16:8)];
                int ok;fixed(byte* ps=raw)fixed(byte* pd=candidate)ok=dxtFn(ps,pd,64,64,c,alphaInput?1:0);
                int different=0;
                if(unity.Length==candidate.Length && ok==1) { for(int i=0;i<unity.Length;i++) if(unity[i]!=candidate[i]) different++; }
                else different=-1;
                s.Append(" dxt").Append(alphaInput?"5":"1").Append(" format=").Append(tex.format).Append(" unityBytes=").Append(unity.Length).Append(" candidateBytes=").Append(candidate.Length).Append(" differentBytes=").Append(different);
            } finally { UnityEngine.Object.Destroy(tex); }
        }
        try {
            string path=Environment.GetEnvironmentVariable("VAMKIT_IMAGE_TEST_PATH");if(string.IsNullOrEmpty(path))return "pixels cases="+cases+" failures="+failures+s;
            byte[] data=FileManager.ReadAllBytes(path,true);
            var a=new ImageLoaderThreaded.QueuedImage();var b=new ImageLoaderThreaded.QueuedImage();a.compress=b.compress=true;
            using(var m=new MemoryStream(data,false)){pixelBypass=true;try{originalStream.Invoke(a,new object[]{m});}finally{pixelBypass=false;}}
            using(var m=new MemoryStream(data,false))originalStream.Invoke(b,new object[]{m});
            bool match=a.width==b.width&&a.height==b.height&&a.textureFormat==b.textureFormat&&a.raw.Length==b.raw.Length;
            if(match)for(int i=0;i<a.raw.Length;i++)if(a.raw[i]!=b.raw[i]){match=false;break;}
            s.Append(" realImage=").Append(match?"match":"different").Append(" size=").Append(a.width).Append("x").Append(a.height);
        }catch(Exception e){s.Append(" realImage=error:").Append(e.GetType().Name);}
        return "pixels cases="+cases+" failures="+failures+s;
    }
}
