// SimXR: this VDXR build drives a simulated Quest 3 (LibOVRRT64_1.dll from
// ..\hmd, loaded from this DLL's folder) instead of Virtual Desktop. It reports
// itself to applications exactly as Virtual Desktop's bundled runtime does
// ("VirtualDesktopXR", floor-level tracking), so OpenComposite and the OFXR
// layer take the same paths as with the real headset. Only these differ:
//  - settings come from SIMXR_<name> environment variables, never from
//    HKLM\SOFTWARE\Virtual Desktop, Inc.\OpenXR (so the runtime NR of the
//    DLSS-NR VDXR build, the mirror window etc. are off unless asked for);
//  - the log goes to %SIMXR_LOG_DIR%\OpenXR.log (else %LOCALAPPDATA%\SimXR),
//    never over Virtual Desktop's own C:\ProgramData\Virtual Desktop\OpenXR.log.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace virtualdesktop_openxr::simxr {

    // SIMXR_<name> as an integer, if set and a number.
    std::optional<int> GetSetting(const std::string& name);

    // Where OpenXR.log goes (created by the caller).
    std::filesystem::path LogFolder();

} // namespace virtualdesktop_openxr::simxr
