// Desktop DXGI owner. Unity sees D3D11 wrapped surfaces; the presenter owns the
// flip-model D3D12 chain. The frame generators (DLSS Frame Generation and FSR's,
// xrfg/d3d12_frame_generator.hpp) are compiled from OFXR's source files.
//
// v2 (2026-11-04, user: "帧率不理想，波动极大" / "总会闪出windows后台"):
// - The presenter has a HIGH-priority D3D12 queue of its own, like OFXR's
//   synthesis queue in VR. DLSS-G, the copies into the swapchain and the
//   swapchain's presents run there. v1 ran them on Unity's 11on12 queue and
//   drained that queue after every step, so each generated frame waited behind
//   Unity's next frame and the vsync waits of the presents stalled Unity's GPU
//   work (OC's "output" stage read 7-21 ms).
// - Unity's queue waits (on the GPU) only until the presenter has taken its own
//   copy of the frame; the presenter shows that copy, so Unity renders the next
//   frame while the current one is still being shown.
// - Generated frames are spread evenly over the measured real frame period
//   (at least one refresh apart) instead of one refresh apart plus repeats.
// - A ResizeBuffers to the size the buffers already have (Unity does this when
//   its window is activated again) keeps buffers, frames and DLSS-G. v1 tore it
//   all down; the window had no presented image until DLSS-G was made again
//   (0.2-0.4 s) and showed the desktop behind it.
// - Foreground changes and every ResizeBuffers go to dlssg.log.
//
// v3 (2026-11-12): a stall in presentation no longer stops VaM. With v2 it could leave VaM "not responding", the GPU
// nearly idle, Unity's thread inside 11on12 and the presenter in a fence wait:
// - Unity's queue no longer waits on the GPU for the presenter's queue. In v2 it waited for the presenter's copy of
//   the last frame, and that copy sits on the presenter's queue behind the copies into the swap chain's buffers, which
//   the GPU holds until the presentation engine frees those buffers. So any stall in presentation stopped Unity's
//   queue, then Unity's thread in 11on12, then the main thread. Now Unity's frames go through a ring of three inputs:
//   Unity writes one whose last read by the presenter has completed (checked on the CPU, not waited for), the
//   presenter reads another.
// - Unity still waits for the presenter to take its last frame (that wait is the pacing), but for at most a second;
//   after that it replaces the frame the presenter has not taken and goes on, so the window keeps answering.
// - Every CPU wait on a fence uses an event: one that lasts more than 2 s is logged with the fence, the value and how
//   far it got; so are latency-object timeouts, slow Present calls and the presenter's current step.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "desktop_presenter.h"
#include <d3d11on12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "xrfg/d3d12_dlssg.hpp"
#include "xrfg/d3d12_frame_generator.hpp"
#include "xrfg/provider_api.hpp"
#include "../../shading/rt-core/include/startup_display.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
template<class T>using Ptr=ComPtr<T>;
void Check(HRESULT h){if(FAILED(h))throw h;}
std::wstring Env(const wchar_t* name){wchar_t b[32768]{};DWORD n=GetEnvironmentVariableW(name,b,32768);return n&&n<32768?std::wstring(b,n):L"";}
double Ms(){static double f=[](){LARGE_INTEGER q;QueryPerformanceFrequency(&q);return double(q.QuadPart);}();LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart*1000./f;}
DXGI_FORMAT Raw(DXGI_FORMAT f){switch(f){case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return DXGI_FORMAT_R8G8B8A8_UNORM;case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return DXGI_FORMAT_B8G8R8A8_UNORM;default:return f;}}
Ptr<ID3D11Device> unity;
Ptr<ID3D12Device> device;
Ptr<ID3D12CommandQueue> queue;         // Unity's 11on12 queue
Ptr<ID3D12CommandQueue> presentQueue;  // the presenter's own, HIGH priority
std::mutex publicationMutex;
OFXR_D3D11GuidePublicationV3 publication;
Ptr<ID3D11Texture2D> publishedDepth,publishedMotion;
std::atomic<UINT> multiplier{2};
// The most frames per real frame the generator in use makes (FSR: 2; DLSS-G: what the card's does, up to 4); 4 until
// one exists. The frames shown per real frame are the smaller of this and the multiplier.
std::atomic<UINT> generatorMost{4};
std::atomic<bool> active{false};
std::wstring config,performance;
std::mutex captureMutex;
std::wstring capturePath;
std::atomic<ID3D11Resource*> windowSurface{nullptr};
std::atomic<HWND> gameWindow{nullptr};
struct Surface{Ptr<ID3D12Resource> native;Ptr<ID3D11Resource> wrapped;};

