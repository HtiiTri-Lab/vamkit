#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#include "dx11compositor.h"
#include "vam_render_host.h"

#include "../Misc/Config.h"
#include "../Misc/xr_ext.h"
#include "../Misc/lodepng.h"
#include "vam_performance_config.h"
#include "dx11_vam_mip_bias.h"

#include <d3dcompiler.h> // For compiling shaders! D3DCompile

#pragma comment(lib, "d3dcompiler.lib")

constexpr char fs_shader_code[] = R"_(
Texture2D shaderTexture : register(t0);

SamplerState SampleType : register(s0);

struct psIn {
	float4 pos : SV_POSITION;
	float2 tex : TEXCOORD0;
};

psIn vs_fs(uint vI : SV_VERTEXID)
{
	psIn output;
    output.tex = float2(vI&1,vI>>1);
    output.pos = float4((output.tex.x-0.5f)*2,-(output.tex.y-0.5f)*2,0,1);
	output.tex.y = 1.0f - output.tex.y;
	return output;
}

float4 ps_fs(psIn inputPS) : SV_TARGET
{
	float4 textureColor = shaderTexture.Sample(SampleType, inputPS.tex);
	return textureColor;
})_";

static void XTrace(LPCSTR lpszFormat, ...)
{
	va_list args;
	va_start(args, lpszFormat);
	int nBuf;
	char szBuffer[512]; // get rid of this hard-coded buffer
	nBuf = _vsnprintf_s(szBuffer, 511, lpszFormat, args);
	OutputDebugStringA(szBuffer);
	OOVR_LOG(szBuffer);
	va_end(args);
}

#define ERR(msg)                                                                                                                                       \
	{                                                                                                                                                  \
		std::string str = "Hit DX11-related error " + string(msg) + " at " __FILE__ ":" + std::to_string(__LINE__) + " func " + std::string(__func__); \
		OOVR_LOG(str.c_str());                                                                                                                         \
		OOVR_MESSAGE(str.c_str(), "Errored func!");                                                                                                    \
		/**((int*)NULL) = 0;*/                                                                                                                         \
		throw str;                                                                                                                                     \
	}

void DX11Compositor::ThrowIfFailed(HRESULT test)
{
	if ((test) != S_OK) {
		OOVR_FAILED_DX_ABORT(device->GetDeviceRemovedReason());
		throw "ThrowIfFailed err";
	}
}

ID3DBlob* d3d_compile_shader(const char* hlsl, const char* entrypoint, const char* target)
{
	DWORD flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob *compiled, *errors;
	if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entrypoint, target, flags, 0, &compiled, &errors)))
		OOVR_ABORTF("Error: D3DCompile failed %s", (char*)errors->GetBufferPointer());
	if (errors)
		errors->Release();

	return compiled;
}

ID3D11RenderTargetView* d3d_make_rtv(ID3D11Device* d3d_device, XrBaseInStructure& swapchain_img, const DXGI_FORMAT& format)
{
	ID3D11RenderTargetView* result = nullptr;

	// Get information about the swapchain image that OpenXR made for us
	XrSwapchainImageD3D11KHR& d3d_swapchain_img = (XrSwapchainImageD3D11KHR&)swapchain_img;

	// Create a render target view resource for the swapchain image
	D3D11_RENDER_TARGET_VIEW_DESC target_desc = {};
	target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	target_desc.Format = format;
	target_desc.Texture2D.MipSlice = 0;
	OOVR_FAILED_DX_ABORT(d3d_device->CreateRenderTargetView(d3d_swapchain_img.texture, &target_desc, &result));

	return result;
}

