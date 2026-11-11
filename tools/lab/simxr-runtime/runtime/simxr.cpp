#include "pch.h"

#include "simxr.h"

namespace virtualdesktop_openxr::simxr {

    std::optional<int> GetSetting(const std::string& name) {
        char value[64]{};
        const DWORD length = GetEnvironmentVariableA(("SIMXR_" + name).c_str(), value, (DWORD)sizeof(value));
        if (length == 0 || length >= sizeof(value)) {
            return {};
        }
        char* end = nullptr;
        const long number = strtol(value, &end, 10);
        if (end == value) {
            return {};
        }
        return (int)number;
    }

    std::filesystem::path LogFolder() {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"SIMXR_LOG_DIR", path, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            return std::filesystem::path(path);
        }
        WCHAR* localAppData = nullptr;
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
        std::filesystem::path folder = std::filesystem::path(localAppData ? localAppData : L"") / L"SimXR";
        CoTaskMemFree(localAppData);
        return folder;
    }

} // namespace virtualdesktop_openxr::simxr
