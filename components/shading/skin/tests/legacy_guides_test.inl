void SssTestRR(bool enabled);
void SssTestGuides(ID3D11Texture2D **out);
static int PartC() {
    const char* code=R"(
float4 VS(uint id:SV_VertexID):SV_Position {
    float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),.5,1);
}
struct O{float4 c:SV_Target0;float4 e:SV_Target1;float4 a:SV_Target2;float z:SV_Target3;
         float4 t:SV_Target4;float4 n:SV_Target5;float4 s:SV_Target6;};
O PS(float4 p:SV_Position){O o;o.c=float4(.2,.2,.2,1);o.e=float4(.3,.3,.3,1);
    o.a=float4(.5,.5,.5,1);o.z=p.z;o.t=float4(.1,.1,.1,1);
    o.n=float4(0,0,1,.6);o.s=float4(.04,.04,.04,1);return o;}
)";
    auto vsc=Compile(code,"VS","vs_5_0"),psc=Compile(code,"PS","ps_5_0");
    if(vsc.empty()||psc.empty())return 1;
    ID3D11VertexShader* vs=nullptr;ID3D11PixelShader* ps=nullptr;
    ID3D11Device_CreateVertexShader(dev,vsc.data(),vsc.size(),nullptr,&vs);
    ID3D11Device_CreatePixelShader(dev,psc.data(),psc.size(),nullptr,&ps);
    auto color=Tex(128,64,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE);
    auto depth=Tex(128,64,DXGI_FORMAT_R32_TYPELESS,D3D11_BIND_DEPTH_STENCIL);
    ID3D11RenderTargetView* rtv=nullptr;ID3D11DepthStencilView* dsv=nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT;dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)color,nullptr,&rtv);
    ID3D11Device_CreateDepthStencilView(dev,(ID3D11Resource*)depth,&dv,&dsv);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ID3D11RasterizerState* raster=nullptr;ID3D11Device_CreateRasterizerState(dev,&rd,&raster);
    struct Mode {bool rr;float sss,trans;bool guideOnly;};
    const Mode modes[]={{true,0,0,false},{true,1,0,true},{true,0,1,false},{false,0,1,false},{false,0,0,false}};
    int failures=0;
    for(unsigned i=0;i<5;i++) {
        auto m=modes[i];ID3D11DeviceContext_ClearState(ctx);
        SssTestRR(m.rr);SssSetParams(m.sss,1,1.17f);SssSetTranslucency(m.trans,1);
        float zero[4]={};ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,zero);
        ID3D11DeviceContext_ClearDepthStencilView(ctx,dsv,D3D11_CLEAR_DEPTH,1,0);
        D3D11_VIEWPORT vp{0,0,128,64,0,1};ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);
        ID3D11DeviceContext_RSSetState(ctx,raster);ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&rtv,dsv);
        ID3D11DeviceContext_VSSetShader(ctx,vs,nullptr,0);ID3D11DeviceContext_PSSetShader(ctx,ps,nullptr,0);
        SssBegin(2);SssDrawSave saved;bool bound=SssBeforeDraw(ctx,saved,m.guideOnly);
        ID3D11DeviceContext_Draw(ctx,3,0);if(bound)SssAfterDraw(ctx,saved);
        SssEnd(ctx,(SssSetPSFn)ctx->lpVtbl->PSSetShader);SssFrame();
        auto pixels=ReadBack(color,8);auto data=reinterpret_cast<const uint16_t*>(pixels.data());
        float expected=.2f+(m.trans>0?Q8(.5f)*.1f:0.f),error=0;
        for(unsigned p=0;p<128*64;p++)for(unsigned ch=0;ch<3;ch++)error=std::max(error,std::abs(FromHalf(data[p*4+ch])-expected));
        ID3D11Texture2D* targets[8]={},*guides[3]={};SssTestTargets(targets);SssTestGuides(guides);
        bool resourceOk=true,guideOk=true;
        if(m.sss==0&&m.trans==0)for(unsigned k:{0u,3u,4u,5u,6u,7u})resourceOk&=targets[k]==nullptr;
        if(m.rr) {
            guideOk=guides[0]&&guides[1]&&targets[1]&&targets[2];
            if(guideOk){auto bytes=ReadBack(guides[0],8);auto n=reinterpret_cast<const uint16_t*>(bytes.data());guideOk=std::abs(FromHalf(n[2])-1)<.001f&&std::abs(FromHalf(n[3])-.6f)<.001f;}
        } else for(auto g:guides)resourceOk&=g==nullptr;
        if(i==4)for(auto t:targets)resourceOk&=t==nullptr;
        int debug=DrainMessages("independent RR / diffusion / translucency");
        bool pass=bound==(i!=4)&&error<.002f&&resourceOk&&guideOk&&debug==0;failures+=pass?0:1;
        printf("C mode%u rr=%d sss=%.0f trans=%.0f guideOnly=%d bound=%d error=%.7g resources=%d guides=%d debug=%d %s\n",
            i,m.rr,m.sss,m.trans,m.guideOnly,bound,error,resourceOk,guideOk,debug,pass?"PASS":"FAILED");
    }
    SssTestRR(false);ID3D11DeviceContext_ClearState(ctx);
    ID3D11VertexShader_Release(vs);ID3D11PixelShader_Release(ps);ID3D11RenderTargetView_Release(rtv);
    ID3D11DepthStencilView_Release(dsv);ID3D11Texture2D_Release(color);ID3D11Texture2D_Release(depth);ID3D11RasterizerState_Release(raster);
    return failures?1:0;
}
