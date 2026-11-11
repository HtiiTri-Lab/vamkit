#include "xrfg/d3d12_dlssg.hpp"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

// Project id shared with the VaM OpenComposite DLSS SR build; NGX allows a
// custom-engine project to identify itself this way.
constexpr char kProjectId[] = "6d1c2f0a-4b8e-4f62-9d3a-7a5e8c1b2f40";
constexpr char kEngineVersion[] = "OFXR-0.7443-VaM";
// Every interpolation target is made with the generator, the third one too
// though only 4X uses it: it costs one image per eye (40 MB at 3072x3264),
// and made on demand it would be made in the middle of a frame.
constexpr std::uint32_t kMaxOutputs = kDlssgMaxOutputs;
// Consecutive evaluation failures before DLSS-G gives up for the session.
constexpr std::uint32_t kFailureLimit = 8;
// Create-time switch of NVIDIA's lighter Frame Generation mode (NVIDIA App
// "Preset C": a lighter candidate blend replaces the second network). Not in
// the 310.7 SDK headers; nvngx_dlssg 310.9.16 reads it beside the documented
// DLSSG.UserInterfaceRecompositionEnabled. Older runtimes ignore it.
constexpr char kLiteCandidateBlendingEnabled[] = "DLSSG.LiteCandidateBlendingEnabled";

std::wstring environment(const wchar_t* name) {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return length != 0 && length < std::size(buffer) ? std::wstring(buffer, length) : std::wstring{};
}

// v13: DLSS-G reads B where it was captured and writes its frames straight
// into the destinations when those allow it (see readable_directly /
// writable_directly), instead of a copy of B in and a copy per frame out:
// at 3072x3264 four of the eye's 40 MB copies per 4X pair. OFXR_DLSSG_COPIES=1
// keeps v12's copies, for comparison. Read once.
bool copies_forced() noexcept {
    static const bool forced = []() {
        try {
            return environment(L"OFXR_DLSSG_COPIES") == L"1";
        } catch (...) {
            return false;
        }
    }();
    return forced;
}

constexpr bool typeless_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

// ---- VaM's GPU scheduling priority (v13) ----------------------------------
// Off-line DLSS-G (quality) takes about 2.3 ms an eye at 3072x3264; on the
// headset 4.4 ms, and only while Virtual Desktop streams - whose streamer
// runs elevated and calls D3DKMTSetProcessSchedulingPriorityClass. Windows
// time-slices the GPU between processes by that class; VaM is NORMAL unless
// someone raised it. [Performance] GpuPriority in the VaM profile
// (VAM_PERFORMANCE_CONFIG), read when the file changes:
//   default  - leave the class as it is (and log what it is, and changes)
//   high     - HIGH (4) for the VaM process
//   realtime - REALTIME (5)
//   cycle    - 20 s each of: as found, HIGH, REALTIME, round and round; each
//              phase's pairs per second and DLSS-G GPU time per eye go to
//              dlssg.log, so one session compares them.
// An ordinary process may set its own class (checked on this machine,
// investigations\fg-cost-20261101\probe\selfprio.cpp).
enum class GpuPriorityMode { leave, high, realtime, cycle };

typedef LONG(APIENTRY* GetSchedulingClass)(HANDLE, INT*);
typedef LONG(APIENTRY* SetSchedulingClass)(HANDLE, INT);

struct GpuPriorityControl {
    std::mutex mutex;
    GetSchedulingClass get{};
    SetSchedulingClass set{};
    bool loaded{};
    bool started{};
    INT found{-1};       // the class at the first pair
    INT seen{-1};        // the class last read
    INT applied{-1};     // the class this layer last set, -1 none
    GpuPriorityMode mode{GpuPriorityMode::leave};
    ULONGLONG next_read{}, next_check{}, stamp_time{}, stamp_size{};
    bool profile_read{};
    // cycle
    std::uint32_t phase{};
    ULONGLONG phase_start{};
    std::uint64_t phase_pairs{};
    double phase_gpu_ms{};
    std::uint64_t phase_gpu_samples{};
    double phase_gpu_max{};
};

GpuPriorityControl& gpu_priority() {
    static GpuPriorityControl control;
    return control;
}

const char* class_name(INT value) noexcept {
    switch (value) {
    case 0: return "IDLE";
    case 1: return "BELOW_NORMAL";
    case 2: return "NORMAL";
    case 3: return "ABOVE_NORMAL";
    case 4: return "HIGH";
    case 5: return "REALTIME";
    default: return "unknown";
    }
}

INT read_class(GpuPriorityControl& c) noexcept {
    INT value = -1;
    if (c.get == nullptr || c.get(GetCurrentProcess(), &value) != 0) return -1;
    return value;
}

void apply_class(GpuPriorityControl& c, INT value, const char* why) noexcept {
    if (c.set == nullptr || value < 0) return;
    const INT before = read_class(c);
    const LONG status = c.set(GetCurrentProcess(), value);
    const INT after = read_class(c);
    c.applied = status == 0 ? value : -1;
    c.seen = after;
    dlssg_log("GPU priority: VaM's class %s -> %s (%s; status 0x%08lx)", class_name(before), class_name(after), why,
        static_cast<unsigned long>(status));
}