DX11Compositor::DX11Compositor(ID3D11Texture2D* initial)
{
	initial->GetDevice(&device);
	device->GetImmediateContext(&context);
	OOVR_FAILED_DX_ABORT(eyeBlitter.Initialize(device, context));
	VamInitializeRenderHost(device, nrPrepass, nis, dlss);
	if (VamPerformanceStartup().enabled) {
		ffr = VamFfr::GetIfEnabled(device);
		OOVR_LOGF("VAM performance pipeline requested: renderScale=%.2f FFR -> eye crop -> optional NR -> %s -> XR submission", VamPerformanceStartup().scale, !VamPerformanceStartup().upscale ? "Native" : (VamPerformanceStartup().dlss ? (dlss ? "DLSS SR (linear blit fallback)" : "Linear blit (DLSS bridge unavailable)") : (VamPerformanceStartup().fsr ? "Linear blit (FSR disabled)" : "NIS")));
	}

	// Shaders for inverting copy
	ID3DBlob* fs_vert_shader_blob = d3d_compile_shader(fs_shader_code, "vs_fs", "vs_5_0");
	ID3DBlob* fs_pixel_shader_blob = d3d_compile_shader(fs_shader_code, "ps_fs", "ps_5_0");
	OOVR_FAILED_DX_ABORT(device->CreateVertexShader(fs_vert_shader_blob->GetBufferPointer(), fs_vert_shader_blob->GetBufferSize(), nullptr, &fs_vshader));
	OOVR_FAILED_DX_ABORT(device->CreatePixelShader(fs_pixel_shader_blob->GetBufferPointer(), fs_pixel_shader_blob->GetBufferSize(), nullptr, &fs_pshader));

	// Create a texture sampler state description.
	D3D11_SAMPLER_DESC samplerDesc;
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 4;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	samplerDesc.BorderColor[0] = 0;
	samplerDesc.BorderColor[1] = 0;
	samplerDesc.BorderColor[2] = 0;
	samplerDesc.BorderColor[3] = 0;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = 0;

	// Create the texture sampler state.
	OOVR_FAILED_DX_ABORT(device->CreateSamplerState(&samplerDesc, &quad_sampleState));
}

DX11Compositor::~DX11Compositor()
{
	nrPrepass.reset();
	for (auto&& rtv : swapchain_rtvs)
		rtv->Release();

	swapchain_rtvs.clear();

	for (auto&& tex : resolvedMSAATextures)
		tex->Release();

	resolvedMSAATextures.clear();

	context->Release();
	device->Release();
}

