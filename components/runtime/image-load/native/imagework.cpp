#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#define STB_DXT_IMPLEMENTATION
#include "third_party/stb/stb_dxt.h"

using Run = void (*)(int, void (*)(void*, int, int), void*, int, int, int);
using Task = int (*)(const char*, int);
using Api = int (*)();
static Run run = nullptr;
static int pixelTask = -1, compressionTask = -1, readTask = -1;
static bool init() {
    if (run) return true;
    HMODULE m = GetModuleHandleW(L"VaMCpuBoostNative.bin");
    if (!m) return false;
    auto api = (Api)GetProcAddress(m,"CB_ApiVersion");
    auto task = (Task)GetProcAddress(m,"CB_Task");
    auto cb = (Run)GetProcAddress(m,"CB_Run");
    if (!api || !task || !cb || api() < 5) return false;
    pixelTask = task("image pixels candidate",0);
    compressionTask = task("image compression candidate",0);
    readTask = task("image cache reads candidate",0);
    run = cb;
    return true;
}
extern "C" __declspec(dllexport) int ILW_Api() { return 1; }
extern "C" __declspec(dllexport) int ILW_Init() { return init() ? 1 : 0; }

struct Pixels { uint8_t* p; int channels; int normal; int invert; int alpha; };
static void transform(void* ctx,int b,int e) {
    auto& c=*(Pixels*)ctx;
    for(int i=b;i<e;i++) {
        uint8_t* p=c.p+(size_t)i*c.channels;
        uint8_t t=p[0]; p[0]=p[2]; p[2]=t;
        if(c.normal && c.channels==4) p[3]=255;
        if(c.invert) for(int j=0;j<c.channels;j++) p[j]=(uint8_t)(255-p[j]);
        if(c.alpha && c.channels==4) p[3]=(uint8_t)(((int)p[0]+p[1]+p[2])/3);
    }
}
extern "C" __declspec(dllexport) int ILW_Pixels(uint8_t* p,int pixels,int channels,int normal,int invert,int alpha) {
    if(!p || pixels<0 || (channels!=3 && channels!=4) || (alpha && channels!=4)) return 0;
    Pixels c{p,channels,normal,invert,alpha};
    if(init() && pixels>=32768) run(pixelTask,transform,&c,0,pixels,8192);
    else transform(&c,0,pixels);
    return 1;
}

struct Blocks { const uint8_t* src; uint8_t* dst; int width; int channels; int alpha; };
static void compress(void* ctx,int b,int e) {
    auto& c=*(Blocks*)ctx;
    int across=c.width/4, outbytes=c.alpha?16:8;
    for(int i=b;i<e;i++) {
        int x=(i%across)*4, y=(i/across)*4;
        uint8_t rgba[64];
        for(int dy=0;dy<4;dy++) for(int dx=0;dx<4;dx++) {
            const uint8_t* p=c.src+((size_t)(y+dy)*c.width+x+dx)*c.channels;
            uint8_t* q=rgba+(dy*4+dx)*4;
            q[0]=p[0];q[1]=p[1];q[2]=p[2];q[3]=c.channels==4?p[3]:255;
        }
        stb_compress_dxt_block(c.dst+(size_t)i*outbytes,rgba,c.alpha,STB_DXT_HIGHQUAL);
    }
}
// Experimental compression: used only by the fidelity test, never by the live texture path.
extern "C" __declspec(dllexport) int ILW_DXT(const uint8_t* src,uint8_t* dst,int width,int height,int channels,int alpha) {
    if(!src || !dst || width<4 || height<4 || width%4 || height%4 || (channels!=3 && channels!=4) || (alpha && channels!=4)) return 0;
    Blocks c{src,dst,width,channels,alpha}; int n=(width/4)*(height/4);
    if(init() && n>=256) run(compressionTask,compress,&c,0,n,128); else compress(&c,0,n);
    return 1;
}

// Compress all Unity-generated mip levels. Each block is independent; edge pixels repeat for mips smaller than 4x4.
struct MipLevel { int w,h,across,blocks,first; size_t srcOffset,dstOffset; };
struct MipWork { const uint8_t* src;uint8_t* dst;int channels,alpha,nlevels;MipLevel levels[32]; };
static void compressMips(void* context,int begin,int end) {
    auto& c=*(MipWork*)context;
    for(int i=begin;i<end;i++) {
        int level=0;while(level+1<c.nlevels && i>=c.levels[level+1].first)level++;
        const auto& m=c.levels[level];int n=i-m.first,x=(n%m.across)*4,y=(n/m.across)*4;
        uint8_t rgba[64];
        for(int dy=0;dy<4;dy++)for(int dx=0;dx<4;dx++) {
            int sx=min(x+dx,m.w-1),sy=min(y+dy,m.h-1);
            const uint8_t* p=c.src+m.srcOffset+((size_t)sy*m.w+sx)*c.channels;
            uint8_t* q=rgba+(dy*4+dx)*4;q[0]=p[0];q[1]=p[1];q[2]=p[2];q[3]=c.channels==4?p[3]:255;
        }
        stb_compress_dxt_block(c.dst+m.dstOffset+(size_t)n*(c.alpha?16:8),rgba,c.alpha,STB_DXT_HIGHQUAL);
    }
}
extern "C" __declspec(dllexport) int ILW_DXTMips(const uint8_t* src,int srcBytes,uint8_t* dst,int dstBytes,int width,int height,int channels,int nlevels) {
    if(!src||!dst||width<=0||height<=0||width>16384||height>16384||(channels!=3&&channels!=4)||nlevels<=0||nlevels>32)return 0;
    MipWork c{};c.src=src;c.dst=dst;c.channels=channels;c.alpha=channels==4;c.nlevels=nlevels;
    size_t si=0,di=0;int total=0,w=width,h=height;
    for(int i=0;i<nlevels;i++) {
        int bx=(w+3)/4,by=(h+3)/4,blocks=bx*by;
        c.levels[i]={w,h,bx,blocks,total,si,di};
        si+=(size_t)w*h*channels;di+=(size_t)blocks*(c.alpha?16:8);total+=blocks;
        if(si>(size_t)srcBytes||di>(size_t)dstBytes)return 0;
        w=max(1,w/2);h=max(1,h/2);
    }
    if(si!=(size_t)srcBytes||di!=(size_t)dstBytes)return 0;
    if(init() && total>=256)run(compressionTask,compressMips,&c,0,total,128);else compressMips(&c,0,total);
    return 1;
}