void read_priority_profile(GpuPriorityControl& c, ULONGLONG now) noexcept {
    if (c.profile_read && now < c.next_read) return;
    c.next_read = now + 250;
    static const std::wstring profile = environment(L"VAM_PERFORMANCE_CONFIG");
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const bool exists = !profile.empty() && GetFileAttributesExW(profile.c_str(), GetFileExInfoStandard, &data);
    const ULONGLONG time = exists
        ? (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime : 0;
    const ULONGLONG size = exists ? (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow : 0;
    if (c.profile_read && time == c.stamp_time && size == c.stamp_size) return;
    c.stamp_time = time;
    c.stamp_size = size;
    c.profile_read = true;
    wchar_t value[32]{};
    if (exists) GetPrivateProfileStringW(L"Performance", L"GpuPriority", L"default", value, 32, profile.c_str());
    const std::wstring text = exists ? std::wstring(value) : std::wstring(L"default");
    const GpuPriorityMode mode = text == L"high" ? GpuPriorityMode::high
        : text == L"realtime" ? GpuPriorityMode::realtime
        : text == L"cycle" ? GpuPriorityMode::cycle
        : GpuPriorityMode::leave;
    if (mode == c.mode && c.started) return;
    const bool first = !c.started;
    c.mode = mode;
    switch (mode) {
    case GpuPriorityMode::leave:
        if (!first && c.applied >= 0) apply_class(c, c.found, "[Performance] GpuPriority=default: back to the class found");
        c.applied = -1;
        break;
    case GpuPriorityMode::high:
        apply_class(c, 4, "[Performance] GpuPriority=high");
        break;
    case GpuPriorityMode::realtime:
        apply_class(c, 5, "[Performance] GpuPriority=realtime");
        break;
    case GpuPriorityMode::cycle:
        c.phase = 0;
        c.phase_start = now;
        c.phase_pairs = 0;
        c.phase_gpu_ms = 0.0;
        c.phase_gpu_samples = 0;
        c.phase_gpu_max = 0.0;
        apply_class(c, c.found, "[Performance] GpuPriority=cycle: phase 1 of 3, the class found");
        break;
    }
}

// Once per pair, under the NGX mutex (both eyes' generators share it).
// `first_eye` counts the pair for the cycle's rate.
void gpu_priority_tick(ULONGLONG now, bool first_eye) noexcept {
    try {
        GpuPriorityControl& c = gpu_priority();
        std::scoped_lock lock(c.mutex);
        if (!c.loaded) {
            c.loaded = true;
            if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll"); gdi != nullptr) {
                c.get = reinterpret_cast<GetSchedulingClass>(GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass"));
                c.set = reinterpret_cast<SetSchedulingClass>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"));
            }
        }
        if (!c.started) {
            c.found = read_class(c);
            c.seen = c.found;
            dlssg_log("GPU priority: VaM's GPU scheduling class at the first pair: %s (%d)", class_name(c.found), c.found);
        }
        read_priority_profile(c, now);
        c.started = true;
        if (now >= c.next_check) {
            // Someone else (the runtime, the streamer) may set it too.
            c.next_check = now + 5000;
            const INT value = read_class(c);
            if (value != c.seen) {
                dlssg_log("GPU priority: VaM's class changed to %s (%d) by someone else (was %s)", class_name(value), value,
                    class_name(c.seen));
                c.seen = value;
                if (c.mode == GpuPriorityMode::leave) c.found = value;
            }
        }
        if (c.mode != GpuPriorityMode::cycle) return;
        if (first_eye) ++c.phase_pairs;
        constexpr ULONGLONG kPhaseMs = 20000;
        if (now - c.phase_start < kPhaseMs) return;
        static constexpr INT kNames[3] = {-1, 4, 5};
        const INT current = c.phase == 0 ? c.found : kNames[c.phase];
        const double seconds = static_cast<double>(now - c.phase_start) / 1000.0;
        dlssg_log("GPU priority cycle: %s for %.1f s: %.1f pairs/s, DLSS-G GPU per eye mean %.2f ms, max %.2f (%llu)",
            class_name(current), seconds, static_cast<double>(c.phase_pairs) / seconds,
            c.phase_gpu_samples ? c.phase_gpu_ms / static_cast<double>(c.phase_gpu_samples) : 0.0, c.phase_gpu_max,
            static_cast<unsigned long long>(c.phase_gpu_samples));
        c.phase = (c.phase + 1) % 3;
        c.phase_start = now;
        c.phase_pairs = 0;
        c.phase_gpu_ms = 0.0;
        c.phase_gpu_samples = 0;
        c.phase_gpu_max = 0.0;
        const INT next = c.phase == 0 ? c.found : kNames[c.phase];
        apply_class(c, next, c.phase == 0 ? "cycle phase 1 of 3, the class found"
            : (c.phase == 1 ? "cycle phase 2 of 3" : "cycle phase 3 of 3"));
    } catch (...) {
    }
}

// A DLSS-G GPU time (one eye's list) for the cycle's averages.
void gpu_priority_sample(double ms) noexcept {
    GpuPriorityControl& c = gpu_priority();
    std::scoped_lock lock(c.mutex);
    if (c.mode != GpuPriorityMode::cycle) return;
    c.phase_gpu_ms += ms;
    ++c.phase_gpu_samples;
    c.phase_gpu_max = std::max(c.phase_gpu_max, ms);
}

// The lighter mode wanted now (v12, live): [Performance] DLSSGLite in the VaM
// launch panel's per-run profile (VAM_PERFORMANCE_CONFIG; VaM's in-game menu
// switches it), looked at every 250 ms and read again only after the file
// changed; without the profile or the key, OFXR_DLSSG_LITE=1 (set by the
// launcher, read once). A change recreates the features (ensure_feature).
bool lite_wanted() noexcept {
    static std::mutex mutex;
    static ULONGLONG next{}, seen_time{}, seen_size{};
    static bool read{}, wanted{};
    try {
        static const bool fallback = environment(L"OFXR_DLSSG_LITE") == L"1";
        static const std::wstring profile = environment(L"VAM_PERFORMANCE_CONFIG");
        std::scoped_lock lock(mutex);
        const ULONGLONG now = GetTickCount64();
        if (read && now < next) return wanted;
        next = now + 250;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const bool exists = !profile.empty() &&
            GetFileAttributesExW(profile.c_str(), GetFileExInfoStandard, &data);
        const ULONGLONG time = exists
            ? (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime : 0;
        const ULONGLONG size = exists ? (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow : 0;
        if (read && time == seen_time && size == seen_size) return wanted;
        seen_time = time;
        seen_size = size;
        read = true;
        wanted = exists
            ? GetPrivateProfileIntW(L"Performance", L"DLSSGLite", fallback ? 1 : 0, profile.c_str()) != 0
            : fallback;
        return wanted;
    } catch (...) {
        return wanted;
    }
}

// OFXR_DLSSG_CAPTURE=<directory> (the launcher's diagnostic switch): bursts
// of DLSS-G's inputs and outputs are written there. Read once.
const std::wstring& capture_root() {
    static const std::wstring root = []() {
        try {
            return environment(L"OFXR_DLSSG_CAPTURE");
        } catch (...) {
            return std::wstring{};
        }
    }();
    return root;
}

bool write_file(const std::wstring& path, const void* data, std::size_t size) noexcept {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    bool ok = true;
    while (size > 0 && ok) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, std::size_t{64} << 20));
        DWORD written = 0;
        ok = WriteFile(file, bytes, chunk, &written, nullptr) && written == chunk;
        bytes += chunk;
        size -= chunk;
    }
    CloseHandle(file);
    return ok;
}

std::mutex& log_mutex() {
    static std::mutex mutex;
    return mutex;
}

// NGX is not thread safe; two synthesizers (one per eye) share it.
std::mutex& ngx_mutex() {
    static std::mutex mutex;
    return mutex;
}

// Why DLSS Frame Generation is not available, in words, for running.json (d3d12_fsrfg.cpp).
std::mutex g_why_mutex;
std::string g_why_not;
void note_why_not(const std::string& why) noexcept {
    try {
        std::scoped_lock lock(g_why_mutex);
        g_why_not = why;
    } catch (...) {
    }
}

void write_status(const char* status, const std::string& detail) noexcept {
    if (std::strcmp(status, "failed") == 0) note_why_not(detail);
    try {
        const std::wstring directory = environment(L"OFXR_DLSSG_LOG");
        if (directory.empty()) return;
        std::string text = std::string("{\"status\":\"") + status + "\",\"detail\":\"";
        for (char c : detail) text += (c == '"' || c == '\\') ? '\'' : c;
        text += "\"}\n";
        const std::wstring path = directory + L"\\dlssg-status.json";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(file);
    } catch (...) {
    }
}

void NVSDK_CONV ngx_log_callback(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (message != nullptr) dlssg_log("NGX: %s", message);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

// The typed, non-sRGB member of the colour families the synthesizer accepts:
// DLSS-G reads the gamma-encoded bytes as an SDR backbuffer and writes
// interpolated frames through a typed UAV, which sRGB formats do not allow.
DXGI_FORMAT unorm_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

struct NgxDevice {
    ComPtr<ID3D12Device> device;
    bool ready{};
    bool failed{};
    unsigned multi_frame_count_max{1};
};

// dlssg_state(): the outcome of the last NGX bring-up for DLSS-G.
std::atomic<int> g_ngx_state{-1};

// One NGX initialization per D3D12 device for the life of the process: the
// device is kept referenced so its address is never reused by another.
NgxDevice* ngx_for(ID3D12Device* device) {
    static std::vector<std::unique_ptr<NgxDevice>> devices;
    for (auto& entry : devices) {
        if (entry->device.Get() == device) return entry.get();
    }
    auto entry = std::make_unique<NgxDevice>();
    entry->device = device;
    NgxDevice* result = entry.get();
    devices.push_back(std::move(entry));
    // Every early return below is a failure; the one success sets 1.
    g_ngx_state = 0;

    // A test of the path a card without DLSS Frame Generation takes, on a card that has it.
    if (environment(L"VAMKIT_NO_DLSS") == L"1") {
        dlssg_log("DLSS-G unavailable: switched off for this run (VAMKIT_NO_DLSS=1)");
        note_why_not("DLSS is switched off for this run (VAMKIT_NO_DLSS=1)");
        result->failed = true;
        return result;
    }
    const std::wstring runtime = environment(L"OFXR_DLSSG_RUNTIME_PATH");
    std::wstring data = environment(L"OFXR_DLSSG_LOG");
    if (data.empty()) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        data = temp;
    }
    CreateDirectoryW(data.c_str(), nullptr);
    if (runtime.empty()) {
        dlssg_log("DLSS-G unavailable: OFXR_DLSSG_RUNTIME_PATH is not set");
        write_status("failed", "OFXR_DLSSG_RUNTIME_PATH is not set");
        result->failed = true;
        return result;
    }
    // NGX keeps the first D3D12 setup's search path for the whole process,
    // and this one comes first.
    const wchar_t* paths[] = {runtime.c_str()};
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1U;
    common.LoggingInfo.LoggingCallback = &ngx_log_callback;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    common.LoggingInfo.DisableOtherLoggingSinks = false;
    NVSDK_NGX_Result status = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, data.c_str(), device, &common);
    if (NVSDK_NGX_FAILED(status)) {
        dlssg_log("DLSS-G unavailable: NVSDK_NGX_D3D12_Init_with_ProjectID failed 0x%08x", static_cast<unsigned>(status));
        write_status("failed", "NGX D3D12 initialization failed");
        result->failed = true;
        return result;
    }
    NVSDK_NGX_Parameter* capabilities = nullptr;
    status = NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities);
    if (NVSDK_NGX_FAILED(status) || capabilities == nullptr) {
        dlssg_log("DLSS-G unavailable: NVSDK_NGX_D3D12_GetCapabilityParameters failed 0x%08x", static_cast<unsigned>(status));
        write_status("failed", "NGX capability query failed");
        result->failed = true;
        return result;
    }
    int available = 0, needs_driver = 0, init_result = 0;
    capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs_driver);
    capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init_result);
    unsigned max_count = 0;
    if (NVSDK_NGX_FAILED(capabilities->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &max_count)) || max_count == 0) {
        max_count = 1;
    }
    NVSDK_NGX_D3D12_DestroyParameters(capabilities);
    if (!available) {
        dlssg_log("DLSS-G unavailable: FrameGeneration.Available=0 (needs driver update=%d, init result=0x%08x)",
            needs_driver, static_cast<unsigned>(init_result));
        write_status("failed", "DLSS Frame Generation is not available on this system");
        result->failed = true;
        return result;
    }
    result->multi_frame_count_max = max_count;
    result->ready = true;
    g_ngx_state = 1;
    dlssg_log("DLSS-G: NGX D3D12 initialized, frame generation available, MultiFrameCountMax=%u", max_count);
    return result;
}

}  // namespace

int dlssg_state() noexcept {
    return g_ngx_state.load();
}

std::string dlssg_why_not() noexcept {
    try {
        std::scoped_lock lock(g_why_mutex);
        return g_why_not;
    } catch (...) {
        return {};
    }
}

bool dlssg_available(ID3D12Device* device) noexcept {
    if (device == nullptr) return false;
    try {
        std::scoped_lock lock(ngx_mutex());
        const NgxDevice* ngx = ngx_for(device);
        return ngx != nullptr && ngx->ready;
    } catch (...) {
        return false;
    }
}

