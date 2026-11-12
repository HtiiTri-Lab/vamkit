// PT material textures kept on the D3D12 side (frame-rate study 2026-11-02).
// Borrowing a D3D11 texture for the PT (UnwrapUnderlyingResource ... ReturnUnderlyingResource) makes 11on12 queue a
// sync sequence for it (its fence and the paging fence, on each of the queue's three hardware contexts); ~80 of them
// a frame left the GPU waiting before each eye's PT. A texture D3D11 can only write with copies, UpdateSubresource or
// Discard (no render-target, UAV, depth or CPU access) is copied once into a D3D12 texture of our own, and that copy
// is bound from then on. Those context methods are watched (their slots in the immediate context's vtable); a write
// marks the texture for another copy before the PT uses it again. Render targets and the like are still borrowed,
// once a frame. No new copy within kPathCopyReserve of the process's video-memory budget: that texture is borrowed.
// Render thread, except the hooks (any thread that uses the immediate context).
#include <dxgi1_4.h>

namespace {
constexpr uint64_t kPathCopyReserve=2ull<<30;
constexpr int kPathCopiesPerFrame=16;              // a scene's first copies spread over a few frames
constexpr uint64_t kPathCopyBytesPerFrame=512ull<<20;

SRWLOCK gPathWriteLock=SRWLOCK_INIT;
std::unordered_map<ID3D11Resource *,uint64_t> gPathWrites;  // watched originals -> D3D11 writes seen
std::atomic<bool> gPathWatching{false};

void PathWritten(ID3D11Resource *r) {
    if(!r || !gPathWatching.load(std::memory_order_relaxed))return;
    AcquireSRWLockExclusive(&gPathWriteLock);
    auto it=gPathWrites.find(r);
    if(it!=gPathWrites.end())it->second++;
    ReleaseSRWLockExclusive(&gPathWriteLock);
}
uint64_t PathWrites(ID3D11Resource *r) {
    AcquireSRWLockShared(&gPathWriteLock);
    auto it=gPathWrites.find(r);
    const uint64_t n=it!=gPathWrites.end()?it->second:0;
    ReleaseSRWLockShared(&gPathWriteLock);
    return n;
}
void PathWatch(ID3D11Resource *r,bool on) {
    AcquireSRWLockExclusive(&gPathWriteLock);
    if(on)gPathWrites.emplace(r,0);else gPathWrites.erase(r);
    gPathWatching=!gPathWrites.empty();
    ReleaseSRWLockExclusive(&gPathWriteLock);
}

// ID3D11DeviceContext / ID3D11DeviceContext1 vtable slots (d3d11.h, d3d11_1.h C interfaces)
enum {kCopySubresourceRegion=46,kCopyResource=47,kUpdateSubresource=48,kResolveSubresource=57,
      kCopySubresourceRegion1=115,kUpdateSubresource1=116,kDiscardResource=117,kPathSlots=118};
void *gPathOriginal[kPathSlots];
void **gPathVtable=nullptr;
bool gPathHookFailed=false;
template<class F> F PathOriginal(int slot){return reinterpret_cast<F>(gPathOriginal[slot]);}

void STDMETHODCALLTYPE HookCopySubresourceRegion(ID3D11DeviceContext *c,ID3D11Resource *dst,UINT ds,UINT x,UINT y,UINT z,
                                                 ID3D11Resource *src,UINT ss,const D3D11_BOX *box) {
    PathWritten(dst);
    PathOriginal<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*)>
        (kCopySubresourceRegion)(c,dst,ds,x,y,z,src,ss,box);
}
void STDMETHODCALLTYPE HookCopyResource(ID3D11DeviceContext *c,ID3D11Resource *dst,ID3D11Resource *src) {
    PathWritten(dst);
    PathOriginal<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*)>(kCopyResource)(c,dst,src);
}
void STDMETHODCALLTYPE HookUpdateSubresource(ID3D11DeviceContext *c,ID3D11Resource *dst,UINT ds,const D3D11_BOX *box,
                                             const void *data,UINT row,UINT depth) {
    PathWritten(dst);
    PathOriginal<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT)>
        (kUpdateSubresource)(c,dst,ds,box,data,row,depth);
}
void STDMETHODCALLTYPE HookResolveSubresource(ID3D11DeviceContext *c,ID3D11Resource *dst,UINT ds,ID3D11Resource *src,UINT ss,DXGI_FORMAT f) {
    PathWritten(dst);
    PathOriginal<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,ID3D11Resource*,UINT,DXGI_FORMAT)>
        (kResolveSubresource)(c,dst,ds,src,ss,f);
}
void STDMETHODCALLTYPE HookCopySubresourceRegion1(ID3D11DeviceContext1 *c,ID3D11Resource *dst,UINT ds,UINT x,UINT y,UINT z,
                                                  ID3D11Resource *src,UINT ss,const D3D11_BOX *box,UINT flags) {
    PathWritten(dst);
    PathOriginal<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*,UINT)>
        (kCopySubresourceRegion1)(c,dst,ds,x,y,z,src,ss,box,flags);
}
void STDMETHODCALLTYPE HookUpdateSubresource1(ID3D11DeviceContext1 *c,ID3D11Resource *dst,UINT ds,const D3D11_BOX *box,
                                              const void *data,UINT row,UINT depth,UINT flags) {
    PathWritten(dst);
    PathOriginal<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT,UINT)>
        (kUpdateSubresource1)(c,dst,ds,box,data,row,depth,flags);
}
void STDMETHODCALLTYPE HookDiscardResource(ID3D11DeviceContext1 *c,ID3D11Resource *r) {
    PathWritten(r);
    PathOriginal<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*)>(kDiscardResource)(c,r);
}
const int kPathHookSlots[]={kCopySubresourceRegion,kCopyResource,kUpdateSubresource,kResolveSubresource,
                            kCopySubresourceRegion1,kUpdateSubresource1,kDiscardResource};