// Native-only async cache reads. Shared workers never call managed code or attach to Mono.
struct ReadJob {
    std::wstring path;
    int size;
    std::vector<uint8_t> data;
    std::atomic<int> state{0}; // 0 pending/running, 1 ready, -1 failed
};
static std::mutex jobsLock;
static std::deque<std::shared_ptr<ReadJob>> pendingJobs;
static std::unordered_map<int,std::shared_ptr<ReadJob>> jobsById;
static HANDLE jobEvent = nullptr, driverHandle = nullptr;
static std::atomic<bool> stopping{false};
static int nextId=1;
static std::atomic<long> activeReads{0}, peakReads{0};
static void ReadOne(void* context,int begin,int end) {
    auto& batch=*(std::vector<std::shared_ptr<ReadJob>>*)context;
    for(int i=begin;i<end;i++) {
        auto j=batch[i]; long now=++activeReads, old=peakReads.load();
        while(now>old && !peakReads.compare_exchange_weak(old,now)) {}
        bool ok=false;
        HANDLE f=CreateFileW(j->path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(f!=INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if(GetFileSizeEx(f,&sz) && sz.QuadPart==j->size) {
                try {
                    j->data.resize((size_t)j->size); size_t pos=0;
                    while(pos<j->data.size()) {
                        DWORD count=(DWORD)min((size_t)(1<<20),j->data.size()-pos), got=0;
                        if(!ReadFile(f,j->data.data()+pos,count,&got,nullptr) || got==0) break;
                        pos+=got;
                    }
                    ok=pos==j->data.size();
                } catch(...) { ok=false; }
            }
            CloseHandle(f);
        }
        if(!ok) j->data.clear();
        j->state.store(ok?1:-1,std::memory_order_release);
        --activeReads;
    }
}
static DWORD WINAPI ReadDriver(void*) {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    while(!stopping.load()) {
        WaitForSingleObject(jobEvent,INFINITE);
        for(;;) {
            std::vector<std::shared_ptr<ReadJob>> batch;
            { std::lock_guard<std::mutex> l(jobsLock); while(!pendingJobs.empty()) { batch.push_back(pendingJobs.front()); pendingJobs.pop_front(); } }
            if(batch.empty()) break;
            if(run) run(readTask,ReadOne,&batch,0,(int)batch.size(),1); else ReadOne(&batch,0,(int)batch.size());
        }
    }
    return 0;
}
extern "C" __declspec(dllexport) int ILW_ReadSubmit(const wchar_t* path,int size) {
    if(!path || size<=0 || size>128*1024*1024 || !init()) return -1;
    std::lock_guard<std::mutex> l(jobsLock);
    if(stopping.load()) return -1;
    if(!driverHandle) {
        jobEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!jobEvent) return -1;
        driverHandle=CreateThread(nullptr,0,ReadDriver,nullptr,0,nullptr);
        if(!driverHandle) { CloseHandle(jobEvent);jobEvent=nullptr;return -1; }
    }
    auto j=std::make_shared<ReadJob>(); j->path=path;j->size=size;
    int id=nextId++;jobsById[id]=j;pendingJobs.push_back(j);SetEvent(jobEvent);return id;
}
static std::shared_ptr<ReadJob> getJob(int id) {
    std::lock_guard<std::mutex> l(jobsLock); auto i=jobsById.find(id);return i==jobsById.end()?nullptr:i->second;
}
extern "C" __declspec(dllexport) int ILW_ReadPoll(int id) {
    auto j=getJob(id);return j?j->state.load(std::memory_order_acquire):-1;
}
extern "C" __declspec(dllexport) int ILW_ReadCopy(int id,uint8_t* output,int size) {
    auto j=getJob(id);if(!j || j->state.load(std::memory_order_acquire)!=1 || !output || size!=j->size) return 0;
    memcpy(output,j->data.data(),(size_t)size);return 1;
}
extern "C" __declspec(dllexport) void ILW_ReadFree(int id) {
    std::lock_guard<std::mutex> l(jobsLock);jobsById.erase(id);
}
extern "C" __declspec(dllexport) long ILW_ReadPeak() { return peakReads.load(); }
extern "C" __declspec(dllexport) int ILW_ReadStop() {
    stopping.store(true);HANDLE event=jobEvent, thread=driverHandle;if(event)SetEvent(event);
    if(thread && WaitForSingleObject(thread,5000)!=WAIT_OBJECT_0) return 0;
    if(thread)CloseHandle(thread);if(event)CloseHandle(event);driverHandle=nullptr;jobEvent=nullptr;
    { std::lock_guard<std::mutex> l(jobsLock);jobsById.clear();pendingJobs.clear(); }
    return 1;
}
