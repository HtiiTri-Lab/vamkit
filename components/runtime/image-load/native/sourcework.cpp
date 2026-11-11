#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"ole32.lib")
using namespace Gdiplus;
using Run=void (*)(int,void (*)(void*,int,int),void*,int,int,int);
using Task=int (*)(const char*,int);
using ZipRead=int64_t (*)(const wchar_t*,int64_t,int64_t,int,uint8_t*,int64_t);
static Run srcRun=nullptr;static int srcTask=-1;static ZipRead zipRead=nullptr;static ULONG_PTR gdiplusToken=0;
static bool srcInit(){
 if(srcRun)return true;HMODULE cpu=GetModuleHandleW(L"VaMCpuBoostNative.bin"),scene=GetModuleHandleW(L"VaMSceneLoadNative.bin");if(!cpu||!scene)return false;
 auto api=(int(*)())GetProcAddress(cpu,"CB_ApiVersion");auto task=(Task)GetProcAddress(cpu,"CB_Task");auto r=(Run)GetProcAddress(cpu,"CB_Run");auto z=(ZipRead)GetProcAddress(scene,"SL_ReadZipEntry");
 if(!api||api()<5||!task||!r||!z)return false;GdiplusStartupInput input;if(GdiplusStartup(&gdiplusToken,&input,nullptr)!=Ok)return false;
 srcTask=task("image source prepare",0);zipRead=z;srcRun=r;return true;
}
struct SourceOptions{int compress,normal,invert,alpha,setSize,fill,width,height;};
struct SourceData{std::vector<uint8_t> raw;int width=0,height=0,channels=0;};
static int decode(const uint8_t* data,int n,const SourceOptions& o,SourceData& out){
 if(!data||n<=0||n>32*1024*1024)return -1;
 HGLOBAL mem=GlobalAlloc(GMEM_MOVEABLE,n);if(!mem)return -2;void* ptr=GlobalLock(mem);if(!ptr){GlobalFree(mem);return -2;}memcpy(ptr,data,n);GlobalUnlock(mem);
 IStream* stream=nullptr;if(CreateStreamOnHGlobal(mem,TRUE,&stream)!=S_OK){GlobalFree(mem);return -2;}
 int result=-3;
 {
 Bitmap* bitmap=Bitmap::FromStream(stream,FALSE);
 if(bitmap&&bitmap->GetLastStatus()==Ok){
  bitmap->RotateFlip(Rotate180FlipX);int w=o.setSize?o.width:(int)bitmap->GetWidth(),h=o.setSize?o.height:(int)bitmap->GetHeight();
  if(!o.setSize&&o.compress){w=(w/4?w/4:1)*4;h=(h/4?h/4:1)*4;}
  int channels=(o.alpha||o.normal||bitmap->GetPixelFormat()==PixelFormat32bppARGB)?4:3;
  if(w>0&&h>0&&(int64_t)w*h<=4096LL*4096){
   PixelFormat fmt=channels==4?PixelFormat32bppARGB:PixelFormat24bppRGB;
   Bitmap target(w,h,fmt);Graphics graphics(&target);Status status=target.GetLastStatus();
   if(o.setSize){if(o.fill){SolidBrush white(Color::White);graphics.FillRectangle(&white,0,0,w,h);}float ratio=min((float)w/bitmap->GetWidth(),(float)h/bitmap->GetHeight());int dw=(int)(bitmap->GetWidth()*ratio),dh=(int)(bitmap->GetHeight()*ratio);status=graphics.DrawImage(bitmap,(w-dw)/2,(h-dh)/2,dw,dh);}
   else status=graphics.DrawImage(bitmap,0,0,w,h);
   if(status==Ok){Rect rect(0,0,w,h);BitmapData bits{};if(target.LockBits(&rect,ImageLockModeRead,fmt,&bits)==Ok){
    int used=w*h*channels;size_t alloc=(size_t)ceilf((float)used*1.5f);out.raw.assign(alloc,0);memcpy(out.raw.data(),bits.Scan0,used);target.UnlockBits(&bits);
    for(int i=0;i<w*h;i++){uint8_t* p=out.raw.data()+(size_t)i*channels;uint8_t v=p[0];p[0]=p[2];p[2]=v;if(o.normal&&channels==4)p[3]=255;if(o.invert)for(int j=0;j<channels;j++)p[j]=(uint8_t)(255-p[j]);if(o.alpha&&channels==4)p[3]=(uint8_t)(((int)p[0]+p[1]+p[2])/3);}
    out.width=w;out.height=h;out.channels=channels;result=1;
   }}
  }
 }
 delete bitmap;
 }
 stream->Release();return result;
}
struct Job {std::wstring path;int64_t offset,csize,size;int method,zip;SourceOptions options;SourceData output;std::atomic<int> state{0};int wantBC=0,createMips=0,bcLevels=0;std::vector<uint8_t> bc;std::atomic<int> bcState{-1};};
static void chainBC(const std::shared_ptr<Job>& j);
static std::mutex jobLock;static std::deque<std::shared_ptr<Job>> queue;static std::unordered_map<int,std::shared_ptr<Job>> jobs;static int nextJob=1;static HANDLE eventHandle=nullptr,driver=nullptr;static std::atomic<bool> stopped{false};static std::atomic<int> active{0},peak{0};
static void oneSource(void* context,int begin,int end){auto& batch=*(std::vector<std::shared_ptr<Job>>*)context;for(int i=begin;i<end;i++){
 auto j=batch[i];int a=++active,old=peak.load();while(a>old&&!peak.compare_exchange_weak(old,a)){}
 int ok=-1;try{std::vector<uint8_t> bytes((size_t)j->size);if(j->zip){if(zipRead(j->path.c_str(),j->offset,j->csize,j->method,bytes.data(),j->size)==j->size)ok=decode(bytes.data(),(int)bytes.size(),j->options,j->output);}else{
 HANDLE f=CreateFileW(j->path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);if(f!=INVALID_HANDLE_VALUE){LARGE_INTEGER s{};DWORD read=0;if(GetFileSizeEx(f,&s)&&s.QuadPart==j->size&&ReadFile(f,bytes.data(),(DWORD)bytes.size(),&read,nullptr)&&read==bytes.size())ok=decode(bytes.data(),(int)bytes.size(),j->options,j->output);CloseHandle(f);}}
 }catch(...){ok=-1;}j->state.store(ok==1?1:-1,std::memory_order_release);if(j->wantBC){if(ok==1)chainBC(j);else j->bcState.store(-1,std::memory_order_release);}--active;
}}
static DWORD WINAPI sourceDriver(void*){SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);while(!stopped.load()){WaitForSingleObject(eventHandle,INFINITE);for(;;){std::vector<std::shared_ptr<Job>> batch;{std::lock_guard<std::mutex> l(jobLock);while(!queue.empty()){batch.push_back(queue.front());queue.pop_front();}}if(batch.empty())break;srcRun(srcTask,oneSource,&batch,0,(int)batch.size(),1);}}return 0;}
static int submitSource(const wchar_t* path,int64_t offset,int64_t csize,int64_t size,int method,int zip,int compress,int normal,int invert,int alpha,int setSize,int fill,int width,int height,int wantBC,int createMips){
 if(!path||size<=0||size>32*1024*1024||!srcInit())return -1;std::lock_guard<std::mutex> l(jobLock);if(stopped.load())return -1;
 if(!driver){eventHandle=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!eventHandle)return -1;driver=CreateThread(nullptr,0,sourceDriver,nullptr,0,nullptr);if(!driver){CloseHandle(eventHandle);eventHandle=nullptr;return -1;}}
 auto j=std::make_shared<Job>();j->path=path;j->offset=offset;j->csize=csize;j->size=size;j->method=method;j->zip=zip;j->options={compress,normal,invert,alpha,setSize,fill,width,height};j->wantBC=wantBC;j->createMips=createMips;j->bcState.store(wantBC?0:-1);int id=nextJob++;jobs[id]=j;queue.push_back(j);SetEvent(eventHandle);return id;
}
extern "C" __declspec(dllexport) int ILW_SourceSubmit(const wchar_t* path,int64_t offset,int64_t csize,int64_t size,int method,int zip,int compress,int normal,int invert,int alpha,int setSize,int fill,int width,int height){return submitSource(path,offset,csize,size,method,zip,compress,normal,invert,alpha,setSize,fill,width,height,0,0);}
// Same as ILW_SourceSubmit; when wantBC the decoded image continues natively into mipmaps + BC1/BC3.
extern "C" __declspec(dllexport) int ILW_SourceSubmit2(const wchar_t* path,int64_t offset,int64_t csize,int64_t size,int method,int zip,int compress,int normal,int invert,int alpha,int setSize,int fill,int width,int height,int wantBC,int createMips){return submitSource(path,offset,csize,size,method,zip,compress,normal,invert,alpha,setSize,fill,width,height,wantBC,createMips);}
static std::shared_ptr<Job> getSource(int id){std::lock_guard<std::mutex> l(jobLock);auto i=jobs.find(id);return i==jobs.end()?nullptr:i->second;}
extern "C" __declspec(dllexport) int ILW_SourcePoll(int id){auto j=getSource(id);return j?j->state.load(std::memory_order_acquire):-1;}
extern "C" __declspec(dllexport) int ILW_SourceInfo(int id,int* w,int* h,int* channels,int* n){auto j=getSource(id);if(!j||j->state.load(std::memory_order_acquire)!=1)return 0;*w=j->output.width;*h=j->output.height;*channels=j->output.channels;*n=(int)j->output.raw.size();return 1;}
extern "C" __declspec(dllexport) int ILW_SourceCopy(int id,uint8_t* dst,int n){auto j=getSource(id);if(!j||j->state.load(std::memory_order_acquire)!=1||!dst||n!=(int)j->output.raw.size())return 0;memcpy(dst,j->output.raw.data(),n);return 1;}
extern "C" __declspec(dllexport) void ILW_SourceFree(int id){std::lock_guard<std::mutex> l(jobLock);jobs.erase(id);}
extern "C" __declspec(dllexport) int ILW_SourcePeak(){return peak.load();}
extern "C" __declspec(dllexport) int ILW_SourceStop(){stopped.store(true);if(eventHandle)SetEvent(eventHandle);if(driver&&WaitForSingleObject(driver,5000)!=WAIT_OBJECT_0)return 0;if(driver)CloseHandle(driver);if(eventHandle)CloseHandle(eventHandle);driver=nullptr;eventHandle=nullptr;{std::lock_guard<std::mutex> l(jobLock);jobs.clear();queue.clear();}if(gdiplusToken)GdiplusShutdown(gdiplusToken);gdiplusToken=0;return 1;}
// The coordinator waits outside the shared pool; each mip chain still uses CpuBoost's block parallelism.
extern "C" int ILW_DXTMips(const uint8_t*,int,uint8_t*,int,int,int,int,int);
struct BCJob {std::vector<uint8_t> source,output;int width,height,channels,mips;std::atomic<int> state{0};};
extern "C" int ILW_MipDXT(const uint8_t*,int,uint8_t*,int,int,int,int,int);
static std::mutex bcLock;static std::deque<std::shared_ptr<BCJob>> bcQueue;static std::deque<std::shared_ptr<Job>> chainQueue;static std::unordered_map<int,std::shared_ptr<BCJob>> bcJobs;static int nextBC=1;static HANDLE bcEvent=nullptr,bcThread=nullptr;static std::atomic<bool> bcStopped{false};
static std::atomic<long> chainDone{0},chainFailed{0};
static void runChain(const std::shared_ptr<Job>& j){
 int r=-1;try{int w=j->output.width,h=j->output.height,c=j->output.channels;int levels=1;
  if(j->createMips){int m=w>h?w:h;while((1<<(levels-1))<m)levels++;}
  int64_t n=0;int mw=w,mh=h;for(int i=0;i<levels;i++){n+=(int64_t)((mw+3)/4)*((mh+3)/4)*(c==4?16:8);mw=mw>1?mw/2:1;mh=mh>1?mh/2:1;}
  if(n>0&&n<=128*1024*1024){j->bc.resize((size_t)n);if(ILW_MipDXT(j->output.raw.data(),(int)j->output.raw.size(),j->bc.data(),(int)n,w,h,c,levels)==1){j->bcLevels=levels;r=1;}}
 }catch(...){r=-1;}
 if(r!=1){std::vector<uint8_t>().swap(j->bc);chainFailed++;}else chainDone++;
 j->bcState.store(r,std::memory_order_release);
}
static DWORD WINAPI bcDriver(void*){SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);while(!bcStopped.load()){WaitForSingleObject(bcEvent,INFINITE);for(;;){std::shared_ptr<BCJob> j;std::shared_ptr<Job> cj;{std::lock_guard<std::mutex> l(bcLock);if(!chainQueue.empty()){cj=chainQueue.front();chainQueue.pop_front();}else if(!bcQueue.empty()){j=bcQueue.front();bcQueue.pop_front();}else break;}
 if(cj){runChain(cj);continue;}
 int r=ILW_DXTMips(j->source.data(),(int)j->source.size(),j->output.data(),(int)j->output.size(),j->width,j->height,j->channels,j->mips);j->source.clear();j->state.store(r==1?1:-1,std::memory_order_release);}}return 0;}