void *const kPathHooks[]={(void*)&HookCopySubresourceRegion,(void*)&HookCopyResource,(void*)&HookUpdateSubresource,
                          (void*)&HookResolveSubresource,(void*)&HookCopySubresourceRegion1,(void*)&HookUpdateSubresource1,
                          (void*)&HookDiscardResource};

// Unity and the other plugins reach the immediate context through the one object, so one vtable; if its
// ID3D11DeviceContext1 had a vtable of its own, nothing is copied (every texture stays borrowed).
bool PathHooked() {
    if(gPathVtable)return true;
    if(gPathHookFailed || !g.d11)return false;
    gPathHookFailed=true;
    ComPtr<ID3D11DeviceContext> c;g.d11->GetImmediateContext(&c);
    ComPtr<ID3D11DeviceContext1> c1;
    if(!c || FAILED(c.As(&c1)))return false;
    void **vt=*reinterpret_cast<void***>(c.Get());
    if(*reinterpret_cast<void***>(c1.Get())!=vt)return false;
    DWORD old=0;
    if(!VirtualProtect(vt+kCopySubresourceRegion,(kPathSlots-kCopySubresourceRegion)*sizeof(void*),PAGE_READWRITE,&old))return false;
    for(int i=0;i<7;i++) {
        gPathOriginal[kPathHookSlots[i]]=vt[kPathHookSlots[i]];
        InterlockedExchangePointer(vt+kPathHookSlots[i],kPathHooks[i]);
    }
    VirtualProtect(vt+kCopySubresourceRegion,(kPathSlots-kCopySubresourceRegion)*sizeof(void*),old,&old);
    gPathVtable=vt;gPathHookFailed=false;
    return true;
}
void PathUnhook() {
    if(!gPathVtable)return;
    void **vt=gPathVtable;DWORD old=0;
    if(VirtualProtect(vt+kCopySubresourceRegion,(kPathSlots-kCopySubresourceRegion)*sizeof(void*),PAGE_READWRITE,&old)) {
        for(int i=0;i<7;i++)if(vt[kPathHookSlots[i]]==kPathHooks[i])InterlockedExchangePointer(vt+kPathHookSlots[i],gPathOriginal[kPathHookSlots[i]]);
        VirtualProtect(vt+kCopySubresourceRegion,(kPathSlots-kCopySubresourceRegion)*sizeof(void*),old,&old);
    }
    gPathVtable=nullptr;
}

