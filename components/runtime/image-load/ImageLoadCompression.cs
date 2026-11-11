using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using BepInEx.Configuration;
using HarmonyLib;
using MVR.FileManagement;
using SimpleJSON;
using UnityEngine;

public sealed unsafe partial class ImageLoadPipeline
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int MipCompressFn(byte* src,int srcBytes,byte* dst,int dstBytes,int w,int h,int channels,int mips);
    static ConfigEntry<bool> compressionEnabled,forceCold;
    static MipCompressFn compressMips;
    static MethodInfo compressionDiskPath;
    static long compressionApplied,compressionUnsupported,compressionFailed,compressionInputBytes,compressionTicks;
    public sealed class CompressionState { public bool converted; public byte[] data; }
    static bool PowerOfTwo(int n) { return n>0 && (n&(n-1))==0; }

    void SetUpCompression() {
        compressionEnabled=Config.Bind("ImageLoad","ParallelCompression",false,"Experimental BC1/BC3 compression of Unity-generated mipmaps on CpuBoost");
        forceCold=Config.Bind("ImageLoadTest","ForceCold",false,"Test only: temporarily disable image disk-cache reads and writes");
        try {
            BindingFlags f=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance;
            Type q=typeof(ImageLoaderThreaded.QueuedImage);
            compressionDiskPath=q.GetMethod("GetDiskCachePath",f);
            if(compressionDiskPath==null)throw new MissingMethodException("GetDiskCachePath");
            var h=new Harmony("local.vam.imageloadpipeline.compression");
            if(forceCold.Value)h.Patch(compressionDiskPath,null,new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("ColdCachePath")),null,null);
            if(compressionEnabled.Value) {
                IntPtr m=LoadLibraryW(Path.Combine(Path.GetDirectoryName(Info.Location),"ImageLoadWorkNative.bin"));
                if(m==IntPtr.Zero||Native<ApiFn>(m,"ILW_Init")()!=1)throw new Exception("native image work or CpuBoost unavailable");
                compressMips=Native<MipCompressFn>(m,"ILW_DXTMips");
                SetUpOffMainNative(m);
                MethodInfo finish=q.GetMethod("Finish",f);if(finish==null)throw new MissingMethodException("QueuedImage.Finish");
                h.Patch(finish,new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("BeforeCompressFinish")),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("AfterCompressFinish")),null,null);
            }
            logger.LogInfo("Image load pipeline: parallel compression "+(compressionEnabled.Value?"on":"off")+", forced cold test "+forceCold.Value);
        }catch(Exception e){compressionEnabled.Value=false;logger.LogError("Image load pipeline: compression setup failed: "+e);}
    }
    public static void ColdCachePath(ref string __result) { if(forceCold!=null&&forceCold.Value)__result=null; }

    static bool CompressBytes(byte[] source,int width,int height,int channels,int mips,out byte[] result) {
        result=null;long src=0,dst=0;int w=width,h=height;
        for(int i=0;i<mips;i++){src+=(long)w*h*channels;dst+=(long)((w+3)/4)*((h+3)/4)*(channels==4?16:8);w=Math.Max(1,w/2);h=Math.Max(1,h/2);}
        if(src!=source.LongLength||dst<=0||dst>int.MaxValue)return false;
        var output=new byte[(int)dst];int ok;
        fixed(byte* ps=source)fixed(byte* pd=output)ok=compressMips(ps,source.Length,pd,output.Length,width,height,channels,mips);
        if(ok!=1)return false;result=output;return true;
    }
    public static void BeforeCompressFinish(ImageLoaderThreaded.QueuedImage __instance,out CompressionState __state) {
        __state=null;
        if(TakeCrossCompressed(__instance)) { __state=new CompressionState();__state.converted=true;__state.data=__instance.raw;return; }
        if(compressionEnabled==null||!compressionEnabled.Value||__instance==null)return;
        var q=__instance;
        if(q.hadError||q.finished||q.preprocessed||!q.compress||q.raw==null||q.tex!=null||q.forceReload||String.IsNullOrEmpty(q.imgPath)||q.imgPath.StartsWith("http",StringComparison.OrdinalIgnoreCase)||q.width<4||q.height<4||q.width%4!=0||q.height%4!=0||(q.createMipMaps&&(!PowerOfTwo(q.width)||!PowerOfTwo(q.height)))||(q.textureFormat!=TextureFormat.RGB24&&q.textureFormat!=TextureFormat.RGBA32)) { Interlocked.Increment(ref compressionUnsupported);return; }
        long t=System.Diagnostics.Stopwatch.GetTimestamp();Texture2D temp=null;
        try {
            int channels=q.textureFormat==TextureFormat.RGBA32?4:3;
            byte[] direct;
            if(NativeMipCompress(q,channels,out direct)) {
                q.raw=direct;q.textureFormat=channels==4?TextureFormat.DXT5:TextureFormat.DXT1;q.preprocessed=true;
                __state=new CompressionState();__state.converted=true;__state.data=direct;
                Interlocked.Increment(ref compressionApplied);return;
            }
            temp=new Texture2D(q.width,q.height,q.textureFormat,q.createMipMaps,q.linear);
            long mipStart=StageStart();
            temp.LoadRawTextureData(q.raw);temp.Apply();
            byte[] mips=temp.GetRawTextureData(),encoded;
            StageEnd(ref stageMipTicks,ref stageMipCount,mipStart);
            long bcStart=StageStart();
            bool compressed=CompressBytes(mips,q.width,q.height,channels,temp.mipmapCount,out encoded);
            StageEnd(ref stageBcTicks,ref stageBcCount,bcStart);
            if(!compressed)throw new Exception("unsupported mip layout");
            q.raw=encoded;q.textureFormat=channels==4?TextureFormat.DXT5:TextureFormat.DXT1;q.preprocessed=true;
            __state=new CompressionState();__state.converted=true;__state.data=encoded;
            Interlocked.Increment(ref compressionApplied);Interlocked.Add(ref compressionInputBytes,mips.Length);
        }catch(Exception e){Interlocked.Increment(ref compressionFailed);logger.LogWarning("Image compression fell back: "+e.Message);}
        finally{if(temp!=null)UnityEngine.Object.Destroy(temp);Interlocked.Add(ref compressionTicks,System.Diagnostics.Stopwatch.GetTimestamp()-t);}
    }
    public static void AfterCompressFinish(ImageLoaderThreaded.QueuedImage __instance,CompressionState __state) {
        if(__state==null||!__state.converted||__instance.hadError||__instance.tex==null||!CacheManager.CachingEnabled)return;
        try {
            string path=(string)compressionDiskPath.Invoke(__instance,null);
            if(path==null||FileManager.FileExists(path,false,false)||CacheWritePending(path))return;
            var meta=new JSONClass();meta["type"]="image";meta["width"].AsInt=__instance.tex.width;meta["height"].AsInt=__instance.tex.height;meta["format"]=__instance.tex.format.ToString();
            var tex=__instance.tex;byte[] mine=__state.data;
            // The texture was loaded from exactly these bytes; reuse them instead of copying back from Unity.
            if(mine!=null&&(tex.format==TextureFormat.DXT1||tex.format==TextureFormat.DXT5)&&mine.LongLength==BlockBytes(tex.width,tex.height,tex.mipmapCount,tex.format==TextureFormat.DXT5)&&QueueCacheWrite(path,meta.ToString(string.Empty),mine))return;
            byte[] data=tex.GetRawTextureData();File.WriteAllText(path+"meta",meta.ToString(string.Empty));File.WriteAllBytes(path,data);
        }catch(Exception e){__instance.hadError=true;__instance.errorText="Exception during caching of "+__instance.imgPath+": "+e;logger.LogError(__instance.errorText);}
    }
    static string CompressionStats(){return " compression="+compressionApplied+" compressionUnsupported="+compressionUnsupported+" compressionFailed="+compressionFailed+" compressionMiB="+(compressionInputBytes/1048576.0).ToString("F1")+" compressionWallMs="+(compressionTicks*1000.0/System.Diagnostics.Stopwatch.Frequency).ToString("F1");}
}