void DX11Compositor::CheckCreateSwapChain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, bool cube, std::optional<XruEye> eye)
{
	XrSwapchainCreateInfo& desc = createInfo;

	auto* src = (ID3D11Texture2D*)texture->handle;

	D3D11_TEXTURE2D_DESC srcDesc;
	src->GetDesc(&srcDesc);

	if (bounds) {
		if (std::fabs(bounds->uMax - bounds->uMin) > 0.1)
			srcDesc.Width = uint32_t(float(srcDesc.Width) * std::fabs(bounds->uMax - bounds->uMin));
		if (std::fabs(bounds->vMax - bounds->vMin) > 0.1)
			srcDesc.Height = uint32_t(float(srcDesc.Height) * std::fabs(bounds->vMax - bounds->vMin));
	}

	if (cube) {
		// LibOVR can only use square cubemaps, while SteamVR can use any shape
		// Note we use CopySubresourceRegion later on, so this won't cause problems with that
		srcDesc.Height = srcDesc.Width = std::min(srcDesc.Height, srcDesc.Width);
	}
	if (performanceEye && !cube) {
		performanceInputWidth = srcDesc.Width;
		performanceInputHeight = srcDesc.Height;
		if (VamPerformanceStartup().upscale && eye.has_value()) {
			// The output is the current runtime's target for THIS eye. Never
			// reconstruct it from the reduced/rounded game texture: Unity's
			// independent render multiplier must not shrink the output too.
			// DLSS launcher (v18): times the live [Performance] OutputScale,
			// within the runtime's largest image; a change makes the chain
			// below incompatible, so it is rebuilt at the new size.
			const auto& target = xr_main_view(*eye);
			const int output = VamPerformanceStartup().dlss ? VamLiveRenderSettings().outputPercent : 100;
			srcDesc.Width = VamOutputDimension(target.recommendedImageRectWidth, output);
			srcDesc.Height = VamOutputDimension(target.recommendedImageRectHeight, output);
			if (target.maxImageRectWidth) srcDesc.Width = std::min(srcDesc.Width, target.maxImageRectWidth);
			if (target.maxImageRectHeight) srcDesc.Height = std::min(srcDesc.Height, target.maxImageRectHeight);
		}
	}

	bool usable = chain == NULL ? false : CheckChainCompatible(srcDesc, texture->eColorSpace);

	if (!usable) {
		OOVR_LOG("Generating new swap chain");
		if (performanceEye && eye.has_value())
			OOVR_LOGF("VAM eye=%u actual cropped input=%ux%u output=%ux%u target=%s", static_cast<unsigned>(*eye),
				performanceInputWidth, performanceInputHeight, srcDesc.Width, srcDesc.Height,
				VamPerformanceStartup().upscale ? "current OpenXR view recommendation" : "native submitted texture");

		if (bounds)
			OOVR_LOGF("Bounds: uMin %f uMax %f vMin %f vMax %f", bounds->uMin, bounds->uMax, bounds->vMin, bounds->vMax);
		OOVR_LOGF("Texture desc format: %d", srcDesc.Format);
		OOVR_LOGF("Texture desc bind flags: %d", srcDesc.BindFlags);
		OOVR_LOGF("Texture desc MiscFlags: %d", srcDesc.MiscFlags);
		OOVR_LOGF("Texture desc Usage: %d", srcDesc.Usage);
		OOVR_LOGF("Texture desc width: %d", srcDesc.Width);
		OOVR_LOGF("Texture desc height: %d", srcDesc.Height);

		// First, delete the old chain if necessary
		if (chain) {
			OOVR_FAILED_XR_ABORT(xrDestroySwapchain(chain));
			chain = XR_NULL_HANDLE;
		}

		for (auto&& rtv : swapchain_rtvs)
			rtv->Release();

		swapchain_rtvs.clear();

		for (auto&& tex : resolvedMSAATextures)
			tex->Release();

		resolvedMSAATextures.clear();

		// Figure out what format we need to use
		DxgiFormatInfo info = {};
		if (!GetFormatInfo(srcDesc.Format, info)) {
			OOVR_ABORTF("Unknown (by OC) DXGI texture format %d", srcDesc.Format);
		}
		bool useLinearFormat;
		switch (texture->eColorSpace) {
		case vr::ColorSpace_Gamma:
			useLinearFormat = false;
			break;
		case vr::ColorSpace_Linear:
			useLinearFormat = true;
			break;
		default:
			// As per the docs for the auto mode, at eight bits per channel or less it assumes gamma
			// (using such small channels for linear colour would result in significant banding)
			useLinearFormat = info.bpc > 8;
			break;
		}

		DXGI_FORMAT type = useLinearFormat ? info.linear : info.srgb;

		if (type == DXGI_FORMAT_UNKNOWN) {
			OOVR_ABORTF("Invalid DXGI target format found: useLinear=%d type=DXGI_FORMAT_UNKNOWN fmt=%d", useLinearFormat, srcDesc.Format);
		}

		// Set aside the old format for checking later
		createInfoFormat = srcDesc.Format;

		// Make eye render buffer
		desc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		// TODO desc.Type = cube ? ovrTexture_Cube : ovrTexture_2D;
		desc.faceCount = cube ? 6 : 1;
		desc.width = srcDesc.Width;
		desc.height = srcDesc.Height;
		desc.format = type;
		desc.mipCount = srcDesc.MipLevels;
		desc.sampleCount = 1;
		desc.arraySize = 1;
		desc.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

		XrResult result = xrCreateSwapchain(xr_session.get(), &desc, &chain);
		if (!XR_SUCCEEDED(result))
			OOVR_ABORTF("Cannot create DX texture swap chain: err %d", result);

		// Go through the images and retrieve them - this will be used later in Invoke, since OpenXR doesn't
		// have a convenient way to request one specific image.
		uint32_t imageCount;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr));

		imagesHandles = std::vector<XrSwapchainImageD3D11KHR>(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain,
		    imagesHandles.size(), &imageCount, (XrSwapchainImageBaseHeader*)imagesHandles.data()));

		OOVR_FALSE_ABORT(imageCount == imagesHandles.size());

		swapchain_rtvs.resize(imageCount, nullptr);

		for (uint32_t i = 0; i < imageCount; i++) {
			swapchain_rtvs[i] = d3d_make_rtv(device, (XrBaseInStructure&)imagesHandles[i], type);
		}

		if (srcDesc.SampleDesc.Count > 1 && !performanceEye) {
			OOVR_LOGF("Creating resolver textures for MSAA source with sample count x%d", srcDesc.SampleDesc.Count);
			D3D11_TEXTURE2D_DESC resDesc = srcDesc;
			resDesc.SampleDesc.Count = 1;

			resolvedMSAATextures.resize(imageCount, nullptr);

			for (uint32_t i = 0; i < imageCount; i++) {
				device->CreateTexture2D(&resDesc, nullptr, &resolvedMSAATextures[i]);
			}
		}

		// TODO do we need to release the images at some point, or does the swapchain do that for us?
	}
}

