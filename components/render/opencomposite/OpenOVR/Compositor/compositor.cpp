#include "stdafx.h"

#include "../Misc/Config.h"
#include "compositor.h"

Compositor::~Compositor()
{
	if (chain) {
		OOVR_FAILED_XR_SOFT_ABORT(xrDestroySwapchain(chain));
		chain = XR_NULL_HANDLE;
	}
}

void Compositor::Invoke(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, XrSwapchainSubImage& subImage, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	bool invertInCompositor = oovr_global_configuration.InvertUsingShaders();
	if (bounds && bounds->vMin == 0.0f && bounds->vMax == 1.0f && bounds->uMin == 0.0f && bounds->uMax == 1.0f)
		bounds = nullptr;
	CopyToSwapchain(texture, invertInCompositor ? bounds : nullptr, eye, submitFlags);
	subImage.swapchain = GetSwapChain();
	subImage.imageArrayIndex = 0; // This is *not* the swapchain index
	XrExtent2Di src = GetSrcSize();
	CalculateViewport(invertInCompositor ? nullptr : bounds, src.width, src.height, true, subImage.imageRect);
	static std::atomic<unsigned> diagnosticCount{0};
	unsigned n = ++diagnosticCount;
	if (n <= 40 || n % 600 == 0) OOVR_LOGF("VAMDIAG Viewport n=%u source=%dx%d rect=%d,%d,%d,%d", n, src.width, src.height, subImage.imageRect.offset.x, subImage.imageRect.offset.y, subImage.imageRect.extent.width, subImage.imageRect.extent.height);
}

bool Compositor::CalculateViewport(const vr::VRTextureBounds_t* ptrBounds, int32_t width, int32_t height, bool supportsInvert, XrRect2Di& viewport)
{
	bool submitVerticallyFlipped = false;

	if (ptrBounds) {
		vr::VRTextureBounds_t newBounds = *ptrBounds;
		if (!supportsInvert && newBounds.vMin > newBounds.vMax) {
			float newMax = newBounds.vMin;
			newBounds.vMin = newBounds.vMax;
			newBounds.vMax = newMax;
			submitVerticallyFlipped = true;
		} else {
			submitVerticallyFlipped = false;
		}

		viewport.offset.x = (int)(newBounds.uMin * (float)width);
		viewport.offset.y = (int)(newBounds.vMin * (float)height);
		viewport.extent.width = (int)((newBounds.uMax - newBounds.uMin) * (float)width);
		viewport.extent.height = (int)((newBounds.vMax - newBounds.vMin) * (float)height);
	} else {
		viewport.offset.x = viewport.offset.y = 0;
		viewport.extent.width = width;
		viewport.extent.height = height;
		submitVerticallyFlipped = false;
	}
	return submitVerticallyFlipped;
}
