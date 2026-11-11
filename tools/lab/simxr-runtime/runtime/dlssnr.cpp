// MIT License
//
// Copyright(c) 2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "log.h"
#include "runtime.h"
#include "utils.h"
#include "DlssnrFeatherPS.h"

#define NVSDK_NGX_Parameter_DLSSNR_Width "DLSSNR.Width"
#define NVSDK_NGX_Parameter_DLSSNR_Height "DLSSNR.Height"
#define NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset "DLSSNR.Hint.Render.Preset"
#define NVSDK_NGX_Parameter_DLSSNR_Enabled "DLSSNR.Enabled"
#define NVSDK_NGX_Parameter_DLSSNR_DepthInverted "DLSSNR.DepthInverted"

#define NVSDK_NGX_Parameter_DLSSNR_Color "DLSSNR.Color"
#define NVSDK_NGX_Parameter_DLSSNR_Output "DLSSNR.Output"
#define NVSDK_NGX_Parameter_DLSSNR_Depth "DLSSNR.Depth"
#define NVSDK_NGX_Parameter_DLSSNR_MVec "DLSSNR.MVec"
#define NVSDK_NGX_Parameter_DLSSNR_MVecScaleX "DLSSNR.MVecScaleX"
#define NVSDK_NGX_Parameter_DLSSNR_MVecScaleY "DLSSNR.MVecScaleY"
#define NVSDK_NGX_Parameter_DLSSNR_Reset "DLSSNR.Reset"
#define NVSDK_NGX_Parameter_DLSSNR_UICorrection "DLSSNR.UICorrection"

#define NVSDK_NGX_Parameter_DLSSNR_Style "DLSSNR.Style"
#define NVSDK_NGX_Parameter_DLSSNR_Intensity "DLSSNR.Intensity"
#define NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength "DLSSNR.LocalToneStrength"
#define NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength "DLSSNR.LocalStructureStrength"
#define NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength "DLSSNR.SkinStructureStrength"
#define NVSDK_NGX_Parameter_DLSSNR_UseAutoMask "DLSSNR.UseAutoMask"

#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX "DLSSNR.ColorSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY "DLSSNR.ColorSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth "DLSSNR.ColorSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight "DLSSNR.ColorSubrectHeight"

#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX "DLSSNR.OutputSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY "DLSSNR.OutputSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth "DLSSNR.OutputSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight "DLSSNR.OutputSubrectHeight"

#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX "DLSSNR.MVecSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY "DLSSNR.MVecSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth "DLSSNR.MVecSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight "DLSSNR.MVecSubrectHeight"

#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX "DLSSNR.DepthSubrectBaseX"
#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY "DLSSNR.DepthSubrectBaseY"
#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth "DLSSNR.DepthSubrectWidth"
#define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight "DLSSNR.DepthSubrectHeight"

namespace ngx {

    const bool forceManualLoad = false;
    wil::unique_hmodule dlssnrModule;
    auto NVSDK_NGX_D3D12_CreateFeature = &::NVSDK_NGX_D3D12_CreateFeature;
    auto NVSDK_NGX_D3D12_ReleaseFeature = &::NVSDK_NGX_D3D12_ReleaseFeature;
    auto NVSDK_NGX_D3D12_EvaluateFeature = &::NVSDK_NGX_D3D12_EvaluateFeature;

    // Hack to avoid NVSDK_NGX_Result_FAIL_PlatformError when loading the DLSS-NR DLL outside of NGX. Pretend that the
    // caller is NGX.
    DEFINE_DETOUR_FUNCTION(DWORD, GetModuleFileNameW, HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
        HMODULE callerModule;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)_ReturnAddress(),
                           &callerModule);
        if (callerModule == dlssnrModule.get()) {
            static const std::wstring nvngx = L"nvngx.dll";
            _snwprintf_s(lpFilename, nSize, _TRUNCATE, nvngx.c_str());
            if (nSize < nvngx.size() + 1) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
            }
            return nSize;
        }

        return original_GetModuleFileNameW(hModule, lpFilename, nSize);
    }

} // namespace ngx

