using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using BepInEx.Configuration;
using MVR.FileManagement;
using UnityEngine;

// Step 6 (ImageLoadOffMain): the head waits for its running native decode, the decoded image continues natively
// into mipmaps + BC, and the disk-cache write of our compressed textures leaves the main thread.
public sealed unsafe partial class ImageLoadPipeline {
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SourceSubmit2Fn([MarshalAs(UnmanagedType.LPWStr)] string path,long offset,long csize,long size,int method,int zip,int compress,int normal,int invert,int alpha,int setSize,int fill,int width,int height,int wantBC,int createMips);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int BCInfoFn(int id,out int n,out int levels);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int BuildMipsFn(byte* src,int srcBytes,byte* dst,int dstBytes,int w,int h,int c,int levels,int mode);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int MipDXTFn(byte* src,int srcBytes,byte* dst,int dstBytes,int w,int h,int c,int levels);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void SetModeFn(int mode);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int StatFn(int which);
 static ConfigEntry<bool> nativeMipsEnabled,asyncWriteEnabled;static ConfigEntry<int> headWaitMs,mipMode;
 static SourceSubmit2Fn sourceSubmit2;static PollFn sourceBCPoll;static BCInfoFn sourceBCInfo;static CopyFn sourceBCCopy;static BuildMipsFn buildMips;static MipDXTFn mipDXT;static SetModeFn setMipMode;static StatFn chainStat;
 static bool nativeMipsReady;
 static long headWaits,headWaitTicks,headWaitMax,nativeChainApplied,nativeMipFinish,nativeMipFinishFailed;
 static bool NativeMipsOn(){return nativeMipsReady&&nativeMipsEnabled!=null&&nativeMipsEnabled.Value;}
 static int HeadWaitMs(){return headWaitMs==null?30:Math.Max(0,Math.Min(30000,headWaitMs.Value));}
 static void NoteHeadWait(long ticks){Interlocked.Increment(ref headWaits);Interlocked.Add(ref headWaitTicks,ticks);long m;while(ticks>(m=Interlocked.Read(ref headWaitMax))&&Interlocked.CompareExchange(ref headWaitMax,ticks,m)!=m){}}
 void BindOffMain(){
  nativeMipsEnabled=Config.Bind("ImageLoad","NativeMipmaps",false,"Build mipmaps natively (same box filter as Unity) and compress decoded images without main-thread Unity work");
  asyncWriteEnabled=Config.Bind("ImageLoad","AsyncCacheWrite",false,"Write the disk cache of our compressed textures on a background thread (temporary file, then rename)");
  headWaitMs=Config.Bind("ImageLoad","HeadWaitMs",30,"How long the queue head waits for its native decode before VaM decodes it again (30 = previous behaviour)");
  mipMode=Config.Bind("ImageLoadTest","MipMode",2,"Native mipmap filter: 2 = Unity's own (row pairs then rows, truncated); 0/1 = test boxes");
  if(asyncWriteEnabled.Value)StartWriter();
 }
 static void SetUpOffMainNative(IntPtr m){
  if(nativeMipsEnabled==null||!nativeMipsEnabled.Value)return;
  try{
   sourceSubmit2=Native<SourceSubmit2Fn>(m,"ILW_SourceSubmit2");sourceBCPoll=Native<PollFn>(m,"ILW_SourceBCPoll");sourceBCInfo=Native<BCInfoFn>(m,"ILW_SourceBCInfo");sourceBCCopy=Native<CopyFn>(m,"ILW_SourceBCCopy");
   buildMips=Native<BuildMipsFn>(m,"ILW_BuildMips");mipDXT=Native<MipDXTFn>(m,"ILW_MipDXT");setMipMode=Native<SetModeFn>(m,"ILW_SetMipMode");chainStat=Native<StatFn>(m,"ILW_SourceBCStats");
   setMipMode(mipMode.Value);nativeMipsReady=true;logger.LogInfo("Image load pipeline: native mipmaps on (mode "+mipMode.Value+"), head waits up to "+HeadWaitMs()+" ms");
  }catch(Exception e){nativeMipsReady=false;logger.LogError("Image load pipeline: native mipmaps off: "+e.Message);}
 }
 static int MipLevels(int w,int h,bool mips){if(!mips)return 1;int m=Math.Max(w,h),n=1;while((1<<(n-1))<m)n++;return n;}
 static long BlockBytes(int w,int h,int levels,bool alpha){long n=0;for(int i=0;i<levels;i++){n+=(long)((w+3)/4)*((h+3)/4)*(alpha?16:8);w=Math.Max(1,w/2);h=Math.Max(1,h/2);}return n;}
 // Main-thread fallback for heads that did not go through the cross pipeline: native mips + BC in one call.
 static bool NativeMipCompress(ImageLoaderThreaded.QueuedImage q,int channels,out byte[] encoded){
  encoded=null;if(!NativeMipsOn())return false;
  int levels=MipLevels(q.width,q.height,q.createMipMaps);long baseN=(long)q.width*q.height*channels;if(q.raw.LongLength<baseN)return false;
  long n=BlockBytes(q.width,q.height,levels,channels==4);if(n<=0||n>int.MaxValue)return false;
  var dst=new byte[(int)n];int ok;fixed(byte* s=q.raw)fixed(byte* d=dst)ok=mipDXT(s,q.raw.Length,d,dst.Length,q.width,q.height,channels,levels);
  if(ok!=1){Interlocked.Increment(ref nativeMipFinishFailed);return false;}
  Interlocked.Increment(ref nativeMipFinish);encoded=dst;return true;
 }

