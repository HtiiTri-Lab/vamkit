// PT overlay layer. Replay the actual UI shader/blend/stencil against black
// and white. Their difference is RGB transmittance; black is premultiplied
// foreground. No obsolete raster scene colour enters the PT composite.
#include "path_overlay_programs.h"
struct PathOverlayLayer {
    ID3D11Resource *source=nullptr,*sourceDepth=nullptr;
    ID3D11Texture2D *color[2]={},*depth[2]={};
    ID3D11RenderTargetView *rtv[2]={};
    ID3D11DepthStencilView *dsv[2]={};
    UINT width=0,height=0,stage=0;int frame=-1;uint64_t draws=0,bytes=0;
    uint64_t readDraws=0;int readFrame=-1; // test record: draws replayed when the composite last took this layer
    int readAt=-1;                         // gFrames when the composite last took it (11-12, late layers in VR)
    uint64_t seq[32]={};int seqN=0;          // test record: this frame's replayed pixel programs, in order
    void Release() {
        for(int k=0;k<2;k++) {
            if(rtv[k])ID3D11RenderTargetView_Release(rtv[k]);rtv[k]=nullptr;
            if(dsv[k])ID3D11DepthStencilView_Release(dsv[k]);dsv[k]=nullptr;
            if(color[k])ID3D11Texture2D_Release(color[k]);color[k]=nullptr;
            if(depth[k])ID3D11Texture2D_Release(depth[k]);depth[k]=nullptr;
        }
        if(source)ID3D11Resource_Release(source);source=nullptr;
        if(sourceDepth)ID3D11Resource_Release(sourceDepth);sourceDepth=nullptr;
        width=height=0;frame=-1;bytes=0;
    }
};
static PathOverlayLayer gPathOverlays[6]; // 0.23.3: 4 -> 6, late UI may need its own after-tone layers
static bool gOverlayReplay=false;
static uint64_t gOverlayFailures=0,gOverlayDraws=0;
static uint64_t gOverlayEffectDraws=0,gOverlayUiDraws=0;
// 0.23.3: a canvas element is UI whatever its shader. The stock catalog (path_overlay_programs.h) only knows VaM's own
// UI programs; a plugin's own UI shader (VaMStory's HZM/UI/UIShader01: every menu button of one stage pack) or a scene
// shader on a RawImage was never captured, so PT dropped it and only the text stayed. The plugin hands over the
// textures the active canvas elements with their own material draw (fonts, sprites, images; never Unity's default
// white/black or a render texture). A draw the scene registry does not know that samples one of them is UI.
static SRWLOCK gUiTextureLock=SRWLOCK_INIT;
static std::vector<ID3D11Resource*> gUiTextures; // sorted; identities only, never dereferenced
static uint64_t gOverlayCanvasDraws=0,gOverlayCanvasElsewhere=0;
static bool gOverlayCanvasOff=false; // test switch (ST_SetPathUiTexturesV1 count -1): 0.23.2 behaviour
// 0.23.3: PT takes its HDR layers (stages 0 / 2) at the main camera's late composition (bridge event 3, before tone
// mapping). A camera that draws UI into the same target after that (VaM's desktop CameraHook: every world canvas on
// layer 5, among them all of that pack's VaMStory menus) came after the read and was dropped, text and all. As in
// VaM, where it draws over the already tone-mapped image, those draws go to the after-tone layer (stage 1 / 3) that
// OC composites at submission. Test switch: count -3 = off (0.23.2), -4 = on.
static bool gOverlayLateOff=false;
static uint64_t gOverlayLateDraws=0;
// 0.23.4 test switch (count -5 = off: 0.23.3 behaviour, no author-effect replay; -6 = on). The managed side has its own
// half (PathCanvasUi.TestAuthor -> RtScene.authorOff).
static bool gOverlayAuthorOff=false;
EXPORT void ST_SetPathUiTexturesV1(void* const* textures,int count) {
    std::vector<ID3D11Resource*> list;
    if(textures && count>0) {
        list.assign((ID3D11Resource* const*)textures,(ID3D11Resource* const*)textures+count);
        std::sort(list.begin(),list.end());list.erase(std::unique(list.begin(),list.end()),list.end());
        list.erase(std::remove(list.begin(),list.end(),nullptr),list.end());
    }
    AcquireSRWLockExclusive(&gUiTextureLock);
    if(count==-1)gOverlayCanvasOff=true;else if(count==-2)gOverlayCanvasOff=false;
    else if(count==-3)gOverlayLateOff=true;else if(count==-4)gOverlayLateOff=false;
    else if(count==-5)gOverlayAuthorOff=true;else if(count==-6)gOverlayAuthorOff=false;else gUiTextures.swap(list);
    ReleaseSRWLockExclusive(&gUiTextureLock);
}
static bool PathUiTextureBound(ID3D11DeviceContext* c) {
    AcquireSRWLockShared(&gUiTextureLock);bool none=gUiTextures.empty()||gOverlayCanvasOff;ReleaseSRWLockShared(&gUiTextureLock);
    if(none)return false;
    ID3D11ShaderResourceView* views[8]={};ID3D11DeviceContext_PSGetShaderResources(c,0,8,views);
    bool found=false;
    for(auto* view:views) {
        if(!view)continue;
        if(!found) {
            ID3D11Resource* resource=nullptr;ID3D11View_GetResource((ID3D11View*)view,&resource);
            if(resource) {
                AcquireSRWLockShared(&gUiTextureLock);
                found=std::binary_search(gUiTextures.begin(),gUiTextures.end(),resource);
                ReleaseSRWLockShared(&gUiTextureLock);
                ID3D11Resource_Release(resource);
            }
        }
        ID3D11ShaderResourceView_Release(view);
    }
    return found;
}
// A canvas element's own shader may read the scene behind it from a GrabPass copy of the camera target instead of
// blending with it (that pack's VaMStory buttons, HZM/UI/UIShader01: ShaderForge refraction glass,
// out = lerp(grab(uv + refraction), colour, alpha) with alpha 1). In PT that copy holds Unity's raster target, not the
// PT image. In the replay the copy is the replay base itself: black onto the black layer, white onto the white one,
// so the two layers give the glass's transmittance and the PT scene shows through it (without the refraction offset,
// which a flat base cannot carry). A GrabPass copy is a render-target texture of the target's own size; the textures
// a canvas element draws (in the UI set) never are.
static ID3D11ShaderResourceView* gGrabBase[2]={};
static uint64_t gOverlayGrabDraws=0;
static bool PathGrabBases() {
    if(gGrabBase[0] && gGrabBase[1])return true;
    for(int k=0;k<2;k++) {
        if(gGrabBase[k])continue;
        const uint32_t texel=k?0xffffffffu:0xff000000u;
        D3D11_TEXTURE2D_DESC d={};d.Width=d.Height=1;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data={&texel,4,4};ID3D11Texture2D* t=nullptr;
        if(FAILED(ID3D11Device_CreateTexture2D(gDevice,&d,&data,&t)))return false;
        HRESULT hr=ID3D11Device_CreateShaderResourceView(gDevice,(ID3D11Resource*)t,nullptr,&gGrabBase[k]);
        ID3D11Texture2D_Release(t);if(FAILED(hr))return false;
    }
    return true;
}
// Pixel-shader slots 0-7 bound to a GrabPass copy of a w x h target. views: what is bound (released by the caller).
static UINT PathGrabSlots(ID3D11DeviceContext* c,UINT w,UINT h,ID3D11ShaderResourceView* views[8]) {
    ID3D11DeviceContext_PSGetShaderResources(c,0,8,views);UINT slots=0;
    for(UINT k=0;k<8;k++) {
        if(!views[k])continue;
        ID3D11Resource* r=nullptr;ID3D11View_GetResource((ID3D11View*)views[k],&r);if(!r)continue;
        ID3D11Texture2D* t=nullptr;
        if(SUCCEEDED(ID3D11Resource_QueryInterface(r,IID_ID3D11Texture2D,(void**)&t))) {
            D3D11_TEXTURE2D_DESC d;ID3D11Texture2D_GetDesc(t,&d);ID3D11Texture2D_Release(t);
            if(d.Width==w && d.Height==h && d.ArraySize==1 && (d.BindFlags&D3D11_BIND_RENDER_TARGET)) {
                AcquireSRWLockShared(&gUiTextureLock);
                const bool ui=std::binary_search(gUiTextures.begin(),gUiTextures.end(),r);
                ReleaseSRWLockShared(&gUiTextureLock);
                if(!ui)slots|=1u<<k;
            }
        }
        ID3D11Resource_Release(r);
    }
    return slots;
}
static uint64_t PathProgramHash(ID3D11DeviceContext* c) {
    // Context-state swaps in post-processing do not call PSSetShader hooks.
    // Query the actual shader for unregistered draws, not stale hook metadata.
    ID3D11PixelShader* shader=nullptr;ID3D11DeviceContext_PSGetShader(c,&shader,nullptr,nullptr);
    uint64_t hash=0;
    AcquireSRWLockShared(&gLock);
    auto found=gPsHash.find(shader);if(found!=gPsHash.end())hash=found->second;
    ReleaseSRWLockShared(&gLock);
    if(shader)ID3D11PixelShader_Release(shader);
    return hash;
}
// Test only (PathCanvasUi.Dump): the previous frame's replay layers to <dir>\overlay-<stage>-<black|white>.bin
// (u32 width, height, DXGI format, row pitch; then the rows) plus overlay.txt. Taken at the next frame's start,
// when the composite has read them and before anything clears them.
static wchar_t gOverlayDumpDir[MAX_PATH];static volatile LONG gOverlayDumpRequest=0;
EXPORT int ST_DumpPathOverlayV1(const wchar_t* dir) {
    if(!dir||!*dir)return 0;
    wcsncpy_s(gOverlayDumpDir,dir,_TRUNCATE);InterlockedExchange(&gOverlayDumpRequest,1);return 1;
}
static void PathOverlayDump() {
    if(!InterlockedExchange(&gOverlayDumpRequest,0) || !gDevice || !gCtx)return;
    wchar_t name[MAX_PATH+64];swprintf_s(name,L"%s\\overlay.txt",gOverlayDumpDir);
    FILE* text=nullptr;_wfopen_s(&text,name,L"w");
    for(auto& layer:gPathOverlays) {
        if(text)fprintf(text,"stage %u %ux%u frame %d (now %d) draws %llu depth %d, composite took it in frame %d after %llu draws\n",layer.stage,layer.width,layer.height,layer.frame,(int)gFrames,(unsigned long long)layer.draws,layer.depth[0]?1:0,layer.readFrame,(unsigned long long)layer.readDraws);
        // bit 63: canvas element (0.23.3), bit 62: registered interface renderer
        if(text)for(int q=0;q<layer.seqN;q++)fprintf(text,"  %2d %016llx\n",q,(unsigned long long)layer.seq[q]);
        if(layer.frame<0 || gFrames-layer.frame!=1 || !layer.color[0])continue;
        for(int k=0;k<2;k++) {
            D3D11_TEXTURE2D_DESC d;ID3D11Texture2D_GetDesc(layer.color[k],&d);
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
            ID3D11Texture2D* staging=nullptr;
            if(FAILED(ID3D11Device_CreateTexture2D(gDevice,&d,nullptr,&staging)))continue;
            ID3D11DeviceContext_CopyResource(gCtx,(ID3D11Resource*)staging,(ID3D11Resource*)layer.color[k]);
            D3D11_MAPPED_SUBRESOURCE m;
            if(SUCCEEDED(ID3D11DeviceContext_Map(gCtx,(ID3D11Resource*)staging,0,D3D11_MAP_READ,0,&m))) {
                swprintf_s(name,L"%s\\overlay-%u-%ux%u-%s.bin",gOverlayDumpDir,layer.stage,layer.width,layer.height,k?L"white":L"black");
                FILE* f=nullptr;_wfopen_s(&f,name,L"wb");
                if(f) {
                    uint32_t head[4]={d.Width,d.Height,(uint32_t)d.Format,m.RowPitch};fwrite(head,4,4,f);
                    fwrite(m.pData,1,size_t(m.RowPitch)*d.Height,f);fclose(f);
                }
                ID3D11DeviceContext_Unmap(gCtx,(ID3D11Resource*)staging,0);
            }
            ID3D11Texture2D_Release(staging);
        }
    }
    if(text)fclose(text);
}
static void PathOverlayFrame() {
    PathOverlayDump();
    for(auto& layer:gPathOverlays)if((!RtPathRendering()&&!gRrActive) || (layer.frame>=0 && gFrames-layer.frame>2))layer.Release();
}
static PathOverlayLayer* PathOverlayPrepare(ID3D11DeviceContext* c,ID3D11RenderTargetView* original,ID3D11DepthStencilView* depth,bool effect) {
    ID3D11Resource* source=nullptr;ID3D11View_GetResource((ID3D11View*)original,&source);
    ID3D11Texture2D* texture=nullptr;HRESULT hr=ID3D11Resource_QueryInterface(source,IID_ID3D11Texture2D,(void**)&texture);
    if(FAILED(hr)){ID3D11Resource_Release(source);return nullptr;}
    D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(texture,&desc);ID3D11Texture2D_Release(texture);
    if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.Width<256 || desc.Height<256) {ID3D11Resource_Release(source);return nullptr;}
    D3D11_RENDER_TARGET_VIEW_DESC view;ID3D11RenderTargetView_GetDesc(original,&view);
    UINT stage=(view.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?0u:1u)+(effect?2u:0u);
    if(!(stage&1) && RtPathRendering() && !gOverlayLateOff)
        for(auto& item:gPathOverlays)if(item.source==source && item.stage==stage && item.frame==gFrames && item.readFrame==gFrames){stage++;gOverlayLateDraws++;break;}
    PathOverlayLayer* layer=nullptr;
    for(auto& item:gPathOverlays)if(item.source==source && item.stage==stage){layer=&item;break;}
    if(!layer)for(auto& item:gPathOverlays)if(item.frame!=gFrames){layer=&item;layer->Release();break;}
    if(!layer){gOverlayFailures++;ID3D11Resource_Release(source);return nullptr;}
    if(!layer->source) {
        layer->source=source;source=nullptr;layer->width=desc.Width;layer->height=desc.Height;
        layer->stage=stage;
        // Float capture preserves additive UI energy (white must not clip).
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.MipLevels=1;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags=desc.MiscFlags=0;
        for(int k=0;k<2 && SUCCEEDED(hr);k++) {
            hr=ID3D11Device_CreateTexture2D(gDevice,&desc,nullptr,&layer->color[k]);
            if(SUCCEEDED(hr))hr=ID3D11Device_CreateRenderTargetView(gDevice,(ID3D11Resource*)layer->color[k],nullptr,&layer->rtv[k]);
        }
        layer->bytes=uint64_t(desc.Width)*desc.Height*16;
    }
    if(source)ID3D11Resource_Release(source);
    if(FAILED(hr)){gOverlayFailures++;layer->Release();return nullptr;}
    ID3D11Resource* depthSource=nullptr;
    if(depth)ID3D11View_GetResource((ID3D11View*)depth,&depthSource);
    if(depthSource!=layer->sourceDepth) {
        for(int k=0;k<2;k++) {
            if(layer->dsv[k])ID3D11DepthStencilView_Release(layer->dsv[k]);layer->dsv[k]=nullptr;
            if(layer->depth[k])ID3D11Texture2D_Release(layer->depth[k]);layer->depth[k]=nullptr;
        }
        if(layer->sourceDepth)ID3D11Resource_Release(layer->sourceDepth);
        layer->sourceDepth=depthSource;depthSource=nullptr;
        layer->bytes=uint64_t(layer->width)*layer->height*16;
        if(layer->sourceDepth) {
            ID3D11Texture2D* dt=nullptr;
            hr=ID3D11Resource_QueryInterface(layer->sourceDepth,IID_ID3D11Texture2D,(void**)&dt);
            D3D11_DEPTH_STENCIL_VIEW_DESC vd;ID3D11DepthStencilView_GetDesc(depth,&vd);
            vd.Flags=0; // independent stencil/depth replay is writable
            if(SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC dd;ID3D11Texture2D_GetDesc(dt,&dd);ID3D11Texture2D_Release(dt);
                dd.Usage=D3D11_USAGE_DEFAULT;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;dd.CPUAccessFlags=dd.MiscFlags=0;
                for(int k=0;k<2 && SUCCEEDED(hr);k++) {
                    hr=ID3D11Device_CreateTexture2D(gDevice,&dd,nullptr,&layer->depth[k]);
                    if(SUCCEEDED(hr))hr=ID3D11Device_CreateDepthStencilView(gDevice,(ID3D11Resource*)layer->depth[k],&vd,&layer->dsv[k]);
                }
                layer->bytes+=uint64_t(dd.Width)*dd.Height*8;
            }
        }
        layer->frame=-1;
    }
    if(depthSource)ID3D11Resource_Release(depthSource);
    if(FAILED(hr)){gOverlayFailures++;layer->Release();return nullptr;}
    if(layer->frame!=gFrames) {
        // Both bases start with the scene's opaque alpha. Only RGB varies.
        const float black[4]={0,0,0,1},white[4]={1,1,1,1};
        ID3D11DeviceContext_ClearRenderTargetView(c,layer->rtv[0],black);
        ID3D11DeviceContext_ClearRenderTargetView(c,layer->rtv[1],white);
        for(int k=0;k<2;k++)if(layer->depth[k])ID3D11DeviceContext_CopyResource(c,(ID3D11Resource*)layer->depth[k],layer->sourceDepth);
        layer->frame=gFrames;layer->draws=0;layer->seqN=0;
    }
    return layer;
}
// 0.23.4: an author's effect the scene registry does not know (a particle system, a procedural or command-buffer draw
// with the author's own shader): drawn into the PT camera's own target, depth-tested against the scene, blending
// (not One/Zero) and writing colour. It is a forward effect by construction (Unity's transparent queue): replayed over
// PT like the stock particles instead of dropped. Opaque unknown draws stay out (a registered surface whose range the
// registry could not resolve would otherwise be drawn twice). Sky programs and the plugin's own shaders never.
static uint64_t gOverlayAuthorDraws=0,gOverlayRegisteredEffectDraws=0;
static bool PathAuthorEffect(ID3D11DeviceContext* c,uint64_t hash) {
    if(!RtPathRendering() || !SssPathTarget(c))return false;
    static const GUID ownedId={0xe32d8c57,0xd7c9,0x4a21,{0x91,0x65,0x43,0x2e,0xaa,0x57,0x19,0x31}};
    ID3D11PixelShader* ps=nullptr;ID3D11DeviceContext_PSGetShader(c,&ps,nullptr,nullptr);
    if(!ps)return false;
    UINT owned=0,bytes=sizeof owned;ID3D11PixelShader_GetPrivateData(ps,ownedId,&bytes,&owned);ID3D11PixelShader_Release(ps);
    if(owned)return false;
    if(std::binary_search(std::begin(kPathEnvironmentPrograms),std::end(kPathEnvironmentPrograms),hash))return false;
    ID3D11DepthStencilView* targetDepth=nullptr;ID3D11DeviceContext_OMGetRenderTargets(c,0,nullptr,&targetDepth);
    if(!targetDepth)return false;ID3D11DepthStencilView_Release(targetDepth);
    ID3D11DepthStencilState* state=nullptr;UINT stencil;ID3D11DeviceContext_OMGetDepthStencilState(c,&state,&stencil);
    D3D11_DEPTH_STENCIL_DESC depth={};depth.DepthEnable=true;
    if(state){ID3D11DepthStencilState_GetDesc(state,&depth);ID3D11DepthStencilState_Release(state);}
    if(!depth.DepthEnable)return false; // screen compositors and post effects
    ID3D11BlendState* blend=nullptr;FLOAT factor[4];UINT mask;ID3D11DeviceContext_OMGetBlendState(c,&blend,factor,&mask);
    if(!blend)return false;
    D3D11_BLEND_DESC desc;ID3D11BlendState_GetDesc(blend,&desc);ID3D11BlendState_Release(blend);
    const auto& rt=desc.RenderTarget[0];
    return rt.BlendEnable && rt.RenderTargetWriteMask && !(rt.SrcBlend==D3D11_BLEND_ONE && rt.DestBlend==D3D11_BLEND_ZERO);
}
// known: 0 the catalogs only, 1 a registered interface renderer (UI), 2 a registered author effect (0.23.4, RtScene
// RT_DRAW_EFFECT), 3 a draw the registry does not know (0.23.4: may be an author effect, PathAuthorEffect).
template<class Draw>static bool PathOverlayDraw(ID3D11DeviceContext* c,int known,const Draw& draw) {
    if(gOverlayReplay || c!=gCtx || !gReady || (!RtPathRendering()&&!gRrActive))return false;
    if(gOverlayAuthorOff && known==2)return false;
    const bool knownOverlay=known==1;
    uint64_t hash=knownOverlay?0:PathProgramHash(c);
    bool effect=known==2 || (!knownOverlay && std::binary_search(std::begin(kPathEffectPrograms),std::end(kPathEffectPrograms),hash));
    // Legacy already rasterizes effects before RR. Capture only UI coverage;
    // otherwise additive particles would be composited a second time.
    if(effect && !RtPathRendering())return false;
    if(known==2 && !SssPathTarget(c))return false; // mirrors and render-texture cameras draw it themselves
    bool listed=knownOverlay || effect || std::binary_search(std::begin(kPathOverlayPrograms),std::end(kPathOverlayPrograms),hash);
    // PT only: the legacy route's RR bypass mask keeps its 0.23.2 catalog. Only draws into the PT camera's own target:
    // the same textures are drawn into other cameras' targets (mirrors, render-texture cameras), which the composite
    // never reads and which would take the four replay layers (first candidate: PT paused on every such frame).
    bool canvas=false;
    if(!listed && RtPathRendering() && PathUiTextureBound(c)) {
        canvas=SssPathTarget(c);
        if(!canvas)gOverlayCanvasElsewhere++;
    }
    bool author=false;
    if(!listed && !canvas && known==3 && !gOverlayAuthorOff && PathAuthorEffect(c,hash)) {
        effect=listed=author=true;
        static uint64_t noted[64];static int notedCount=0;
        if(notedCount<64 && std::find(noted,noted+notedCount,hash)==noted+notedCount) {
            noted[notedCount++]=hash;char text[128];
            snprintf(text,sizeof text,"PT effect: unregistered blended draw replayed over PT as an effect (PS=%016llx)",(unsigned long long)hash);
            RtPathNotice(text);
        }
    }
    if(!listed && !canvas)return false;
    ID3D11BlendState* blend=nullptr;FLOAT factor[4];UINT mask;
    ID3D11DeviceContext_OMGetBlendState(c,&blend,factor,&mask);
    if(blend) {
        D3D11_BLEND_DESC desc;ID3D11BlendState_GetDesc(blend,&desc);ID3D11BlendState_Release(blend);
        const auto& rt=desc.RenderTarget[0];
        if(rt.BlendEnable && (rt.BlendOp==D3D11_BLEND_OP_MIN || rt.BlendOp==D3D11_BLEND_OP_MAX || rt.DestBlend==D3D11_BLEND_DEST_COLOR || rt.DestBlend==D3D11_BLEND_INV_DEST_COLOR)) {
            // 0.22.62: this one effect draw is not composited over PT (it no longer stops PT).
            if(RtPathRendering())RtPathNotice("PT effect with a min/max/multiply blend not composited");return false;
        }
    }
    ID3D11RenderTargetView* targets[8]={};ID3D11DepthStencilView* depth=nullptr;
    ID3D11DeviceContext_OMGetRenderTargets(c,8,targets,&depth);
    PathOverlayLayer* layer=nullptr;
    const uint64_t previousFailures=gOverlayFailures;
    bool simple=targets[0]!=nullptr;for(int k=1;k<8;k++)simple&=targets[k]==nullptr;
    if(simple)layer=PathOverlayPrepare(c,targets[0],depth,effect);
    if(gOverlayFailures!=previousFailures) {
        static int told=0;
        if(told<8) {
            told++;char text[160];unsigned w=0,h=0,f=0,used=0;
            ID3D11Resource* r=nullptr;ID3D11View_GetResource((ID3D11View*)targets[0],&r);
            ID3D11Texture2D* t2=nullptr;
            if(r && SUCCEEDED(ID3D11Resource_QueryInterface(r,IID_ID3D11Texture2D,(void**)&t2))){D3D11_TEXTURE2D_DESC d;ID3D11Texture2D_GetDesc(t2,&d);w=d.Width;h=d.Height;f=d.Format;ID3D11Texture2D_Release(t2);}
            if(r)ID3D11Resource_Release(r);
            for(auto& item:gPathOverlays)if(item.frame==gFrames)used++;
            snprintf(text,sizeof text,"PT UI replay failed: target %ux%u format %u, PT target %d, canvas %d, layers in use %u",w,h,f,SssPathTarget(c)?1:0,canvas?1:0,used);
            RtPathNotice(text);
        }
        RtInvalidatePathFrame("PT UI replay allocation/coverage failed");
    }
    if(layer) {
        gOverlayReplay=true;
        draw(); // ordinary Unity output retains its original state and stencil
        ID3D11ShaderResourceView* bound[8]={};
        // 0.23.4: effects too (an author's water, glass or distortion particle reads its GrabPass copy: in PT that is the
        // raster target, not the PT image; the bases give its transmittance, without the refraction offset).
        const UINT grab=(canvas || (effect && RtPathRendering() && !gOverlayAuthorOff)) && PathGrabBases()?PathGrabSlots(c,layer->width,layer->height,bound):0;
        for(int k=0;k<2;k++) {
            for(UINT s=0;s<8;s++)if(grab&(1u<<s))ID3D11DeviceContext_PSSetShaderResources(c,s,1,&gGrabBase[k]);
            ID3D11DeviceContext_OMSetRenderTargets(c,1,&layer->rtv[k],layer->dsv[k]);draw();
        }
        for(UINT s=0;s<8;s++)if(grab&(1u<<s))ID3D11DeviceContext_PSSetShaderResources(c,s,1,&bound[s]);
        for(auto* v:bound)if(v)ID3D11ShaderResourceView_Release(v);
        if(grab)gOverlayGrabDraws++;
        ID3D11DeviceContext_OMSetRenderTargets(c,8,targets,depth);
        // bit 61 (0.23.4): an author effect, registered or not
        if(layer->seqN<32)layer->seq[layer->seqN++]=hash|(canvas?1ull<<63:0)|(knownOverlay?1ull<<62:0)|(author||known==2?1ull<<61:0);
        gOverlayReplay=false;layer->draws++;gOverlayDraws++;
        if(effect)gOverlayEffectDraws++;else gOverlayUiDraws++;
        if(author)gOverlayAuthorDraws++;
        if(known==2)gOverlayRegisteredEffectDraws++;
        if(canvas) {
            gOverlayCanvasDraws++;
            static uint64_t noted[64];static int notedCount=0;
            if(notedCount<64 && std::find(noted,noted+notedCount,hash)==noted+notedCount) {
                noted[notedCount++]=hash;char text[128];
                snprintf(text,sizeof text,"PT UI: canvas element drawn with its own shader, composited as UI (PS=%016llx)",(unsigned long long)hash);
                RtPathNotice(text);
            }
        }
    }
    for(auto* t:targets)if(t)ID3D11RenderTargetView_Release(t);
    if(depth)ID3D11DepthStencilView_Release(depth);
    return layer!=nullptr;
}
// 11-12: the late (after-tone) layers are taken when the frame is presented. The desktop presents before the frame's
// OP_FRAME (WaitForEndOfFrame); a VR runtime takes the eyes after it, when gFrames already names the next frame, so the
// late layer never matched and UI drawn after the PT composite (a stage's own UI camera, VaM's main menu there) was lost in VR.
// The frame just ended still counts for a late layer, unless a read during an earlier frame took it (no stale UI).
static bool PathOverlayCurrent(const PathOverlayLayer& layer) {
    if(layer.frame==gFrames)return true;
    return (layer.stage&1) && layer.frame>=0 && layer.frame==gFrames-1 && (layer.readFrame!=layer.frame || layer.readAt==gFrames);
}
// Borrowed on the same render thread/queue. Only this frame's capture is valid (a late layer: see PathOverlayCurrent).
EXPORT int ST_GetPathOverlayV1(UINT stage,UINT width,UINT height,ID3D11Resource** black,ID3D11Resource** white) {
    if(!black||!white)return 0;*black=*white=nullptr;
    for(auto& layer:gPathOverlays)if(PathOverlayCurrent(layer) && layer.stage==stage && layer.width==width && layer.height==height && layer.draws) {
        layer.readDraws=layer.draws;layer.readFrame=layer.frame;layer.readAt=gFrames;
        *black=(ID3D11Resource*)layer.color[0];*white=(ID3D11Resource*)layer.color[1];return 1;
    }
    return 0;
}
