#include "pch.h"

#include "sim.h"
#include "utils.h"

using namespace ovrnull::utils;

namespace simxr {

    namespace {

        std::optional<double> EnvNumber(const wchar_t* name) {
            wchar_t value[64]{};
            const DWORD length = GetEnvironmentVariableW(name, value, 64);
            if (length == 0 || length >= 64) {
                return {};
            }
            wchar_t* end = nullptr;
            const double number = wcstod(value, &end);
            if (end == value) {
                return {};
            }
            return number;
        }

        Config LoadConfig() {
            Config config;
            // Quest 3 through Virtual Desktop (tangents: up 44.0, down 55.0, outer 54.0, inner 40.0 degrees).
            config.fov[ovrEye_Left].UpTan = 0.965688765f;
            config.fov[ovrEye_Left].DownTan = 1.42814791f;
            config.fov[ovrEye_Left].LeftTan = 1.37638187f;
            config.fov[ovrEye_Left].RightTan = 0.839099586f;
            config.fov[ovrEye_Right] = config.fov[ovrEye_Left];
            std::swap(config.fov[ovrEye_Right].LeftTan, config.fov[ovrEye_Right].RightTan);

            if (auto v = EnvNumber(L"SIMXR_REFRESH"); v && *v >= 30 && *v <= 240) {
                config.displayRate = (float)*v;
            }
            if (auto v = EnvNumber(L"SIMXR_WIDTH"); v && *v >= 256 && *v <= 8192) {
                config.width = (int)*v;
            }
            if (auto v = EnvNumber(L"SIMXR_HEIGHT"); v && *v >= 256 && *v <= 8192) {
                config.height = (int)*v;
            }
            if (auto v = EnvNumber(L"SIMXR_IPD_MM"); v && *v >= 40 && *v <= 90) {
                config.ipd = (float)(*v / 1000.0);
            }
            if (auto v = EnvNumber(L"SIMXR_HEAD_HEIGHT_CM"); v && *v >= 0 && *v <= 250) {
                config.headHeight = (float)(*v / 100.0);
            }
            if (auto v = EnvNumber(L"SIMXR_LATCH_US"); v && *v >= 0 && *v <= 8000) {
                config.latchMs = (float)(*v / 1000.0);
            }
            if (auto v = EnvNumber(L"SIMXR_MAX_QUEUED_FRAMES"); v && *v >= -1 && *v <= 16) {
                config.maxQueuedFrames = std::max(0, (int)*v);
            }
            if (auto v = EnvNumber(L"SIMXR_PORT"); v && *v >= 0 && *v <= 65535) {
                config.port = (int)*v;
            }
            if (auto v = EnvNumber(L"SIMXR_VSYNC_LOG"); v) {
                config.vsyncLog = *v != 0;
            }
            if (auto v = EnvNumber(L"SIMXR_CAPTURE_MB"); v && *v >= 64 && *v <= 16384) {
                config.captureMemoryMB = (size_t)*v;
            }

            wchar_t path[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"SIMXR_LOG_DIR", path, MAX_PATH);
            if (length > 0 && length < MAX_PATH) {
                config.logDir = path;
            } else {
                PWSTR localAppData = nullptr;
                SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
                config.logDir = std::filesystem::path(localAppData ? localAppData : L".") / L"SimXR";
                CoTaskMemFree(localAppData);
            }
            std::error_code ignored;
            std::filesystem::create_directories(config.logDir, ignored);
            return config;
        }

        std::mutex g_logMutex;
        std::ofstream g_logStream;

        const double g_startTime = [] {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return QpcToOvrTime(now);
        }();

        std::mutex g_liveMutex;
        IDriverControl* g_liveDriver = nullptr;

        double Smoothstep(double x) {
            x = std::clamp(x, 0.0, 1.0);
            return x * x * (3 - 2 * x);
        }