std::string Narrow(const wchar_t* w){char b[512]{};WideCharToMultiByte(CP_UTF8,0,w,-1,b,sizeof b-1,nullptr,nullptr);return b;}
std::string WindowOwner(HWND w){
    if(!w)return "none";
    DWORD pid=0;GetWindowThreadProcessId(w,&pid);
    wchar_t exe[MAX_PATH]=L"?";
    if(HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)){DWORD n=MAX_PATH;if(!QueryFullProcessImageNameW(p,0,exe,&n))wcscpy_s(exe,L"?");CloseHandle(p);}
    const wchar_t* name=wcsrchr(exe,L'\\');name=name?name+1:exe;
    wchar_t cls[96]{},title[48]{};GetClassNameW(w,cls,96);GetWindowTextW(w,title,48);
    char out[800];snprintf(out,sizeof out,"%s pid=%lu class=%s title=\"%s\"%s%s",Narrow(name).c_str(),pid,Narrow(cls).c_str(),Narrow(title).c_str(),
        IsWindowVisible(w)?"":" hidden",w==gameWindow.load()?" [VaM]":"");
    return out;
}
void CALLBACK OnForeground(HWINEVENTHOOK,DWORD,HWND w,LONG object,LONG,DWORD,DWORD){
    if(object!=OBJID_WINDOW)return;
    xrfg::dlssg_log("desktop foreground -> %s",WindowOwner(w).c_str());
}
// Windows' verdict "not responding" on VaM's window, logged when it changes: with
// the input idle time, the window thread's GUI state and how long it took to
// answer a sent WM_NULL. With the keepalive below, it should only appear while
// the main thread is really busy (scene loading).
void CheckHung(bool& was){
    HWND w=gameWindow.load();if(!w)return;
    bool hung=IsHungAppWindow(w)!=FALSE;if(hung==was)return;was=hung;
    LASTINPUTINFO li{sizeof li};GetLastInputInfo(&li);
    DWORD tid=GetWindowThreadProcessId(w,nullptr);GUITHREADINFO gui{sizeof gui};GetGUIThreadInfo(tid,&gui);
    ULONGLONG t0=GetTickCount64();DWORD_PTR r=0;BOOL answered=SendMessageTimeoutW(w,WM_NULL,0,0,SMTO_NORMAL,2000,&r)!=0;
    xrfg::dlssg_log("desktop VaM window %s by Windows; window thread %lu (gui flags 0x%lx); last input %lu ms ago; WM_NULL %s after %llu ms",
        hung?"judged NOT RESPONDING":"responding again",tid,gui.flags,GetTickCount()-li.dwTime,answered?"answered":"unanswered",GetTickCount64()-t0);
}
void StartForegroundLog(){
    std::thread([]{
        SetThreadDescription(GetCurrentThread(),L"VaM desktop foreground log");
        HWINEVENTHOOK hook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,OnForeground,0,0,WINEVENT_OUTOFCONTEXT);
        // Keepalive: Windows judges VaM "not responding" whenever its main thread
        // has taken no message for 5 s, even while it presents every frame and
        // answers sent messages at once (Unity peeks every frame; an empty peek
        // does not count). With no input that is every 5 s of watching; a
        // not-responding foreground window gets ghosted and may lose the focus.
        // A WM_NULL posted each second is taken by Unity's per-frame peek. A main
        // thread that really stops takes none and is still judged so.
        SetTimer(nullptr,0,250,nullptr);bool hung=false;unsigned tick=0;
        MSG m;while(GetMessageW(&m,nullptr,0,0)>0){
            if(m.message!=WM_TIMER)continue;
            if(HWND w=gameWindow.load();w&&++tick%4==0)PostMessageW(w,WM_NULL,0,0);
            CheckHung(hung);
        }
        if(hook)UnhookWinEvent(hook);
    }).detach();
}
class Swapchain final:public IDXGISwapChain4 {
    std::atomic<ULONG> refs{1};
    Ptr<IDXGISwapChain4> real;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11On12Device> on12;
    DXGI_SWAP_CHAIN_DESC desc{};
    std::vector<Surface> app;
    // Unity's frames for the presenter: the frame (color), the compositor's guides (depth, motion) and their
    // description. ready: the ready-fence value after Unity wrote it; read: the present-fence value after the
    // presenter's last read of it (0: never read). resized: the guides changed size with this frame.
    struct Input{Surface color,depth,motion;xrfg::DlssMotionVectorFrame guides;UINT64 ready=0,read=0;bool resized=false;};
    static constexpr int kInputs=3;
    Input inputs[kInputs];
    UINT guideWidth=0,guideHeight=0;     // Unity's side: the size of the guides last copied
    Ptr<ID3D12Resource> shown;          // the presenter's copy of the real frame
    Ptr<ID3D12Resource> generated[3];
    Ptr<ID3D12DescriptorHeap> rtvHeap;
    std::unique_ptr<xrfg::D3D12GuidedFrameGenerator> generator;
    xrfg::FrameGenModel asked=xrfg::FrameGenModel::dlss;   // [Performance] FrameGenModel the generator was made for
    struct Slot{Ptr<ID3D12CommandAllocator> allocator;Ptr<ID3D12GraphicsCommandList> list;UINT64 value=0;};
    Slot slots[8];
    UINT slotNext=0;
    Slot* open=nullptr;
    ID3D12GraphicsCommandList* list=nullptr;
    Ptr<ID3D12Fence> readyFence,presentFence;
    std::atomic<UINT64> readyValue{0};   // signaled on Unity's queue
    std::atomic<UINT64> presentValue{0}; // signaled on the presenter queue
    HANDLE pauseEvent=nullptr,timer=nullptr,latency=nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    // Under `mutex`. pendingInput: written by Unity, not yet taken; takenInput: taken by the presenter, its read
    // not yet submitted; writtenLast: the input Unity wrote last. writing: Unity writes an input; busy: the presenter
    // works on a frame; exclusive: a resize or teardown owns everything; stalled: Unity stopped waiting for the
    // presenter (the time it started to wait in stalledSince) and does not wait again until the presenter takes one.
    int pendingInput=-1,takenInput=-1,writtenLast=-1;
    bool stop=false,writing=false,busy=false,exclusive=false,stalled=false;
    double stalledSince=0;
    // What the presenter is doing and since when, for the logs of long waits.
    std::atomic<const char*> step{"starting"};
    std::atomic<double> stepSince{0};
    unsigned latencyMisses=0;double latencyLogged=0;
    std::atomic<DWORD> presenterThread{0};
    std::thread thread;
    UINT sync=0,flags=0,index=0;
    ULONGLONG nextRead=0,retryAt=0;
    FILETIME configStamp{},performanceStamp{};
    bool framegen=true;
    int lite=-1;
    std::wstring overlay=L"off";
    std::wstring presentedCapture;
    double refresh=160;
    double period=0,lastReady=0,lastPresent=0;  // pacing: real frame period estimate (ms)
    bool readyKnown=false;
    UINT64 serial=0,displayed=0,baseCount=0,seq=0,kept=0;
    std::mutex flightMutex;
    double fpsAt=0,realFps=0,displayFps=0;
    FILE* flight=nullptr;
    Ptr<ID3D12Resource> overlayUpload;
    unsigned char* overlayPixels=nullptr;
    static constexpr UINT overlayW=184,overlayH=24,overlayPitch=768;