static void CaptureEyeTexture(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* source, UINT slice, const std::string& path)
{
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    OOVR_LOGF("VAMCAP texture %s width=%u height=%u array=%u mips=%u samples=%u format=%u slice=%u",
        path.c_str(), desc.Width, desc.Height, desc.ArraySize, desc.MipLevels, desc.SampleDesc.Count, desc.Format, slice);
    bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if ((!rgba && !bgra) || desc.SampleDesc.Count != 1 || slice >= desc.ArraySize) return;
    UINT sub = D3D11CalcSubresource(0, slice, desc.MipLevels);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return;
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, sub, nullptr);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
    std::vector<unsigned char> pixels(size_t(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        auto* row = pixels.data() + size_t(y) * desc.Width * 4;
        memcpy(row, static_cast<unsigned char*>(map.pData) + size_t(y) * map.RowPitch, size_t(desc.Width) * 4);
        for (UINT x = 0; x < desc.Width; ++x) {
            if (bgra) std::swap(row[x * 4], row[x * 4 + 2]);
            row[x * 4 + 3] = 255;
        }
    }
    context->Unmap(staging.Get(), 0);
    unsigned error = lodepng::encode(path, pixels, desc.Width, desc.Height);
    OOVR_LOGF("VAMCAP saved %s result=%u", path.c_str(), error);
}

