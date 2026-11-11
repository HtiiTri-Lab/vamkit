#pragma once
#include "dx11_nr_prepass.h"
#include "dx11_vam_nis.h"
#include "dx11_vam_dlss.h"
#include "dx11_vam_mip_bias.h"
#include "vam_performance_config.h"
#include "../../../../shading/rt-core/include/startup_display.h"

// Called on Unity's rendering thread, from the first XR Submit or desktop
// capture. NR retains the existing NGX initialization order.
inline void VamInitializeRenderHost(ID3D11Device* device, std::shared_ptr<VamNrPrepass>& nr,
                                    std::unique_ptr<VamNis>& nis, VamDlss*& dlss) {
    nr = VamNrPrepass::GetIfEnabled(device);
    if (VamPerformanceStartup().enabled) {
        nis = std::make_unique<VamNis>(device);
        dlss = VamDlss::GetIfEnabled(device);
        if (dlss) VamMipBias::Install(device);
    }
}