    void Trace(const char* op,UINT count=0,char phase='I',double duration=0,UINT64 a=0,UINT64 b=0,UINT64 c=0){
        if(!flight)return;std::lock_guard lock(flightMutex);
        if(!strcmp(op,"generated_present")||!strcmp(op,"real_present")||!strcmp(op,"repeat_present")){c=!strcmp(op,"generated_present")?2:!strcmp(op,"real_present")?1:0;op="presenter_submission";a=serial;b=multiplier.load();}
        else if(!strcmp(op,"generated")){op="desktop_generated";a=serial;b=multiplier.load();c=count;}
        fprintf(flight,"seq=%llu ms=%.3f tid=%lu phase=%c op=%s result=%u dur_us=%u a=%llu b=%llu c=%llu\n",++seq,Ms(),GetCurrentThreadId(),phase,op,count,unsigned(duration*1000),a,b,c);fflush(flight);
    }
    void Step(const char* s){step=s;stepSince=Ms();}
    // Where both queues and the presenter are, for the log lines of long waits.
    std::string State(){
        char b[400];snprintf(b,sizeof b,"presenter: %s for %.0f ms; Unity's queue at %llu of %llu; presenter's queue at %llu of %llu",
            step.load(),Ms()-stepSince.load(),readyFence->GetCompletedValue(),readyValue.load(),presentFence->GetCompletedValue(),presentValue.load());
        return b;
    }
    // A CPU wait for a fence, through an event so that it can be timed: one that lasts more than 2 s is logged (then
    // every 5 s) with what it waits for, and again when it ends.
    void WaitFence(ID3D12Fence* fence,UINT64 value,const char* what){
        if(!value||fence->GetCompletedValue()>=value)return;
        thread_local HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event){Check(fence->SetEventOnCompletion(value,nullptr));return;}
        Check(fence->SetEventOnCompletion(value,event));
        const double begin=Ms();double next=begin+2000;
        while(WaitForSingleObject(event,250)==WAIT_TIMEOUT){
            if(Ms()<next)continue;next+=5000;
            xrfg::dlssg_log("desktop wait: %s has waited %.0f ms for %s (fence at %llu, waiting for %llu); %s",
                GetCurrentThreadId()==presenterThread.load()?"the presenter":"Unity's thread",Ms()-begin,what,fence->GetCompletedValue(),value,State().c_str());
        }
        if(Ms()-begin>=2000)xrfg::dlssg_log("desktop wait: %s came after %.0f ms",what,Ms()-begin);
    }
    // Presenter-queue command lists: a ring, so nothing waits for the queue to drain.
    void Begin(){
        Slot& s=slots[slotNext++%8];WaitFence(presentFence.Get(),s.value,"a free command list on the presenter's queue");
        Check(s.allocator->Reset());Check(s.list->Reset(s.allocator.Get(),nullptr));open=&s;list=s.list.Get();
    }
    UINT64 Submit(){
        Check(list->Close());ID3D12CommandList* lists[]={list};presentQueue->ExecuteCommandLists(1,lists);
        UINT64 v=++presentValue;Check(presentQueue->Signal(presentFence.Get(),v));open->value=v;open=nullptr;list=nullptr;return v;
    }
    void Abandon(){if(list){list->Close();list=nullptr;open=nullptr;}}
    // Both queues idle; only while `exclusive` (or before the presenter runs).
    void DrainAll(){
        UINT64 v=++presentValue;Check(presentQueue->Signal(presentFence.Get(),v));WaitFence(presentFence.Get(),v,"the presenter's queue to drain");
        context->Flush();UINT64 r=++readyValue;Check(queue->Signal(readyFence.Get(),r));WaitFence(readyFence.Get(),r,"Unity's queue to drain");
    }
    static void Barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        if(a==b)return;D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&v);
    }
    Surface MakeSurface(UINT w,UINT h,DXGI_FORMAT format,bool render){
        Surface s;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;
        d.Flags=render?D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET:D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s.native)));
        D3D11_RESOURCE_FLAGS f{};f.BindFlags=D3D11_BIND_SHADER_RESOURCE|(render?D3D11_BIND_RENDER_TARGET:0);
        Check(on12->CreateWrappedResource(s.native.Get(),&f,render?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON,IID_PPV_ARGS(&s.wrapped)));
        return s;
    }
    Ptr<ID3D12Resource> MakeTexture(const D3D12_RESOURCE_DESC& like,D3D12_RESOURCE_FLAGS flags){
        auto d=like;d.Flags=flags;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;Ptr<ID3D12Resource> r;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)));return r;
    }
    void CreateBuffers(){
        DXGI_SWAP_CHAIN_DESC1 actual{};Check(real->GetDesc1(&actual));
        desc.BufferDesc.Width=actual.Width;desc.BufferDesc.Height=actual.Height;
        app.clear();index=0;
        for(UINT i=0;i<std::max(1u,desc.BufferCount);i++)app.push_back(MakeSurface(actual.Width,actual.Height,Raw(actual.Format),true));
        for(auto& in:inputs){in={};in.color=MakeSurface(actual.Width,actual.Height,Raw(actual.Format),false);}
        pendingInput=takenInput=writtenLast=-1;guideWidth=guideHeight=0;
        windowSurface=app[0].wrapped.Get();
        for(auto& s:app){ID3D11Resource* r=s.wrapped.Get();on12->AcquireWrappedResources(&r,1);}
        auto like=inputs[0].color.native->GetDesc();
        shown=MakeTexture(like,D3D12_RESOURCE_FLAG_NONE);
        for(auto& target:generated)target=MakeTexture(like,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        generator.reset();serial=0;readyKnown=false;
        lastPresent=Ms();
    }
    bool Stamp(const std::wstring& path,FILETIME& previous){WIN32_FILE_ATTRIBUTE_DATA a{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&a))return false;bool change=CompareFileTime(&a.ftLastWriteTime,&previous)!=0;previous=a.ftLastWriteTime;return change;}
    void Settings(){
        ULONGLONG now=GetTickCount64();if(now<nextRead)return;nextRead=now+250;
        if(Stamp(config,configStamp)){
            multiplier=std::clamp(GetPrivateProfileIntW(L"ofxr",L"frame_multiplier",2,config.c_str()),2u,4u);
            wchar_t b[64]{};GetPrivateProfileStringW(L"overlay",L"position",L"off",b,64,config.c_str());overlay=b;
        }
        if(Stamp(performance,performanceStamp)){
            framegen=GetPrivateProfileIntW(L"Performance",L"FrameGeneration",1,performance.c_str())!=0;
            int next=GetPrivateProfileIntW(L"Performance",L"DLSSGLite",0,performance.c_str());
            if(lite!=next){generator.reset();retryAt=0;lite=next;}
        }
        // The generator follows [Performance] FrameGenModel (DLSS / FSR): another choice, another generator, made
        // after the next frame is shown like the first one.
        if(const auto want=xrfg::frame_gen_model_wanted();want!=asked){asked=want;if(generator){generator.reset();retryAt=0;generatorMost=4;}}
        Ptr<IDXGIOutput> output;
        if(SUCCEEDED(real->GetContainingOutput(&output))){DXGI_OUTPUT_DESC o{};if(SUCCEEDED(output->GetDesc(&o))){DEVMODEW dm{};dm.dmSize=sizeof(dm);if(EnumDisplaySettingsW(o.DeviceName,ENUM_CURRENT_SETTINGS,&dm)&&dm.dmDisplayFrequency>1)refresh=dm.dmDisplayFrequency;}}
    }
    void SleepTo(double deadline){
        double delay=deadline-Ms();if(delay<=0)return;
        LARGE_INTEGER due;due.QuadPart=-LONGLONG(delay*10000.);
        if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,1000);
    }
    void Overlay(){
        if(overlay==L"off")return;
        if(!overlayUpload){
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=overlayPitch*overlayH;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
            Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&overlayUpload)));
            D3D12_RANGE empty{};Check(overlayUpload->Map(0,&empty,reinterpret_cast<void**>(&overlayPixels)));
        }
        // Tiny diagnostic overlay: real fps / displayed fps and selected xN.
        // The upload buffer is rewritten while an earlier copy may still read it;
        // a stale digit for one frame is all that can come of it.
        static const char* glyphs[]={"111101101101111","010110010010111","111001111100111","111001111001111","101101111001001","111100111001111","111100111101111","111001001001001","111101111101111","111101111001111"};
        memset(overlayPixels,20,overlayPitch*overlayH);
        char text[48];sprintf_s(text,"%03u/%03u x%u",unsigned(realFps+.5),unsigned(displayFps+.5),std::min(multiplier.load(),generatorMost.load()));
        unsigned x=6;
        for(char* p=text;*p&&x+10<overlayW;p++,x+=12){
            for(unsigned y=0;y<5;y++)for(unsigned c=0;c<3;c++){
                bool on=*p>='0'&&*p<='9'?glyphs[*p-'0'][y*3+c]=='1':*p=='/'?c==2-y/2:*p=='x'?(y==1||y==3?c!=1:y==2&&c==1):false;
                if(!on)continue;
                for(unsigned dy=0;dy<3;dy++)for(unsigned dx=0;dx<3;dx++)memset(overlayPixels+(y*3+4+dy)*overlayPitch+(x+c*3+dx)*4,235,4);
            }
        }
    }
    HRESULT Display(ID3D12Resource* source,unsigned kind,double target,bool paced){
        double waitBegin=Ms();Trace("internal_wait_frame",0,'B');
        Step("pacing");if(paced)SleepTo(target);
        Step("waiting for the swap chain's latency object");
        if(latency&&!IsIconic(desc.OutputWindow)&&WaitForSingleObject(latency,100)==WAIT_TIMEOUT){
            // The presentation engine retired no frame for 100 ms: presented anyway. Logged at most every 5 s.
            latencyMisses++;
            if(Ms()-latencyLogged>=5000){xrfg::dlssg_log("desktop presenter: the swap chain's latency object stayed unsignaled for 100 ms (%u times since the last such line); %s",latencyMisses,State().c_str());latencyMisses=0;latencyLogged=Ms();}
        }
        Trace("internal_wait_frame",0,'E',Ms()-waitBegin,UINT64(target*1000000.));
        Trace("internal_begin_frame",0,'E');double endBegin=Ms();
        Ptr<ID3D12Resource> back;Check(real->GetBuffer(real->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)));
        Begin();Barrier(list,source,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list,back.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(back.Get(),source);
        if(overlay!=L"off"){
            Overlay();D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=overlayUpload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint={Raw(desc.BufferDesc.Format),overlayW,overlayH,1,overlayPitch};
            D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=back.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            UINT x=overlay.find(L"right")!=std::wstring::npos?desc.BufferDesc.Width-overlayW:0,y=overlay.find(L"lower")!=std::wstring::npos?desc.BufferDesc.Height-overlayH:0;
            list->CopyTextureRegion(&dst,x,y,0,&src,nullptr);
        }
        Barrier(list,back.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
        Barrier(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);Submit();
        if(!presentedCapture.empty()){
            std::wstring path;path.swap(presentedCapture);SaveCapture(back.Get(),path,D3D12_RESOURCE_STATE_PRESENT);
        }
        lastPresent=Ms();
        Step("in Present");
        HRESULT h=real->Present(paced?1:sync,flags&~DXGI_PRESENT_TEST);
        if(double took=Ms()-lastPresent;took>=250)xrfg::dlssg_log("desktop presenter: Present took %.0f ms (0x%08x)",took,unsigned(h));
        Trace("internal_end_frame",0,'E',Ms()-endBegin);
        Trace(kind==2?"generated_present":kind==1?"real_present":"repeat_present",1);displayed++;
        return h;
    }
    void Blank(){
        // Right after new buffers: black instead of the window's empty surface.
        if(!rtvHeap){D3D12_DESCRIPTOR_HEAP_DESC h{};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;h.NumDescriptors=1;Check(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&rtvHeap)));}
        Ptr<ID3D12Resource> back;Check(real->GetBuffer(real->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)));
        auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(back.Get(),nullptr,rtv);
        const float black[4]{0,0,0,1};
        Begin();Barrier(list,back.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);list->ClearRenderTargetView(rtv,black,0,nullptr);
        Barrier(list,back.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);Submit();
        real->Present(0,0);lastPresent=Ms();
    }
    // One real frame: take a copy of input `slot` (and DLSS-G's frames from it),
    // hand the input back to Unity, then show the group evenly spaced.
    void Group(int slot,double idleSince){
        Input& in=inputs[slot];
        Step("waiting for Unity's frame on Unity's queue");
        WaitFence(readyFence.Get(),in.ready,"Unity's frame (the ready fence)");
        Step("recording the copy");
        double ready=Ms();
        Settings();
        const double vsync=1000./refresh;
        // The real frame period, from frames the presenter had to wait for (their
        // time is their own). A frame that was already waiting means the presenter
        // is the slower side: shrink, so it never holds Unity to its own pace.
        bool known=ready-idleSince>0.2;
        if(known&&readyKnown){double s=ready-lastReady;if(s>0&&s<250)period=period>0?period*0.75+s*0.25:s;}
        readyKnown=known;lastReady=ready;
        bool paced=framegen&&WaitForSingleObject(pauseEvent,0)!=WAIT_OBJECT_0;
        bool makeGenerator=false;
        UINT count=0;
        Begin();
        ID3D12Resource* const color=in.color.native.Get();
        Barrier(list,color,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list,shown.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(shown.Get(),color);
        Barrier(list,shown.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        Barrier(list,color,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        if(in.resized){generator.reset();in.resized=false;}
        if(paced&&in.guides.dlssg_camera){
            if(generator){
                // No more frames than this generator makes per pair: FSR one, DLSS-G what the card's can.
                xrfg::GuidedRecordInput input{};input.backbuffer=color;input.guides=&in.guides;input.outputs=std::min(multiplier.load()-1,generator->max_outputs());
                for(UINT i=0;i<input.outputs;i++)input.destinations[i]=generated[i].Get();
                Check(generator->record(list,input,&count));
            }else makeGenerator=GetTickCount64()>=retryAt;
        }else if(generator)generator->invalidate_history();
        UINT64 released=Submit();
        // The input is Unity's again once this list has run on the GPU; Unity checks that before it writes it.
        {std::lock_guard lock(mutex);in.read=released;takenInput=-1;}changed.notify_all();
        if(generator&&!generator->usable()){generator.reset();retryAt=GetTickCount64()+1000;}
        Trace("generated",count);
        Step("waiting for its copy of the frame on its own queue");
        WaitFence(presentFence.Get(),released,"its copy of the frame (the presenter's queue)");
        if(count)CaptureGenerated(generated[0].Get());
        const UINT frames=count+1;
        if(!known&&period>0)period=std::max(period*0.97,vsync*frames);
        const double interval=std::clamp(period>0?period/frames:vsync,vsync,50.);
        double at=paced?std::max(Ms(),lastPresent+interval):Ms();
        for(UINT i=0;i<count;i++){
            if(Display(generated[i].Get(),2,at,true)==DXGI_STATUS_OCCLUDED){paced=false;break;}
            at+=interval;
        }
        if(Display(shown.Get(),1,at,paced)==DXGI_STATUS_OCCLUDED)Sleep(16);
        baseCount++;
        double now=Ms();if(now-fpsAt>=1000){realFps=baseCount*1000./(now-fpsAt);displayFps=displayed*1000./(now-fpsAt);baseCount=displayed=0;fpsAt=now;}
        // A new generator takes 0.15-0.4 s (DLSS-G's feature; FSR's context comes with its first pair): made after
        // this frame is shown. The one chosen, or the other where it cannot run.
        if(makeGenerator){
            Step("making a frame generator");
            xrfg::FrameGenModel made{};
            auto next=xrfg::make_guided_frame_generator(device.Get(),presentQueue.Get(),in.color.native->GetDesc(),&made);
            if(next){generator=std::move(next);generatorMost=generator->max_outputs()+1;}
            else{retryAt=GetTickCount64()+1000;Trace("ngx_retry");}
        }
    }
    void Run(){
        SetThreadDescription(GetCurrentThread(),L"VaM Desktop presenter");
        presenterThread=GetCurrentThreadId();
        bool pinned=false;
        for(;;){
            int slot=-1;double idleSince=Ms();
            Step("waiting for Unity's next frame");
            {std::unique_lock lock(mutex);
                changed.wait(lock,[&]{return stop||(pendingInput>=0&&!exclusive);});
                if(stop)return;
                slot=pendingInput;pendingInput=-1;takenInput=slot;busy=true;
                if(stalled){stalled=false;xrfg::dlssg_log("desktop presenter: takes Unity's frames again, %.0f ms after Unity first waited in vain",Ms()-stalledSince);}
            }
            if(!pinned)if(auto cpu=GetModuleHandleW(L"VaMCpuBoostNative.bin")){
                auto api=reinterpret_cast<int(__cdecl*)()>(GetProcAddress(cpu,"CB_ApiVersion"));
                auto pin=reinterpret_cast<unsigned long long(__cdecl*)(int)>(GetProcAddress(cpu,"CB_PinThread"));
                if(api&&api()>=5&&pin){pin(1);pinned=true;}
            }
            try{Group(slot,idleSince);}
            catch(HRESULT h){Abandon();generator.reset();retryAt=GetTickCount64()+1000;Trace("device_retry");xrfg::dlssg_log("desktop presenter error 0x%08x; retry",unsigned(h));}
            catch(...){Abandon();generator.reset();retryAt=GetTickCount64()+1000;Trace("presenter_retry");}
            // After an error the input's read (if any was submitted) is in inputs[slot].read; one never submitted
            // leaves read as it was, so the input is free.
            {std::lock_guard lock(mutex);takenInput=-1;busy=false;}changed.notify_all();
        }
    }
    // Unity, before it writes a frame: which input to write. It waits, as v2 did, until the presenter has taken the
    // last frame and submitted its read (that wait is what paces Unity), but for at most kTakeMs: then it logs where
    // the presenter is, stops waiting until the presenter takes a frame again, and writes over the frame not yet
    // taken. Otherwise it takes an input that is neither taken nor still being read: the presenter has at most one
    // input in flight (it waits for each read before it shows the frame), so with three one is always free.
    static constexpr int kTakeMs=1000;
    int Claim(){
        std::unique_lock lock(mutex);
        if(!stalled){
            const double begin=Ms();
            if(!changed.wait_for(lock,std::chrono::milliseconds(kTakeMs),[&]{return pendingInput<0&&takenInput<0;})){
                stalled=true;stalledSince=begin;
                xrfg::dlssg_log("desktop presenter: has not taken Unity's last frame in %d ms; Unity goes on without waiting until it does; %s",kTakeMs,State().c_str());
            }
        }
        changed.wait(lock,[&]{return !exclusive;});
        int slot=-1;
        if(pendingInput>=0){slot=pendingInput;pendingInput=-1;}   // not taken: written over
        else{
            const UINT64 done=presentFence->GetCompletedValue();
            for(int i=1;i<=kInputs&&slot<0;i++){int c=(writtenLast+i+kInputs)%kInputs;if(c!=takenInput&&inputs[c].read<=done)slot=c;}
            if(slot<0){
                // Not expected (see above): the input the presenter read longest ago, once its read is done.
                for(int i=1;i<=kInputs&&slot<0;i++){int c=(writtenLast+i+kInputs)%kInputs;if(c!=takenInput)slot=c;}
                const UINT64 read=inputs[slot].read;
                lock.unlock();WaitFence(presentFence.Get(),read,"the presenter's read of an input");lock.lock();
            }
        }
        writing=true;return slot;
    }
    void Publish(int slot,bool frame){{std::lock_guard lock(mutex);writing=false;if(frame){pendingInput=writtenLast=slot;}}changed.notify_all();}
    // Resize, fullscreen switch, teardown: the presenter stopped between groups. A frame it has not taken is dropped.
    void Exclusive(){
        std::unique_lock lock(mutex);pendingInput=-1;
        const double begin=Ms();double next=begin+2000;
        while(!changed.wait_for(lock,std::chrono::milliseconds(250),[&]{return !busy&&!writing;}))
            if(Ms()>=next){next+=5000;xrfg::dlssg_log("desktop wait: a resize has waited %.0f ms for the presenter to stop; %s",Ms()-begin,State().c_str());}
        exclusive=true;
    }
    void Shared(){{std::lock_guard lock(mutex);exclusive=false;}changed.notify_all();}
    void CaptureGenerated(ID3D12Resource* source){
        std::wstring path;{std::lock_guard lock(captureMutex);path.swap(capturePath);}if(path.empty())return;
        SaveCapture(shown.Get(),path+L".real.ppm");
        SaveCapture(source,path);
        presentedCapture=path+L".presented.ppm";
    }
    void SaveCapture(ID3D12Resource* source,const std::wstring& path,D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON){
        auto d=source->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows;UINT64 rowSize,total;
        device->GetCopyableFootprints(&d,0,1,0,&footprint,&rows,&rowSize,&total);
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=total;rd.Height=rd.DepthOrArraySize=rd.MipLevels=rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;Ptr<ID3D12Resource> buffer;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer)));
        Begin();Barrier(list,source,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=source;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=buffer.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,state);WaitFence(presentFence.Get(),Submit(),"a test capture");
        unsigned char* bytes=nullptr;D3D12_RANGE range{0,size_t(total)};Check(buffer->Map(0,&range,reinterpret_cast<void**>(&bytes)));
        FILE*f=_wfopen(path.c_str(),L"wb");if(f){fprintf(f,"P6\n%u %u\n255\n",UINT(d.Width),d.Height);std::vector<unsigned char> row(size_t(d.Width)*3);
            bool bgra=Raw(d.Format)==DXGI_FORMAT_B8G8R8A8_UNORM;
            for(UINT y=0;y<d.Height;y++){auto*p=bytes+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch;for(UINT x=0;x<d.Width;x++){row[x*3]=p[x*4+(bgra?2:0)];row[x*3+1]=p[x*4+1];row[x*3+2]=p[x*4+(bgra?0:2)];}fwrite(row.data(),1,row.size(),f);}fclose(f);
        }D3D12_RANGE empty{};buffer->Unmap(0,&empty);Trace("test_generated_capture");
    }
    void CaptureUnity(){
        std::wstring path;{std::lock_guard lock(captureMutex);path=capturePath;}if(path.empty())return;
        Ptr<ID3D11Texture2D> source;Check(app[index].wrapped.As(&source));D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.BindFlags=d.MiscFlags=0;
        Ptr<ID3D11Texture2D> staging;Check(unity->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),source.Get());
        D3D11_MAPPED_SUBRESOURCE m{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));
        FILE*f=_wfopen((path+L".unity.ppm").c_str(),L"wb");if(f){fprintf(f,"P6\n%u %u\n255\n",d.Width,d.Height);std::vector<unsigned char> row(size_t(d.Width)*3);bool bgra=Raw(d.Format)==DXGI_FORMAT_B8G8R8A8_UNORM;
            for(UINT y=0;y<d.Height;y++){auto*p=static_cast<unsigned char*>(m.pData)+size_t(y)*m.RowPitch;for(UINT x=0;x<d.Width;x++){row[x*3]=p[x*4+(bgra?2:0)];row[x*3+1]=p[x*4+1];row[x*3+2]=p[x*4+(bgra?0:2)];}fwrite(row.data(),1,row.size(),f);}fclose(f);}
        context->Unmap(staging.Get(),0);
    }
