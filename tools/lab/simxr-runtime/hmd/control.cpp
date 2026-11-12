// SimXR control port: newline-delimited JSON over TCP on 127.0.0.1 only
// (SIMXR_PORT, default 37601). One request per line, one response per line:
//   {"cmd":"status"}                       -> {"ok":true, "device":{...}, "frames":{...}, "head":{...}, ...}
//   {"cmd":"head","ypr":[30,0,0]}          -> sets the head's base pose (pos and/or ypr), stops its motion
//   {"cmd":"motion","target":"head","type":"sine","axis":"yaw","amplitude":30,"period":2,"duration":8}
//   {"cmd":"press","hand":"left","button":"menu","ms":120}
//   {"cmd":"capture","frames":8,"scale":0.5,"format":"png"}
//   {"cmd":"stats","reset":true}
// "help" lists every command and its fields. A request may carry "id", which
// the response echoes. Started by the first swapchain (sim.h EnsureControlServer),
// stopped by ovr_Shutdown before the DLL can be unloaded.

#include "pch.h"

#include <cJSON.h>

#include "sim.h"

namespace simxr {
    void WithLiveDriver(const std::function<void(IDriverControl*)>& fn);
}

namespace {

    using namespace simxr;

    std::mutex g_serverMutex;
    std::thread g_serverThread;
    std::atomic<bool> g_stopServer{false};
    std::atomic<int> g_clients{0};

    const char* k_Help =
        "commands (fields optional unless noted; angles in degrees, distances in metres, times in ms unless *_s):\n"
        " ping | status | help\n"
        " stats {reset:bool}  display model since the last reset: vsyncs, new frames, repeats, dropped, gpu_late, "
        "submit intervals, age at display, pose error\n"
        " head {pos:[x,y,z], ypr:[yaw,pitch,roll]}  base pose (floor origin; yaw+ = left, pitch+ = up), stops the head motion\n"
        " motion {target:head|left|right, type:still|turn|sine|keyframes, axis:yaw|pitch|roll|x|y|z, speed (per s), "
        "amplitude, period_s, phase, duration_s (<0 = forever), delay_ms, loop, keys:[{t_s, pos:[..], ypr:[..]}]}  "
        "offsets added to the base pose, continuing from the current pose\n"
        " controller {hand:left|right (required), connected, follow_head, pos, ypr, trigger, grip, stick:[x,y], "
        "buttons:{a,b,x,y,menu,system,stick:bool}, touches:{...}}\n"
        " press {hand (required), button:a|b|x|y|menu|system|stick|trigger|grip (required), ms=100, delay_ms=0, value=1}\n"
        " capture {frames=1, every=1, delay_ms=0, eyes:both|left|right, scale=1, crop:[x,y,w,h], format:png|raw, "
        "dir, name}  eye images of the next submitted frames -> PNG or raw RGBA8 + capture.jsonl\n"
        " capture_status\n"
        " mark {label}  a line in simxr-markers.csv (vsync, submitted count)\n"
        " session {hmd_mounted, should_quit, visible, input_focus, should_recenter}  OVR session status flags (should_recenter: one system recenter, like the headset's button)\n";

    // ---------------------------------------------------------------- parsing

