using System;
using System.IO;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Diagnostics;
using BepInEx.Configuration;
using HarmonyLib;
using MVR.FileManagement;
using ICSharpCode.SharpZipLib.Zip;
using UnityEngine;

public sealed unsafe partial class ImageLoadPipeline {
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SourceSubmitFn([MarshalAs(UnmanagedType.LPWStr)] string path,long offset,long csize,long size,int method,int zip,int compress,int normal,int invert,int alpha,int setSize,int fill,int width,int height);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SourceInfoFn(int id,out int width,out int height,out int channels,out int n);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int BcSubmitFn(byte* data,int n,int output,int w,int h,int channels,int mips);
 static ConfigEntry<bool> crossEnabled;static SourceSubmitFn sourceSubmit;static PollFn sourcePoll,bcPoll;static SourceInfoFn sourceInfo;static CopyFn sourceCopy,bcCopy;static FreeFn sourceFree,bcFree;static StopFn sourceStop,bcStop;static ApiFn sourcePeak;static BcSubmitFn bcSubmit;static FieldInfo zipOffsetCross;
 static readonly object crossLock=new object();static readonly Dictionary<ImageLoaderThreaded.QueuedImage,CrossWork> crossByRequest=new Dictionary<ImageLoaderThreaded.QueuedImage,CrossWork>();static readonly List<CrossWork> crossAll=new List<CrossWork>();static readonly HashSet<ImageLoaderThreaded.QueuedImage> crossRejected=new HashSet<ImageLoaderThreaded.QueuedImage>();static readonly HashSet<ImageLoaderThreaded.QueuedImage> crossCompressed=new HashSet<ImageLoaderThreaded.QueuedImage>();
 const int CrossMax=6;const long CrossReservation=320L*1024*1024;
 static long crossSubmitted,crossApplied,crossCompressedApplied,crossMiss,crossErrors,crossRetired,crossScheduledBC,crossQueueFull;static long crossReserved;
 sealed class CrossWork{public ImageLoaderThreaded.QueuedImage q;public string key,file,expectedCache;public long stamp,fileLength;public int sourceId,bcId,w,h,channels,bcBytes,rawBytes;public byte[] raw;public bool retired,dims,chained;}
 void SetUpCrossPipeline(){
  crossEnabled=Config.Bind("ImageLoad","CrossImagePipeline",false,"Prepare separate source images and their compression asynchronously with CpuBoost");if(!crossEnabled.Value)return;
  try{
   if(!asyncEnabled.Value||!compressionEnabled.Value)throw new Exception("requires asynchronous preparation and parallel compression");
   IntPtr m=LoadLibraryW(Path.Combine(Path.GetDirectoryName(Info.Location),"ImageLoadWorkNative.bin"));if(m==IntPtr.Zero)throw new Exception("native library unavailable");
   sourceSubmit=Native<SourceSubmitFn>(m,"ILW_SourceSubmit");sourcePoll=Native<PollFn>(m,"ILW_SourcePoll");sourceInfo=Native<SourceInfoFn>(m,"ILW_SourceInfo");sourceCopy=Native<CopyFn>(m,"ILW_SourceCopy");sourceFree=Native<FreeFn>(m,"ILW_SourceFree");sourceStop=Native<StopFn>(m,"ILW_SourceStop");sourcePeak=Native<ApiFn>(m,"ILW_SourcePeak");if(nativeMipsEnabled.Value&&!nativeMipsReady)SetUpOffMainNative(m);
   bcSubmit=Native<BcSubmitFn>(m,"ILW_BCSubmit");bcPoll=Native<PollFn>(m,"ILW_BCPoll");bcCopy=Native<CopyFn>(m,"ILW_BCCopy");bcFree=Native<FreeFn>(m,"ILW_BCFree");bcStop=Native<StopFn>(m,"ILW_BCStop");
   zipOffsetCross=typeof(ZipFile).GetField("offsetOfFirstEntry",BindingFlags.Instance|BindingFlags.NonPublic|BindingFlags.Public);if(zipOffsetCross==null)throw new Exception("zip metadata unavailable");
   var f=BindingFlags.Instance|BindingFlags.NonPublic|BindingFlags.Public;var h=new Harmony("local.vam.imageloadpipeline.cross");
   h.Patch(typeof(ImageLoaderThreaded).GetMethod("QueueImage",f),null,new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("CrossQueue")),null,null);
   h.Patch(typeof(ImageLoaderThreaded).GetMethod("Update",f),null,new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("CrossUpdate")),null,null);
   h.Patch(typeof(ImageLoaderThreaded.QueuedImage).GetMethod("Process",f),new HarmonyMethod(typeof(ImageLoadPipeline).GetMethod("CrossProcess")),null,null,null);
   logger.LogInfo("Image load cross-image pipeline on");
  }catch(Exception e){crossEnabled.Value=false;logger.LogError("Cross-image pipeline disabled: "+e);}
 }
 static int Flag(bool x){return x?1:0;}
 static bool CrossQueueOne(ImageLoaderThreaded loader,ImageLoaderThreaded.QueuedImage q){
  if(q==null||q.processed||q.cancel||q.forceReload||q.isThumbnail||q.createNormalFromBump||q.webRequest!=null||q.useWebCache||String.IsNullOrEmpty(q.imgPath)||q.imgPath=="NULL"||q.imgPath.StartsWith("http",StringComparison.OrdinalIgnoreCase))return false;
  lock(crossLock){if(crossByRequest.ContainsKey(q)||crossRejected.Contains(q))return true;if(crossAll.Count>=CrossMax){crossQueueFull++;return false;}}
  try{
   if(InMemory(loader,q)){lock(crossLock)crossRejected.Add(q);return false;}
   var fe=FileManager.GetFileEntry(q.imgPath,true);if(fe==null||!fe.Exists){lock(crossLock)crossRejected.Add(q);return false;}
   string cache=CacheManager.CachingEnabled?(string)diskPath.Invoke(q,null):null;if(!String.IsNullOrEmpty(cache)&&File.Exists(cache)&&File.Exists(cache+"meta")){lock(crossLock)crossRejected.Add(q);return false;}
   string file;long off=0,cs=0,size;int method=0,zip=0;var ve=fe as VarFileEntry;
   if(ve!=null&&!ve.Simulated&&ve.Package!=null&&ve.Package.ZipFile!=null){ZipFile z=ve.Package.ZipFile;ZipEntry e=z.GetEntry(ve.InternalSlashPath);if(e==null||e.IsCrypted||!e.IsFile||((int)e.CompressionMethod!=0&&(int)e.CompressionMethod!=8)||e.Size<=0||e.Size>32L*1024*1024||e.CompressedSize<0){lock(crossLock)crossRejected.Add(q);return false;}file=Path.GetFullPath(ve.Package.Path);off=(long)zipOffsetCross.GetValue(z)+e.Offset;cs=e.CompressedSize;size=e.Size;method=(int)e.CompressionMethod;zip=1;}
   else{file=Path.GetFullPath(fe.FullPath);var fi=new FileInfo(file);if(!fi.Exists||fi.Length<=0||fi.Length>32L*1024*1024){lock(crossLock)crossRejected.Add(q);return false;}size=fi.Length;}
   var f=new FileInfo(file);if(!f.Exists)return false;
   lock(crossLock){if(crossAll.Count>=CrossMax){crossQueueFull++;return false;}bool chain=NativeMipsOn();int id=chain?sourceSubmit2(file,off,cs,size,method,zip,Flag(q.compress),Flag(q.isNormalMap),Flag(q.invert),Flag(q.createAlphaFromGrayscale),Flag(q.setSize),Flag(q.fillBackground),q.width,q.height,Flag(q.compress),Flag(q.createMipMaps)):sourceSubmit(file,off,cs,size,method,zip,Flag(q.compress),Flag(q.isNormalMap),Flag(q.invert),Flag(q.createAlphaFromGrayscale),Flag(q.setSize),Flag(q.fillBackground),q.width,q.height);if(id<=0){crossRejected.Add(q);crossErrors++;return false;}var w=new CrossWork();w.q=q;w.key=Signature(q);w.file=file;w.expectedCache=cache;w.stamp=f.LastWriteTimeUtc.Ticks;w.fileLength=f.Length;w.sourceId=id;w.chained=chain&&q.compress;crossAll.Add(w);crossByRequest[q]=w;crossReserved+=CrossReservation;crossSubmitted++;return true;}
  }catch(Exception e){lock(crossLock){crossRejected.Add(q);crossErrors++;}logger.LogWarning("cross source prepare failed: "+e.Message);return false;}
 }
 public static void CrossQueue(ImageLoaderThreaded __instance,ImageLoaderThreaded.QueuedImage qi){if(crossEnabled!=null&&crossEnabled.Value&&Thread.CurrentThread.ManagedThreadId==mainThread)CrossQueueOne(__instance,qi);}
 static bool SourceDims(CrossWork w){if(w.dims)return true;if(sourcePoll(w.sourceId)!=1)return false;int size;if(sourceInfo(w.sourceId,out w.w,out w.h,out w.channels,out size)!=1||size<=0||size>128*1024*1024)return false;w.rawBytes=size;w.dims=true;return true;}
 static bool SourceToRaw(CrossWork w){if(w.raw!=null)return true;if(!SourceDims(w))return false;var data=new byte[w.rawBytes];fixed(byte* p=data)if(sourceCopy(w.sourceId,p,w.rawBytes)!=1)return false;w.raw=data;return true;}
 static void CrossPrepareBC(CrossWork w){
  var q=w.q;if(w.bcId!=0||!q.compress||w.raw==null||w.w<4||w.h<4||w.w%4!=0||w.h%4!=0||(q.createMipMaps&&(!PowerOfTwo(w.w)||!PowerOfTwo(w.h))))return;
  Texture2D tex=null;try{TextureFormat fmt=w.channels==4?TextureFormat.RGBA32:TextureFormat.RGB24;tex=new Texture2D(w.w,w.h,fmt,q.createMipMaps,q.linear);tex.LoadRawTextureData(w.raw);tex.Apply();byte[] mips=tex.GetRawTextureData();int mw=w.w,mh=w.h;long n=0;for(int i=0;i<tex.mipmapCount;i++){n+=(long)((mw+3)/4)*((mh+3)/4)*(w.channels==4?16:8);mw=Math.Max(1,mw/2);mh=Math.Max(1,mh/2);}if(n<=0||n>int.MaxValue)return;w.bcBytes=(int)n;fixed(byte* p=mips)w.bcId=bcSubmit(p,mips.Length,w.bcBytes,w.w,w.h,w.channels,tex.mipmapCount);if(w.bcId>0)crossScheduledBC++;else w.bcId=-1;}catch(Exception){w.bcId=-1;crossErrors++;}finally{if(tex!=null)UnityEngine.Object.Destroy(tex);}
 }
 static void CrossRetire(CrossWork w){if(w.retired)return;w.retired=true;crossByRequest.Remove(w.q);crossRetired++;}
 static void CrossCollect(){for(int i=crossAll.Count-1;i>=0;i--){var w=crossAll[i];if(!w.retired)continue;if(sourcePoll(w.sourceId)==0||(w.bcId>0&&bcPoll(w.bcId)==0)||(w.chained&&sourceBCPoll(w.sourceId)==0))continue;sourceFree(w.sourceId);if(w.bcId>0)bcFree(w.bcId);crossAll.RemoveAt(i);crossReserved-=CrossReservation;}}
 public static void CrossUpdate(ImageLoaderThreaded __instance){
  if(crossEnabled==null||!crossEnabled.Value)return;try{
   var queue=(LinkedList<ImageLoaderThreaded.QueuedImage>)queueField.GetValue(__instance);if(queue==null)return;
   lock(crossLock){foreach(var w in crossAll)if(!w.retired&&(w.q.cancel||w.q.processed||!queue.Contains(w.q)))CrossRetire(w);crossRejected.RemoveWhere(delegate(ImageLoaderThreaded.QueuedImage q){return q.cancel||q.processed||!queue.Contains(q);});if(crossCompressed.Count!=0)crossCompressed.RemoveWhere(delegate(ImageLoaderThreaded.QueuedImage q){return q.cancel||q.finished||!queue.Contains(q);});CrossCollect();}
   var n=queue.First;int scanned=0;while(n!=null&&scanned++<64){CrossQueueOne(__instance,n.Value);n=n.Next;}
   // At most one ready non-head image makes Unity mipmaps per Update. Workers only touch bytes.
   if(!NativeMipsOn()){n=queue.First;if(n!=null)n=n.Next;}else n=null;while(n!=null){lock(crossLock){CrossWork w;if(crossByRequest.TryGetValue(n.Value,out w)&&!w.retired&&w.bcId==0&&SourceToRaw(w)){CrossPrepareBC(w);break;}}n=n.Next;}
  }catch(Exception e){Interlocked.Increment(ref crossErrors);logger.LogWarning("cross update: "+e.Message);}
 }
 public static bool CrossProcess(ImageLoaderThreaded.QueuedImage __instance){
  if(crossEnabled==null||!crossEnabled.Value||!inImageWorker||__instance.processed)return true;
  CrossWork w;lock(crossLock){if(!crossByRequest.TryGetValue(__instance,out w))return true;}
  long t0=Stopwatch.GetTimestamp(),limit=t0+Stopwatch.Frequency*HeadWaitMs()/1000;bool waited=sourcePoll(w.sourceId)==0;
  // The head's native decode is already running in parallel; waiting for it is cheaper than starting VaM's own decode.
  while(sourcePoll(w.sourceId)==0&&Stopwatch.GetTimestamp()<limit)Thread.Sleep(1);
  if(w.chained&&sourcePoll(w.sourceId)==1){if(sourceBCPoll(w.sourceId)==0)waited=true;while(sourceBCPoll(w.sourceId)==0&&Stopwatch.GetTimestamp()<limit)Thread.Sleep(1);}
  if(waited)NoteHeadWait(Stopwatch.GetTimestamp()-t0);
  lock(crossLock){try{
   if(w.retired||__instance.cancel||Signature(__instance)!=w.key){CrossRetire(w);crossMiss++;return true;}
   var f=new FileInfo(w.file);if(!f.Exists||f.Length!=w.fileLength||f.LastWriteTimeUtc.Ticks!=w.stamp||(!String.IsNullOrEmpty(w.expectedCache)&&File.Exists(w.expectedCache))||!SourceDims(w)){CrossRetire(w);crossMiss++;return true;}
   byte[] raw=null;bool encoded=false;
   if(w.chained&&sourceBCPoll(w.sourceId)==1){int bn,levels;if(sourceBCInfo(w.sourceId,out bn,out levels)==1&&bn>0){byte[] b=new byte[bn];fixed(byte* p=b)if(sourceBCCopy(w.sourceId,p,bn)==1){raw=b;encoded=true;Interlocked.Increment(ref nativeChainApplied);}}}
   else if(w.bcId>0&&bcPoll(w.bcId)==1){byte[] b=new byte[w.bcBytes];fixed(byte* p=b)if(bcCopy(w.bcId,p,b.Length)==1){raw=b;encoded=true;}}
   if(!encoded){if(!SourceToRaw(w)){CrossRetire(w);crossMiss++;return true;}raw=w.raw;}
   __instance.width=w.w;__instance.height=w.h;__instance.textureFormat=encoded?(w.channels==4?TextureFormat.DXT5:TextureFormat.DXT1):(w.channels==4?TextureFormat.RGBA32:TextureFormat.RGB24);__instance.raw=raw;__instance.preprocessed=encoded;__instance.processed=true;
   if(encoded){crossCompressed.Add(__instance);crossCompressedApplied++;}crossApplied++;CrossRetire(w);return false;
  }catch(Exception){CrossRetire(w);crossErrors++;return true;}}
 }
 static bool TakeCrossCompressed(ImageLoaderThreaded.QueuedImage q){lock(crossLock)return q!=null&&crossCompressed.Remove(q);}
 static void StopCrossPipeline(){if(bcStop!=null)bcStop();if(sourceStop!=null)sourceStop();}
 public static string CrossStats(){lock(crossLock)return "sourceSubmitted="+crossSubmitted+" sourceApplied="+crossApplied+" sourceMiss="+crossMiss+" sourceErrors="+crossErrors+" bcScheduled="+crossScheduledBC+" bcApplied="+crossCompressedApplied+" sourcePeak="+(sourcePeak==null?0:sourcePeak())+" active="+crossAll.Count+" reservedMiB="+(crossReserved/1048576)+" full="+crossQueueFull+OffMainStats();}
}