public:
    Swapchain(IDXGISwapChain1* chain,const DXGI_SWAP_CHAIN_DESC& requested):desc(requested){
        Check(chain->QueryInterface(IID_PPV_ARGS(&real)));unity->GetImmediateContext(&context);Check(unity.As(&on12));
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&readyFence)));
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&presentFence)));
        for(auto& s:slots){
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator)));
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list)));Check(s.list->Close());
        }
        timer=CreateWaitableTimerExW(nullptr,nullptr,2,TIMER_ALL_ACCESS);
        if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        std::wstring id=std::to_wstring(GetCurrentProcessId());
        pauseEvent=CreateEventW(nullptr,TRUE,FALSE,(L"Local\\OFXRBridgePause-"+id).c_str());
        WritePrivateProfileStringW(L"ofxr",L"control_event",(L"Local\\OFXRBridgeArmStop-"+id).c_str(),config.c_str());
        real->SetMaximumFrameLatency(1);latency=real->GetFrameLatencyWaitableObject();
        gameWindow=desc.OutputWindow;
        CreateBuffers();Settings();fpsAt=Ms();
        xrfg::dlssg_log("desktop Unity surfaces count=%u effect=%u format=%u; physical flip buffers=3 on the presenter's own HIGH queue (v3: inputs in a ring of %d, no GPU waits between the queues, bounded waits for Unity)",desc.BufferCount,UINT(desc.SwapEffect),UINT(desc.BufferDesc.Format),kInputs);
        auto logs=Env(L"D3DINTEROP_LOG");if(!logs.empty())flight=_wfopen((logs+L"\\ofxr-bridge-flight-desktop.log").c_str(),L"w");
        active=true;thread=std::thread([this]{Run();});
    }
    ~Swapchain(){Exclusive();{std::lock_guard lock(mutex);stop=true;}changed.notify_all();if(thread.joinable())thread.join();try{DrainAll();}catch(...){}
        active=false;for(HANDLE h:{pauseEvent,timer,latency})if(h)CloseHandle(h);if(flight)fclose(flight);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(id==__uuidof(IUnknown)||id==__uuidof(IDXGIObject)||id==__uuidof(IDXGIDeviceSubObject)||id==__uuidof(IDXGISwapChain)||id==__uuidof(IDXGISwapChain1)||id==__uuidof(IDXGISwapChain2)||id==__uuidof(IDXGISwapChain3)||id==__uuidof(IDXGISwapChain4)){*out=static_cast<IDXGISwapChain4*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a,UINT b,const void*c)override{return real->SetPrivateData(a,b,c);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a,const IUnknown*b)override{return real->SetPrivateDataInterface(a,b);}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a,UINT*b,void*c)override{return real->GetPrivateData(a,b,c);}
    HRESULT STDMETHODCALLTYPE GetParent(REFIID a,void**b)override{return real->GetParent(a,b);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID a,void**b)override{return unity->QueryInterface(a,b);}
    HRESULT STDMETHODCALLTYPE Present(UINT interval,UINT presentFlags)override{
        if(presentFlags&DXGI_PRESENT_TEST)return real->Present(interval,presentFlags);
        int slot=-1;
        try{
            double waited=Ms();Trace("app_wait_frame",0,'B');slot=Claim();Trace("app_wait_frame",0,'E',Ms()-waited);sync=interval;flags=presentFlags;
            // The input is neither taken nor still read by the presenter's queue (Claim checked its fence on the CPU):
            // Unity's queue writes it without waiting for the presenter's queue.
            Input& in=inputs[slot];
            CaptureUnity();
            OFXR_D3D11GuidePublicationV3 p;Ptr<ID3D11Texture2D> d,m;
            {std::lock_guard lock(publicationMutex);p=publication;d=publishedDepth;m=publishedMotion;}
            auto& guides=in.guides;
            guides={};guides.serial=p.serial;guides.previous_serial=serial;guides.reset=p.reset||p.serial!=serial+1;serial=p.serial;
            if(d&&m&&p.width&&p.height){
                // New guide sizes: the generator is made again (Group); each input gets surfaces of the new size when
                // it is next written.
                if(p.width!=guideWidth||p.height!=guideHeight){guideWidth=p.width;guideHeight=p.height;in.resized=true;guides.reset=true;}
                if(!in.depth.native||in.depth.native->GetDesc().Width!=p.width||in.depth.native->GetDesc().Height!=p.height){
                    in.depth=MakeSurface(p.width,p.height,DXGI_FORMAT_R32_FLOAT,false);in.motion=MakeSurface(p.width,p.height,DXGI_FORMAT_R16G16_FLOAT,false);
                }
                ID3D11Resource* snapshots[]={in.depth.wrapped.Get(),in.motion.wrapped.Get()};on12->AcquireWrappedResources(snapshots,2);
                context->CopyResource(in.depth.wrapped.Get(),d.Get());context->CopyResource(in.motion.wrapped.Get(),m.Get());on12->ReleaseWrappedResources(snapshots,2);
                guides.depth=in.depth.native;guides.motion_vectors=in.motion.native;guides.depth_width=guides.motion_width=p.width;guides.depth_height=guides.motion_height=p.height;
                guides.dlssg_camera=true;guides.depth_inverted=p.depth_inverted!=0;guides.scale_x=p.motion_scale_x;guides.scale_y=p.motion_scale_y;guides.jitter_x=p.jitter_x;guides.jitter_y=p.jitter_y;
                guides.jittered=p.jitter_x!=0 || p.jitter_y!=0;
                guides.resource_state=guides.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
                guides.frame_time_delta_ms=p.frame_time_delta_ms;guides.camera_near=p.camera_near;guides.camera_far=p.camera_far;guides.camera_fov=p.camera_fov;guides.camera_aspect=p.camera_aspect;
                memcpy(guides.clip_to_prev_clip.data(),p.clip_to_prev_clip,sizeof p.clip_to_prev_clip);memcpy(guides.prev_clip_to_clip.data(),p.prev_clip_to_clip,sizeof p.prev_clip_to_clip);
                memcpy(guides.view_to_clip.data(),p.view_to_clip,sizeof p.view_to_clip);memcpy(guides.clip_to_view.data(),p.clip_to_view,sizeof p.clip_to_view);
                memcpy(guides.camera_position.data(),p.camera_position,sizeof p.camera_position);memcpy(guides.camera_up.data(),p.camera_up,sizeof p.camera_up);
                memcpy(guides.camera_right.data(),p.camera_right,sizeof p.camera_right);memcpy(guides.camera_forward.data(),p.camera_forward,sizeof p.camera_forward);
            }
            ID3D11Resource* output=in.color.wrapped.Get();on12->AcquireWrappedResources(&output,1);context->CopyResource(output,app[index].wrapped.Get());on12->ReleaseWrappedResources(&output,1);context->Flush();
            const UINT64 ready=++readyValue;Check(queue->Signal(readyFence.Get(),ready));in.ready=ready;
            // BLT chains rotate the storage behind buffer 0; Unity keeps the
            // one GetBuffer(0) RTV. Only a flip-model caller rotates indices.
            if(desc.SwapEffect==DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || desc.SwapEffect==DXGI_SWAP_EFFECT_FLIP_DISCARD)index=(index+1)%app.size();
            Publish(slot,true);return S_OK;
        }catch(HRESULT h){if(slot>=0)Publish(slot,false);return h;}catch(...){if(slot>=0)Publish(slot,false);return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT n,REFIID id,void**out)override{return n<app.size()?app[n].wrapped->QueryInterface(id,out):DXGI_ERROR_INVALID_CALL;}
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f,IDXGIOutput* o)override{
        xrfg::dlssg_log("desktop SetFullscreenState %d; foreground %s",int(f),WindowOwner(GetForegroundWindow()).c_str());
        Exclusive();HRESULT h=real->SetFullscreenState(f,o);Shared();return h;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*f,IDXGIOutput**o)override{return real->GetFullscreenState(f,o);}
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*d)override{if(!d)return E_POINTER;*d=desc;real->GetFullscreenState(&d->Windowed,nullptr);d->Windowed=!d->Windowed;return S_OK;}
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT resizeFlags)override{
        UINT rw=w,rh=h;RECT rc{};
        if((!rw||!rh)&&GetClientRect(desc.OutputWindow,&rc)){if(!rw)rw=UINT(rc.right-rc.left);if(!rh)rh=UINT(rc.bottom-rc.top);}
        const DXGI_FORMAT nf=f==DXGI_FORMAT_UNKNOWN?desc.BufferDesc.Format:f;
        // Same size (or a minimized window's 0x0): the buffers Unity gets back are
        // the ones it had; nothing on screen changes.
        const bool same=!rw||!rh||(rw==desc.BufferDesc.Width&&rh==desc.BufferDesc.Height&&Raw(nf)==Raw(desc.BufferDesc.Format));
        xrfg::dlssg_log("desktop ResizeBuffers count=%u %ux%u -> %ux%u format=%u flags=0x%x: %s (%llu kept so far); foreground %s",n,w,h,rw,rh,UINT(f),resizeFlags,
            same?"same size, buffers and DLSS-G kept":"new buffers",static_cast<unsigned long long>(kept+(same?1:0)),WindowOwner(GetForegroundWindow()).c_str());
        if(same){kept++;return S_OK;}
        try{Exclusive();DrainAll();context->ClearState();for(auto&s:app){ID3D11Resource*r=s.wrapped.Get();on12->ReleaseWrappedResources(&r,1);}context->Flush();app.clear();for(auto&in:inputs)in={};shown.Reset();generator.reset();for(auto&t:generated)t.Reset();DrainAll();
            HRESULT result=real->ResizeBuffers(3,w,h,f==DXGI_FORMAT_UNKNOWN?Raw(desc.BufferDesc.Format):Raw(f),DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT|DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
            if(FAILED(result)){CreateBuffers();Shared();return result;}if(n)desc.BufferCount=n;if(f!=DXGI_FORMAT_UNKNOWN)desc.BufferDesc.Format=f;CreateBuffers();
            try{Blank();}catch(...){Abandon();}
            Shared();return S_OK;
        }catch(HRESULT e){Abandon();Shared();return e;}catch(...){Abandon();Shared();return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*d)override{Exclusive();HRESULT h=real->ResizeTarget(d);Shared();return h;}
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**o)override{return real->GetContainingOutput(o);}
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*s)override{return real->GetFrameStatistics(s);}
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*n)override{return real->GetLastPresentCount(n);}
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1*d)override{HRESULT h=real->GetDesc1(d);if(SUCCEEDED(h)){d->BufferCount=desc.BufferCount;d->Format=desc.BufferDesc.Format;d->SwapEffect=desc.SwapEffect;d->Flags=desc.Flags;}return h;}
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC*d)override{return real->GetFullscreenDesc(d);}
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND*h)override{return real->GetHwnd(h);}
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID i,void**o)override{return real->GetCoreWindow(i,o);}
    HRESULT STDMETHODCALLTYPE Present1(UINT a,UINT b,const DXGI_PRESENT_PARAMETERS*)override{return Present(a,b);}
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported()override{return TRUE;}
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput**o)override{return real->GetRestrictToOutput(o);}
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA*c)override{return real->SetBackgroundColor(c);}
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA*c)override{return real->GetBackgroundColor(c);}
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r)override{return real->SetRotation(r);}
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION*r)override{return real->GetRotation(r);}
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w,UINT h)override{return real->SetSourceSize(w,h);}
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT*w,UINT*h)override{return real->GetSourceSize(w,h);}
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT n)override{return real->SetMaximumFrameLatency(n);}
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT*n)override{return real->GetMaximumFrameLatency(n);}
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject()override{return nullptr;}
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F*m)override{return real->SetMatrixTransform(m);}
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F*m)override{return real->GetMatrixTransform(m);}
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex()override{return index;}
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c,UINT*s)override{return real->CheckColorSpaceSupport(c,s);}
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c)override{return real->SetColorSpace1(c);}
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags,const UINT*,IUnknown*const*)override{return ResizeBuffers(n,w,h,f,flags);}
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t,UINT n,void*d)override{return real->SetHDRMetaData(t,n,d);}
};