        // Catmull-Rom between p1 and p2 (keys p0..p3 at times t0..t3), non-uniform spacing.
        double CatmullRom(double p0, double p1, double p2, double p3, double t0, double t1, double t2, double t3, double t) {
            const double m1 = (t2 - t0) > 0 ? (p2 - p0) / (t2 - t0) * (t2 - t1) : 0;
            const double m2 = (t3 - t1) > 0 ? (p3 - p1) / (t3 - t1) * (t2 - t1) : 0;
            const double u = (t2 - t1) > 0 ? (t - t1) / (t2 - t1) : 1;
            const double u2 = u * u, u3 = u2 * u;
            return (2 * u3 - 3 * u2 + 1) * p1 + (u3 - 2 * u2 + u) * m1 + (-2 * u3 + 3 * u2) * p2 + (u3 - u2) * m2;
        }

        float Component(const Motion::Key& key, int c) {
            return c < 3 ? key.ypr[c] : key.pos[c - 3];
        }

    } // namespace

    const Config& GetConfig() {
        static const Config config = LoadConfig();
        return config;
    }

    double Now() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return QpcToOvrTime(now);
    }

    double StartTime() {
        return g_startTime;
    }

    void Log(const char* fmt, ...) {
        char message[2048];
        va_list va;
        va_start(va, fmt);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, va);
        va_end(va);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char stamp[64];
        sprintf_s(stamp,
                  "%04u-%02u-%02u %02u:%02u:%02u.%03u +%.3fs ",
                  st.wYear,
                  st.wMonth,
                  st.wDay,
                  st.wHour,
                  st.wMinute,
                  st.wSecond,
                  st.wMilliseconds,
                  Now() - g_startTime);

        std::unique_lock lock(g_logMutex);
        if (!g_logStream.is_open()) {
            g_logStream.open(GetConfig().logDir / L"simxr-hmd.log", std::ios_base::app);
        }
        g_logStream << stamp << message;
        if (message[0] && message[strlen(message) - 1] != '\n') {
            g_logStream << '\n';
        }
        g_logStream.flush();
        OutputDebugStringA(message);
    }

    // ------------------------------------------------------------------ pose

    OVR::Quatf YprToQuat(const OVR::Vector3f& ypr) {
        const float d = (float)(M_PI / 180.0);
        const OVR::Quatf yaw(OVR::Vector3f(0, 1, 0), ypr.x * d);
        const OVR::Quatf pitch(OVR::Vector3f(1, 0, 0), ypr.y * d);
        const OVR::Quatf roll(OVR::Vector3f(0, 0, 1), ypr.z * d);
        return (yaw * pitch * roll).Normalized();
    }

    OVR::Vector3f QuatToYpr(const OVR::Quatf& q) {
        float yaw = 0, pitch = 0, roll = 0;
        q.GetYawPitchRoll(&yaw, &pitch, &roll);
        const float r = (float)(180.0 / M_PI);
        return {yaw * r, pitch * r, roll * r};
    }

    OVR::Posef ToPosef(const PoseYpr& pose) {
        return OVR::Posef(YprToQuat(pose.ypr), pose.pos);
    }

    void Motion::Evaluate(double t, OVR::Vector3f& pos, OVR::Vector3f& ypr) const {
        pos = {0, 0, 0};
        ypr = {0, 0, 0};
        if (type == Type::Still || t <= start) {
            if (type != Type::Keyframes || keys.empty()) {
                return;
            }
        }
        double local = std::max(0.0, t - start);
        if (duration >= 0) {
            local = std::min(local, duration);
        }
        auto add = [&](int c, double value) {
            if (c < 3) {
                ypr[c] += (float)value;
            } else {
                pos[c - 3] += (float)value;
            }
        };
        switch (type) {
        case Type::Turn:
            add(axis, speed * local);
            break;
        case Type::Sine:
            add(axis, amplitude * sin(2 * M_PI * local / std::max(1e-3, period) + phase * M_PI / 180.0));
            break;
        case Type::Keyframes: {
            if (keys.empty()) {
                break;
            }
            double kt = local;
            const double last = keys.back().t;
            if (loop && last > 0) {
                kt = fmod(local, last);
            }
            if (kt <= keys.front().t || keys.size() == 1) {
                for (int c = 0; c < 6; c++) {
                    add(c, Component(keys.front(), c));
                }
                break;
            }
            if (kt >= last) {
                for (int c = 0; c < 6; c++) {
                    add(c, Component(keys.back(), c));
                }
                break;
            }
            size_t i = 0;
            while (i + 1 < keys.size() && keys[i + 1].t < kt) {
                i++;
            }
            const auto& k1 = keys[i];
            const auto& k2 = keys[i + 1];
            const auto& k0 = i > 0 ? keys[i - 1] : k1;
            const auto& k3 = i + 2 < keys.size() ? keys[i + 2] : k2;
            for (int c = 0; c < 6; c++) {
                add(c,
                    CatmullRom(Component(k0, c),
                               Component(k1, c),
                               Component(k2, c),
                               Component(k3, c),
                               k0.t,
                               k1.t,
                               k2.t,
                               k3.t,
                               kt));
            }
            break;
        }
        default:
            break;
        }
    }

    std::string Motion::Describe() const {
        static const char* axes[] = {"yaw", "pitch", "roll", "x", "y", "z"};
        char text[256];
        switch (type) {
        case Type::Turn:
            sprintf_s(text, "turn %s %.3f/s for %.3f s", axes[axis], speed, duration);
            break;
        case Type::Sine:
            sprintf_s(text, "sine %s amplitude %.3f period %.3f s for %.3f s", axes[axis], amplitude, period, duration);
            break;
        case Type::Keyframes:
            sprintf_s(text, "keyframes x%zu over %.3f s%s", keys.size(), keys.empty() ? 0.0 : keys.back().t, loop ? " (loop)" : "");
            break;
        default:
            sprintf_s(text, "still");
        }
        return text;
    }

    PoseYpr Tracked::At(double t) const {
        OVR::Vector3f pos, ypr;
        motion.Evaluate(t, pos, ypr);
        PoseYpr result = base;
        result.pos += pos;
        result.ypr += ypr;
        return result;
    }

    void Tracked::Restart(const Motion& next, double now) {
        base = At(now);
        motion = next;
        if (motion.start < now) {
            motion.start = now;
        }
    }

    // ----------------------------------------------------------------- state

    State& State::Get() {
        static State state;
        return state;
    }

    State::State() {
        const auto& config = GetConfig();
        m_head.base.pos = {0, config.headHeight, 0};
        // Hands low and in front, pointing forward and a little down.
        m_controllers[Left].pose.base.pos = {-0.18f, -0.30f, -0.32f};
        m_controllers[Left].pose.base.ypr = {0, -20, 0};
        m_controllers[Right].pose.base.pos = {0.18f, -0.30f, -0.32f};
        m_controllers[Right].pose.base.ypr = {0, -20, 0};
    }

    PoseYpr State::HeadAt(double t) const {
        return m_head.At(t);
    }

    ovrPoseStatef State::Sample(const std::function<OVR::Posef(double)>& poseAt, double t) const {
        constexpr double h = 0.001;
        const OVR::Posef p = poseAt(t);
        const OVR::Posef a = poseAt(t - h);
        const OVR::Posef b = poseAt(t + h);
        ovrPoseStatef state{};
        state.ThePose = p;
        state.LinearVelocity = (b.Translation - a.Translation) / (float)(2 * h);
        // World-frame angular velocity from q(t+h) * q(t-h)^-1.
        const OVR::Quatf dq = (b.Rotation * a.Rotation.Inverted()).Normalized();
        OVR::Vector3f axis;
        float angle = 0;
        dq.GetAxisAngle(&axis, &angle);
        if (angle > (float)M_PI) {
            angle -= (float)(2 * M_PI);
        }
        state.AngularVelocity = axis * (angle / (float)(2 * h));
        state.TimeInSeconds = t;
        return state;
    }

    ovrPoseStatef State::HeadPose(double t, bool floorOrigin) const {
        std::shared_lock lock(m_mutex);
        const float lift = floorOrigin ? 0.f : -GetConfig().headHeight;
        return Sample(
            [&](double time) {
                OVR::Posef pose = ToPosef(HeadAt(time));
                pose.Translation.y += lift;
                return pose;
            },
            t);
    }

    bool State::HasController(int hand) const {
        std::shared_lock lock(m_mutex);
        return m_controllers[hand].connected;
    }

    ovrPoseStatef State::ControllerPose(int hand, double t, bool floorOrigin) const {
        std::shared_lock lock(m_mutex);
        const float lift = floorOrigin ? 0.f : -GetConfig().headHeight;
        const ControllerState& controller = m_controllers[hand];
        return Sample(
            [&](double time) {
                OVR::Posef pose = ToPosef(controller.pose.At(time));
                if (controller.followHead) {
                    pose = ToPosef(HeadAt(time)) * pose;
                }
                pose.Translation.y += lift;
                return pose;
            },
            t);
    }

    ovrInputState State::Input(double t) const {
        std::shared_lock lock(m_mutex);
        ovrInputState input{};
        input.TimeInSeconds = t;
        input.ControllerType = ovrControllerType_Touch;
        float trigger[2]{}, grip[2]{};
        for (int hand = 0; hand < 2; hand++) {
            const auto& c = m_controllers[hand];
            if (!c.connected) {
                continue;
            }
            input.Buttons |= c.buttons;
            input.Touches |= c.touches;
            trigger[hand] = c.trigger;
            grip[hand] = c.grip;
            input.Thumbstick[hand] = c.stick;
        }
        for (const auto& press : m_presses) {
            if (t < press.start || t >= press.end || !m_controllers[press.hand].connected) {
                continue;
            }
            const bool left = press.hand == Left;
            if (press.control == "a" || press.control == "x") {
                input.Buttons |= left ? ovrButton_X : ovrButton_A;
                input.Touches |= left ? ovrTouch_X : ovrTouch_A;
            } else if (press.control == "b" || press.control == "y") {
                input.Buttons |= left ? ovrButton_Y : ovrButton_B;
                input.Touches |= left ? ovrTouch_Y : ovrTouch_B;
            } else if (press.control == "menu") {
                input.Buttons |= ovrButton_Enter; // the left controller's menu button
            } else if (press.control == "system") {
                input.Buttons |= ovrButton_Home;
            } else if (press.control == "stick") {
                input.Buttons |= left ? ovrButton_LThumb : ovrButton_RThumb;
                input.Touches |= left ? ovrTouch_LThumb : ovrTouch_RThumb;
            } else if (press.control == "trigger") {
                trigger[press.hand] = std::max(trigger[press.hand], press.value);
            } else if (press.control == "grip") {
                grip[press.hand] = std::max(grip[press.hand], press.value);
            }
        }
        for (int hand = 0; hand < 2; hand++) {
            if (trigger[hand] > 0.f) {
                input.Touches |= hand == Left ? ovrTouch_LIndexTrigger : ovrTouch_RIndexTrigger;
            }
            input.IndexTrigger[hand] = input.IndexTriggerNoDeadzone[hand] = input.IndexTriggerRaw[hand] = trigger[hand];
            input.HandTrigger[hand] = input.HandTriggerNoDeadzone[hand] = input.HandTriggerRaw[hand] = grip[hand];
            input.ThumbstickNoDeadzone[hand] = input.ThumbstickRaw[hand] = input.Thumbstick[hand];
        }
        return input;
    }

    SessionFlags State::Flags() const {
        std::shared_lock lock(m_mutex);
        return m_flags;
    }

    void State::SetHead(const PoseYpr& pose) {
        std::unique_lock lock(m_mutex);
        m_head.base = pose;
        m_head.motion = Motion{};
    }

    void State::SetHeadMotion(const Motion& motion) {
        std::unique_lock lock(m_mutex);
        m_head.Restart(motion, Now());
    }

    void State::SetController(int hand, const std::function<void(ControllerState&)>& edit) {
        std::unique_lock lock(m_mutex);
        edit(m_controllers[hand]);
    }

    void State::SetControllerMotion(int hand, const Motion& motion) {
        std::unique_lock lock(m_mutex);
        m_controllers[hand].pose.Restart(motion, Now());
    }

    void State::AddPress(const Press& press) {
        std::unique_lock lock(m_mutex);
        // Forget presses that are over.
        const double now = Now();
        m_presses.erase(
            std::remove_if(m_presses.begin(), m_presses.end(), [&](const Press& p) { return p.end < now - 1.0; }),
            m_presses.end());
        m_presses.push_back(press);
    }

    void State::SetFlags(const std::function<void(SessionFlags&)>& edit) {
        std::unique_lock lock(m_mutex);
        edit(m_flags);
    }

    std::string State::DescribeJson(double now) const {
        std::shared_lock lock(m_mutex);
        const PoseYpr head = HeadAt(now);
        std::string json = "\"head\":{\"pos\":" + JsonVec3(head.pos) + ",\"ypr\":" + JsonVec3(head.ypr) +
                           ",\"motion\":\"" + JsonEscape(m_head.motion.Describe()) + "\"" +
                           ",\"motion_ends_in_s\":" +
                           (std::isinf(m_head.motion.End()) ? std::string("null") : std::to_string(std::max(0.0, m_head.motion.End() - now))) +
                           "},\"controllers\":[";
        for (int hand = 0; hand < 2; hand++) {
            const auto& c = m_controllers[hand];
            const PoseYpr pose = c.pose.At(now);
            char extra[256];
            sprintf_s(extra,
                      ",\"buttons\":%u,\"touches\":%u,\"trigger\":%.3f,\"grip\":%.3f,\"stick\":[%.3f,%.3f]}",
                      c.buttons,
                      c.touches,
                      c.trigger,
                      c.grip,
                      c.stick.x,
                      c.stick.y);
            json += std::string(hand ? "," : "") + "{\"hand\":\"" + (hand ? "right" : "left") +
                    "\",\"connected\":" + (c.connected ? "true" : "false") +
                    ",\"follow_head\":" + (c.followHead ? "true" : "false") + ",\"pos\":" + JsonVec3(pose.pos) +
                    ",\"ypr\":" + JsonVec3(pose.ypr) + ",\"motion\":\"" + JsonEscape(c.pose.motion.Describe()) + "\"" +
                    extra;
        }
        json += "],\"session\":{\"hmd_mounted\":";
        json += m_flags.hmdMounted ? "true" : "false";
        json += ",\"should_quit\":";
        json += m_flags.shouldQuit ? "true" : "false";
        json += ",\"visible\":";
        json += m_flags.visible ? "true" : "false";
        json += ",\"input_focus\":";
        json += m_flags.inputFocus ? "true" : "false";
        json += "}";
        int active = 0;
        for (const auto& press : m_presses) {
            if (press.end > now) {
                active++;
            }
        }
        json += ",\"pending_presses\":" + std::to_string(active);
        return json;
    }

    // ----------------------------------------------------------- live driver

    void SetLiveDriver(IDriverControl* driver) {
        std::unique_lock lock(g_liveMutex);
        g_liveDriver = driver;
    }

    void ClearLiveDriver(IDriverControl* driver) {
        std::unique_lock lock(g_liveMutex);
        if (g_liveDriver == driver) {
            g_liveDriver = nullptr;
        }
    }

    // Used by control.cpp: runs fn with the live driver (or nullptr) under the lock,
    // so the driver cannot be destroyed meanwhile.
    void WithLiveDriver(const std::function<void(IDriverControl*)>& fn) {
        std::unique_lock lock(g_liveMutex);
        fn(g_liveDriver);
    }

    // ------------------------------------------------------------------ json

    std::string JsonEscape(const std::string& text) {
        std::string out;
        out.reserve(text.size() + 8);
        for (const char ch : text) {
            switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if ((unsigned char)ch < 0x20) {
                    char hex[8];
                    sprintf_s(hex, "\\u%04x", (unsigned char)ch);
                    out += hex;
                } else {
                    out += ch;
                }
            }
        }
        return out;
    }

    std::string JsonVec3(const OVR::Vector3f& v) {
        char text[96];
        sprintf_s(text, "[%.5f,%.5f,%.5f]", v.x, v.y, v.z);
        return text;
    }

} // namespace simxr