    std::optional<double> Number(const cJSON* object, const char* name) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
        if (cJSON_IsNumber(item)) {
            return item->valuedouble;
        }
        return {};
    }

    std::optional<bool> Bool(const cJSON* object, const char* name) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
        if (cJSON_IsBool(item)) {
            return cJSON_IsTrue(item) != 0;
        }
        if (cJSON_IsNumber(item)) {
            return item->valuedouble != 0;
        }
        return {};
    }

    std::optional<std::string> String(const cJSON* object, const char* name) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
        if (cJSON_IsString(item) && item->valuestring) {
            return std::string(item->valuestring);
        }
        return {};
    }

    std::optional<OVR::Vector3f> Vec3(const cJSON* object, const char* name) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
        if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 3) {
            return {};
        }
        OVR::Vector3f v;
        for (int i = 0; i < 3; i++) {
            const cJSON* c = cJSON_GetArrayItem(item, i);
            if (!cJSON_IsNumber(c)) {
                return {};
            }
            v[i] = (float)c->valuedouble;
        }
        return v;
    }

    int HandOf(const cJSON* request, std::string& error) {
        const auto hand = String(request, "hand");
        if (hand == "left" || hand == "l") {
            return Left;
        }
        if (hand == "right" || hand == "r") {
            return Right;
        }
        error = "hand must be \"left\" or \"right\"";
        return -1;
    }

    int AxisOf(const std::string& name) {
        static const char* axes[] = {"yaw", "pitch", "roll", "x", "y", "z"};
        for (int i = 0; i < 6; i++) {
            if (name == axes[i]) {
                return i;
            }
        }
        return -1;
    }

    unsigned ButtonBit(int hand, const std::string& name, bool touch) {
        const bool left = hand == Left;
        if (name == "a" || name == "x") {
            return touch ? (left ? ovrTouch_X : ovrTouch_A) : (left ? ovrButton_X : ovrButton_A);
        }
        if (name == "b" || name == "y") {
            return touch ? (left ? ovrTouch_Y : ovrTouch_B) : (left ? ovrButton_Y : ovrButton_B);
        }
        if (name == "stick") {
            return touch ? (left ? ovrTouch_LThumb : ovrTouch_RThumb) : (left ? ovrButton_LThumb : ovrButton_RThumb);
        }
        if (!touch && name == "menu") {
            return ovrButton_Enter;
        }
        if (!touch && name == "system") {
            return ovrButton_Home;
        }
        if (touch && name == "thumbrest") {
            return left ? ovrTouch_LThumbRest : ovrTouch_RThumbRest;
        }
        if (touch && name == "trigger") {
            return left ? ovrTouch_LIndexTrigger : ovrTouch_RIndexTrigger;
        }
        return 0;
    }

    // --------------------------------------------------------------- commands

    std::string Ok(const std::string& members = {}) {
        return members.empty() ? "{\"ok\":true}" : "{\"ok\":true," + members + "}";
    }

    std::string Fail(const std::string& error) {
        return "{\"ok\":false,\"error\":\"" + JsonEscape(error) + "\"}";
    }

    std::string NeedsSession() {
        return Fail("no headset session yet (the application has not created its swapchains)");
    }

    std::string Status() {
        std::string driver;
        WithLiveDriver([&](IDriverControl* live) {
            if (live) {
                driver = live->StatusJson();
            }
        });
        const double now = Now();
        return Ok(std::string("\"session\":") + (driver.empty() ? "false" : "true") + (driver.empty() ? "" : "," + driver) +
                  "," + State::Get().DescribeJson(now));
    }

    std::string Head(const cJSON* request) {
        const double now = Now();
        // Start from where the head is now.
        PoseYpr pose;
        {
            const ovrPoseStatef current = State::Get().HeadPose(now, true);
            pose.pos = OVR::Vector3f(current.ThePose.Position);
            pose.ypr = QuatToYpr(OVR::Quatf(current.ThePose.Orientation));
        }
        if (auto pos = Vec3(request, "pos")) {
            pose.pos = *pos;
        }
        if (auto ypr = Vec3(request, "ypr")) {
            pose.ypr = *ypr;
        }
        State::Get().SetHead(pose);
        return Ok("\"pos\":" + JsonVec3(pose.pos) + ",\"ypr\":" + JsonVec3(pose.ypr));
    }

    std::string MotionCommand(const cJSON* request) {
        Motion motion;
        const std::string type = String(request, "type").value_or("still");
        if (type == "still") {
            motion.type = Motion::Type::Still;
        } else if (type == "turn") {
            motion.type = Motion::Type::Turn;
        } else if (type == "sine") {
            motion.type = Motion::Type::Sine;
        } else if (type == "keyframes") {
            motion.type = Motion::Type::Keyframes;
        } else {
            return Fail("type must be still, turn, sine or keyframes");
        }
        if (motion.type == Motion::Type::Turn || motion.type == Motion::Type::Sine) {
            motion.axis = AxisOf(String(request, "axis").value_or("yaw"));
            if (motion.axis < 0) {
                return Fail("axis must be yaw, pitch, roll, x, y or z");
            }
        }
        motion.speed = Number(request, "speed").value_or(0);
        motion.amplitude = Number(request, "amplitude").value_or(0);
        motion.period = std::max(0.01, Number(request, "period_s").value_or(Number(request, "period").value_or(1)));
        motion.phase = Number(request, "phase").value_or(0);
        motion.loop = Bool(request, "loop").value_or(false);
        motion.duration = Number(request, "duration_s").value_or(Number(request, "duration").value_or(-1));
        if (motion.type == Motion::Type::Sine && motion.duration > 0) {
            // Whole periods, so that the head comes back to where it started.
            motion.duration = std::ceil(motion.duration / motion.period - 1e-6) * motion.period;
        }
        if (motion.type == Motion::Type::Keyframes) {
            const cJSON* keys = cJSON_GetObjectItemCaseSensitive(request, "keys");
            if (!cJSON_IsArray(keys) || cJSON_GetArraySize(keys) < 1) {
                return Fail("keyframes need keys:[{t_s, pos, ypr}, ...]");
            }
            const cJSON* key = nullptr;
            cJSON_ArrayForEach(key, keys) {
                Motion::Key k;
                k.t = Number(key, "t_s").value_or(Number(key, "t").value_or(0));
                k.pos = Vec3(key, "pos").value_or(OVR::Vector3f(0, 0, 0));
                k.ypr = Vec3(key, "ypr").value_or(OVR::Vector3f(0, 0, 0));
                motion.keys.push_back(k);
            }
            std::stable_sort(motion.keys.begin(), motion.keys.end(), [](const Motion::Key& a, const Motion::Key& b) { return a.t < b.t; });
            if (motion.duration < 0 && !motion.loop) {
                motion.duration = motion.keys.back().t;
            }
        }
        const double now = Now();
        motion.start = now + Number(request, "delay_ms").value_or(0) / 1000.0;

        const std::string target = String(request, "target").value_or("head");
        if (target == "head") {
            State::Get().SetHeadMotion(motion);
        } else if (target == "left" || target == "right") {
            State::Get().SetControllerMotion(target == "left" ? Left : Right, motion);
        } else {
            return Fail("target must be head, left or right");
        }
        char members[160];
        sprintf_s(members,
                  "\"starts_in_s\":%.4f,\"ends_in_s\":%s",
                  motion.start - now,
                  std::isinf(motion.End()) ? "null" : std::to_string(motion.End() - now).c_str());
        return Ok(std::string(members) + ",\"motion\":\"" + JsonEscape(motion.Describe()) + "\"");
    }

    std::string Controller(const cJSON* request) {
        std::string error;
        const int hand = HandOf(request, error);
        if (hand < 0) {
            return Fail(error);
        }
        State::Get().SetController(hand, [&](ControllerState& c) {
            if (auto v = Bool(request, "connected")) {
                c.connected = *v;
            }
            if (auto v = Bool(request, "follow_head")) {
                c.followHead = *v;
            }
            if (auto v = Vec3(request, "pos")) {
                c.pose.base.pos = *v;
                c.pose.motion = Motion{};
            }
            if (auto v = Vec3(request, "ypr")) {
                c.pose.base.ypr = *v;
                c.pose.motion = Motion{};
            }
            if (auto v = Number(request, "trigger")) {
                c.trigger = (float)std::clamp(*v, 0.0, 1.0);
            }
            if (auto v = Number(request, "grip")) {
                c.grip = (float)std::clamp(*v, 0.0, 1.0);
            }
            const cJSON* stick = cJSON_GetObjectItemCaseSensitive(request, "stick");
            if (cJSON_IsArray(stick) && cJSON_GetArraySize(stick) == 2) {
                c.stick.x = (float)std::clamp(cJSON_GetArrayItem(stick, 0)->valuedouble, -1.0, 1.0);
                c.stick.y = (float)std::clamp(cJSON_GetArrayItem(stick, 1)->valuedouble, -1.0, 1.0);
            }
            for (const bool touch : {false, true}) {
                const cJSON* map = cJSON_GetObjectItemCaseSensitive(request, touch ? "touches" : "buttons");
                if (!cJSON_IsObject(map)) {
                    continue;
                }
                const cJSON* item = nullptr;
                cJSON_ArrayForEach(item, map) {
                    const unsigned bit = ButtonBit(hand, item->string ? item->string : "", touch);
                    if (!bit) {
                        error = std::string("unknown ") + (touch ? "touch " : "button ") + (item->string ? item->string : "");
                        continue;
                    }
                    const bool on = cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valuedouble != 0);
                    unsigned& bits = touch ? c.touches : c.buttons;
                    bits = on ? (bits | bit) : (bits & ~bit);
                }
            }
        });
        return error.empty() ? Ok() : Fail(error);
    }

    std::string PressCommand(const cJSON* request) {
        std::string error;
        const int hand = HandOf(request, error);
        if (hand < 0) {
            return Fail(error);
        }
        const std::string button = String(request, "button").value_or("");
        static const char* known[] = {"a", "b", "x", "y", "menu", "system", "stick", "trigger", "grip"};
        if (std::find_if(std::begin(known), std::end(known), [&](const char* k) { return button == k; }) == std::end(known)) {
            return Fail("button must be a, b, x, y, menu, system, stick, trigger or grip");
        }
        Press press;
        press.hand = hand;
        press.control = button;
        press.value = (float)std::clamp(Number(request, "value").value_or(1.0), 0.0, 1.0);
        const double now = Now();
        press.start = now + Number(request, "delay_ms").value_or(0) / 1000.0;
        press.end = press.start + std::max(1.0, Number(request, "ms").value_or(100)) / 1000.0;
        State::Get().AddPress(press);
        char members[96];
        sprintf_s(members, "\"starts_in_s\":%.4f,\"ends_in_s\":%.4f", press.start - now, press.end - now);
        return Ok(members);
    }

    std::string Capture(const cJSON* request) {
        CaptureRequest capture;
        capture.frames = std::clamp((int)Number(request, "frames").value_or(1), 1, 100000);
        capture.every = std::clamp((int)Number(request, "every").value_or(1), 1, 100000);
        capture.startAt = Now() + Number(request, "delay_ms").value_or(0) / 1000.0;
        const std::string eyes = String(request, "eyes").value_or("both");
        capture.eyes = eyes == "left" ? 1 : eyes == "right" ? 2 : 3;
        capture.scale = (float)std::clamp(Number(request, "scale").value_or(1.0), 0.05, 1.0);
        const cJSON* crop = cJSON_GetObjectItemCaseSensitive(request, "crop");
        if (cJSON_IsArray(crop) && cJSON_GetArraySize(crop) == 4) {
            for (int i = 0; i < 4; i++) {
                capture.crop[i] = (int)cJSON_GetArrayItem(crop, i)->valuedouble;
            }
        }
        capture.png = String(request, "format").value_or("png") != "raw";
        capture.name = String(request, "name").value_or("cap");
        for (char& ch : capture.name) {
            if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_') {
                ch = '_';
            }
        }
        if (auto dir = String(request, "dir")) {
            capture.dir = std::filesystem::u8path(*dir);
        } else {
            capture.dir = GetConfig().logDir / L"captures";
        }
        std::string result;
        WithLiveDriver([&](IDriverControl* live) {
            if (!live) {
                result = NeedsSession();
                return;
            }
            std::string error;
            const int id = live->StartCapture(capture, error);
            result = id ? Ok("\"capture_id\":" + std::to_string(id) + ",\"dir\":\"" + JsonEscape(capture.dir.u8string()) + "\"")
                        : Fail(error);
        });
        return result;
    }

    std::string CaptureStatusCommand() {
        std::string result;
        WithLiveDriver([&](IDriverControl* live) {
            if (!live) {
                result = NeedsSession();
                return;
            }
            const CaptureStatus s = live->GetCaptureStatus();
            char members[256];
            sprintf_s(members,
                      "\"capture_id\":%d,\"active\":%s,\"captured\":%d,\"written\":%d,\"dropped\":%d,\"pending\":%d,",
                      s.id,
                      s.active ? "true" : "false",
                      s.captured,
                      s.written,
                      s.dropped,
                      s.pending);
            result = Ok(std::string(members) + "\"done\":" + (!s.active && s.pending == 0 ? "true" : "false") +
                        ",\"dir\":\"" + JsonEscape(s.dir) + "\",\"last_error\":\"" + JsonEscape(s.lastError) + "\"");
        });
        return result;
    }

    std::string Handle(const std::string& line) {
        cJSON* request = cJSON_Parse(line.c_str());
        if (!request || !cJSON_IsObject(request)) {
            cJSON_Delete(request);
            return Fail("not a JSON object");
        }
        std::string response;
        const std::string cmd = String(request, "cmd").value_or("");
        try {
            if (cmd == "ping") {
                char members[128];
                sprintf_s(members, "\"simxr\":\"1.0\",\"pid\":%lu,\"uptime_s\":%.3f", GetCurrentProcessId(), Now() - StartTime());
                response = Ok(members);
            } else if (cmd == "help") {
                response = Ok("\"help\":\"" + JsonEscape(k_Help) + "\"");
            } else if (cmd == "status") {
                response = Status();
            } else if (cmd == "stats") {
                const bool reset = Bool(request, "reset").value_or(false);
                WithLiveDriver([&](IDriverControl* live) { response = live ? Ok(live->StatsJson(reset)) : NeedsSession(); });
            } else if (cmd == "head") {
                response = Head(request);
            } else if (cmd == "motion") {
                response = MotionCommand(request);
            } else if (cmd == "controller") {
                response = Controller(request);
            } else if (cmd == "press") {
                response = PressCommand(request);
            } else if (cmd == "capture") {
                response = Capture(request);
            } else if (cmd == "capture_status") {
                response = CaptureStatusCommand();
            } else if (cmd == "mark") {
                const std::string label = String(request, "label").value_or("");
                WithLiveDriver([&](IDriverControl* live) {
                    if (live) {
                        live->Mark(label);
                    }
                });
                Log("Mark: %s\n", label.c_str());
                response = Ok();
            } else if (cmd == "session") {
                State::Get().SetFlags([&](SessionFlags& flags) {
                    if (auto v = Bool(request, "hmd_mounted")) {
                        flags.hmdMounted = *v;
                    }
                    if (auto v = Bool(request, "should_quit")) {
                        flags.shouldQuit = *v;
                    }
                    if (auto v = Bool(request, "visible")) {
                        flags.visible = *v;
                    }
                    if (auto v = Bool(request, "input_focus")) {
                        flags.inputFocus = *v;
                    }
                    if (auto v = Bool(request, "should_recenter")) {
                        flags.shouldRecenter = *v;
                    }
                });
                Log("Session flags changed: %s\n", line.c_str());
                response = Ok();
            } else {
                response = Fail("unknown cmd \"" + cmd + "\"; try {\"cmd\":\"help\"}");
            }
        } catch (const std::exception& e) {
            response = Fail(std::string("exception: ") + e.what());
        }
        // Echo the request id.
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(request, "id");
        if (id && response.size() > 1) {
            char* printed = cJSON_PrintUnformatted(id);
            if (printed) {
                response.insert(1, std::string("\"id\":") + printed + ",");
                cJSON_free(printed);
            }
        }
        cJSON_Delete(request);
        return response;
    }

    // ----------------------------------------------------------------- server

    bool WaitReadable(SOCKET s, int ms) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval timeout{ms / 1000, (ms % 1000) * 1000};
        return select(0, &set, nullptr, nullptr, &timeout) > 0;
    }

    void ClientThread(SOCKET client) {
        SetThreadDescription(GetCurrentThread(), L"SimXR control client");
        std::string buffer;
        char chunk[4096];
        while (!g_stopServer) {
            if (!WaitReadable(client, 200)) {
                continue;
            }
            const int received = recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                break;
            }
            buffer.append(chunk, received);
            if (buffer.size() > (1 << 20)) {
                break; // no sane request is that long
            }
            size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) {
                    continue;
                }
                const std::string response = Handle(line) + "\n";
                size_t sent = 0;
                while (sent < response.size()) {
                    const int n = send(client, response.data() + sent, (int)(response.size() - sent), 0);
                    if (n <= 0) {
                        break;
                    }
                    sent += n;
                }
            }
        }
        closesocket(client);
        g_clients--;
    }

    void ServerThread(int port) {
        SetThreadDescription(GetCurrentThread(), L"SimXR control port");
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            Log("Control port: WSAStartup failed\n");
            return;
        }
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons((u_short)port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener == INVALID_SOCKET || bind(listener, (sockaddr*)&address, sizeof(address)) != 0 ||
            listen(listener, 4) != 0) {
            Log("Control port: cannot listen on 127.0.0.1:%d (error %d); no remote control in this process\n",
                port,
                WSAGetLastError());
            if (listener != INVALID_SOCKET) {
                closesocket(listener);
            }
            WSACleanup();
            return;
        }
        Log("Control port: listening on 127.0.0.1:%d (pid %lu)\n", port, GetCurrentProcessId());
        {
            std::ofstream info(GetConfig().logDir / L"simxr-control.json", std::ios_base::trunc);
            info << "{\"host\":\"127.0.0.1\",\"port\":" << port << ",\"pid\":" << GetCurrentProcessId() << "}\n";
        }

        std::vector<std::thread> clients;
        while (!g_stopServer) {
            if (!WaitReadable(listener, 200)) {
                continue;
            }
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                continue;
            }
            BOOL noDelay = TRUE;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
            g_clients++;
            clients.emplace_back(ClientThread, client);
        }
        closesocket(listener);
        for (auto& client : clients) {
            if (client.joinable()) {
                client.join();
            }
        }
        WSACleanup();
        Log("Control port closed\n");
    }

} // namespace

namespace simxr {

    void EnsureControlServer() {
        const int port = GetConfig().port;
        if (port <= 0) {
            return;
        }
        std::unique_lock lock(g_serverMutex);
        if (g_serverThread.joinable()) {
            return;
        }
        g_stopServer = false;
        g_serverThread = std::thread(ServerThread, port);
    }

    void StopControlServer() {
        std::unique_lock lock(g_serverMutex);
        if (g_serverThread.joinable()) {
            g_stopServer = true;
            g_serverThread.join();
            g_serverThread = {};
        }
    }

} // namespace simxr