using CreateOld=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**);
using CreateHwnd=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
CreateOld originalOld=nullptr;CreateHwnd originalHwnd=nullptr;
bool IsUnity(IUnknown*d){Ptr<ID3D11Device> v;return d&&SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&v)))&&v.Get()==unity.Get();}
HRESULT Make(IDXGIFactory2*f,HWND hwnd,const DXGI_SWAP_CHAIN_DESC& requested,IDXGISwapChain1**out){
    DXGI_SWAP_CHAIN_DESC1 d{};d.Width=requested.BufferDesc.Width;d.Height=requested.BufferDesc.Height;d.Format=Raw(requested.BufferDesc.Format);d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=3;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;d.Scaling=DXGI_SCALING_STRETCH;d.AlphaMode=DXGI_ALPHA_MODE_IGNORE;d.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT|DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    Ptr<IDXGISwapChain1> chain;HRESULT h=originalHwnd(f,presentQueue.Get(),hwnd,&d,nullptr,nullptr,&chain);if(FAILED(h))return h;
    try{*out=new Swapchain(chain.Get(),requested);return S_OK;}catch(HRESULT e){return e;}catch(...){return E_FAIL;}
}
HRESULT STDMETHODCALLTYPE HookOld(IDXGIFactory*f,IUnknown*d,DXGI_SWAP_CHAIN_DESC*desc,IDXGISwapChain**out){
    if(!IsUnity(d))return originalOld(f,d,desc,out);if(!desc||!out)return E_INVALIDARG;
    Ptr<IDXGIFactory2> f2;HRESULT h=f->QueryInterface(IID_PPV_ARGS(&f2));if(FAILED(h))return h;
    Ptr<IDXGISwapChain1> s;h=Make(f2.Get(),desc->OutputWindow,*desc,&s);if(SUCCEEDED(h))h=s.CopyTo(out);return h;
}
HRESULT STDMETHODCALLTYPE HookHwnd(IDXGIFactory2*f,IUnknown*d,HWND hwnd,const DXGI_SWAP_CHAIN_DESC1*desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*fs,IDXGIOutput*restrict,IDXGISwapChain1**out){
    if(!IsUnity(d))return originalHwnd(f,d,hwnd,desc,fs,restrict,out);if(!desc||!out)return E_INVALIDARG;
    DXGI_SWAP_CHAIN_DESC old{};old.BufferDesc.Width=desc->Width;old.BufferDesc.Height=desc->Height;old.BufferDesc.Format=desc->Format;old.SampleDesc=desc->SampleDesc;old.BufferUsage=desc->BufferUsage;old.BufferCount=desc->BufferCount;old.OutputWindow=hwnd;old.Windowed=!fs||fs->Windowed;old.SwapEffect=desc->SwapEffect;old.Flags=desc->Flags;return Make(f,hwnd,old,out);
}
void Patch(void**slot,void*hook,void**original){DWORD before;Check(VirtualProtect(slot,sizeof(void*),PAGE_EXECUTE_READWRITE,&before)?S_OK:HRESULT_FROM_WIN32(GetLastError()));*original=*slot;InterlockedExchangePointer(slot,hook);DWORD ignored;VirtualProtect(slot,sizeof(void*),before,&ignored);}
}