namespace virtualdesktop_openxr {

    using namespace virtualdesktop_openxr::log;
    using namespace virtualdesktop_openxr::utils;

    void OpenXrRuntime::upliftLayer(const XrSwapchainSubImage** views,
                                    const XrSwapchainSubImage** depth,
                                    float nearZ,
                                    float farZ,
                                    ovrLayerEyeFov& layer) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "DLSSNR");

        const ovrSizei resolution = {std::max(layer.Viewport[0].Size.w, layer.Viewport[1].Size.w),
                                     std::max(layer.Viewport[0].Size.h, layer.Viewport[1].Size.h)};
        const float foveatedScale = std::clamp(m_dlssnrFoveationSize, 0.1f, 1.f);
        const ovrSizei foveatedResolution = {(int)xr::math::AlignTo<4>((uint32_t)(resolution.w * foveatedScale)),
                                             (int)xr::math::AlignTo<4>((uint32_t)(resolution.h * foveatedScale))};
        // TODO: Plumb in the eye tracking data.
        const ovrVector2i offset = {(resolution.w - foveatedResolution.w) / 2,
                                    (resolution.h - foveatedResolution.h) / 2};

        struct BOX {
            int32_t left;
            int32_t top;
            int32_t right;
            int32_t bottom;
        };
        const auto SafeClamp = [](BOX& box, const BOX& limit) {
            box.left = std::max(limit.left, box.left);
            box.right = std::min(limit.right, box.right);
            box.top = std::max(limit.top, box.top);
            box.bottom = std::min(limit.bottom, box.bottom);
        };

        const BOX inputBox[2] = {{views[0]->imageRect.offset.x,
                                  views[0]->imageRect.offset.y,
                                  views[0]->imageRect.offset.x + views[0]->imageRect.extent.width,
                                  views[0]->imageRect.offset.y + views[0]->imageRect.extent.height},
                                 {views[1]->imageRect.offset.x,
                                  views[1]->imageRect.offset.y,
                                  views[1]->imageRect.offset.x + views[1]->imageRect.extent.width,
                                  views[1]->imageRect.offset.y + views[1]->imageRect.extent.height}};

        const BOX outputBox[2] = {{0, 0, resolution.w, resolution.h},
                                  {resolution.w, 0, 2 * resolution.w, resolution.h}};

        // Resize resources if needed.
        if (!m_dlssnrOutputSwapchain || m_dlssnrOutputSwapchainResolution.w < resolution.w ||
            m_dlssnrOutputSwapchainResolution.h < resolution.h) {
            // Finish in-flight work before releasing the resources.
            m_dlssnrContext->Flush();

            Swapchain& xrSwapchain = *(Swapchain*)views[0]->swapchain;
            ensureDlssnrSwapchainResources(xrSwapchain.ovrDesc.Format, resolution);

            m_dlssnrOutputSwapchainResolution = resolution;
        }
        if (m_dlssnrFeatureResolution.w < foveatedResolution.w || m_dlssnrFeatureResolution.h < foveatedResolution.h) {
            // Finish in-flight work before releasing the resources.
            m_dlssnrContext->Flush();

            // Destroy DLSS-NR feature - it will be re-created in the loop further below.
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                if (m_dlssnrFeature[eye]) {
                    ngx::NVSDK_NGX_D3D12_ReleaseFeature(m_dlssnrFeature[eye]);
                    m_dlssnrFeature[eye] = nullptr;
                }
            }

            m_dlssnrFeatureResolution = foveatedResolution;
        }

        // Serialize inputs.
#if 1
        m_fenceValue++;
        CHECK_HRCMD(m_ovrSubmissionContext->Signal(m_ovrSubmissionFence.Get(), m_fenceValue));
        CHECK_HRCMD(m_dlssnrContext->GetCommandQueue()->Wait(m_dlssnrInFence.Get(), m_fenceValue));
