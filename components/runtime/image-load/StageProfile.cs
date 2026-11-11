using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Threading;
using BepInEx.Configuration;
using HarmonyLib;

public sealed unsafe partial class ImageLoadPipeline {
    static ConfigEntry<bool> stageEnabled;
    static long stageProcessTicks,stageProcessCount,stageDecodeTicks,stageDecodeCount,stageReadTicks,stageReadCount,stageReadBytes,stageFinishTicks,stageFinishCount,stageWaitTicks,stageWaitCount;
    static long stageMipTicks,stageMipCount,stageBcTicks,stageBcCount;
    static readonly object stageLock=new object();
    static readonly Dictionary<ImageLoaderThreaded.QueuedImage,long> stageQueued=new Dictionary<ImageLoaderThreaded.QueuedImage,long>();
    static long StageStart(){return stageEnabled!=null&&stageEnabled.Value?Stopwatch.GetTimestamp():0;}
    static void StageEnd(ref long ticks,ref long count,long start){if(start!=0){Interlocked.Add(ref ticks,Stopwatch.GetTimestamp()-start);Interlocked.Increment(ref count);}}
    void SetUpStageProfile(){
        stageEnabled=Config.Bind("ImageLoadTest","StageProfile",false,"Test-only image loading stage timings");if(!stageEnabled.Value)return;
        var f=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance;var q=typeof(ImageLoaderThreaded.QueuedImage);var l=typeof(ImageLoaderThreaded);var h=new Harmony("local.vam.imageloadpipeline.stages");
        h.Patch(l.GetMethod("QueueImage",f),null,new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("StageQueue")),null,null);
        h.Patch(q.GetMethod("Process",f),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("StageProcessStart")),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("StageProcessEnd")),null,null);
        h.Patch(q.GetMethod("ProcessFromStream",f),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("StageDecodeStart")),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("StageDecodeEnd")),null,null);
        h.Patch(q.GetMethod("Finish",f),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("StageFinishStart")),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("StageFinishEnd")),null,null);
    }
    public static void StageQueue(ImageLoaderThreaded.QueuedImage qi){if(qi==null)return;lock(stageLock)stageQueued[qi]=Stopwatch.GetTimestamp();}
    [HarmonyPriority(Priority.First)] public static void StageProcessStart(ImageLoaderThreaded.QueuedImage __instance,out long __state){__state=StageStart();lock(stageLock){long t;if(stageQueued.TryGetValue(__instance,out t)){stageQueued.Remove(__instance);StageEnd(ref stageWaitTicks,ref stageWaitCount,t);}}}
    [HarmonyPriority(Priority.Last)] public static void StageProcessEnd(long __state){StageEnd(ref stageProcessTicks,ref stageProcessCount,__state);}
    sealed class MeteredStream:Stream {
        readonly Stream inner;public MeteredStream(Stream s){inner=s;}
        public override bool CanRead{get{return inner.CanRead;}}public override bool CanSeek{get{return inner.CanSeek;}}public override bool CanWrite{get{return inner.CanWrite;}}
        public override long Length{get{return inner.Length;}}public override long Position{get{return inner.Position;}set{inner.Position=value;}}
        public override void Flush(){inner.Flush();}public override long Seek(long offset,SeekOrigin origin){return inner.Seek(offset,origin);}public override void SetLength(long value){inner.SetLength(value);}public override void Write(byte[] b,int o,int c){inner.Write(b,o,c);}
        public override int Read(byte[] b,int o,int c){long t=StageStart();try{int n=inner.Read(b,o,c);Interlocked.Add(ref stageReadBytes,n);return n;}finally{StageEnd(ref stageReadTicks,ref stageReadCount,t);}}
    }
    [HarmonyPriority(Priority.First)] public static void StageDecodeStart(ref Stream st,out long __state){__state=StageStart();if(st!=null)st=new MeteredStream(st);}
    [HarmonyPriority(Priority.Last)] public static void StageDecodeEnd(long __state){StageEnd(ref stageDecodeTicks,ref stageDecodeCount,__state);}
    [HarmonyPriority(Priority.First)] public static void StageFinishStart(out long __state){__state=StageStart();}
    [HarmonyPriority(Priority.Last)] public static void StageFinishEnd(long __state){StageEnd(ref stageFinishTicks,ref stageFinishCount,__state);}
    public static string StageReset(){Interlocked.Exchange(ref stageProcessTicks,0);Interlocked.Exchange(ref stageProcessCount,0);Interlocked.Exchange(ref stageDecodeTicks,0);Interlocked.Exchange(ref stageDecodeCount,0);Interlocked.Exchange(ref stageReadTicks,0);Interlocked.Exchange(ref stageReadCount,0);Interlocked.Exchange(ref stageReadBytes,0);Interlocked.Exchange(ref stageFinishTicks,0);Interlocked.Exchange(ref stageFinishCount,0);Interlocked.Exchange(ref stageWaitTicks,0);Interlocked.Exchange(ref stageWaitCount,0);Interlocked.Exchange(ref stageMipTicks,0);Interlocked.Exchange(ref stageMipCount,0);Interlocked.Exchange(ref stageBcTicks,0);Interlocked.Exchange(ref stageBcCount,0);lock(stageLock)stageQueued.Clear();return "reset";}
    static string Pair(string name,long ticks,long count){return name+"="+(ticks*1000.0/Stopwatch.Frequency).ToString("F1",System.Globalization.CultureInfo.InvariantCulture)+"ms/"+count;}
    public static string StageReport(){return Pair("queue",stageWaitTicks,stageWaitCount)+" "+Pair("process",stageProcessTicks,stageProcessCount)+" "+Pair("decode",stageDecodeTicks,stageDecodeCount)+" "+Pair("streamRead",stageReadTicks,stageReadCount)+" readMiB="+(stageReadBytes/1048576.0).ToString("F1",System.Globalization.CultureInfo.InvariantCulture)+" "+Pair("finish",stageFinishTicks,stageFinishCount)+" "+Pair("mip",stageMipTicks,stageMipCount)+" "+Pair("bc",stageBcTicks,stageBcCount);}
}
