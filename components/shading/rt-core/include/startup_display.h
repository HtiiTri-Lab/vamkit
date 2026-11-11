#pragma once
#include <windows.h>

// Immutable for the run. A missing variable preserves stock VR use.
inline bool VamDesktopDisplay() {
    static const bool desktop = [] {
        wchar_t mode[16]{};
        return GetEnvironmentVariableW(L"VAM_DISPLAY_MODE", mode, 16) && _wcsicmp(mode, L"Desktop") == 0;
    }();
    return desktop;
}
inline unsigned VamViewCount() { return VamDesktopDisplay() ? 1u : 2u; }