#else
        flushSubmissionContext();
#endif

        // Time to apply the effect.
        auto cmdList = m_dlssnrContext->GetCommandList();
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthInverted, nearZ > farZ);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Enabled, true);

            // (Re)create with the desired resolution.
            if (!m_dlssnrFeature[eye]) {
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Width, foveatedResolution.w);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Height, foveatedResolution.h);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, 0);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

                if (ngx::forceManualLoad ||
                    NVSDK_NGX_FAILED(ngx::NVSDK_NGX_D3D12_CreateFeature(cmdList.Commands.Get(),
                                                                        NVSDK_NGX_Feature_Reserved18 /* DLSS-NR */,
                                                                        m_ngxParameters,
                                                                        &m_dlssnrFeature[eye]))) {
                    // User might be using an unsigned nvngx_dlssnr.dll that gets bounced by the NGX SDK. Try loading it
                    // manually.
                    if (!ngx::dlssnrModule) {
                        NVSDK_NGX_Result(NVSDK_CONV * Init_Ext)(unsigned long long,
                                                                const wchar_t*,
                                                                ID3D12Device*,
                                                                NVSDK_NGX_Version,
                                                                const NVSDK_NGX_Parameter*);
                        DetourDllAttach("Kernel32.dll",
                                        "GetModuleFileNameW",
                                        ngx::hooked_GetModuleFileNameW,
                                        ngx::original_GetModuleFileNameW);

                        const auto nvngx_dlssnrPath = virtualdesktop_openxr::dllHome / L"nvngx_dlssnr.dll";
                        *ngx::dlssnrModule.put() = LoadLibraryW(nvngx_dlssnrPath.c_str());
                        // clang-format off
                        Init_Ext = (decltype(Init_Ext))GetProcAddress(ngx::dlssnrModule.get(), "NVSDK_NGX_D3D12_Init_Ext");
                        ngx::NVSDK_NGX_D3D12_CreateFeature = (decltype(ngx::NVSDK_NGX_D3D12_CreateFeature))GetProcAddress(
                            ngx::dlssnrModule.get(), "NVSDK_NGX_D3D12_CreateFeature");
                        ngx::NVSDK_NGX_D3D12_ReleaseFeature = (decltype(ngx::NVSDK_NGX_D3D12_ReleaseFeature))GetProcAddress(
                            ngx::dlssnrModule.get(), "NVSDK_NGX_D3D12_ReleaseFeature");
                        ngx::NVSDK_NGX_D3D12_EvaluateFeature = (decltype(ngx::NVSDK_NGX_D3D12_EvaluateFeature))GetProcAddress(
                            ngx::dlssnrModule.get(), "NVSDK_NGX_D3D12_EvaluateFeature");
                        // clang-format on

                        CHECK_NGXCMD(Init_Ext(123456,
                                              virtualdesktop_openxr::programData.c_str(),
                                              m_dlssnrDevice.Get(),
                                              NVSDK_NGX_Version_API,
                                              nullptr));
                    }

                    CHECK_NGXCMD(ngx::NVSDK_NGX_D3D12_CreateFeature(cmdList.Commands.Get(),
                                                                    NVSDK_NGX_Feature_Reserved18 /* DLSS-NR */,
                                                                    m_ngxParameters,
                                                                    &m_dlssnrFeature[eye]));
                }
            }

            // Prepare swapchain outputs.
            int imageIndex = 0;
            CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(m_ovrSession, m_dlssnrOutputSwapchain, &imageIndex));

            // Prepare feature evaluation.
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Output, m_dlssnrOutputSwapchainImages[imageIndex].Get());

            Swapchain& xrSwapchain = *(Swapchain*)views[eye]->swapchain;
            ID3D12Resource* inputImage =
                xrSwapchain.resolvedSlices[views[eye]->imageArrayIndex]
                    .dlssnrImages[xrSwapchain.resolvedSlices[views[eye]->imageArrayIndex].lastCommittedIndex]
                    .Get();
            BOX colorBox{};
            colorBox.left = views[eye]->imageRect.offset.x + offset.x;
            colorBox.top = views[eye]->imageRect.offset.y + offset.y;
            colorBox.right = colorBox.left + foveatedResolution.w;
            colorBox.bottom = colorBox.top + foveatedResolution.h;
            SafeClamp(colorBox, inputBox[eye]);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Color, inputImage);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, colorBox.left);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY, colorBox.top);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, colorBox.right - colorBox.left);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight, colorBox.bottom - colorBox.top);

            BOX destBox{};
            destBox.left = eye * resolution.w + offset.x; // Double-wide.
            destBox.top = offset.y;
            destBox.right = destBox.left + foveatedResolution.w;
            destBox.bottom = destBox.top + foveatedResolution.h;
            SafeClamp(destBox, outputBox[eye]);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, destBox.left);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY, destBox.top);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, destBox.right - destBox.left);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight, destBox.bottom - destBox.top);

