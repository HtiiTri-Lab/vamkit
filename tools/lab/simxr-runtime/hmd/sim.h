// SimXR simulated headset: the state shared by the OVR entry points (api.cpp),
// the display model (driver.cpp) and the control port (control.cpp).
//
// One state per process, not per ovr_Create(): VDXR may destroy and re-create its
// OVR session, and the scripted head/controller motion must survive that.
//
// Time base: OVR seconds = QueryPerformanceCounter seconds (ovr_GetTimeInSeconds).
// Coordinates: OVR tracking space, right-handed, +Y up, -Z forward, metres.
// Floor-level origin (Virtual Desktop's), so the head sits at Config::headHeight.
// Angles in the control protocol are degrees, right-handed about each axis:
// yaw about +Y (positive = turn left), pitch about +X (positive = look up), roll
// about +Z (positive = tilt the head to the left); q = yaw * pitch * roll.
#pragma once

#include "pch.h"

namespace simxr {

    // ---------------------------------------------------------------- config
    struct Config {
        // Virtual Desktop's Quest 3 as it reports itself to VDXR on this PC
        // (VDXR log 2026-10-15: 3072x3264, 120 Hz; FOV from OpenComposite's
        // GetProjectionRaw; IPD from the native VD run 2026-10-12: 57.7 mm).
        float displayRate = 120.f;      // SIMXR_REFRESH (Hz)
        int width = 3072;               // SIMXR_WIDTH (per eye)
        int height = 3264;              // SIMXR_HEIGHT
        float ipd = 0.0577f;            // SIMXR_IPD_MM
        float headHeight = 1.20f;       // SIMXR_HEAD_HEIGHT_CM (floor origin)
        float latchMs = 1.0f;           // SIMXR_LATCH_US: a frame must be complete this long before vsync
        int maxQueuedFrames = 1;        // SIMXR_MAX_QUEUED_FRAMES: submitted frames whose GPU work may still
                                        // run when the next frame begins (0 = no limit)
        int port = 37601;               // SIMXR_PORT (0 = no control port)
        bool vsyncLog = true;           // SIMXR_VSYNC_LOG=0 turns the per-vsync CSV off
        size_t captureMemoryMB = 1536;  // SIMXR_CAPTURE_MB: frames waiting to be written
        std::filesystem::path logDir;   // SIMXR_LOG_DIR (else %LOCALAPPDATA%\SimXR)
        ovrFovPort fov[2]{};
    };
    const Config& GetConfig();

    // ------------------------------------------------------------------- log
    void Log(const char* fmt, ...);
    double Now(); // OVR seconds
    double StartTime(); // OVR seconds when this DLL loaded (CSV times are relative to it)

    // ------------------------------------------------------------------ pose
    // A pose as the protocol speaks it.
    struct PoseYpr {
        OVR::Vector3f pos{0, 0, 0};
        OVR::Vector3f ypr{0, 0, 0}; // degrees
    };
    OVR::Quatf YprToQuat(const OVR::Vector3f& yprDegrees);
    OVR::Vector3f QuatToYpr(const OVR::Quatf& q); // degrees
    OVR::Posef ToPosef(const PoseYpr& pose);

    // An offset added to a base pose over time (head or controller).
    struct Motion {
        enum class Type { Still, Turn, Sine, Keyframes };
        struct Key {
            double t{0};
            OVR::Vector3f pos{0, 0, 0};
            OVR::Vector3f ypr{0, 0, 0};
        };
        Type type{Type::Still};
        int axis{0};          // 0..2 = yaw/pitch/roll (degrees), 3..5 = x/y/z (metres)
        double start{0};      // OVR seconds
        double duration{-1};  // seconds; < 0 = forever
        double speed{0};      // Turn: per second
        double amplitude{0};  // Sine
        double period{1};     // Sine
        double phase{0};      // Sine, degrees
        bool loop{false};     // Keyframes
        std::vector<Key> keys;

        // Offset at time t: pos (metres) and ypr (degrees).
        void Evaluate(double t, OVR::Vector3f& pos, OVR::Vector3f& ypr) const;
        double End() const {
            return duration < 0 ? INFINITY : start + duration;
        }
        std::string Describe() const;
    };

    // A tracked thing: base pose plus a running motion.
    struct Tracked {
        PoseYpr base;
        Motion motion;
        PoseYpr At(double t) const;
        // Starts a new motion from wherever the old one has got to.
        void Restart(const Motion& next, double now);
    };