bool dlssg_requested() noexcept {
    // The launcher sets it before the game starts; read it once.
    static const bool requested = []() noexcept {
        try {
            return environment(L"OFXR_DLSSG") == L"1";
        } catch (...) {
            return false;
        }
    }();
    return requested;
}

namespace {

std::mutex& session_device_mutex() {
    static std::mutex mutex;
    return mutex;
}

// Never released: like the NGX devices above, it must outlive every user,
// and a release during process teardown could run after the driver left.
ComPtr<ID3D12Device>& session_device() {
    static auto* device = new ComPtr<ID3D12Device>();
    return *device;
}

}  // namespace

void dlssg_set_session_device(ID3D12Device* device) noexcept {
    try {
        std::scoped_lock lock(session_device_mutex());
        session_device() = device;
    } catch (...) {
    }
}

bool dlssg_prepare_session_device() noexcept {
    if (!dlssg_requested()) return false;
    try {
        ComPtr<ID3D12Device> device;
        {
            std::scoped_lock lock(session_device_mutex());
            device = session_device();
        }
        if (!device) return false;
        std::scoped_lock lock(ngx_mutex());
        const NgxDevice* ngx = ngx_for(device.Get());
        return ngx != nullptr && ngx->ready;
    } catch (...) {
        return false;
    }
}

void dlssg_log(const char* format, ...) noexcept {
    try {
        static const std::wstring directory = environment(L"OFXR_DLSSG_LOG");
        if (directory.empty() || format == nullptr) return;
        char line[2048];
        SYSTEMTIME time{};
        GetLocalTime(&time);
        int prefix = std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%lu] ",
            time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentThreadId());
        if (prefix < 0) prefix = 0;
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(line + prefix, sizeof(line) - static_cast<std::size_t>(prefix) - 2, format, arguments);
        va_end(arguments);
        std::size_t length = std::strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) --length;
        line[length++] = '\r';
        line[length++] = '\n';
        std::scoped_lock lock(log_mutex());
        CreateDirectoryW(directory.c_str(), nullptr);
        const std::wstring path = directory + L"\\dlssg.log";
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    } catch (...) {
    }
}