void DX11Compositor::CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	auto* src = (ID3D11Texture2D*)texture->handle;
	VamFfrScope fullRateComposition(ffr);
	performanceEye = nis && eye.has_value();

	// OpenXR swap chain doesn't support weird formats like DXGI_FORMAT_BC1_TYPELESS
	D3D11_TEXTURE2D_DESC srcDesc;
	src->GetDesc(&srcDesc);
	if (srcDesc.Format == DXGI_FORMAT_BC1_TYPELESS) {
		if (chain) {
			OOVR_FAILED_XR_ABORT(xrDestroySwapchain(chain));
			chain = XR_NULL_HANDLE;
		}
		return;
	}

	CheckCreateSwapChain(texture, bounds, false, eye);
	if ((bounds && oovr_global_configuration.InvertUsingShaders()) || performanceEye) {
		XrSwapchainImageAcquireInfo acquire{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t imageIndex = 0;
		OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(chain, &acquire, &imageIndex));
		XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = 500000000;
		XrResult waited = xrWaitSwapchainImage(chain, &wait);
		OOVR_FAILED_XR_ABORT(waited);
		if (waited == XR_TIMEOUT_EXPIRED) OOVR_ABORT("xrWaitSwapchainImage timeout");
		const float crop[4] = { bounds ? bounds->uMin : 0, bounds ? bounds->vMin : 0, bounds ? bounds->uMax : 1, bounds ? bounds->vMax : 1 };
		UINT slice = (srcDesc.ArraySize > 1 && eye.has_value()) ? static_cast<UINT>(*eye) : 0;
		if (ffr && eye.has_value()) {
			const auto& views = xr_gbl->GetCachedViews(xr_gbl->seatedSpace);
			const auto& fov = views.views[static_cast<unsigned>(*eye)].fov;
			float left = tanf(fov.angleLeft), right = tanf(fov.angleRight);
			float up = tanf(fov.angleUp), down = tanf(fov.angleDown);
			float cx = right > left ? -left / (right - left) : .5f;
			float cy = up > down ? up / (up - down) : .5f;
			ffr->Observe(src, crop, static_cast<unsigned>(*eye), cx, cy);
		}
		bool processed = performanceEye && nis->Process(src, slice, crop, static_cast<DXGI_FORMAT>(createInfo.format),
			performanceInputWidth, performanceInputHeight, swapchain_rtvs[imageIndex], createInfo.width, createInfo.height,
			nrPrepass.get(), static_cast<unsigned>(*eye), dlss, reinterpret_cast<uint64_t>(chain));
		if (!processed) {
			OOVR_FAILED_DX_ABORT(eyeBlitter.Copy(src, swapchain_rtvs[imageIndex], static_cast<DXGI_FORMAT>(createInfo.format), createInfo.width, createInfo.height, slice, crop));
			if (nrPrepass && eye.has_value())
				nrPrepass->Process(imagesHandles[imageIndex].texture, swapchain_rtvs[imageIndex], static_cast<DXGI_FORMAT>(createInfo.format), static_cast<unsigned>(*eye));
		}
		if (++diagnosticCopyCount == 600) {
			char captureDirectory[MAX_PATH]{};
			if (GetEnvironmentVariableA("VAM_CAPTURE_DIRECTORY", captureDirectory, MAX_PATH)) {
				std::string prefix = std::string(captureDirectory) + "\\eye-" + std::to_string(eye.has_value() ? static_cast<int>(*eye) : -1);
				CaptureEyeTexture(device, context, src, slice, prefix + "-source.png");
				CaptureEyeTexture(device, context, imagesHandles[imageIndex].texture, 0, prefix + "-output.png");
			}
		}
		XrSwapchainImageReleaseInfo release{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(chain, &release));
		return;
	}

	// First reserve an image from the swapchain
	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t currentIndex = 0;
	OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(chain, &acquireInfo, &currentIndex));

	// Wait until the swapchain is ready - this makes sure the compositor isn't writing to it
	// We don't have to pass in currentIndex since it uses the oldest acquired-but-not-waited-on
	// image, so we should be careful with concurrency here.
	// XR_TIMEOUT_EXPIRED is considered successful but swapchain still can't be used so need to handle that
	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 500000000; // time out in nano seconds - 500ms
	XrResult res;
	OOVR_FAILED_XR_ABORT(res = xrWaitSwapchainImage(chain, &waitInfo));

	if (res == XR_TIMEOUT_EXPIRED)
		OOVR_ABORTF("xrWaitSwapchainImage timeout");

	// Copy the source to the destination image
	D3D11_BOX sourceRegion;
	if (bounds) {
		sourceRegion.left = bounds->uMin == 0.0f ? 0 : createInfo.width;
		sourceRegion.right = bounds->uMin == 0.0f ? createInfo.width : createInfo.width * 2;
	} else {
		sourceRegion.left = 0;
		sourceRegion.right = createInfo.width;
	}
	sourceRegion.top = 0;
	sourceRegion.bottom = createInfo.height;
	sourceRegion.front = 0;
	sourceRegion.back = 1;

	// Bounds describe an inverted image so copy texture using pixel shader inverting on copy
	if (bounds && bounds->vMin > bounds->vMax && oovr_global_configuration.InvertUsingShaders() && !swapchain_rtvs.empty()) {
		auto* src = (ID3D11Texture2D*)texture->handle;

		OOVR_FAILED_DX_ABORT(device->CreateShaderResourceView(src, nullptr, &quad_texture_view));

		float blend_factor[4] = { 1.f, 1.f, 1.f, 1.f };
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

		UINT numViewPorts;
		context->RSGetViewports(&numViewPorts, nullptr);
		std::vector<D3D11_VIEWPORT> viewports;
		viewports.resize(numViewPorts);
		context->RSGetViewports(&numViewPorts, viewports.data());

		UINT numScissors;
		context->RSGetScissorRects(&numScissors, nullptr);
		std::vector<D3D11_RECT> scissors;
		scissors.resize(numScissors);
		context->RSGetScissorRects(&numScissors, scissors.data());

		ID3D11RasterizerState* pRSState;
		context->RSGetState(&pRSState);
		context->RSSetState(nullptr);

		D3D11_VIEWPORT viewport = CD3D11_VIEWPORT(src, swapchain_rtvs[currentIndex]);
		context->RSSetViewports(1, &viewport);
		D3D11_RECT rects[1];
		rects[0].top = 0;
		rects[0].left = 0;
		rects[0].bottom = createInfo.height;
		rects[0].right = createInfo.width;
		context->RSSetScissorRects(1, rects);

		// Set up for rendering
		context->OMSetRenderTargets(1, &swapchain_rtvs[currentIndex], nullptr);
		float clear_colour[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearRenderTargetView(swapchain_rtvs[currentIndex], clear_colour);

		// Set the active shaders and constant buffers.
		context->PSSetShaderResources(0, 1, &quad_texture_view);
		context->VSSetShader(fs_vshader, nullptr, 0);
		context->PSSetShader(fs_pshader, nullptr, 0);
		context->PSSetSamplers(0, 1, &quad_sampleState);

		// Set up the mesh's information
		D3D11_PRIMITIVE_TOPOLOGY currTopology;
		context->IAGetPrimitiveTopology(&currTopology);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->Draw(4, 0);
		context->IASetPrimitiveTopology(currTopology);
		quad_texture_view->Release();

		if (numViewPorts)
			context->RSSetViewports(numViewPorts, viewports.data());
		if (numScissors)
			context->RSSetScissorRects(numScissors, scissors.data());

		context->RSSetState(pRSState);
	} else {
		// Apparently SteamVR supports apps just sending array textures without specifying what's what.
		const int arrayIndex = (srcDesc.ArraySize > 1 && eye.has_value()) ? static_cast<int>(*eye) : 0;
		if (srcDesc.SampleDesc.Count > 1) {
			D3D11_TEXTURE2D_DESC resDesc = srcDesc;
			resDesc.SampleDesc.Count = 1;
			context->ResolveSubresource(resolvedMSAATextures[currentIndex], 0, src, arrayIndex, resDesc.Format);
			context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0, 0, 0, 0, resolvedMSAATextures[currentIndex], arrayIndex, &sourceRegion);
		} else {
			context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0, 0, 0, 0, src, arrayIndex, &sourceRegion);
		}
	}

	// Release the swapchain - OpenXR will use the last-released image in a swapchain
	if (nrPrepass && eye.has_value())
		nrPrepass->Process(imagesHandles[currentIndex].texture, swapchain_rtvs[currentIndex], static_cast<DXGI_FORMAT>(createInfo.format), static_cast<unsigned>(*eye));
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(chain, &releaseInfo));
}

