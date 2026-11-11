using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using BepInEx.Configuration;
using HarmonyLib;
using MVR.FileManagement;
using UnityEngine;

public sealed unsafe partial class ImageLoadPipeline
{
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);
    [DllImport("kernel32.dll",CharSet=CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m,string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int ApiFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SubmitFn([MarshalAs(UnmanagedType.LPWStr)] string path,int size);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int PollFn(int id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int CopyFn(int id,byte* data,int size);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void FreeFn(int id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int StopFn();
    static ConfigEntry<bool> asyncEnabled;
    static SubmitFn submit;static PollFn poll;static ApiFn peak;static CopyFn copy;static FreeFn free;static StopFn stop;
    static MethodInfo readMeta,diskPath,cloneMethod;
    static FieldInfo textureCacheField;
    static int mainThread;
    static readonly object gate=new object();
    static readonly Dictionary<string,Work> byKey=new Dictionary<string,Work>();
    static readonly Dictionary<ImageLoaderThreaded.QueuedImage,Work> byRequest=new Dictionary<ImageLoaderThreaded.QueuedImage,Work>();
    static readonly HashSet<ImageLoaderThreaded.QueuedImage> rejected=new HashSet<ImageLoaderThreaded.QueuedImage>();
    static long reserved,submitted,applied,mainPrepared,shared,hitMemory,unsupported,capacity,errors,stale,readyCount,waitFallback;
    const int MaxJobs=4;const long MaxCache=128L*1024*1024,MaxReserved=512L*1024*1024;
    [ThreadStatic] static bool inImageWorker;
    sealed class Work {
        public string key,file,meta; public ImageLoaderThreaded.QueuedImage copy;
        public bool caching,done,success;public long length,stamp,metaLength,metaStamp,reservation;public int refs,id;
    }
    static T Native<T>(IntPtr m,string name) where T:class { IntPtr p=GetProcAddress(m,name);if(p==IntPtr.Zero)throw new MissingMethodException(name);return Marshal.GetDelegateForFunctionPointer(p,typeof(T)) as T; }
    void SetUpAsync() {
        asyncEnabled=Config.Bind("ImageLoad","AsyncPreparation",false,"Prepare disk-cache images with native CpuBoost jobs");
        if(!asyncEnabled.Value){logger.LogInfo("Image load pipeline: async off");return;}
        try {
            BindingFlags f=BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Instance;
            Type q=typeof(ImageLoaderThreaded.QueuedImage),l=typeof(ImageLoaderThreaded);
            readMeta=q.GetMethod("ReadMetaJson",f);diskPath=q.GetMethod("GetDiskCachePath",f);cloneMethod=typeof(object).GetMethod("MemberwiseClone",f);textureCacheField=l.GetField("textureCache",f);
            if(readMeta==null||diskPath==null||cloneMethod==null||textureCacheField==null)throw new MissingMemberException("VaM image methods");
            IntPtr m=LoadLibraryW(Path.Combine(Path.GetDirectoryName(Info.Location),"ImageLoadWorkNative.bin"));
            if(m==IntPtr.Zero || Native<ApiFn>(m,"ILW_Api")()!=1 || Native<ApiFn>(m,"ILW_Init")()!=1)throw new Exception("native image work or CpuBoost unavailable");
            submit=Native<SubmitFn>(m,"ILW_ReadSubmit");poll=Native<PollFn>(m,"ILW_ReadPoll");peak=Native<ApiFn>(m,"ILW_ReadPeak");copy=Native<CopyFn>(m,"ILW_ReadCopy");free=Native<FreeFn>(m,"ILW_ReadFree");stop=Native<StopFn>(m,"ILW_ReadStop");
            mainThread=Thread.CurrentThread.ManagedThreadId;
            var h=new Harmony("local.vam.imageloadpipeline.async");
            h.Patch(l.GetMethod("QueueImage",f),null,new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("OnQueued")),null,null);
            h.Patch(l.GetMethod("Update",f),null,new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("AfterLoaderUpdate")),null,null);
            h.Patch(l.GetMethod("ProcessImageQueueThreaded",f),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("EnterImageWorker")),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("ExitImageWorker")),null,null);
            h.Patch(q.GetMethod("Process",f),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("BeforeImageProcess")),null,null,null);
            logger.LogInfo("Image load pipeline: async native cache reads on");
        } catch(Exception e){asyncEnabled.Value=false;logger.LogError("Image load pipeline: async off: "+e);}
    }
    void OnApplicationQuit(){StopCrossPipeline();StopWriter();if(stop!=null){int r=stop();logger.LogInfo("Image load pipeline: async stop "+r);}}
    static string Signature(ImageLoaderThreaded.QueuedImage q) { return q.imgPath+"|"+q.skipCache+"|"+q.forceReload+"|"+q.createMipMaps+"|"+q.compress+"|"+q.linear+"|"+q.isNormalMap+"|"+q.createAlphaFromGrayscale+"|"+q.createNormalFromBump+"|"+q.bumpStrength.ToString("R",System.Globalization.CultureInfo.InvariantCulture)+"|"+q.invert+"|"+q.setSize+"|"+q.fillBackground+"|"+q.width+"|"+q.height; }
    static bool InMemory(ImageLoaderThreaded loader,ImageLoaderThreaded.QueuedImage q) {
        if(q.skipCache||q.forceReload||q.isThumbnail||q.imgPath==null)return false;
        var cache=textureCacheField.GetValue(loader) as Dictionary<string,Texture2D>;Texture2D t;return cache!=null&&cache.TryGetValue(q.cacheSignature,out t)&&t!=null;
    }
    static Work Describe(ImageLoaderThreaded.QueuedImage q,string key) {
        if(q.cancel||q.processed||q.forceReload||q.isThumbnail||q.webRequest!=null||q.useWebCache||String.IsNullOrEmpty(q.imgPath)||q.imgPath=="NULL"||q.imgPath.StartsWith("http",StringComparison.OrdinalIgnoreCase)||!CacheManager.CachingEnabled)return null;
        var fe=FileManager.GetFileEntry(q.imgPath,true);if(fe==null||!fe.Exists)return null;
        string path=(string)diskPath.Invoke(q,null);if(String.IsNullOrEmpty(path)||!File.Exists(path)||!File.Exists(path+"meta"))return null;
        var f=new FileInfo(path);var m=new FileInfo(path+"meta");if(f.Length<=0||f.Length>MaxCache||m.Length>65536)return null;
        var w=new Work();w.key=key;w.copy=(ImageLoaderThreaded.QueuedImage)cloneMethod.Invoke(q,null);w.caching=true;w.file=f.FullName;w.meta=m.FullName;w.length=f.Length;w.stamp=f.LastWriteTimeUtc.Ticks;w.metaLength=m.Length;w.metaStamp=m.LastWriteTimeUtc.Ticks;w.reservation=f.Length*2+1024*1024;
        readMeta.Invoke(w.copy,new object[]{File.ReadAllText(w.meta)});w.copy.preprocessed=true;return w;
    }
    // 1: handled; 0: declined; -1: capacity full, caller should stop scanning.
    static int TryQueue(ImageLoaderThreaded loader,ImageLoaderThreaded.QueuedImage q) {
        if(q==null||q.cancel||q.processed||Thread.CurrentThread.ManagedThreadId!=mainThread)return 0;
        lock(gate){if(byRequest.ContainsKey(q)||rejected.Contains(q))return 1;}
        try {
            if(InMemory(loader,q)){lock(gate){rejected.Add(q);hitMemory++;}return 1;}
            string key=Signature(q);Work same;
            lock(gate){if(byKey.TryGetValue(key,out same)){same.refs++;byRequest.Add(q,same);shared++;return 1;}if(byKey.Count>=MaxJobs){capacity++;return -1;}}
            Work w=Describe(q,key);if(w==null){lock(gate){rejected.Add(q);unsupported++;}return 0;}
            lock(gate){
                if(byKey.Count>=MaxJobs||reserved+w.reservation>MaxReserved){capacity++;return -1;}
                w.id=submit(w.file,(int)w.length);if(w.id<1){rejected.Add(q);errors++;return 0;}
                w.refs=1;reserved+=w.reservation;byKey.Add(key,w);byRequest.Add(q,w);submitted++;
            }
            return 1;
        }catch{lock(gate){rejected.Add(q);errors++;}return 0;}
    }
    public static void OnQueued(ImageLoaderThreaded __instance,ImageLoaderThreaded.QueuedImage qi){if(asyncEnabled.Value)TryQueue(__instance,qi);}
    static void Retire(Work w){
        Work other;
        // A rejected/cancelled request can leave a native read in flight. Keep its slot and budget
        // until the read has actually ended; otherwise repeated fallbacks can exceed the cap.
        if(w.refs==0&&byKey.TryGetValue(w.key,out other)&&object.ReferenceEquals(other,w)&&poll(w.id)!=0){byKey.Remove(w.key);reserved-=w.reservation;free(w.id);}
    }
    static void Release(ImageLoaderThreaded.QueuedImage q){lock(gate){Work w;if(!byRequest.TryGetValue(q,out w))return;byRequest.Remove(q);w.refs--;Retire(w);}}
    public static void AfterLoaderUpdate(ImageLoaderThreaded __instance){
        if(!asyncEnabled.Value)return;
        try {
            var queue=queueField.GetValue(__instance) as LinkedList<ImageLoaderThreaded.QueuedImage>;if(queue==null)return;
            lock(gate)if(queue.Count==0&&byRequest.Count==0&&rejected.Count==0&&byKey.Count==0)return;
            var remove=new List<ImageLoaderThreaded.QueuedImage>();
            lock(gate){foreach(var kv in byRequest)if(kv.Key.cancel||kv.Key.processed||!queue.Contains(kv.Key))remove.Add(kv.Key);rejected.RemoveWhere(delegate(ImageLoaderThreaded.QueuedImage q){return q.cancel||q.processed||!queue.Contains(q);});}
            foreach(var q in remove)Release(q);
            var orphans=new List<Work>();
            lock(gate){foreach(var kv in byKey)if(kv.Value.refs==0)orphans.Add(kv.Value);foreach(var w in orphans)Retire(w);}
            var node=queue.First;if(node!=null)node=node.Next;int n=0;
            while(node!=null&&n++<64){if(TryQueue(__instance,node.Value)<0)break;node=node.Next;}
        }catch(Exception e){lock(gate)errors++;logger.LogWarning("Image async queue fallback: "+e.Message);}
    }
    public static void EnterImageWorker(){inImageWorker=true;}public static void ExitImageWorker(){inImageWorker=false;}
    static bool Ready(Work w){
        lock(w){
            if(w.done)return w.success;
            int p=poll(w.id);if(p==0)return false;
            w.done=true;if(p<0){Interlocked.Increment(ref errors);return false;}
            byte[] data=new byte[(int)w.length];fixed(byte* ptr=data)if(copy(w.id,ptr,data.Length)!=1){Interlocked.Increment(ref errors);return false;}
            w.copy.raw=data;w.success=true;Interlocked.Increment(ref readyCount);return true;
        }
    }
    static bool CopyReady(ImageLoaderThreaded.QueuedImage q,Work w){
        try {
            var f=new FileInfo(w.file);var m=new FileInfo(w.meta);
            bool valid=!q.cancel&&Signature(q)==w.key&&w.caching==CacheManager.CachingEnabled&&f.Exists&&f.Length==w.length&&f.LastWriteTimeUtc.Ticks==w.stamp&&m.Exists&&m.Length==w.metaLength&&m.LastWriteTimeUtc.Ticks==w.metaStamp;
            if(!valid){Interlocked.Increment(ref stale);Release(q);return false;}
            q.width=w.copy.width;q.height=w.copy.height;q.textureFormat=w.copy.textureFormat;q.raw=w.copy.raw;q.preprocessed=true;q.processed=true;Interlocked.Increment(ref applied);Release(q);return true;
        }catch{Interlocked.Increment(ref errors);Release(q);return false;}
    }
    static void TryTakeReadyOnMain(ImageLoaderThreaded.QueuedImage q){
        if(asyncEnabled==null||!asyncEnabled.Value||q==null||q.processed)return;Work w;lock(gate)if(!byRequest.TryGetValue(q,out w))return;
        if(Ready(w)&&CopyReady(q,w))Interlocked.Increment(ref mainPrepared);
    }
    public static bool BeforeImageProcess(ImageLoaderThreaded.QueuedImage __instance){
        if(!inImageWorker||!asyncEnabled.Value||__instance.processed)return true;Work w;lock(gate)if(!byRequest.TryGetValue(__instance,out w))return true;
        if(!Ready(w)){Interlocked.Increment(ref waitFallback);Release(__instance);return true;}
        return !CopyReady(__instance,w);
    }
    static string AsyncStats(){
        if(asyncEnabled==null||!asyncEnabled.Value)return " async=off";
        lock(gate)return " asyncSubmitted="+submitted+" ready="+readyCount+" applied="+applied+" mainPrepared="+mainPrepared+" shared="+shared+" memorySkip="+hitMemory+" unsupported="+unsupported+" capacity="+capacity+" errors="+errors+" stale="+stale+" waitFallback="+waitFallback+" active="+byKey.Count+" reservedMiB="+(reserved/1048576)+" peakConcurrent="+peak();
    }
    public static string TestBudget(){
        lock(gate){
            if(byKey.Count!=0||byRequest.Count!=0)return "budgetCheck=busy";
            PollFn oldPoll=poll;FreeFn oldFree=free;long oldReserved=reserved;int state=0,freed=0;
            var w=new Work();w.key="__budget_test__";w.id=-123;w.refs=0;w.reservation=12345;
            try {
                poll=delegate(int ignored){return state;};free=delegate(int ignored){freed++;};
                byKey.Add(w.key,w);reserved+=w.reservation;Retire(w);
                bool pendingOk=byKey.ContainsKey(w.key)&&reserved==oldReserved+w.reservation&&freed==0;
                state=1;Retire(w);bool doneOk=!byKey.ContainsKey(w.key)&&reserved==oldReserved&&freed==1;
                Retire(w);bool once=reserved==oldReserved&&freed==1;
                return "budgetCheck="+(pendingOk&&doneOk&&once?"pass":"fail")+" pending="+pendingOk+" complete="+doneOk+" once="+once;
            } finally { byKey.Remove(w.key);reserved=oldReserved;poll=oldPoll;free=oldFree; }
        }
    }
}