// 1: D3D11 can only write it with the watched methods; 2: anything else (borrowed every frame)
int PathTextureKind(ID3D11Resource *r) {
    UINT bind=0,cpu=0,misc=0;D3D11_USAGE usage=D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> t2;ComPtr<ID3D11Texture3D> t3;
    if(SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t2)))) {
        D3D11_TEXTURE2D_DESC d;t2->GetDesc(&d);
        if(d.SampleDesc.Count>1)return 2;
        bind=d.BindFlags;cpu=d.CPUAccessFlags;misc=d.MiscFlags;usage=d.Usage;
    } else if(SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t3)))) {
        D3D11_TEXTURE3D_DESC d;t3->GetDesc(&d);
        bind=d.BindFlags;cpu=d.CPUAccessFlags;misc=d.MiscFlags;usage=d.Usage;
    } else return 2;
    const UINT writable=D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_STREAM_OUTPUT;
    const UINT shared=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX|D3D11_RESOURCE_MISC_SHARED_NTHANDLE|
                      D3D11_RESOURCE_MISC_GDI_COMPATIBLE|D3D11_RESOURCE_MISC_TILED|D3D11_RESOURCE_MISC_GENERATE_MIPS;
    return (bind&writable) || cpu || (misc&shared) || usage==D3D11_USAGE_DYNAMIC || usage==D3D11_USAGE_STAGING ? 2 : 1;
}

ComPtr<IDXGIAdapter3> gPathAdapter;
uint64_t PathMemoryRoom() {
    if(!gPathAdapter) {
        using FactoryFn=HRESULT(WINAPI*)(REFIID,void**);
        auto create=(FactoryFn)GetProcAddress(GetModuleHandleW(L"dxgi.dll"),"CreateDXGIFactory1");
        ComPtr<IDXGIFactory4> factory;
        if(!create || FAILED(create(IID_PPV_ARGS(&factory))) ||
           FAILED(factory->EnumAdapterByLuid(g.d12->GetAdapterLuid(),IID_PPV_ARGS(&gPathAdapter))))return 0;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO m{};
    if(FAILED(gPathAdapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&m)))return 0;
    return m.Budget>m.CurrentUsage+kPathCopyReserve?m.Budget-m.CurrentUsage-kPathCopyReserve:0;
}

struct PathCopyState {
    int frame=INT_MIN, copies=0;
    uint64_t bytes=0, room=0;
    bool roomRead=false;
    int resident=0;uint64_t residentBytes=0;   // diagnostics (VAM_PT_TEXTURE_LOG)
    uint64_t borrowed=0, copied=0, frames=0;
};
PathCopyState gPathCopies;
struct PathGrave {uint64_t value;ComPtr<ID3D12Resource> copy;};
std::vector<PathGrave> gPathGraves;