void DX11Compositor::InvokeCubemap(const vr::Texture_t* textures)
{
	VamFfrScope fullRateComposition(ffr);
	performanceEye = false;
	CheckCreateSwapChain(&textures[0], nullptr, true);

#ifdef OC_XR_PORT
	ID3D11Texture2D* tex = nullptr;
	ERR("TODO cubemap");
#else
	int currentIndex = 0;
	OOVR_FAILED_OVR_ABORT(ovr_GetTextureSwapChainCurrentIndex(OVSS, chain, &currentIndex));

	OOVR_FAILED_OVR_ABORT(ovr_GetTextureSwapChainBufferDX(OVSS, chain, currentIndex, IID_PPV_ARGS(&tex)));
#endif

	ID3D11Texture2D* faceSrc;

	// Front
	faceSrc = (ID3D11Texture2D*)textures[0].handle;
	context->CopySubresourceRegion(tex, 5, 0, 0, 0, faceSrc, 0, nullptr);

	// Back
	faceSrc = (ID3D11Texture2D*)textures[1].handle;
	context->CopySubresourceRegion(tex, 4, 0, 0, 0, faceSrc, 0, nullptr);

	// Left
	faceSrc = (ID3D11Texture2D*)textures[2].handle;
	context->CopySubresourceRegion(tex, 0, 0, 0, 0, faceSrc, 0, nullptr);

	// Right
	faceSrc = (ID3D11Texture2D*)textures[3].handle;
	context->CopySubresourceRegion(tex, 1, 0, 0, 0, faceSrc, 0, nullptr);

	// Top
	faceSrc = (ID3D11Texture2D*)textures[4].handle;
	context->CopySubresourceRegion(tex, 2, 0, 0, 0, faceSrc, 0, nullptr);

	// Bottom
	faceSrc = (ID3D11Texture2D*)textures[5].handle;
	context->CopySubresourceRegion(tex, 3, 0, 0, 0, faceSrc, 0, nullptr);

	tex->Release();
}