 // ---------- background disk-cache writer ----------
 sealed class CacheWrite{public string path,meta;public byte[] data;}
 static readonly object writeLock=new object();static readonly Queue<CacheWrite> writes=new Queue<CacheWrite>();static readonly HashSet<string> writePending=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
 static Thread writer;static bool writerStop;static long writesDone,writesFailed,writesSkipped,writeTicks,writeBytes,writeMaxPending;
 const string TmpSuffix=".ilptmp";
 static void StartWriter(){
  lock(writeLock){if(writer!=null)return;writer=new Thread(WriterLoop);writer.IsBackground=true;writer.Priority=System.Threading.ThreadPriority.BelowNormal;writer.Name="image cache writer";writer.Start();}
 }
 static void WriterLoop(){
  try{string dir=CacheManager.GetTextureCacheDir();if(!String.IsNullOrEmpty(dir)&&Directory.Exists(dir))foreach(string f in Directory.GetFiles(dir,"*"+TmpSuffix))try{File.Delete(f);}catch{}}catch{}
  for(;;){
   CacheWrite w;
   lock(writeLock){while(writes.Count==0&&!writerStop)Monitor.Wait(writeLock);if(writes.Count==0)return;w=writes.Dequeue();}
   long t=Stopwatch.GetTimestamp();string tmpData=w.path+TmpSuffix,tmpMeta=w.path+"meta"+TmpSuffix;
   try{
    if(File.Exists(w.path)){Interlocked.Increment(ref writesSkipped);}
    else{
     File.WriteAllBytes(tmpData,w.data);File.WriteAllText(tmpMeta,w.meta);
     // VaM's order: meta first, then data; VaM reads the meta only when the data exists.
     if(!File.Exists(w.path+"meta"))File.Move(tmpMeta,w.path+"meta");
     if(!File.Exists(w.path))File.Move(tmpData,w.path);
     Interlocked.Increment(ref writesDone);Interlocked.Add(ref writeBytes,w.data.Length);
    }
   }catch(Exception e){Interlocked.Increment(ref writesFailed);try{logger.LogWarning("Image cache write failed "+w.path+": "+e.Message);}catch{}}
   finally{try{if(File.Exists(tmpData))File.Delete(tmpData);}catch{}try{if(File.Exists(tmpMeta))File.Delete(tmpMeta);}catch{}lock(writeLock)writePending.Remove(w.path);Interlocked.Add(ref writeTicks,Stopwatch.GetTimestamp()-t);}
  }
 }
 // true when queued (or already pending); false = caller writes synchronously as before.
 static bool QueueCacheWrite(string path,string meta,byte[] data){
  if(asyncWriteEnabled==null||!asyncWriteEnabled.Value||writer==null||data==null)return false;
  lock(writeLock){if(writerStop)return false;if(writePending.Contains(path))return true;writePending.Add(path);writes.Enqueue(new CacheWrite{path=path,meta=meta,data=data});if(writes.Count>writeMaxPending)writeMaxPending=writes.Count;Monitor.Pulse(writeLock);}
  return true;
 }
 static bool CacheWritePending(string path){lock(writeLock)return writePending.Contains(path);}
 static void StopWriter(){
  Thread t;lock(writeLock){writerStop=true;Monitor.PulseAll(writeLock);t=writer;}
  if(t!=null&&!t.Join(15000))logger.LogWarning("Image cache writer still busy at quit");
 }
 public static string WriterIdle(){lock(writeLock)return writes.Count==0&&writePending.Count==0?"idle":"pending "+writePending.Count;}
 static string OffMainStats(){
  return " headWaits="+headWaits+" headWaitMs="+(headWaitTicks*1000.0/Stopwatch.Frequency).ToString("F0")+" headWaitMaxMs="+(headWaitMax*1000.0/Stopwatch.Frequency).ToString("F0")
   +" nativeChain="+nativeChainApplied+" chainDone="+(chainStat==null?0:chainStat(0))+" chainFailed="+(chainStat==null?0:chainStat(1))+" nativeMipFinish="+nativeMipFinish+" nativeMipFailed="+nativeMipFinishFailed
   +" writes="+writesDone+" writeSkipped="+writesSkipped+" writeFailed="+writesFailed+" writeMiB="+(writeBytes/1048576.0).ToString("F0")+" writeMs="+(writeTicks*1000.0/Stopwatch.Frequency).ToString("F0")+" writeMaxPending="+writeMaxPending;
 }

