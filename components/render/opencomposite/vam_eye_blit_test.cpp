#include "OpenOVR/Compositor/dx11_eye_blit.h"
#include <cstdio>
#include <cstdint>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if(FAILED(hr)) { std::printf("HRESULT 0x%08lx\n", (unsigned long)hr); throw std::runtime_error("D3D operation failed"); } }
int main() {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
        D3D11EyeBlitter blitter;
        check(blitter.Initialize(device.Get(), context.Get()));
        constexpr uint32_t red=0xff0000ff, green=0xff00ff00, blue=0xffff0000, yellow=0xff00ffff;
        uint32_t pixels[8]={red,red,green,green,blue,blue,yellow,yellow};
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width=4; sd.Height=2; sd.MipLevels=sd.ArraySize=1;
        sd.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS; sd.SampleDesc.Count=1;
        sd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixels, 16, 0};
        ComPtr<ID3D11Texture2D> source;
        check(device->CreateTexture2D(&sd, &initial, &source));
        D3D11_TEXTURE2D_DESC dd=sd;
        dd.Width=2; dd.Format=DXGI_FORMAT_R8G8B8A8_UNORM; dd.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        check(device->CreateTexture2D(&dd, nullptr, &target));
        ComPtr<ID3D11RenderTargetView> rtv;
        check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        dd.Usage=D3D11_USAGE_STAGING; dd.BindFlags=0; dd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        check(device->CreateTexture2D(&dd, nullptr, &readback));
        const float crops[4][4]={{0,1,.5f,0},{.5f,1,1,0},{0,0,.5f,1},{.5f,0,1,1}};
        const uint32_t expected[4][2]={{blue,red},{yellow,green},{red,blue},{green,yellow}};
        D3D11_VIEWPORT sentinel{3,4,10,11,.2f,.8f};
        context->RSSetViewports(1,&sentinel);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        for(int test=0;test<4;++test) {
            check(blitter.Copy(source.Get(),rtv.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,2,2,0,crops[test]));
            D3D11_VIEWPORT restored{}; UINT count=1;
            context->RSGetViewports(&count,&restored);
            D3D11_PRIMITIVE_TOPOLOGY topology;
            context->IAGetPrimitiveTopology(&topology);
            if(count!=1 || std::memcmp(&restored,&sentinel,sizeof(sentinel)) || topology!=D3D11_PRIMITIVE_TOPOLOGY_LINELIST) throw std::runtime_error("application graphics state was changed");
            context->CopyResource(readback.Get(),target.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&map));
            bool correct=true;
            for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
                uint32_t value=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(map.pData)+y*map.RowPitch)[x];
                if(value!=expected[test][y]) { std::printf("test=%d pixel=%d,%d actual=%08x expected=%08x\n",test,x,y,value,expected[test][y]); correct=false; }
            }
            context->Unmap(readback.Get(),0);
            if(!correct) throw std::runtime_error("stereo crop/flip pixels differ");
        }
        std::puts("PASS: left/right crop, vertical flip, upright copy, typeless source, application pipeline restored.");
        return 0;
    } catch(const std::exception& ex) { std::puts(ex.what()); return 1; }
}