bool DX11Compositor::CheckChainCompatible(D3D11_TEXTURE2D_DESC& inputDesc, vr::EColorSpace colourSpace)
{
	bool usable = true;
#define FAIL(name)                             \
	do {                                       \
		usable = false;                        \
		OOVR_LOG("Resource mismatch: " #name); \
	} while (0);
#define CHECK(name, chainName)                  \
	if (inputDesc.name != createInfo.chainName) \
		FAIL(name);

	CHECK(Width, width)
	CHECK(Height, height)
	CHECK(MipLevels, mipCount)

	if (inputDesc.Format != createInfoFormat) {
		FAIL("Format");
	}

	// CHECK_ADV(SampleDesc.Count, SampleCount);
	// CHECK_ADV(SampleDesc.Quality);
#undef CHECK
#undef FAIL

	return usable;
}

bool DX11Compositor::GetFormatInfo(DXGI_FORMAT format, DX11Compositor::DxgiFormatInfo& out)
{
#define DEF_FMT_BASE(typeless, linear, srgb, bpp, bpc, channels)            \
	{                                                                       \
		out = DxgiFormatInfo{ srgb, linear, typeless, bpp, bpc, channels }; \
		return true;                                                        \
	}

#define DEF_FMT_NOSRGB(name, bpp, bpc, channels) \
	case name##_TYPELESS:                        \
	case name##_UNORM:                           \
		DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

#define DEF_FMT(name, bpp, bpc, channels) \
	case name##_TYPELESS:                 \
	case name##_UNORM:                    \
	case name##_UNORM_SRGB:               \
		DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, name##_UNORM_SRGB, bpp, bpc, channels)

#define DEF_FMT_UNORM(linear, bpp, bpc, channels) \
	case linear:                                  \
		DEF_FMT_BASE(DXGI_FORMAT_UNKNOWN, linear, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

	// Note that this *should* have pretty much all the types we'll ever see in games
	// Filtering out the non-typeless and non-unorm/srgb types, this is all we're left with
	// (note that types that are only typeless and don't have unorm/srgb variants are dropped too)
	switch (format) {
		// The relatively traditional 8bpp 32-bit types
		DEF_FMT(DXGI_FORMAT_R8G8B8A8, 32, 8, 4)
		DEF_FMT(DXGI_FORMAT_B8G8R8A8, 32, 8, 4)
		DEF_FMT(DXGI_FORMAT_B8G8R8X8, 32, 8, 3)

		// Some larger linear-only types
		DEF_FMT_NOSRGB(DXGI_FORMAT_R16G16B16A16, 64, 16, 4)
		DEF_FMT_NOSRGB(DXGI_FORMAT_R10G10B10A2, 32, 10, 4)

		// A jumble of other weird types
		DEF_FMT_UNORM(DXGI_FORMAT_B5G6R5_UNORM, 16, 5, 3)
		DEF_FMT_UNORM(DXGI_FORMAT_B5G5R5A1_UNORM, 16, 5, 4)
		DEF_FMT_UNORM(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, 32, 10, 4)
		DEF_FMT_UNORM(DXGI_FORMAT_B4G4R4A4_UNORM, 16, 4, 4)
		DEF_FMT(DXGI_FORMAT_BC1, 64, 16, 4)

	default:
		// Unknown type
		return false;
	}

#undef DEF_FMT
#undef DEF_FMT_NOSRGB
#undef DEF_FMT_BASE
#undef DEF_FMT_UNORM
}

#endif