 // Test: dump random base levels and Unity's mip chain for offline fitting of Unity's filter.
 public static string MipDump(string dir){
  Directory.CreateDirectory(dir);var rng=new System.Random(777);int n=0;
  foreach(var sz in new[]{new[]{4,4},new[]{16,16},new[]{8,2},new[]{1,4},new[]{256,256}})foreach(int c in new[]{3,4})foreach(bool linear in new[]{false,true}){
   int w=sz[0],h=sz[1],levels=MipLevels(w,h,true);long chain=0;{int a=w,b=h;for(int i=0;i<levels;i++){chain+=(long)a*b*c;a=Math.Max(1,a/2);b=Math.Max(1,b/2);}}
   var full=new byte[chain];for(int i=0;i<w*h*c;i++)full[i]=(byte)rng.Next(256);
   var tex=new Texture2D(w,h,c==4?TextureFormat.RGBA32:TextureFormat.RGB24,true,linear);
   try{tex.LoadRawTextureData(full);tex.Apply(true);string nm=w+"x"+h+"_c"+c+"_"+(linear?"lin":"srgb");File.WriteAllBytes(Path.Combine(dir,nm+".bin"),tex.GetRawTextureData());File.WriteAllBytes(Path.Combine(dir,nm+".base"),full);n++;}finally{UnityEngine.Object.Destroy(tex);}
  }
  // also: a constant-pair ramp to read the filter directly: pixel pairs (a,b) for all a,b in steps
  {int w=512,h=2;foreach(bool linear in new[]{false,true}){long rc=0;{int a2=w,b2=h;for(int i=0;i<MipLevels(w,h,true);i++){rc+=(long)a2*b2*4;a2=Math.Max(1,a2/2);b2=Math.Max(1,b2/2);}}var full=new byte[rc];int k=0;for(int x=0;x<w;x++){byte a=(byte)((x*7)&255),b=(byte)((x*13+91)&255);full[(0*w+x)*4]=a;full[(1*w+x)*4]=b;full[(0*w+x)*4+1]=a;full[(1*w+x)*4+1]=a;full[(0*w+x)*4+2]=b;full[(1*w+x)*4+2]=b;full[(0*w+x)*4+3]=a;full[(1*w+x)*4+3]=b;k++;}
   var tex=new Texture2D(w,h,TextureFormat.RGBA32,true,linear);try{tex.LoadRawTextureData(full);tex.Apply(true);File.WriteAllBytes(Path.Combine(dir,"ramp_"+(linear?"lin":"srgb")+".bin"),tex.GetRawTextureData());File.WriteAllBytes(Path.Combine(dir,"ramp_base.bin"),full);n++;}finally{UnityEngine.Object.Destroy(tex);}}}
  return "dumped "+n;
 }
 // ---------- test: native mip chain against Unity's Apply(updateMipmaps) ----------
 public static string MipCheck(){
  if(buildMips==null)return "ERROR: native mipmaps not loaded";
  var sb=new StringBuilder();var rng=new System.Random(12345);int[][] sizes={new[]{4,4},new[]{8,2},new[]{2,8},new[]{1,4},new[]{64,64},new[]{256,128},new[]{128,512},new[]{1024,1024},new[]{4096,4096}};
  long[] bad=new long[3];const int Modes=3;long total=0;string[] first=new string[3];
  foreach(var sz in sizes)foreach(int c in new[]{3,4})foreach(bool linear in new[]{false,true})foreach(int pattern in new[]{0,1}){
   if(sz[0]==4096&&(c==3||pattern==1))continue;
   int w=sz[0],h=sz[1],levels=MipLevels(w,h,true);long chain=0;{int a=w,b=h;for(int i=0;i<levels;i++){chain+=(long)a*b*c;a=Math.Max(1,a/2);b=Math.Max(1,b/2);}}
   var full=new byte[chain];int baseN=w*h*c;
   for(int i=0;i<baseN;i++)full[i]=pattern==0?(byte)rng.Next(256):(byte)(((i/c)%w*255/Math.Max(1,w-1)+(i%c)*37)&255);
   var tex=new Texture2D(w,h,c==4?TextureFormat.RGBA32:TextureFormat.RGB24,true,linear);byte[] unity;
   try{tex.LoadRawTextureData(full);tex.Apply(true);unity=tex.GetRawTextureData();if(tex.mipmapCount!=levels){sb.Append(w+"x"+h+" unity levels "+tex.mipmapCount+" vs "+levels+"; ");continue;}}finally{UnityEngine.Object.Destroy(tex);}
   if(unity.LongLength!=chain){sb.Append(w+"x"+h+" unity bytes "+unity.Length+" vs "+chain+"; ");continue;}
   for(int mode=0;mode<Modes;mode++){
    var mine=new byte[chain];int ok;fixed(byte* s=full)fixed(byte* d=mine)ok=buildMips(s,full.Length,d,mine.Length,w,h,c,levels,mode);
    if(ok!=1){bad[mode]+=chain;continue;}
    for(long i=baseN;i<chain;i++)if(mine[i]!=unity[i]){bad[mode]++;if(first[mode]==null){first[mode]=w+"x"+h+" c"+c+" linear="+linear+" byte "+i+" unity="+unity[i]+" native="+mine[i];}}
   }
   total+=chain-baseN;
  }
  for(int mode=0;mode<Modes;mode++)sb.Append("mode"+mode+"="+bad[mode]+"/"+total+(first[mode]==null?"":" first: "+first[mode])+"; ");
  return sb.ToString();
 }
}