// Copies t's borrowed original into its own texture on `list` (creating that when there is room). False: bind
// the original this frame.
bool PathCopy(ID3D12GraphicsCommandList *list,PathTexture &t) {
    auto &s=gPathCopies;
    if(s.frame!=g.lastFrame){s.frame=g.lastFrame;s.copies=0;s.bytes=0;s.roomRead=false;}
    if(s.copies>=kPathCopiesPerFrame)return false;
    bool fresh=false;
    if(!t.copy) {
        D3D12_RESOURCE_DESC d=t.underlying->GetDesc();
        d.Flags=D3D12_RESOURCE_FLAG_NONE;d.Alignment=0;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        const uint64_t bytes=g.d12->GetResourceAllocationInfo(0,1,&d).SizeInBytes;
        if(!s.roomRead){s.room=PathMemoryRoom();s.roomRead=true;}
        if(bytes==UINT64_MAX || bytes>s.room || s.bytes+bytes>kPathCopyBytesPerFrame)return false;
        D3D12_HEAP_PROPERTIES hp={};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        if(FAILED(g.d12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&t.copy))))
            return false;
        t.bytes=bytes;s.room-=bytes;s.resident++;s.residentBytes+=bytes;fresh=true;
    } else if(s.bytes+t.bytes>kPathCopyBytesPerFrame)return false;
    std::vector<D3D12_RESOURCE_BARRIER> b;
    Barrier(b,t.underlying.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    if(!fresh)Barrier(b,t.copy.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier((UINT)b.size(),b.data());
    list->CopyResource(t.copy.Get(),t.underlying.Get());
    b.clear();
    Barrier(b,t.underlying.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    Barrier(b,t.copy.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier((UINT)b.size(),b.data());
    s.copies++;s.bytes+=t.bytes;s.copied++;
    return true;
}

// The queue to name to UnwrapUnderlyingResource: 11on12's own (g.queue) once the proxy's probe has shown that this
// d3d11on12.dll does not stop on it (D3DInterop_UnwrapOnOwnQueue, components/render/d3d11-proxy/unwrap_probe.cpp),
// otherwise g.unwrapQueue, which runs no work. On Windows 10 and older Windows 11, naming g.queue stops it for good.
ID3D12CommandQueue *PathUnwrapQueue() {
    using Ask=int(WINAPI*)();
    static const Ask ask=[]{HMODULE p=ProxyModule();return p?reinterpret_cast<Ask>(GetProcAddress(p,"D3DInterop_UnwrapOnOwnQueue")):nullptr;}();
    return ask&&ask()?g.queue.Get():g.unwrapQueue.Get();
}

// The resource the PT binds for t this frame: its up-to-date copy, or the original, borrowed until
// ST_FinishPathTraceV1. Null after PathFail.
ID3D12Resource *PathTextureFor(ID3D12GraphicsCommandList *list,PathTexture &t) {
    if(!t.kind)t.kind=PathTextureKind(t.source.Get());
    const bool resident=t.kind==1 && PathHooked();
    uint64_t writes=0;
    if(resident) {
        if(!t.watched){PathWatch(t.source.Get(),true);t.watched=true;}
        writes=PathWrites(t.source.Get());
        if(t.copy && t.copiedWrites==writes)return t.copy.Get();
    }
    if(!t.taken) {
        ComPtr<ID3D12Resource> resource;
        HRESULT hr=g.pathOn12->UnwrapUnderlyingResource(t.source.Get(),PathUnwrapQueue(),IID_PPV_ARGS(&resource));
        if(FAILED(hr)){PathFail("PT texture unwrap",hr);return nullptr;}
        t.underlying=resource;t.taken=true;
    }
    if(resident && PathCopy(list,t)){t.copiedWrites=writes;return t.copy.Get();}
    return t.underlying.Get();
}

// t leaves the map: its copy is freed once the PT frames that read it have finished.
void PathForget(PathTexture &t) {
    if(t.watched){PathWatch(t.source.Get(),false);t.watched=false;}
    if(t.copy) {
        gPathGraves.push_back({g.pathFenceValue,t.copy});t.copy.Reset();
        gPathCopies.resident--;gPathCopies.residentBytes-=t.bytes;
    }
}
void PathCollectGraves() {
    if(gPathGraves.empty() || !g.pathFence)return;
    const uint64_t done=g.pathFence->GetCompletedValue();
    if(done==UINT64_MAX)return;
    gPathGraves.erase(std::remove_if(gPathGraves.begin(),gPathGraves.end(),[done](const PathGrave &x){return x.value<=done;}),gPathGraves.end());
}

// VAM_PT_TEXTURE_LOG=<file>: every 600 PT frames, the borrows and copies a frame and the resident copies.
void PathTextureLog(int borrowed) {
    auto &s=gPathCopies;
    s.borrowed+=borrowed;
    if(++s.frames%600)return;
    static wchar_t file[MAX_PATH]={};
    static const bool on=GetEnvironmentVariableW(L"VAM_PT_TEXTURE_LOG",file,MAX_PATH)>0;
    if(!on)return;
    FILE *f=_wfopen(file,L"a");
    if(!f)return;
    fprintf(f,"frame %d textures %zu borrowed/frame %.2f copies/frame %.3f resident %d %.1f MiB hooked %d\n",g.lastFrame,
            g.pathTextures.size(),s.borrowed/600.0,s.copied/600.0,s.resident,s.residentBytes/1048576.0,gPathVtable?1:0);
    fclose(f);
    s.borrowed=0;s.copied=0;
}
}  // namespace