#if 0
            // TODO: Use NVOFA to estimate motion.
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVec, motion);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, 0);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY, 0);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, w);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight, h);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, motionVectorScaleX);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, motionVectorScaleY);
#endif

            if (depth[0] && depth[1]) {
                Swapchain& xrDepthSwapchain = *(Swapchain*)depth[eye]->swapchain;
                m_ngxParameters->Set(
                    NVSDK_NGX_Parameter_DLSSNR_Depth,
                    xrDepthSwapchain.resolvedSlices[depth[eye]->imageArrayIndex]
                        .dlssnrImages[xrDepthSwapchain.resolvedSlices[depth[eye]->imageArrayIndex].lastCommittedIndex]
                        .Get());
                BOX depthBox{};
                depthBox.left = depth[eye]->imageRect.offset.x + offset.x;
                depthBox.top = depth[eye]->imageRect.offset.y + offset.y;
                depthBox.right = depthBox.left + foveatedResolution.w;
                depthBox.bottom = depthBox.top + foveatedResolution.h;
                SafeClamp(depthBox, inputBox[eye]);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Color, inputImage);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, depthBox.left);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY, depthBox.top);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, depthBox.right - depthBox.left);
                m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, depthBox.bottom - depthBox.top);
            }

            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Reset, false);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, true);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Style, m_dlssnrStyle);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_Intensity, m_dlssnrIntensity);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, m_dlssnrLocalToneStrength);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength, m_dlssnrLocalStructureStrength);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, m_dlssnrSkinStructureStrength);
            m_ngxParameters->Set(NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);

            if (foveatedScale < 1.f) {
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                src.pResource = inputImage;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                dst.pResource = m_dlssnrOutputSwapchainImages[imageIndex].Get();

                const auto ToD3dBox = [](D3D12_BOX& d3dBox, const BOX& box) {
                    d3dBox.left = box.left;
                    d3dBox.top = box.top;
                    d3dBox.front = 0;
                    d3dBox.right = box.right;
                    d3dBox.bottom = box.bottom;
                    d3dBox.back = 1;
                };

                D3D12_BOX d3dBox;
                BOX box{};
                box.left = views[eye]->imageRect.offset.x;
                box.top = views[eye]->imageRect.offset.y;
                box.right = box.left + resolution.w;
                box.bottom = box.top + offset.y;
                SafeClamp(box, inputBox[eye]);
                ToD3dBox(d3dBox, box);
                cmdList.Commands->CopyTextureRegion(&dst, eye * resolution.w, 0, 0, &src, &d3dBox);
                box.top = views[eye]->imageRect.offset.y + offset.y + foveatedResolution.h;
                box.bottom = box.top + resolution.h;
                SafeClamp(box, inputBox[eye]);
                ToD3dBox(d3dBox, box);
                cmdList.Commands->CopyTextureRegion(
                    &dst, eye * resolution.w, resolution.h - (box.bottom - box.top), 0, &src, &d3dBox);

                box.left = views[eye]->imageRect.offset.x;
                box.top = views[eye]->imageRect.offset.y + offset.y;
                box.right = box.left + offset.x;
                box.bottom = box.top + foveatedResolution.h;
                SafeClamp(box, inputBox[eye]);
                ToD3dBox(d3dBox, box);
                cmdList.Commands->CopyTextureRegion(
                    &dst, eye * resolution.w, box.top - views[eye]->imageRect.offset.y, 0, &src, &d3dBox);
                box.left = views[eye]->imageRect.offset.x + offset.x + foveatedResolution.w;
                box.right = box.left + resolution.w;
                SafeClamp(box, inputBox[eye]);
                ToD3dBox(d3dBox, box);
                cmdList.Commands->CopyTextureRegion(&dst,
                                                    (eye + 1) * resolution.w - (box.right - box.left),
                                                    box.top - views[eye]->imageRect.offset.y,
                                                    0,
                                                    &src,
                                                    &d3dBox);
            }

            // Go!
            CHECK_NGXCMD(ngx::NVSDK_NGX_D3D12_EvaluateFeature(
                cmdList.Commands.Get(), m_dlssnrFeature[eye], m_ngxParameters, nullptr));
        }

        // Serialize and commit output.
        const auto fenceValue = m_dlssnrContext->SubmitCommandList(cmdList);
        CHECK_HRCMD(m_ovrSubmissionContext->Wait(m_dlssnrOutFence.Get(), fenceValue));
        if (foveatedScale < 1.f) {
            // The D3D11 queue waits for NR before blending its inner border.
            // No extra neural evaluations or full-frame intermediate copies.
            featherDlssnrRegion(views, resolution, foveatedResolution, offset);
        }
        CHECK_OVRCMD(ovr_CommitTextureSwapChain(m_ovrSession, m_dlssnrOutputSwapchain));

        // Patch the layer.
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            layer.ColorTexture[eye] = m_dlssnrOutputSwapchain;
            layer.Viewport[eye].Pos = {(int)eye * resolution.w, 0};
            layer.Viewport[eye].Size = resolution;
        }

        TraceLoggingWriteStop(local, "DLSSNR");
    }

    void OpenXrRuntime::featherDlssnrRegion(const XrSwapchainSubImage** views,
                                           const ovrSizei& resolution,
                                           const ovrSizei& region,
                                           const ovrVector2i& offset) {
        struct Constants {
            int32_t inputBase[2];
            int32_t outputBase[2];
            int32_t regionSize[2];
            float featherPixels;
            float padding;
        };
        static_assert(sizeof(Constants) == 32);
        if (!m_dlssnrFeatherPS) {
            CHECK_HRCMD(m_ovrSubmissionDevice->CreatePixelShader(
                g_DlssnrFeatherPS, sizeof(g_DlssnrFeatherPS), nullptr, m_dlssnrFeatherPS.GetAddressOf()));
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = sizeof(Constants);
            buffer.Usage = D3D11_USAGE_DYNAMIC;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            CHECK_HRCMD(m_ovrSubmissionDevice->CreateBuffer(&buffer, nullptr, m_dlssnrFeatherConstants.GetAddressOf()));
            D3D11_BLEND_DESC blend{};
            auto& target = blend.RenderTarget[0];
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            target.BlendOp = D3D11_BLEND_OP_ADD;
            target.SrcBlendAlpha = D3D11_BLEND_ZERO;
            target.DestBlendAlpha = D3D11_BLEND_ONE;
            target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                          D3D11_COLOR_WRITE_ENABLE_BLUE;
            CHECK_HRCMD(m_ovrSubmissionDevice->CreateBlendState(&blend, m_dlssnrFeatherBlend.GetAddressOf()));
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            raster.ScissorEnable = TRUE;
            CHECK_HRCMD(m_ovrSubmissionDevice->CreateRasterizerState(&raster, m_dlssnrFeatherRasterizer.GetAddressOf()));
        }
        // Follow the runtime's application-context preservation contract.
        if (m_d3d11Device == m_ovrSubmissionDevice && !m_d3d11ContextState) {
            m_ovrSubmissionContext->SwapDeviceContextState(m_ovrSubmissionContextState.Get(),
                                                           m_d3d11ContextState.ReleaseAndGetAddressOf());
        }
        auto* context = m_ovrSubmissionContext.Get();
        ID3D11UnorderedAccessView* noUav = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(m_fullQuadVS.Get(), nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(m_dlssnrFeatherPS.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, m_dlssnrFeatherConstants.GetAddressOf());
        context->RSSetState(m_dlssnrFeatherRasterizer.Get());
        context->OMSetDepthStencilState(nullptr, 0);
        context->OMSetBlendState(m_dlssnrFeatherBlend.Get(), nullptr, 0xffffffff);
        int imageIndex = 0;
        CHECK_OVRCMD(ovr_GetTextureSwapChainCurrentIndex(m_ovrSession, m_dlssnrOutputSwapchain, &imageIndex));
        context->OMSetRenderTargets(1, m_dlssnrOutputRtvs[imageIndex].GetAddressOf(), nullptr);
        const D3D11_VIEWPORT viewport{0.f, 0.f, (float)(resolution.w * 2), (float)resolution.h, 0.f, 1.f};
        context->RSSetViewports(1, &viewport);
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            auto& swapchain = *(Swapchain*)views[eye]->swapchain;
            auto& slice = swapchain.resolvedSlices[views[eye]->imageArrayIndex];
            const int index = slice.lastCommittedIndex;
            if ((int)slice.dlssnrFeatherSrvs.size() <= index) slice.dlssnrFeatherSrvs.resize(index + 1);
            if (!slice.dlssnrFeatherSrvs[index]) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = getShaderResourceViewFormat(swapchain.dxgiFormatForSubmission);
                srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D.MipLevels = 1;
                CHECK_HRCMD(m_ovrSubmissionDevice->CreateShaderResourceView(
                    slice.images[index].Get(), &srv, slice.dlssnrFeatherSrvs[index].GetAddressOf()));
            }
            Constants constants{};
            constants.inputBase[0] = views[eye]->imageRect.offset.x + offset.x;
            constants.inputBase[1] = views[eye]->imageRect.offset.y + offset.y;
            constants.outputBase[0] = eye * resolution.w + offset.x;
            constants.outputBase[1] = offset.y;
            constants.regionSize[0] = std::min(region.w, views[eye]->imageRect.extent.width - offset.x);
            constants.regionSize[1] = std::min(region.h, views[eye]->imageRect.extent.height - offset.y);
            if (constants.regionSize[0] <= 0 || constants.regionSize[1] <= 0) continue;
            constants.featherPixels = std::max(1.f, std::min(constants.regionSize[0], constants.regionSize[1]) * 0.10f);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CHECK_HRCMD(context->Map(m_dlssnrFeatherConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(m_dlssnrFeatherConstants.Get(), 0);
            context->PSSetShaderResources(0, 1, slice.dlssnrFeatherSrvs[index].GetAddressOf());
            const LONG l = constants.outputBase[0], t = constants.outputBase[1];
            const LONG r = l + constants.regionSize[0], b = t + constants.regionSize[1];
            const LONG border = std::min((LONG)std::ceil(constants.featherPixels) + 1,
                                        std::min(r - l, b - t) / 2);
            // Four disjoint bands: the center and outside pixels are never shaded.
            const D3D11_RECT bands[] = {{l, t, r, t + border}, {l, b - border, r, b},
                                       {l, t + border, l + border, b - border},
                                       {r - border, t + border, r, b - border}};
            for (const auto& band : bands) {
                if (band.right <= band.left || band.bottom <= band.top) continue;
                context->RSSetScissorRects(1, &band);
                context->Draw(3, 0);
            }
        }
        ID3D11ShaderResourceView* noSrv = nullptr;
        context->PSSetShaderResources(0, 1, &noSrv);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->RSSetState(nullptr);
    }

    // For output swapchain resources.
    void OpenXrRuntime::ensureDlssnrSwapchainResources(ovrTextureFormat format, const ovrSizei& resolution) {
        if (m_dlssnrOutputSwapchain) {
            m_dlssnrOutputSwapchainImages.clear();
            m_dlssnrOutputRtvs.clear();
            ovr_DestroyTextureSwapChain(m_ovrSession, m_dlssnrOutputSwapchain);
        }

        ovrTextureSwapChainDesc desc{};
        desc.Type = ovrTexture_2D;
        desc.ArraySize = 1;
        desc.Width = resolution.w * 2; // Double-wide.
        desc.Height = resolution.h;
        desc.MipLevels = 1;
        desc.SampleCount = 1;
        desc.Format = format;
        desc.BindFlags = ovrTextureBind_DX_RenderTarget | ovrTextureBind_DX_UnorderedAccess;
        desc.MiscFlags = ovrTextureMisc_DX_Typeless;
        CHECK_OVRCMD(
            ovr_CreateTextureSwapChainDX(m_ovrSession, m_ovrSubmissionDevice.Get(), &desc, &m_dlssnrOutputSwapchain));

        int count = -1;
        CHECK_OVRCMD(ovr_GetTextureSwapChainLength(m_ovrSession, m_dlssnrOutputSwapchain, &count));

        // Query the textures for the swapchain.
        for (int i = 0; i < count; i++) {
            ComPtr<ID3D11Texture2D> texture;
            CHECK_OVRCMD(ovr_GetTextureSwapChainBufferDX(
                m_ovrSession, m_dlssnrOutputSwapchain, i, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())));

            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = ovrToDxgiTextureFormat(format);
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ComPtr<ID3D11RenderTargetView> rtv;
            CHECK_HRCMD(m_ovrSubmissionDevice->CreateRenderTargetView(texture.Get(), &rtvDesc, rtv.GetAddressOf()));
            m_dlssnrOutputRtvs.push_back(std::move(rtv));

            // Export to the DLSS context.
            ComPtr<IDXGIResource1> dxgiResource;
            CHECK_HRCMD(texture->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf())));

            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);

            wil::unique_handle ntHandle;
            HANDLE textureHandle;
            if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                CHECK_HRCMD(dxgiResource->GetSharedHandle(&textureHandle));
            } else {
                CHECK_HRCMD(dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ntHandle.put()));
                textureHandle = ntHandle.get();
            }
            ComPtr<ID3D12Resource> d3d12Resource;
            CHECK_HRCMD(
                m_dlssnrDevice->OpenSharedHandle(textureHandle, IID_PPV_ARGS(d3d12Resource.ReleaseAndGetAddressOf())));

            m_dlssnrOutputSwapchainImages.push_back(std::move(d3d12Resource));
        }
    }

    // For input swapchain(s) resources.
    void OpenXrRuntime::ensureSwapchainDlssnrResources(Swapchain& xrSwapchain, uint32_t slice) {
        // SimXR: initializeDlssnrResources() is skipped, so there is no D3D12 device to share into.
        if (!m_dlssnrDevice) {
            return;
        }
        if (xrSwapchain.resolvedSlices[slice].dlssnrImages.empty()) {
            // Query the textures for the swapchain.
            for (int i = 0; i < xrSwapchain.resolvedSlices[slice].images.size(); i++) {
                // Export to the DLSS context.
                wil::unique_handle handle;
                ComPtr<IDXGIResource1> dxgiResource;
                CHECK_HRCMD(xrSwapchain.resolvedSlices[slice].images[i]->QueryInterface(
                    IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf())));

                D3D11_TEXTURE2D_DESC desc{};
                xrSwapchain.resolvedSlices[slice].images[i]->GetDesc(&desc);

                wil::unique_handle ntHandle;
                HANDLE textureHandle;
                if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
                    CHECK_HRCMD(dxgiResource->GetSharedHandle(&textureHandle));
                } else {
                    CHECK_HRCMD(dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, ntHandle.put()));
                    textureHandle = ntHandle.get();
                }
                ComPtr<ID3D12Resource> d3d12Resource;
                CHECK_HRCMD(m_dlssnrDevice->OpenSharedHandle(textureHandle,
                                                             IID_PPV_ARGS(d3d12Resource.ReleaseAndGetAddressOf())));

                xrSwapchain.resolvedSlices[slice].dlssnrImages.push_back(std::move(d3d12Resource));
            }
        }
    }

    void OpenXrRuntime::initializeDlssnrResources() {
        // DLSS-NR requires a D3D12 context. Create one on the matching adapter.
        ComPtr<IDXGIDevice> dxgiDevice;
        CHECK_HRCMD(m_ovrSubmissionDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())));

        ComPtr<IDXGIAdapter> dxgiAdapter;
        CHECK_HRCMD(D3D12CreateDevice(
            dxgiAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_dlssnrDevice.ReleaseAndGetAddressOf())));

        m_dlssnrContext = std::make_unique<D3D12Utils::CommandContext>(m_dlssnrDevice.Get());

        // We'll use a fence to serialize pre-compositor output to DLSS-NR input.
        {
            wil::unique_handle fenceHandle;
            CHECK_HRCMD(m_ovrSubmissionFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, fenceHandle.put()));
            CHECK_HRCMD(m_dlssnrDevice->OpenSharedHandle(fenceHandle.get(),
                                                         IID_PPV_ARGS(m_dlssnrInFence.ReleaseAndGetAddressOf())));
        }

        // We'll use a fence to serialize DLSS-NR output to OVR input.
        {
            wil::unique_handle handle;
            CHECK_HRCMD(m_dlssnrDevice->CreateSharedHandle(
                m_dlssnrContext->GetCompletionFence(), nullptr, GENERIC_ALL, nullptr, handle.put()));
            CHECK_HRCMD(m_ovrSubmissionDevice->OpenSharedFence(
                handle.get(), IID_PPV_ARGS(m_dlssnrOutFence.ReleaseAndGetAddressOf())));
        }

        NVSDK_NGX_FeatureCommonInfo commonInfo{};
        const wchar_t* paths[] = {virtualdesktop_openxr::dllHome.c_str()};
#ifdef _DEBUG
        commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
#else
        commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
#endif
        commonInfo.PathListInfo.Path = paths;
        commonInfo.PathListInfo.Length = std::size(paths);
        CHECK_NGXCMD(NVSDK_NGX_D3D12_Init(
            123456, virtualdesktop_openxr::programData.c_str(), m_dlssnrDevice.Get(), &commonInfo));

        CHECK_NGXCMD(NVSDK_NGX_D3D12_GetCapabilityParameters(&m_ngxParameters));
    }

    void OpenXrRuntime::cleanupDlssnrResources() {
        if (m_ngxParameters) {
            NVSDK_NGX_D3D12_DestroyParameters(m_ngxParameters);
            m_ngxParameters = nullptr;
        }

        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            if (m_dlssnrFeature[eye]) {
                ngx::NVSDK_NGX_D3D12_ReleaseFeature(m_dlssnrFeature[eye]);
                m_dlssnrFeature[eye] = nullptr;
            }
        }
        // Don't shutdown NGX - the host app might be using DLSS!
    }

} // namespace virtualdesktop_openxr
