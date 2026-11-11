#pragma once
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>

// Copy normalized OpenVR eye bounds to a complete OpenXR eye image. A private
// D3D11.1 context state keeps the application's pipeline intact across the draw.
class D3D11EyeBlitter {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext1> context;
    Ptr<ID3DDeviceContextState> state;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11SamplerState> sampler;
    Ptr<ID3D11Buffer> constants;
public:
    HRESULT Initialize(ID3D11Device* d, ID3D11DeviceContext* c) {
        device = d;
        Ptr<ID3D11Device1> d1;
        HRESULT hr = d->QueryInterface(IID_PPV_ARGS(&d1));
        if (FAILED(hr)) return hr;
        hr = c->QueryInterface(IID_PPV_ARGS(&context));
        if (FAILED(hr)) return hr;
        D3D_FEATURE_LEVEL level = d->GetFeatureLevel();
        UINT flags = (d->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        hr = d1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state);
        if (FAILED(hr)) return hr;
        constexpr char shader[] = R"(
cbuffer Crop : register(b0) { float4 bounds; };
Texture2DArray<float4> source : register(t0);
SamplerState sampleState : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vertexMain(uint id : SV_VertexID) {
    Vertex v;
    float2 p = float2(id & 1, id >> 1);
    v.position = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    v.uv = lerp(bounds.xy, bounds.zw, p);
    return v;
}
float4 pixelMain(Vertex v) : SV_Target { return source.Sample(sampleState, float3(v.uv, 0)); }
)";
        Ptr<ID3DBlob> vb, pb, errors;
        hr = D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "vertexMain", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vb, &errors);
        if (FAILED(hr)) return hr;
        hr = D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "pixelMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pb, &errors);
        if (FAILED(hr)) return hr;
        hr = d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
        if (FAILED(hr)) return hr;
        hr = d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);
        if (FAILED(hr)) return hr;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = d->CreateSamplerState(&sd, &sampler);
        if (FAILED(hr)) return hr;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return d->CreateBuffer(&bd, nullptr, &constants);
    }

    HRESULT Copy(ID3D11Texture2D* src, ID3D11RenderTargetView* target,
                 DXGI_FORMAT format, UINT width, UINT height, UINT slice, const float* bounds, UINT targetX = 0) {
        D3D11_TEXTURE2D_DESC desc{};
        src->GetDesc(&desc);
        if (slice >= desc.ArraySize) return E_INVALIDARG;
        Ptr<ID3D11Texture2D> resolved;
        if (desc.SampleDesc.Count > 1) {
            D3D11_TEXTURE2D_DESC rd = desc;
            rd.MipLevels = 1;
            rd.ArraySize = 1;
            rd.Format = format;
            rd.SampleDesc = {1, 0};
            rd.Usage = D3D11_USAGE_DEFAULT;
            rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            rd.CPUAccessFlags = rd.MiscFlags = 0;
            HRESULT hr = device->CreateTexture2D(&rd, nullptr, &resolved);
            if (FAILED(hr)) return hr;
            context->ResolveSubresource(resolved.Get(), 0, src, D3D11CalcSubresource(0, slice, desc.MipLevels), format);
            src = resolved.Get();
            slice = 0;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.FirstArraySlice = slice;
        view.Texture2DArray.ArraySize = 1;
        Ptr<ID3D11ShaderResourceView> srv;
        HRESULT hr = device->CreateShaderResourceView(src, &view, &srv);
        if (FAILED(hr)) return hr;

        Ptr<ID3DDeviceContextState> previous;
        context->SwapDeviceContextState(state.Get(), &previous);
        context->UpdateSubresource(constants.Get(), 0, nullptr, bounds, 0, 0);
        context->ClearState();
        D3D11_VIEWPORT viewport{static_cast<float>(targetX), 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &target, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->PSSetShaderResources(0, 1, srv.GetAddressOf());
        context->Draw(4, 0);
        context->ClearState();
        context->SwapDeviceContextState(previous.Get(), nullptr);
        return S_OK;
    }
};