static bool ensureBCLocked(){if(bcThread)return true;bcEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!bcEvent)return false;bcThread=CreateThread(nullptr,0,bcDriver,nullptr,0,nullptr);if(!bcThread){CloseHandle(bcEvent);bcEvent=nullptr;return false;}return true;}
static bool PowerOfTwo(int n){return n>0&&(n&(n-1))==0;}
static void chainBC(const std::shared_ptr<Job>& j){
 int w=j->output.width,h=j->output.height;
 if(!j->options.compress||w<4||h<4||w%4||h%4||(j->createMips&&(!PowerOfTwo(w)||!PowerOfTwo(h)))){j->bcState.store(-1,std::memory_order_release);return;}
 std::lock_guard<std::mutex> l(bcLock);if(bcStopped.load()||!ensureBCLocked()){j->bcState.store(-1,std::memory_order_release);return;}chainQueue.push_back(j);SetEvent(bcEvent);
}
extern "C" __declspec(dllexport) int ILW_SourceBCPoll(int id){auto j=getSource(id);return j?j->bcState.load(std::memory_order_acquire):-1;}
extern "C" __declspec(dllexport) int ILW_SourceBCInfo(int id,int* n,int* levels){auto j=getSource(id);if(!j||j->bcState.load(std::memory_order_acquire)!=1)return 0;*n=(int)j->bc.size();*levels=j->bcLevels;return 1;}
extern "C" __declspec(dllexport) int ILW_SourceBCCopy(int id,uint8_t* dst,int n){auto j=getSource(id);if(!j||j->bcState.load(std::memory_order_acquire)!=1||!dst||n!=(int)j->bc.size())return 0;memcpy(dst,j->bc.data(),n);return 1;}
extern "C" __declspec(dllexport) long ILW_SourceBCStats(int which){return which==0?chainDone.load():chainFailed.load();}
extern "C" __declspec(dllexport) int ILW_BCSubmit(const uint8_t* data,int len,int outlen,int width,int height,int channels,int mips){
 if(!data||len<=0||len>128*1024*1024||outlen<=0||outlen>128*1024*1024||!srcInit())return -1;
 std::lock_guard<std::mutex> l(bcLock);if(bcStopped.load())return -1;
 if(!ensureBCLocked())return -1;
 try{auto j=std::make_shared<BCJob>();j->source.assign(data,data+len);j->output.resize(outlen);j->width=width;j->height=height;j->channels=channels;j->mips=mips;int id=nextBC++;bcJobs[id]=j;bcQueue.push_back(j);SetEvent(bcEvent);return id;}catch(...){return -1;}
}
static std::shared_ptr<BCJob> getBC(int id){std::lock_guard<std::mutex> l(bcLock);auto i=bcJobs.find(id);return i==bcJobs.end()?nullptr:i->second;}
extern "C" __declspec(dllexport) int ILW_BCPoll(int id){auto j=getBC(id);return j?j->state.load(std::memory_order_acquire):-1;}
extern "C" __declspec(dllexport) int ILW_BCCopy(int id,uint8_t* dst,int n){auto j=getBC(id);if(!j||j->state.load(std::memory_order_acquire)!=1||!dst||n!=(int)j->output.size())return 0;memcpy(dst,j->output.data(),n);return 1;}
extern "C" __declspec(dllexport) void ILW_BCFree(int id){std::lock_guard<std::mutex> l(bcLock);bcJobs.erase(id);}
extern "C" __declspec(dllexport) int ILW_BCStop(){bcStopped.store(true);if(bcEvent)SetEvent(bcEvent);if(bcThread&&WaitForSingleObject(bcThread,5000)!=WAIT_OBJECT_0)return 0;if(bcThread)CloseHandle(bcThread);if(bcEvent)CloseHandle(bcEvent);bcThread=nullptr;bcEvent=nullptr;{std::lock_guard<std::mutex> l(bcLock);bcJobs.clear();bcQueue.clear();chainQueue.clear();}return 1;}