    // ----------------------------------------------------------- controllers
    enum Hand { Left = 0, Right = 1 };

    struct Press {
        int hand{0};
        std::string control; // a, b, x, y, menu, system, stick, trigger, grip
        double start{0}, end{0};
        float value{1.f};
    };

    struct ControllerState {
        bool connected{true};
        bool followHead{true};  // pose relative to the head (else tracking space)
        Tracked pose;
        unsigned int buttons{0}; // ovrButton_* held
        unsigned int touches{0}; // ovrTouch_* held
        float trigger{0}, grip{0};
        OVR::Vector2f stick{0, 0};
    };

    // ----------------------------------------------------------------- state
    struct SessionFlags {
        bool hmdMounted{true};
        bool shouldQuit{false};
        bool visible{true};
        bool inputFocus{true};
    };

    class State {
      public:
        static State& Get();

        // Head pose at time t (tracking space, floor origin unless eyeLevel).
        ovrPoseStatef HeadPose(double t, bool floorOrigin) const;
        bool HasController(int hand) const;
        ovrPoseStatef ControllerPose(int hand, double t, bool floorOrigin) const;
        ovrInputState Input(double t) const;
        SessionFlags Flags() const;

        // Control port.
        void SetHead(const PoseYpr& pose);
        void SetHeadMotion(const Motion& motion);
        void SetController(int hand, const std::function<void(ControllerState&)>& edit);
        void SetControllerMotion(int hand, const Motion& motion);
        void AddPress(const Press& press);
        void SetFlags(const std::function<void(SessionFlags&)>& edit);
        std::string DescribeJson(double now) const; // head/controllers/flags as JSON members

      private:
        State();
        PoseYpr HeadAt(double t) const;
        ovrPoseStatef Sample(const std::function<OVR::Posef(double)>& poseAt, double t) const;

        mutable std::shared_mutex m_mutex;
        Tracked m_head;
        ControllerState m_controllers[2];
        std::vector<Press> m_presses;
        SessionFlags m_flags;
    };

    // --------------------------------------------------------- display model
    // Filled by the driver; read by the control port.
    struct Stats {
        uint64_t vsyncs{0}, newFrames{0}, repeats{0}, dropped{0}, gpuLate{0};
        uint64_t submitted{0};
        uint64_t longestRepeatStreak{0};
        uint64_t simulatorLate{0}; // vsyncs the simulator itself woke up late for (> 0.5 ms)
        uint64_t queueWaits{0};    // frame begins held back by the GPU queue limit
        double since{0};
        std::vector<float> submitIntervalsMs; // between submissions
        std::vector<float> ageAtDisplayMs;    // submission -> first vsync shown
        std::vector<float> poseErrorDeg;      // rendered vs true head orientation at each vsync
        std::vector<float> queueWaitMs;       // how long each held-back frame begin waited
    };

    // ------------------------------------------------------------- capture
    struct CaptureRequest {
        int id{0};
        std::filesystem::path dir;
        std::string name{"cap"};
        int frames{1};
        int every{1};
        double startAt{0};        // OVR seconds
        int eyes{3};              // bit 0 left, bit 1 right
        float scale{1.f};
        int crop[4]{0, 0, 0, 0}; // x, y, w, h in eye pixels; w == 0 = whole eye
        bool png{true};
    };
    struct CaptureStatus {
        int id{0};
        bool active{false};
        int captured{0}, written{0}, dropped{0}, pending{0};
        std::string dir, lastError;
    };

    // The live driver (the newest ovr_Create) for the control port.
    struct IDriverControl {
        virtual ~IDriverControl() = default;
        virtual std::string StatusJson() const = 0;
        virtual std::string StatsJson(bool reset) = 0;
        virtual int StartCapture(const CaptureRequest& request, std::string& error) = 0;
        virtual CaptureStatus GetCaptureStatus() const = 0;
        virtual void Mark(const std::string& label) = 0;
    };
    void SetLiveDriver(IDriverControl* driver);
    void ClearLiveDriver(IDriverControl* driver);

    // Starts the control port once per process (on the first swapchain, so that
    // probes which only look for a headset never open it); ovr_Shutdown stops it.
    void EnsureControlServer();
    void StopControlServer();

    // Small JSON helpers for the writers.
    std::string JsonEscape(const std::string& text);
    std::string JsonVec3(const OVR::Vector3f& v);

} // namespace simxr