void VamDesktopInstall(ID3D11Device*d11,ID3D12Device*d12,ID3D12CommandQueue*q){
    if(!VamDesktopDisplay()||unity)return;
    try{unity=d11;device=d12;queue=q;config=Env(L"VAM_DESKTOP_CONFIG");performance=Env(L"VAM_PERFORMANCE_CONFIG");
        // Presentation has a deadline Unity's frame does not: HIGH, as OFXR's synthesis queue in VR.
        D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;qd.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;qd.NodeMask=q->GetDesc().NodeMask;
        if(SUCCEEDED(d12->CreateCommandQueue(&qd,IID_PPV_ARGS(&presentQueue))))presentQueue->SetName(L"VaM desktop presenter queue (HIGH)");
        else{presentQueue=q;xrfg::dlssg_log("desktop presenter: no queue of its own; presenting on Unity's queue");}
        xrfg::dlssg_set_session_device(d12);
        Ptr<IDXGIDevice> dx;Ptr<IDXGIAdapter>a;Ptr<IDXGIFactory2>f;
        Check(d11->QueryInterface(IID_PPV_ARGS(&dx)));Check(dx->GetAdapter(&a));Check(a->GetParent(IID_PPV_ARGS(&f)));
        void**v=*reinterpret_cast<void***>(f.Get());Patch(v+10,reinterpret_cast<void*>(&HookOld),reinterpret_cast<void**>(&originalOld));Patch(v+15,reinterpret_cast<void*>(&HookHwnd),reinterpret_cast<void**>(&originalHwnd));
        // The flashes to the desktop: each was DWM's "Ghost" window replacing VaM's
        // (Windows judged it not responding while it was still presenting, see the
        // keepalive in StartForegroundLog), then VaM reactivating. A flip-model
        // window has no redirection surface for the ghost to show, so the ghost is
        // empty. No ghost for VaM, also during long scene loads.
        DisableProcessWindowsGhosting();
        StartForegroundLog();
        xrfg::dlssg_log("desktop DXGI factory hooked; D3D12 flip owner ready; window ghosting off");
    }catch(...){unity.Reset();xrfg::dlssg_log("desktop factory hook failed");}
}
extern "C" {
__declspec(dllexport) int VAM_DesktopIsWindowSurface(ID3D11Resource*r){return r==windowSurface.load()?1:0;}
__declspec(dllexport) int VAM_DesktopCapture(const wchar_t*path){if(!path||!*path)return 0;std::lock_guard lock(captureMutex);if(!capturePath.empty())return 0;capturePath=path;return 1;}
__declspec(dllexport) int OFXR_GetProviderApiVersionV4(){return 4;}
__declspec(dllexport) int OFXR_PrepareFrameGenerationV3(){return xrfg::dlssg_prepare_session_device()?1:0;}
__declspec(dllexport) int OFXR_GetFrameMultiplierV1(UINT*now,UINT*most,UINT*flags){if(!active||!now||!most||!flags)return 0;*most=generatorMost;*now=std::min(multiplier.load(),generatorMost.load());*flags=1;return 1;}
__declspec(dllexport) int OFXR_PublishD3D11GuidesV4(const OFXR_D3D11GuidePublicationV4*p){
    if(!p||p->base.struct_size!=sizeof(*p)||p->base.api_version!=4||p->stream_kind!=1||!p->base.depth||!p->base.motion_vectors)return 0;
    std::lock_guard lock(publicationMutex);publication=p->base;publishedDepth=p->base.depth;publishedMotion=p->base.motion_vectors;return 1;
}
}