struct D3D12DlssgFrameGenerator::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    NgxDevice* ngx{};
    D3D12_RESOURCE_DESC image{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    // B's copy and the interpolation targets, only for a pair whose B or
    // destinations DLSS-G cannot use in place (made on first need then).
    ComPtr<ID3D12Resource> input;   // NON_PIXEL_SHADER_RESOURCE at rest
    std::array<ComPtr<ID3D12Resource>, kMaxOutputs> outputs; // UNORDERED_ACCESS at rest
    // What this pair's DLSS-G read and wrote, and their states at the end of
    // its list: the capture copies from them.
    ID3D12Resource* pair_input{};
    D3D12_RESOURCE_STATES pair_input_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    std::array<ID3D12Resource*, kMaxOutputs> pair_outputs{};
    D3D12_RESOURCE_STATES pair_output_state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    bool logged_direct{};
    NVSDK_NGX_Handle* feature{};
    NVSDK_NGX_Parameter* parameters{};
    UINT render_width{}, render_height{};
    bool feature_lite{};  // the mode `feature` was created in (lite_wanted)
    // Creation lists stay referenced until the generator goes away: NGX may
    // record into them, and they run once.
    std::vector<ComPtr<ID3D12CommandAllocator>> creation_allocators;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> creation_lists;
    std::uint64_t last_serial{};
    std::uint64_t evaluations{}, resets{};
    // Numbers the instances in creation order for the log (one per eye).
    std::uint32_t id{};
    std::uint32_t failures{};
    bool failed{};
    bool ready{};

    // DLSS-G's own verdict on each pair (OutputDisableInterpolation). When it
    // judges that a pair cannot be interpolated (it detects a scene change,
    // or lacks history) it writes B unchanged into the interpolated frame,
    // and the layer would still show that copy at the in-between pose: the
    // eye's image then jumps half a frame of head motion back and forth.
    // The verdict reaches the CPU a frame or two later through a small
    // readback ring; an instance that keeps turning interpolation off hands
    // its eye to optical flow for a while instead.
    static constexpr std::uint32_t kVerdictSlots = 8;
    static constexpr UINT64 kVerdictStride = 256;
    static constexpr std::uint32_t kVerdictPending = 0xFFFFFFFFU;
    ComPtr<ID3D12Resource> verdict;          // COMMON between submissions (a buffer)
    ComPtr<ID3D12Resource> verdict_readback; // COPY_DEST, mapped for the instance's life
    volatile std::uint32_t* verdict_mapped{};
    std::array<bool, kVerdictSlots> verdict_used{};
    std::array<bool, kVerdictSlots> verdict_reset{};
    std::uint32_t next_verdict{};
    bool verdict_enabled{};
    std::uint32_t verdict_window{};  // last known verdicts, newest in bit 0, 1 = turned off
    std::uint64_t verdicts{}, disabled_verdicts{};
    std::uint32_t good_streak{};
    std::uint32_t holds{};
    ULONGLONG optical_flow_until{};  // GetTickCount64 until which this eye uses optical flow
    ULONGLONG hold_ms{1000};
    bool holding{};

    // GPU time of each pair's DLSS-G list (v13): two timestamps a pair into
    // a small readback ring, read a few pairs later. For the log and the GPU
    // priority cycle; a failure only loses the numbers.
    static constexpr std::uint32_t kTimingSlots = 8;
    ComPtr<ID3D12QueryHeap> timing_heap;
    ComPtr<ID3D12Resource> timing_readback;   // COPY_DEST, mapped for the instance's life
    volatile std::uint64_t* timing_mapped{};
    double timing_ms_per_tick{};
    std::array<bool, kTimingSlots> timing_used{};
    std::uint32_t next_timing{};
    double timing_sum{}, timing_max{};
    std::uint64_t timing_count{};

    // Diagnostic capture (OFXR_DLSSG_CAPTURE): while the head turns, a burst
    // of consecutive pairs is copied out exactly as DLSS-G gets them - B,
    // depth, motion and the camera data - with DLSS-G's own output(s), for
    // off-line replay and motion-vector checks. What DLSS-G gets and produces
    // does not change, and the pairs' timing hardly does: their lists only
    // copy into video memory (well under a millisecond per eye). After the
    // burst a copy queue brings it over to readback memory, and a worker
    // thread writes the files. The buffers are made on the first pair, long
    // before any burst. A burst starts only where a turn begins after the
    // head was held still, so putting the headset on or looking about
    // without pause does not use the bursts up.
    static constexpr std::uint32_t kCapturePairs = 5;      // consecutive pairs per burst
    static constexpr std::uint32_t kCaptureMinPairs = 3;   // a burst a reset cuts short is kept from here
    static constexpr std::uint32_t kCaptureBursts = 8;     // per instance and session
    static constexpr std::uint32_t kCaptureImages = 3 + kMaxOutputs; // B, depth, motion, outputs
    static constexpr double kCaptureMinMotion = 16.0;      // render pixels the far screen centre moved: a turn
    static constexpr double kCaptureStillMotion = 6.0;     // below this the head is held still
    static constexpr std::uint32_t kCaptureStillPairs = 15; // still this long (about 0.4 s at VaM's rate)
    static constexpr std::uint32_t kCaptureTurnPairs = 12;  // then the turn gets that fast within this many pairs
    static constexpr ULONGLONG kCaptureCooldownMs = 1000;
    enum class CaptureState { idle, recording, waiting, copying, writing };
    struct CaptureImage {
        ComPtr<ID3D12Resource> staging;   // video memory, COMMON between lists
        ComPtr<ID3D12Resource> readback;  // COPY_DEST, mapped for the instance's life
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT rows{};
        UINT64 row_bytes{}, bytes{};
        UINT width{}, height{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        const std::uint8_t* mapped{};
    };
    struct CapturePair {
        std::array<CaptureImage, kCaptureImages> images;
        std::string text;
    };
    // File header of each image: tightly packed rows follow.
    struct CaptureHeader {
        std::uint32_t magic, version, width, height, format, bytes_per_pixel, rows, reserved;
    };
    bool capture_enabled{};
    CaptureState capture_state{CaptureState::idle};
    std::array<CapturePair, kCapturePairs> capture_pairs;
    std::uint32_t capture_images{};  // images per pair the buffers are made for (3 + outputs)
    std::uint32_t capture_index{}, capture_count{}, capture_bursts{}, capture_wait_calls{};
    std::uint64_t capture_first_serial{}, capture_previous_serial{};
    std::uint32_t capture_still_run{};  // consecutive pairs with the head still
    std::uint32_t capture_since_still{std::numeric_limits<std::uint32_t>::max()};  // pairs since it was still long enough
    bool capture_still_used{};  // that still spell already started a burst: one burst per turn
    ComPtr<ID3D12Fence> capture_fence;
    std::uint64_t capture_fence_value{}, capture_done_value{};
    ComPtr<ID3D12CommandQueue> capture_queue;  // COPY: video memory -> readback
    ComPtr<ID3D12CommandAllocator> capture_allocator;
    ComPtr<ID3D12GraphicsCommandList> capture_list;
    ULONGLONG capture_next_ms{};
    std::thread capture_writer;
    std::atomic<bool> capture_writing{};

    ~Impl() {
        capture_finish();
        if (verdict_readback && verdict_mapped != nullptr) verdict_readback->Unmap(0, nullptr);
        if (timing_readback && timing_mapped != nullptr) timing_readback->Unmap(0, nullptr);
        // The synthesizer drains its queue before its members are destroyed.
        std::scoped_lock lock(ngx_mutex());
        if (feature != nullptr) NVSDK_NGX_D3D12_ReleaseFeature(feature);
        if (parameters != nullptr) NVSDK_NGX_D3D12_DestroyParameters(parameters);
    }

    HRESULT create_texture(DXGI_FORMAT texture_format, D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>* output) noexcept {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = image.Width;
        description.Height = image.Height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = texture_format;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = flags;
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state,
            nullptr, IID_PPV_ARGS(output->ReleaseAndGetAddressOf()));
    }

    // Optional: without them DLSS-G runs as before, just without its verdict.
    void create_verdict_buffers() noexcept {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = kVerdictStride;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(verdict.ReleaseAndGetAddressOf())))) {
            verdict.Reset();
            return;
        }
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        buffer.Width = kVerdictStride * kVerdictSlots;
        buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
        void* mapped = nullptr;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(verdict_readback.ReleaseAndGetAddressOf()))) ||
            FAILED(verdict_readback->Map(0, nullptr, &mapped)) || mapped == nullptr) {
            verdict.Reset();
            verdict_readback.Reset();
            return;
        }
        verdict_mapped = static_cast<volatile std::uint32_t*>(mapped);
        verdict_enabled = true;
    }

    void create_timing() noexcept {
        UINT64 frequency = 0;
        if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0) return;
        D3D12_QUERY_HEAP_DESC description{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kTimingSlots, queue->GetDesc().NodeMask};
        if (FAILED(device->CreateQueryHeap(&description, IID_PPV_ARGS(timing_heap.ReleaseAndGetAddressOf())))) return;
        void* mapped = nullptr;
        if (FAILED(create_buffer(D3D12_HEAP_TYPE_READBACK, 2 * kTimingSlots * sizeof(std::uint64_t),
                D3D12_RESOURCE_STATE_COPY_DEST, &timing_readback)) ||
            FAILED(timing_readback->Map(0, nullptr, &mapped)) || mapped == nullptr) {
            timing_heap.Reset();
            timing_readback.Reset();
            return;
        }
        timing_mapped = static_cast<volatile std::uint64_t*>(mapped);
        timing_ms_per_tick = 1000.0 / static_cast<double>(frequency);
    }

    void poll_timing() noexcept {
        if (timing_mapped == nullptr) return;
        for (std::uint32_t slot = 0; slot < kTimingSlots; ++slot) {
            if (!timing_used[slot]) continue;
            const std::uint64_t begin = timing_mapped[2 * slot];
            const std::uint64_t end = timing_mapped[2 * slot + 1];
            if (begin == 0 || end == 0) continue;
            timing_used[slot] = false;
            if (end < begin) continue;
            const double ms = static_cast<double>(end - begin) * timing_ms_per_tick;
            timing_sum += ms;
            timing_max = std::max(timing_max, ms);
            ++timing_count;
            gpu_priority_sample(ms);
        }
    }

    // Verdicts the GPU has written since the last call. A reset pair is a
    // copy of B by request and is shown at B's pose, so it does not count.
    void poll_verdicts(ULONGLONG now) noexcept {
        if (!verdict_enabled || verdict_mapped == nullptr) return;
        for (std::uint32_t n = 0; n < kVerdictSlots; ++n) {
            const std::uint32_t slot = (next_verdict + n) % kVerdictSlots;
            if (!verdict_used[slot]) continue;
            const std::uint32_t value = verdict_mapped[slot * (kVerdictStride / 4)];
            if (value == kVerdictPending) continue;
            verdict_used[slot] = false;
            if (verdict_reset[slot]) continue;
            const bool disabled = (value & 0xFFU) != 0;
            ++verdicts;
            verdict_window = (verdict_window << 1) | (disabled ? 1U : 0U);
            if (!disabled) {
                // A clean minute (at 60 fps) earns the short hold back.
                if (++good_streak >= 3600) hold_ms = 1000;
                continue;
            }
            good_streak = 0;
            if (++disabled_verdicts == 1) {
                dlssg_log("DLSS-G[%u]: DLSS-G turned interpolation off for a pair (first time; it outputs B unchanged)", id);
            }
            const int recent = std::popcount(verdict_window & 0xFFU);
            if (recent >= 3 && now >= optical_flow_until) {
                ++holds;
                dlssg_log("DLSS-G[%u]: DLSS-G turned interpolation off on %d of this eye's last 8 pairs "
                    "(%llu of %llu so far); the eye uses optical flow for %llu ms, then DLSS-G again (hold %u)",
                    id, recent, static_cast<unsigned long long>(disabled_verdicts),
                    static_cast<unsigned long long>(verdicts), static_cast<unsigned long long>(hold_ms), holds);
                optical_flow_until = now + hold_ms;
                hold_ms = std::min<ULONGLONG>(hold_ms * 2, 16000);
                holding = true;
                verdict_window = 0;
                // Verdicts still in flight belong to the pairs before the hold.
                verdict_used.fill(false);
                return;
            }
        }
    }

    // ---- diagnostic capture ------------------------------------------------

    void capture_arm() noexcept {
        if (capture_root().empty()) return;
        D3D12_COMMAND_QUEUE_DESC copy{};
        copy.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        copy.NodeMask = queue->GetDesc().NodeMask;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(capture_fence.ReleaseAndGetAddressOf()))) ||
            FAILED(device->CreateCommandQueue(&copy, IID_PPV_ARGS(capture_queue.ReleaseAndGetAddressOf()))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                IID_PPV_ARGS(capture_allocator.ReleaseAndGetAddressOf()))) ||
            FAILED(device->CreateCommandList(copy.NodeMask, D3D12_COMMAND_LIST_TYPE_COPY, capture_allocator.Get(),
                nullptr, IID_PPV_ARGS(capture_list.ReleaseAndGetAddressOf()))) ||
            FAILED(capture_list->Close())) {
            capture_list.Reset();
            capture_allocator.Reset();
            capture_queue.Reset();
            capture_fence.Reset();
            dlssg_log("DLSS-G[%u] capture unavailable: its fence or copy queue could not be made", id);
            return;
        }
        capture_enabled = true;
        dlssg_log("DLSS-G[%u] capture armed: up to %u bursts of %u consecutive pairs, each where a head turn starts "
            "after the head was still, into %ls", id, kCaptureBursts, kCapturePairs, capture_root().c_str());
    }

    // Where the far point under the screen centre was in the previous frame,
    // relative to now, in render pixels: how far the camera turned this pair.
    static double centre_motion(const DlssMotionVectorFrame& g, double* dx, double* dy) noexcept {
        const float* m = g.clip_to_prev_clip.data();  // row-major, row vectors
        const double z = g.depth_inverted ? 0.0 : 1.0;
        const double x = z * m[8] + m[12], y = z * m[9] + m[13], w = z * m[11] + m[15];
        *dx = *dy = 0.0;
        if (!(std::fabs(w) > 1e-9)) return 0.0;
        *dx = x / w * 0.5 * g.motion_width;
        *dy = -y / w * 0.5 * g.motion_height;
        return std::hypot(*dx, *dy);
    }

    HRESULT create_buffer(D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state,
        ComPtr<ID3D12Resource>* output) noexcept {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, state, nullptr,
            IID_PPV_ARGS(output->ReleaseAndGetAddressOf()));
    }

    // What a capture copies, in file order: B's copy, depth, motion, then
    // every output.
    std::array<ID3D12Resource*, kCaptureImages> capture_sources(const DlssMotionVectorFrame& g) const noexcept {
        std::array<ID3D12Resource*, kCaptureImages> sources{pair_input, g.depth.Get(), g.motion_vectors.Get()};
        for (std::uint32_t k = 0; k < kMaxOutputs; ++k) sources[3 + k] = pair_outputs[k];
        return sources;
    }

    // Video-memory and readback buffers for a whole burst, made after this
    // pair's images, and made again if those change. Only when no burst is
    // under way.
    bool capture_buffers(const DlssMotionVectorFrame& g, std::uint32_t count) noexcept {
        const std::array<ID3D12Resource*, kCaptureImages> sources = capture_sources(g);
        const std::uint32_t n = 3 + count;
        std::array<D3D12_RESOURCE_DESC, kCaptureImages> descriptions{};
        bool same = capture_images >= n;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (sources[i] == nullptr) return false;
            descriptions[i] = sources[i]->GetDesc();
            const CaptureImage& first = capture_pairs[0].images[i];
            same = same && first.readback && first.width == descriptions[i].Width &&
                first.height == descriptions[i].Height && first.format == descriptions[i].Format;
        }
        if (same) return true;
        capture_images = 0;
        for (auto& pair : capture_pairs) {
            for (auto& slot : pair.images) slot = {};
        }
        UINT64 total = 0;
        for (auto& pair : capture_pairs) {
            for (std::uint32_t i = 0; i < n; ++i) {
                CaptureImage& slot = pair.images[i];
                device->GetCopyableFootprints(&descriptions[i], 0, 1, 0, &slot.layout, &slot.rows, &slot.row_bytes,
                    &slot.bytes);
                void* mapped = nullptr;
                if (FAILED(create_buffer(D3D12_HEAP_TYPE_DEFAULT, slot.bytes, D3D12_RESOURCE_STATE_COMMON, &slot.staging)) ||
                    FAILED(create_buffer(D3D12_HEAP_TYPE_READBACK, slot.bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                        &slot.readback)) ||
                    FAILED(slot.readback->Map(0, nullptr, &mapped)) || mapped == nullptr) {
                    for (auto& other : capture_pairs) {
                        for (auto& image_slot : other.images) image_slot = {};
                    }
                    return false;
                }
                slot.mapped = static_cast<const std::uint8_t*>(mapped);
                slot.width = static_cast<UINT>(descriptions[i].Width);
                slot.height = descriptions[i].Height;
                slot.format = descriptions[i].Format;
                total += slot.bytes;
            }
        }
        capture_images = n;
        dlssg_log("DLSS-G[%u] capture buffers ready: %u pairs of %u images, %.0f MB video memory and %.0f MB readback",
            id, kCapturePairs, n, static_cast<double>(total) / 1048576.0, static_cast<double>(total) / 1048576.0);
        return true;
    }

    // At the end of the pair's list, into video memory. States here: what
    // DLSS-G read in pair_input_state (its copy NON_PIXEL_SHADER_RESOURCE,
    // B itself COMMON), the guides COMMON, what it wrote in pair_output_state
    // (its targets UNORDERED_ACCESS, the destinations theirs); the buffers
    // COMMON.
    void capture_copies(ID3D12GraphicsCommandList* list, const DlssMotionVectorFrame& g, std::uint32_t count) noexcept {
        const std::array<ID3D12Resource*, kCaptureImages> sources = capture_sources(g);
        std::array<D3D12_RESOURCE_STATES, kCaptureImages> states{pair_input_state,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
        for (std::uint32_t k = 0; k < kMaxOutputs; ++k) states[3 + k] = pair_output_state;
        const UINT n = 3 + count;
        CapturePair& pair = capture_pairs[capture_index];
        std::array<D3D12_RESOURCE_BARRIER, 2 * kCaptureImages> barriers{};
        for (UINT i = 0; i < n; ++i) {
            barriers[i] = transition(sources[i], states[i], D3D12_RESOURCE_STATE_COPY_SOURCE);
            barriers[n + i] = transition(pair.images[i].staging.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
        }
        list->ResourceBarrier(2 * n, barriers.data());
        for (UINT i = 0; i < n; ++i) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = pair.images[i].staging.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = pair.images[i].layout;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = sources[i];
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        for (UINT i = 0; i < n; ++i) {
            barriers[i] = transition(sources[i], D3D12_RESOURCE_STATE_COPY_SOURCE, states[i]);
            barriers[n + i] = transition(pair.images[i].staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON);
        }
        list->ResourceBarrier(2 * n, barriers.data());
    }

    // The copy queue waits for the main queue's latest capture signal, brings
    // every image of the burst over to its readback buffer and signals again.
    bool capture_submit_copies() noexcept {
        if (FAILED(capture_allocator->Reset()) || FAILED(capture_list->Reset(capture_allocator.Get(), nullptr))) {
            return false;
        }
        for (std::uint32_t p = 0; p < capture_index; ++p) {
            for (std::uint32_t i = 0; i < 3 + capture_count; ++i) {
                const CaptureImage& slot = capture_pairs[p].images[i];
                capture_list->CopyBufferRegion(slot.readback.Get(), 0, slot.staging.Get(), 0, slot.bytes);
            }
        }
        if (FAILED(capture_list->Close()) || FAILED(capture_queue->Wait(capture_fence.Get(), capture_fence_value))) {
            return false;
        }
        ID3D12CommandList* lists[] = {capture_list.Get()};
        capture_queue->ExecuteCommandLists(1, lists);
        if (FAILED(capture_queue->Signal(capture_fence.Get(), ++capture_fence_value))) return false;
        capture_done_value = capture_fence_value;
        return true;
    }

    std::string capture_text(const DlssMotionVectorFrame& g, std::uint32_t count, bool reset,
        double dx, double dy, ULONGLONG now) const {
        std::string text;
        char line[320];
        const auto values = [&](const char* name, const float* v, int n) {
            text += name;
            for (int i = 0; i < n; ++i) {
                std::snprintf(line, sizeof(line), " %.9g", v[i]);
                text += line;
            }
            text += '\n';
        };
        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        const double time_us = frequency.QuadPart != 0 ?
            static_cast<double>(counter.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart) : 0.0;
        std::snprintf(line, sizeof(line), "pair %u\ndlssg %u\nlite %d\nserial %llu\nprevious_serial %llu\n"
            "epoch %llu\npublication %llu\nreset %d\noutputs %u\ntick_ms %llu\ntime_us %.0f\n",
            capture_index, id, feature_lite ? 1 : 0, static_cast<unsigned long long>(g.serial),
            static_cast<unsigned long long>(g.previous_serial), static_cast<unsigned long long>(g.epoch),
            static_cast<unsigned long long>(g.publication), reset ? 1 : 0, count,
            static_cast<unsigned long long>(now), time_us);
        text += line;
        std::snprintf(line, sizeof(line), "output %llu %u %u\nrender %u %u\noutput_rect %u %u %u %u\n"
            "motion_rect %u %u %u %u\ndepth_offset %u %u\n", static_cast<unsigned long long>(image.Width),
            image.Height, static_cast<unsigned>(format), g.depth_width, g.depth_height, g.output_x, g.output_y,
            g.output_width, g.output_height, g.motion_x, g.motion_y, g.motion_width, g.motion_height,
            g.depth_x, g.depth_y);
        text += line;
        std::snprintf(line, sizeof(line), "jittered %d\nframe_time_delta_ms %.9g\ndepth_inverted %d\ndepth_infinite %d\n",
            g.jittered ? 1 : 0, g.frame_time_delta_ms, g.depth_inverted ? 1 : 0, g.depth_infinite ? 1 : 0);
        text += line;
        const float jitter[2] = {g.jitter_x, g.jitter_y}, previous_jitter[2] = {g.previous_jitter_x, g.previous_jitter_y};
        const float scale[2] = {g.scale_x, g.scale_y};
        const float camera[4] = {g.camera_near, g.camera_far, g.camera_fov, g.camera_aspect};
        values("jitter", jitter, 2);
        values("previous_jitter", previous_jitter, 2);
        values("mvec_scale", scale, 2);
        values("near_far_fov_aspect", camera, 4);
        values("clip_to_prev_clip", g.clip_to_prev_clip.data(), 16);
        values("prev_clip_to_clip", g.prev_clip_to_clip.data(), 16);
        values("view_to_clip", g.view_to_clip.data(), 16);
        values("clip_to_view", g.clip_to_view.data(), 16);
        values("camera_position", g.camera_position.data(), 3);
        values("camera_up", g.camera_up.data(), 3);
        values("camera_right", g.camera_right.data(), 3);
        values("camera_forward", g.camera_forward.data(), 3);
        std::snprintf(line, sizeof(line), "centre_motion_px %.4f %.4f\npairs_since_still %u\n", dx, dy, capture_since_still);
        text += line;
        return text;
    }

    std::wstring capture_directory() const {
        return capture_root() + L"\\dlssg" + std::to_wstring(id) + L"-burst" + std::to_wstring(capture_bursts) +
            L"-serial" + std::to_wstring(capture_first_serial);
    }

    // Worker thread: the copies into the readback buffers are complete, and
    // nothing writes those until this returns.
    void capture_write(const std::wstring& directory, std::uint32_t pairs, std::uint32_t count) noexcept {
        try {
            CreateDirectoryW(capture_root().c_str(), nullptr);
            CreateDirectoryW(directory.c_str(), nullptr);
            static const wchar_t* const names[kCaptureImages] = {L"color", L"depth", L"motion", L"out1", L"out2",
                L"out3"};
            std::uint64_t bytes = 0;
            bool ok = true;
            for (std::uint32_t p = 0; p < pairs; ++p) {
                const CapturePair& pair = capture_pairs[p];
                const std::wstring stem = directory + L"\\pair" + std::to_wstring(p);
                ok = write_file(stem + L".txt", pair.text.data(), pair.text.size()) && ok;
                for (std::uint32_t i = 0; i < 3 + count; ++i) {
                    const CaptureImage& slot = pair.images[i];
                    const std::size_t row_bytes = static_cast<std::size_t>(slot.row_bytes);
                    std::vector<std::uint8_t> data(sizeof(CaptureHeader) + row_bytes * slot.rows);
                    const CaptureHeader header{0x4358464FU, 1U, slot.width, slot.height,
                        static_cast<std::uint32_t>(slot.format),
                        slot.width != 0 ? static_cast<std::uint32_t>(row_bytes / slot.width) : 0U, slot.rows, 0U};
                    std::memcpy(data.data(), &header, sizeof(header));
                    for (UINT row = 0; row < slot.rows; ++row) {
                        std::memcpy(data.data() + sizeof(header) + row * row_bytes,
                            slot.mapped + slot.layout.Offset + static_cast<std::size_t>(row) * slot.layout.Footprint.RowPitch,
                            row_bytes);
                    }
                    ok = write_file(stem + L"-" + names[i] + L".bin", data.data(), data.size()) && ok;
                    bytes += data.size();
                }
            }
            dlssg_log("DLSS-G[%u] capture: burst written to %ls (%u pairs, %.0f MB)%s", id, directory.c_str(),
                pairs, static_cast<double>(bytes) / 1048576.0, ok ? "" : "; some files failed");
        } catch (...) {
            dlssg_log("DLSS-G[%u] capture: writing the burst failed", id);
        }
    }

    void capture_stop(const char* reason) noexcept {
        capture_enabled = false;
        dlssg_log("DLSS-G[%u] capture stopped: %s", id, reason);
    }

    void capture_end_burst() noexcept {
        ++capture_bursts;
        capture_state = CaptureState::waiting;
        capture_wait_calls = 0;
    }

    // Once per evaluated pair, after its commands: starts, extends or ends a
    // burst, and moves a finished one on to the copy queue and the writer.
    void capture_pair(ID3D12GraphicsCommandList* list, const DlssMotionVectorFrame& g,
        std::uint32_t count, bool reset, ULONGLONG now) noexcept {
        try {
            double dx = 0.0, dy = 0.0;
            const double motion = centre_motion(g, &dx, &dy);
            if (!reset && motion < kCaptureStillMotion) {
                if (++capture_still_run >= kCaptureStillPairs) {
                    capture_since_still = 0;
                    capture_still_used = false;
                }
            } else {
                capture_still_run = 0;
                if (capture_since_still != std::numeric_limits<std::uint32_t>::max()) ++capture_since_still;
            }
            if (capture_state == CaptureState::waiting) {
                // The burst's last list went to the queue after its record(),
                // so a signal now lands behind it.
                if (++capture_wait_calls < 2) return;
                if (FAILED(queue->Signal(capture_fence.Get(), ++capture_fence_value)) || !capture_submit_copies()) {
                    capture_stop("the copy to readback memory could not be queued");
                    return;
                }
                capture_state = CaptureState::copying;
                return;
            }
            if (capture_state == CaptureState::copying) {
                if (capture_fence->GetCompletedValue() < capture_done_value) return;
                capture_state = CaptureState::writing;
                capture_writing.store(true);
                capture_writer = std::thread([this, directory = capture_directory(), pairs = capture_index,
                                                 images = capture_count]() {
                    capture_write(directory, pairs, images);
                    capture_writing.store(false);
                });
                return;
            }
            if (capture_state == CaptureState::writing) {
                if (capture_writing.load()) return;
                if (capture_writer.joinable()) capture_writer.join();
                capture_state = CaptureState::idle;
                capture_next_ms = now + kCaptureCooldownMs;
                if (capture_bursts >= kCaptureBursts) {
                    capture_enabled = false;
                    dlssg_log("DLSS-G[%u] capture: all %u bursts written", id, kCaptureBursts);
                }
                return;
            }
            if (capture_state == CaptureState::idle) {
                if (!capture_buffers(g, count)) {
                    capture_stop("its buffers could not be made (out of memory?)");
                    return;
                }
                if (reset || now < capture_next_ms || motion < kCaptureMinMotion ||
                    capture_since_still > kCaptureTurnPairs || capture_still_used) {
                    return;
                }
                capture_still_used = true;
                capture_state = CaptureState::recording;
                capture_index = 0;
                capture_count = count;
                capture_first_serial = g.serial;
                dlssg_log("DLSS-G[%u] capture: burst %u of %u starts at serial %llu (screen centre moved %.1f render px, "
                    "%u pairs after the head was still)", id, capture_bursts + 1, kCaptureBursts,
                    static_cast<unsigned long long>(g.serial), motion, capture_since_still);
            } else if (reset || g.serial != capture_previous_serial + 1 || count != capture_count) {
                if (capture_index >= kCaptureMinPairs) {
                    dlssg_log("DLSS-G[%u] capture: burst %u cut short after %u pairs (reset or gap at serial %llu)",
                        id, capture_bursts + 1, capture_index, static_cast<unsigned long long>(g.serial));
                    capture_end_burst();
                } else {
                    dlssg_log("DLSS-G[%u] capture: burst abandoned after %u pairs (reset or gap at serial %llu)",
                        id, capture_index, static_cast<unsigned long long>(g.serial));
                    capture_state = CaptureState::idle;
                    capture_next_ms = now + 1000;
                }
                return;
            }
            capture_copies(list, g, count);
            capture_pairs[capture_index].text = capture_text(g, count, reset, dx, dy, now);
            capture_previous_serial = g.serial;
            if (++capture_index == kCapturePairs) capture_end_burst();
        } catch (...) {
            capture_stop("exception");
        }
    }

    // The synthesizer drains its queue before it lets the generator go: a
    // burst still on its way is finished here, and the copy queue is idle
    // before the buffers go.
    void capture_finish() noexcept {
        try {
            if (capture_writer.joinable()) capture_writer.join();
            if (!capture_fence || !capture_queue || !queue) return;
            if (capture_state == CaptureState::waiting) {
                if (FAILED(queue->Signal(capture_fence.Get(), ++capture_fence_value)) || !capture_submit_copies()) {
                    capture_state = CaptureState::idle;
                } else {
                    capture_state = CaptureState::copying;
                }
            }
            if (FAILED(capture_queue->Signal(capture_fence.Get(), ++capture_fence_value))) return;
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (event != nullptr) {
                if (capture_fence->GetCompletedValue() < capture_fence_value &&
                    SUCCEEDED(capture_fence->SetEventOnCompletion(capture_fence_value, event))) {
                    WaitForSingleObject(event, 5000);
                }
                CloseHandle(event);
            }
            if (capture_state == CaptureState::copying && capture_fence->GetCompletedValue() >= capture_done_value) {
                capture_write(capture_directory(), capture_index, capture_count);
            }
            capture_state = CaptureState::idle;
        } catch (...) {
        }
    }

    HRESULT initialize(ID3D12Device* input_device, ID3D12CommandQueue* input_queue,
        const D3D12_RESOURCE_DESC& description) noexcept {
        if (input_device == nullptr || input_queue == nullptr ||
            description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            description.DepthOrArraySize != 1 || description.MipLevels != 1 ||
            description.SampleDesc.Count != 1) {
            return E_INVALIDARG;
        }
        format = unorm_format(description.Format);
        if (format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
        device = input_device;
        queue = input_queue;
        image = description;
        {
            std::scoped_lock lock(ngx_mutex());
            ngx = ngx_for(input_device);
        }
        if (ngx == nullptr || !ngx->ready) return E_FAIL;
        // Images DLSS-G can write in place (the layer's staging, v13) need no
        // targets of the generator's own; B's copy is made on first need.
        const bool in_place = writable_directly(image);
        if (!in_place) {
            HRESULT result = ensure_copy_input();
            if (FAILED(result)) return result;
            result = ensure_copy_outputs();
            if (FAILED(result)) return result;
        }
        create_verdict_buffers();
        create_timing();
        ready = true;
        static std::atomic<std::uint32_t> next_id{0};
        id = ++next_id;
        dlssg_log("DLSS-G[%u] generator ready for %llux%u images, format %u (interpolates as %u); %s%s",
            id, static_cast<unsigned long long>(image.Width), image.Height,
            static_cast<unsigned>(image.Format), static_cast<unsigned>(format),
            in_place ? "it writes the destinations in place" :
                (copies_forced() ? "copies in and out (OFXR_DLSSG_COPIES=1)" : "copies in and out (destinations not writable)"),
            verdict_enabled ? "" : "; DLSS-G's interpolation verdict is not read (buffers unavailable)");
        capture_arm();
        return S_OK;
    }

    // Same size, one mip, and a format DLSS-G sees as `format`: the typed
    // UNORM member itself, or its typeless family (DLSS-G makes its views
    // from DLSSG.BackbufferFormat; checked against the copies off-line,
    // investigations\fg-cost-20261101).
    bool same_image(const D3D12_RESOURCE_DESC& d) const noexcept {
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == image.Width &&
            d.Height == image.Height && d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1 &&
            (d.Format == format || (typeless_format(d.Format) && unorm_format(d.Format) == format));
    }
    bool readable_directly(ID3D12Resource* resource) const noexcept {
        return resource != nullptr && !copies_forced() && same_image(resource->GetDesc());
    }
    bool writable_directly(const D3D12_RESOURCE_DESC& d) const noexcept {
        return !copies_forced() && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && same_image(d);
    }
    HRESULT ensure_copy_input() noexcept {
        if (input) return S_OK;
        return create_texture(format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &input);
    }
    HRESULT ensure_copy_outputs() noexcept {
        for (auto& output : outputs) {
            if (output) continue;
            const HRESULT result = create_texture(format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
            if (FAILED(result)) return result;
        }
        return S_OK;
    }

    bool can_generate(const DlssMotionVectorFrame& guides, std::uint32_t count) const noexcept {
        return ready && !failed && ngx != nullptr && ngx->ready && guides.dlssg_camera &&
            GetTickCount64() >= optical_flow_until &&
            guides.depth && guides.motion_vectors && guides.depth_width != 0 &&
            guides.depth_height != 0 && guides.motion_width == guides.depth_width &&
            guides.motion_height == guides.depth_height && count >= 1 && count <= kMaxOutputs &&
            count <= ngx->multi_frame_count_max;
    }

    // NGX may record into the list it creates the feature with, so it gets
    // one of its own, run straight away on the synthesis queue - ahead of the
    // pair that is being recorded now.
    HRESULT ensure_feature(UINT width, UINT height) noexcept {
        const bool lite = lite_wanted();
        if (feature != nullptr && render_width == width && render_height == height && feature_lite == lite) return S_OK;
        if (feature != nullptr) {
            if (feature_lite != lite) {
                dlssg_log("DLSS-G[%u]: switching live to the %s mode ([Performance] DLSSGLite=%d); the feature is made again",
                    id, lite ? "lighter (Preset C)" : "default", lite ? 1 : 0);
            }
            // Size or mode change: wait for everything that may use the old feature.
            ComPtr<ID3D12Fence> fence;
            if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))) &&
                SUCCEEDED(queue->Signal(fence.Get(), 1))) {
                HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (event != nullptr) {
                    if (fence->GetCompletedValue() < 1 && SUCCEEDED(fence->SetEventOnCompletion(1, event))) {
                        WaitForSingleObject(event, 2000);
                    }
                    CloseHandle(event);
                }
            }
            NVSDK_NGX_D3D12_ReleaseFeature(feature);
            feature = nullptr;
        }
        // A block that asked for the lighter mode is replaced for the default
        // one, which leaves the key unset as before.
        if (parameters != nullptr && feature_lite && !lite) {
            NVSDK_NGX_D3D12_DestroyParameters(parameters);
            parameters = nullptr;
        }
        if (parameters == nullptr && NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&parameters))) {
            parameters = nullptr;
            return E_FAIL;
        }
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        HRESULT result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(allocator.GetAddressOf()));
        if (FAILED(result)) return result;
        result = device->CreateCommandList(queue->GetDesc().NodeMask, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
        if (FAILED(result)) return result;
        NVSDK_NGX_DLSSG_Create_Params create{};
        create.Width = static_cast<unsigned>(image.Width);
        create.Height = image.Height;
        create.NativeBackbufferFormat = static_cast<unsigned>(format);
        create.RenderWidth = width;
        create.RenderHeight = height;
        create.DynamicResolutionScaling = false;
        // Left unset otherwise, so an NVIDIA App override still applies.
        if (lite) NVSDK_NGX_Parameter_SetUI(parameters, kLiteCandidateBlendingEnabled, 1);
        const NVSDK_NGX_Result status = NGX_D3D12_CREATE_DLSSG(list.Get(), 1, 1, &feature, parameters, &create);
        result = list->Close();
        if (NVSDK_NGX_FAILED(status) || feature == nullptr) {
            feature = nullptr;
            dlssg_log("DLSS-G[%u]: NGX_D3D12_CREATE_DLSSG failed 0x%08x (output %llux%u, render %ux%u)",
                id, static_cast<unsigned>(status), static_cast<unsigned long long>(image.Width), image.Height, width, height);
            return E_FAIL;
        }
        if (FAILED(result)) return result;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        creation_allocators.push_back(allocator);
        creation_lists.push_back(list);
        render_width = width;
        render_height = height;
        feature_lite = lite;
        last_serial = 0;
        dlssg_log("DLSS-G[%u] feature created: output %llux%u, render %ux%u, MultiFrameCountMax %u, mode %s",
            id, static_cast<unsigned long long>(image.Width), image.Height, width, height, ngx->multi_frame_count_max,
            lite ? "lite requested (DLSSG.LiteCandidateBlendingEnabled=1, Preset C)" : "default");
        write_status("generating", "DLSS Frame Generation feature created");
        return S_OK;
    }

    void copy_fallback(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer,
        const DlssgRecordInput& in) noexcept {
        std::array<D3D12_RESOURCE_BARRIER, 1 + kMaxOutputs> before{};
        UINT count = 0;
        before[count++] = transition(backbuffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (std::uint32_t k = 0; k < in.outputs; ++k) {
            before[count++] = transition(in.destinations[k], in.destination_state, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        list->ResourceBarrier(count, before.data());
        for (std::uint32_t k = 0; k < in.outputs; ++k) list->CopyResource(in.destinations[k], backbuffer);
        count = 0;
        before[count++] = transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        for (std::uint32_t k = 0; k < in.outputs; ++k) {
            before[count++] = transition(in.destinations[k], D3D12_RESOURCE_STATE_COPY_DEST, in.destination_state);
        }
        list->ResourceBarrier(count, before.data());
    }

    HRESULT record(ID3D12GraphicsCommandList* list, const DlssgRecordInput& in, std::uint32_t* generated) noexcept {
        if (generated == nullptr) return E_POINTER;
        *generated = 0;
        if (list == nullptr || in.backbuffer == nullptr || in.guides == nullptr || in.outputs == 0 ||
            in.outputs > kMaxOutputs || !can_generate(*in.guides, in.outputs)) {
            return E_INVALIDARG;
        }
        for (std::uint32_t k = 0; k < in.outputs; ++k) {
            if (in.destinations[k] == nullptr) return E_INVALIDARG;
        }
        const DlssMotionVectorFrame& g = *in.guides;
        std::scoped_lock lock(ngx_mutex());
        const ULONGLONG now = GetTickCount64();
        if (holding) {
            holding = false;
            dlssg_log("DLSS-G[%u]: optical-flow hold over; this eye is back on DLSS-G (history reset)", id);
        }
        poll_verdicts(now);
        poll_timing();
        gpu_priority_tick(now, id % 2 == 1);
        // Nothing recorded yet: a failure here leaves the caller free to use
        // the optical-flow path for this pair.
        HRESULT result = ensure_feature(g.depth_width, g.depth_height);
        if (FAILED(result)) {
            failed = true;
            write_status("failed", "DLSS Frame Generation feature creation failed");
            return result;
        }

        // In place where possible (v13): DLSS-G reads B where it was captured
        // and writes the destinations themselves. Otherwise its own copy of B
        // and its own targets, copied out at the end, as before.
        const bool direct_input = readable_directly(in.backbuffer);
        bool direct_output = true;
        for (std::uint32_t k = 0; k < in.outputs; ++k) {
            direct_output = direct_output && writable_directly(in.destinations[k]->GetDesc());
        }
        if (!direct_input) {
            result = ensure_copy_input();
            if (FAILED(result)) return result;
        }
        if (!direct_output) {
            result = ensure_copy_outputs();
            if (FAILED(result)) return result;
        }
        if (!logged_direct) {
            logged_direct = true;
            dlssg_log("DLSS-G[%u]: B is read %s, frames are written %s", id,
                direct_input ? "where it was captured" : "from a copy",
                direct_output ? "into the destinations in place" : "to its own targets and copied out");
        }
        ID3D12Resource* const source = direct_input ? in.backbuffer : input.Get();
        pair_input = source;
        pair_input_state = direct_input ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        for (std::uint32_t k = 0; k < kMaxOutputs; ++k) {
            pair_outputs[k] = direct_output ? (k < in.outputs ? in.destinations[k] : nullptr) : outputs[k].Get();
        }
        pair_output_state = direct_output ? in.destination_state : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        ID3D12Resource* const depth = g.depth.Get();
        ID3D12Resource* const motion = g.motion_vectors.Get();
        const bool reset = g.reset || last_serial == 0 || g.serial != last_serial + 1;
        const std::uint32_t timing_slot = next_timing;
        const bool timed = timing_heap && !timing_used[timing_slot];
        if (timed) list->EndQuery(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * timing_slot);
        {
            std::array<D3D12_RESOURCE_BARRIER, 3 + kMaxOutputs> barriers{};
            UINT count = 0;
            if (direct_input) {
                barriers[count++] = transition(in.backbuffer, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            } else {
                const std::array<D3D12_RESOURCE_BARRIER, 2> before{
                    transition(in.backbuffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    transition(input.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
                list->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
                list->CopyResource(input.Get(), in.backbuffer);
                barriers[count++] = transition(in.backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                barriers[count++] = transition(input.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            barriers[count++] = transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barriers[count++] = transition(motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (direct_output && in.destination_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                for (std::uint32_t k = 0; k < in.outputs; ++k) {
                    barriers[count++] = transition(in.destinations[k], in.destination_state,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
            }
            list->ResourceBarrier(count, barriers.data());
        }
        // Back to the states the caller expects, after the evaluations.
        const auto restore_in_place = [&]() {
            std::array<D3D12_RESOURCE_BARRIER, 1 + kMaxOutputs> barriers{};
            UINT count = 0;
            if (direct_input) {
                barriers[count++] = transition(in.backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            }
            if (direct_output && in.destination_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                for (std::uint32_t k = 0; k < in.outputs; ++k) {
                    barriers[count++] = transition(in.destinations[k], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        in.destination_state);
                }
            }
            if (count != 0) list->ResourceBarrier(count, barriers.data());
        };

        NVSDK_NGX_D3D12_DLSSG_Eval_Params evaluation{};
        evaluation.pBackbuffer = source;
        evaluation.pDepth = depth;
        evaluation.pMVecs = motion;
        if (verdict_enabled) {
            // Buffers decay to COMMON after every submission; DLSS-G wants it
            // writable.
            const auto writable = transition(verdict.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &writable);
            evaluation.pOutputDisableInterpolation = verdict.Get();
        }
        NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
        std::memcpy(options.cameraViewToClip, g.view_to_clip.data(), sizeof(options.cameraViewToClip));
        std::memcpy(options.clipToCameraView, g.clip_to_view.data(), sizeof(options.clipToCameraView));
        std::memcpy(options.clipToPrevClip, g.clip_to_prev_clip.data(), sizeof(options.clipToPrevClip));
        std::memcpy(options.prevClipToClip, g.prev_clip_to_clip.data(), sizeof(options.prevClipToClip));
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) options.clipToLensClip[i][j] = i == j ? 1.0F : 0.0F;
        }
        options.jitterOffset[0] = g.jittered ? g.jitter_x : 0.0F;
        options.jitterOffset[1] = g.jittered ? g.jitter_y : 0.0F;
        options.mvecScale[0] = g.scale_x;
        options.mvecScale[1] = g.scale_y;
        options.cameraPinholeOffset[0] = options.cameraPinholeOffset[1] = 0.0F;
        std::memcpy(options.cameraPos, g.camera_position.data(), sizeof(options.cameraPos));
        std::memcpy(options.cameraUp, g.camera_up.data(), sizeof(options.cameraUp));
        std::memcpy(options.cameraRight, g.camera_right.data(), sizeof(options.cameraRight));
        std::memcpy(options.cameraFwd, g.camera_forward.data(), sizeof(options.cameraFwd));
        options.cameraNear = g.camera_near;
        options.cameraFar = g.camera_far;
        options.cameraFOV = g.camera_fov;
        options.cameraAspectRatio = g.camera_aspect;
        options.colorBuffersHDR = false;
        options.depthInverted = g.depth_inverted;
        options.cameraMotionIncluded = true;
        options.reset = reset;
        options.automodeOverrideReset = false;
        options.notRenderingGameFrames = false;
        options.orthoProjection = false;
        // A value the motion never takes, as Streamline passes: 0 here would
        // have DLSS-G treat every still pixel as having no motion vector.
        options.motionVectorsInvalidValue = std::numeric_limits<float>::max();
        options.motionVectorsDilated = false;
        options.menuDetectionEnabled = false;
        options.multiFrameCount = in.outputs;

        bool evaluated = true;
        for (std::uint32_t k = 0; k < in.outputs && evaluated; ++k) {
            evaluation.pOutputInterpFrame = pair_outputs[k];
            options.multiFrameIndex = k + 1;
            NVSDK_NGX_Result status = NGX_D3D12_EVALUATE_DLSSG(list, feature, parameters, &evaluation, &options);
            if (NVSDK_NGX_FAILED(status) && evaluation.pOutputDisableInterpolation != nullptr &&
                evaluations == 0 && verdicts == 0) {
                // A runtime that refuses the verdict buffer runs without it.
                dlssg_log("DLSS-G[%u]: interpolation verdict buffer refused (0x%08x); continuing without it",
                    id, static_cast<unsigned>(status));
                verdict_enabled = false;
                evaluation.pOutputDisableInterpolation = nullptr;
                status = NGX_D3D12_EVALUATE_DLSSG(list, feature, parameters, &evaluation, &options);
            }
            if (NVSDK_NGX_FAILED(status)) {
                evaluated = false;
                dlssg_log("DLSS-G[%u]: NGX_D3D12_EVALUATE_DLSSG failed 0x%08x (frame %u of %u)",
                    id, static_cast<unsigned>(status), k + 1, in.outputs);
            }
            if (k + 1 < in.outputs) {
                D3D12_RESOURCE_BARRIER uav{};
                uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uav.UAV.pResource = nullptr;
                list->ResourceBarrier(1, &uav);
            }
        }
        {
            const std::array<D3D12_RESOURCE_BARRIER, 2> guides_back{
                transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                transition(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)};
            list->ResourceBarrier(static_cast<UINT>(guides_back.size()), guides_back.data());
        }
        restore_in_place();
        if (!evaluated) {
            // The list stays valid: B goes into every synthetic and DLSS-G
            // starts over on the next pair.
            copy_fallback(list, in.backbuffer, in);
            last_serial = 0;
            if (++failures >= kFailureLimit) {
                failed = true;
                dlssg_log("DLSS-G[%u] disabled for this session after %u failed evaluations", id, failures);
                write_status("failed", "DLSS Frame Generation evaluation kept failing");
            }
            return S_OK;
        }
        failures = 0;
        if (verdict_enabled && evaluation.pOutputDisableInterpolation != nullptr) {
            const std::uint32_t slot = next_verdict;
            next_verdict = (next_verdict + 1U) % kVerdictSlots;
            verdict_mapped[slot * (kVerdictStride / 4)] = kVerdictPending;
            const auto readable = transition(verdict.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &readable);
            list->CopyBufferRegion(verdict_readback.Get(), slot * kVerdictStride, verdict.Get(), 0, 4);
            verdict_used[slot] = true;
            verdict_reset[slot] = reset;
        }

        if (!direct_output) {
            std::array<D3D12_RESOURCE_BARRIER, 2 * kMaxOutputs> before{};
            UINT count = 0;
            for (std::uint32_t k = 0; k < in.outputs; ++k) {
                before[count++] = transition(outputs[k].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                before[count++] = transition(in.destinations[k], in.destination_state, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            list->ResourceBarrier(count, before.data());
            for (std::uint32_t k = 0; k < in.outputs; ++k) list->CopyResource(in.destinations[k], outputs[k].Get());
            count = 0;
            for (std::uint32_t k = 0; k < in.outputs; ++k) {
                before[count++] = transition(outputs[k].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                before[count++] = transition(in.destinations[k], D3D12_RESOURCE_STATE_COPY_DEST, in.destination_state);
            }
            list->ResourceBarrier(count, before.data());
        }
        if (capture_enabled) capture_pair(list, g, in.outputs, reset, now);
        if (timed) {
            list->EndQuery(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * timing_slot + 1);
            timing_mapped[2 * timing_slot] = 0;
            timing_mapped[2 * timing_slot + 1] = 0;
            list->ResolveQueryData(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * timing_slot, 2,
                timing_readback.Get(), 2 * timing_slot * sizeof(std::uint64_t));
            timing_used[timing_slot] = true;
            next_timing = (next_timing + 1U) % kTimingSlots;
        }

        last_serial = g.serial;
        ++evaluations;
        if (reset) ++resets;
        if (evaluations == 1) {
            dlssg_log("DLSS-G[%u] first evaluation recorded: %u generated frame(s), render %ux%u, serial %llu",
                id, in.outputs, g.depth_width, g.depth_height, static_cast<unsigned long long>(g.serial));
        } else if (evaluations % 1800 == 0) {
            dlssg_log("DLSS-G[%u]: %llu evaluations, %llu resets, serial %llu; DLSS-G turned interpolation off "
                "on %llu of %llu checked pairs, %u optical-flow holds; GPU per pair mean %.2f ms, max %.2f (%llu timed)", id,
                static_cast<unsigned long long>(evaluations), static_cast<unsigned long long>(resets),
                static_cast<unsigned long long>(g.serial), static_cast<unsigned long long>(disabled_verdicts),
                static_cast<unsigned long long>(verdicts), holds,
                timing_count ? timing_sum / static_cast<double>(timing_count) : 0.0, timing_max,
                static_cast<unsigned long long>(timing_count));
            timing_sum = 0.0;
            timing_max = 0.0;
            timing_count = 0;
        }
        // A reset frame is a copy of B and belongs at B's pose.
        *generated = reset ? 0 : in.outputs;
        return S_OK;
    }
};

D3D12DlssgFrameGenerator::D3D12DlssgFrameGenerator() noexcept = default;
D3D12DlssgFrameGenerator::~D3D12DlssgFrameGenerator() = default;

HRESULT D3D12DlssgFrameGenerator::initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
    const D3D12_RESOURCE_DESC& image) noexcept {
    try {
        auto impl = std::make_unique<Impl>();
        const HRESULT result = impl->initialize(device, queue, image);
        if (FAILED(result)) return result;
        impl_ = std::move(impl);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

std::uint32_t D3D12DlssgFrameGenerator::max_outputs() const noexcept {
    return impl_ != nullptr && impl_->ngx != nullptr
        ? std::min<std::uint32_t>(kMaxOutputs, impl_->ngx->multi_frame_count_max) : kMaxOutputs;
}

bool D3D12DlssgFrameGenerator::can_generate(const DlssMotionVectorFrame& guides, std::uint32_t outputs) const noexcept {
    return impl_ != nullptr && impl_->can_generate(guides, outputs);
}

HRESULT D3D12DlssgFrameGenerator::record(ID3D12GraphicsCommandList* list, const DlssgRecordInput& input,
    std::uint32_t* generated) noexcept {
    if (generated != nullptr) *generated = 0;
    if (impl_ == nullptr) return E_UNEXPECTED;
    try {
        return impl_->record(list, input, generated);
    } catch (...) {
        return E_FAIL;
    }
}

void D3D12DlssgFrameGenerator::invalidate_history() noexcept {
    if (impl_ != nullptr) impl_->last_serial = 0;
}

bool D3D12DlssgFrameGenerator::usable() const noexcept {
    return impl_ != nullptr && impl_->ready && !impl_->failed;
}

}  // namespace xrfg

// For VaM's in-game menu (v12): 1 when this process uses DLSS Frame Generation,
// whose mode then follows [Performance] DLSSGLite live (lite_wanted), else 0.
// Layers before v12 have no such export and read the mode once.
extern "C" __declspec(dllexport) int OFXR_GetDlssgLiveModeV1() noexcept {
    return xrfg::dlssg_requested() ? 1 : 0;
}
