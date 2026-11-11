// VaM test agent (BepInEx 5, Unity 2018.1, C# 3): lets automated VR tests on
// SimXR (the simulated headset, tools\simxr) see and drive VaM itself.
//
// Dormant - no thread, no port, no file - unless VAM_TEST_AGENT=1, which only
// Launch.ps1 -XrRuntime SimXR sets. Then it listens on 127.0.0.1 only
// (VAM_TEST_AGENT_PORT, default 37602) for newline-delimited JSON requests, one
// JSON response per line, and runs every request on Unity's main thread in
// Update(). It writes vam-agent.json (port, pid) into VAM_TEST_AGENT_LOG.
//   {"cmd":"status"}                  frame rate, frame times, loading, main HUD, scene, XR, head pose
//   {"cmd":"load_scene","path":"Saves/scene/x.json"}   SuperController.Load (poll status.loading);
//                                     also "Creator.Package.1:/Saves/scene/x.json" inside a .var
//   {"cmd":"hud","visible":true}      main menu shown (ShowMainHUDAuto) or hidden; {"toggle":true};
//                                     {"close_all":true} also closes the world UI (the scene browser
//                                     VaM opens at start). status.menus_open is what the DLSS bridge
//                                     and the VR menu test: MainHUDVisible || worldUIActivated
//   {"cmd":"freeze","on":true}        animation freeze
//   {"cmd":"wait_frames","n":10}      answers after n more frames
//   {"cmd":"frame_log","on":true}     one CSV row per frame (vam-frames.csv): frame, QPC time, delta,
//                                     loading, menus open, gc (Mono collections so far: a long frame
//                                     where it steps was a garbage collection)
//   {"cmd":"get","type":"SuperController","member":"activeUI"}   reflection on a static or singleton
//   {"cmd":"set","type":...,"member":...,"value":...}  {"cmd":"invoke","type":...,"method":...,"args":[...]}
//   {"cmd":"click","text":"Part1"}    presses a scene's own button (onClick of an active UI Button
//                                     whose text or object name is that); {"list":true} names them
//   {"cmd":"log","message":"..."}     a line in BepInEx's log
//   {"cmd":"quit"}
// Nothing here runs on a timer or scans the scene: work happens only when a
// request arrives (FindObjectOfType only for an explicit reflection request on
// a type without a singleton).
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Text;
using System.Threading;
using BepInEx;
using SimpleJSON;
using UnityEngine;
using UnityEngine.XR;

[BepInPlugin("local.vam.vrframegen.testagent", "VaM Test Agent", "1.2.0")]
public sealed class VaMTestAgent : BaseUnityPlugin
{
    private const int DefaultPort = 37602;
    private const int FrameWindow = 240;

    private sealed class Pending
    {
        public JSONNode request;
        public string response;
        public int waitUntilFrame = -1;
        public readonly ManualResetEvent done = new ManualResetEvent(false);
    }

    private sealed class JObj
    {
        public readonly List<KeyValuePair<string, object>> items = new List<KeyValuePair<string, object>>();
        public JObj Add(string key, object value) { items.Add(new KeyValuePair<string, object>(key, value)); return this; }
    }

    private readonly Queue<Pending> queue = new Queue<Pending>();
    private readonly List<Pending> deferred = new List<Pending>();
    private readonly float[] frameMs = new float[FrameWindow];
    private readonly double[] frameStamp = new double[FrameWindow];
    private int frameCursor, frameFilled;
    private Thread server;
    private volatile bool stopping;
    private int port;
    private string logDir;
    private StreamWriter frameLog;
    private int frameLogRows;

    private static double Qpc() { return Stopwatch.GetTimestamp() / (double)Stopwatch.Frequency; }

    private void Awake()
    {
        if (Environment.GetEnvironmentVariable("VAM_TEST_AGENT") != "1")
        {
            enabled = false;
            return;
        }
        port = DefaultPort;
        int parsed;
        string portText = Environment.GetEnvironmentVariable("VAM_TEST_AGENT_PORT");
        if (!string.IsNullOrEmpty(portText) && int.TryParse(portText, out parsed) && parsed > 0 && parsed < 65536) port = parsed;
        logDir = Environment.GetEnvironmentVariable("VAM_TEST_AGENT_LOG");
        if (string.IsNullOrEmpty(logDir)) logDir = Path.Combine(Paths.BepInExRootPath, "LogOutput");
        try { Directory.CreateDirectory(logDir); } catch (Exception) { }
        server = new Thread(ServerThread);
        server.IsBackground = true;
        server.Name = "VaM test agent";
        server.Start();
    }

    private void OnDestroy()
    {
        stopping = true;
        if (frameLog != null) { frameLog.Close(); frameLog = null; }
    }

    // ------------------------------------------------------------------ server

    private void ServerThread()
    {
        TcpListener listener = null;
        try
        {
            listener = new TcpListener(IPAddress.Loopback, port);
            listener.Start();
            Logger.LogInfo("VaM test agent listening on 127.0.0.1:" + port);
            try
            {
                File.WriteAllText(Path.Combine(logDir, "vam-agent.json"),
                    "{\"host\":\"127.0.0.1\",\"port\":" + port + ",\"pid\":" + Process.GetCurrentProcess().Id + "}\n");
            }
            catch (Exception e) { Logger.LogWarning("vam-agent.json: " + e.Message); }
            while (!stopping)
            {
                if (!listener.Pending()) { Thread.Sleep(50); continue; }
                TcpClient client = listener.AcceptTcpClient();
                Thread thread = new Thread(ClientThread);
                thread.IsBackground = true;
                thread.Name = "VaM test agent client";
                thread.Start(client);
            }
        }
        catch (Exception e)
        {
            Logger.LogError("VaM test agent server stopped: " + e.Message);
        }
        finally
        {
            if (listener != null) listener.Stop();
        }
    }

    private void ClientThread(object state)
    {
        TcpClient client = (TcpClient)state;
        try
        {
            client.NoDelay = true;
            NetworkStream stream = client.GetStream();
            StreamReader reader = new StreamReader(stream, new UTF8Encoding(false));
            StreamWriter writer = new StreamWriter(stream, new UTF8Encoding(false));
            string line;
            while (!stopping && (line = reader.ReadLine()) != null)
            {
                if (line.Trim().Length == 0) continue;
                string response;
                JSONNode request = null;
                try { request = JSON.Parse(line); } catch (Exception) { }
                if (request == null || request.AsObject == null)
                {
                    response = Fail("not a JSON object");
                }
                else
                {
                    Pending pending = new Pending();
                    pending.request = request;
                    lock (queue) queue.Enqueue(pending);
                    // Requests run on the main thread; a loading scene can hold it for a while.
                    if (!pending.done.WaitOne(120000, false)) response = Fail("timed out waiting for VaM's main thread");
                    else response = pending.response;
                }
                writer.Write(response);
                writer.Write('\n');
                writer.Flush();
            }
        }
        catch (Exception) { }
        finally { client.Close(); }
    }

    // ------------------------------------------------------------- main thread

    private void Update()
    {
        float dt = Time.unscaledDeltaTime * 1000f;
        double now = Qpc();
        frameMs[frameCursor] = dt;
        frameStamp[frameCursor] = now;
        frameCursor = (frameCursor + 1) % FrameWindow;
        if (frameFilled < FrameWindow) frameFilled++;
        if (frameLog != null)
        {
            frameLog.Write(Time.frameCount.ToString(CultureInfo.InvariantCulture));
            frameLog.Write(',');
            frameLog.Write(now.ToString("F6", CultureInfo.InvariantCulture));
            frameLog.Write(',');
            frameLog.Write(dt.ToString("F3", CultureInfo.InvariantCulture));
            frameLog.Write(',');
            frameLog.Write(IsLoading() ? '1' : '0');
            frameLog.Write(',');
            frameLog.Write(MenusOpen() ? '1' : '0');
            frameLog.Write(',');
            frameLog.Write(GC.CollectionCount(0).ToString(CultureInfo.InvariantCulture));
            frameLog.Write('\n');
            if (++frameLogRows % 120 == 0) frameLog.Flush();
        }

        for (int i = deferred.Count - 1; i >= 0; i--)
        {
            if (Time.frameCount >= deferred[i].waitUntilFrame)
            {
                // the request's id goes into a deferred answer as into an immediate one: the client matches answers by it
                string answer = Ok(new JObj().Add("frame", Time.frameCount));
                string answerId = deferred[i].request["id"] != null ? deferred[i].request["id"].ToString() : null;
                if (answerId != null && answer.Length > 1) answer = "{\"id\":" + answerId + "," + answer.Substring(1);
                deferred[i].response = answer;
                deferred[i].done.Set();
                deferred.RemoveAt(i);
            }
        }
        while (true)
        {
            Pending pending;
            lock (queue)
            {
                if (queue.Count == 0) break;
                pending = queue.Dequeue();
            }
            string response;
            try
            {
                response = Handle(pending);
            }
            catch (Exception e)
            {
                Exception inner = e is TargetInvocationException && e.InnerException != null ? e.InnerException : e;
                response = Fail(inner.GetType().Name + ": " + inner.Message);
            }
            if (response == null) { deferred.Add(pending); continue; }
            string id = pending.request["id"] != null ? pending.request["id"].ToString() : null;
            if (id != null && response.Length > 1) response = "{\"id\":" + id + "," + response.Substring(1);
            pending.response = response;
            pending.done.Set();
        }
    }

    private string Handle(Pending pending)
    {
        JSONNode r = pending.request;
        string cmd = r["cmd"] != null ? r["cmd"].Value : "";
        switch (cmd)
        {
            case "ping":
                return Ok(new JObj().Add("agent", "1.1").Add("frame", Time.frameCount).Add("qpc_s", Qpc())
                    .Add("unity", Application.unityVersion).Add("pid", Process.GetCurrentProcess().Id));
            case "status":
                return Ok(Status());
            case "load_scene":
                {
                    string path = r["path"] != null ? r["path"].Value : "";
                    if (path.Length == 0) return Fail("load_scene needs path");
                    // A scene inside a package has no file of its own; VaM resolves "Package:/path".
                    if (path.IndexOf(":/", StringComparison.Ordinal) < 0 &&
                        !File.Exists(path) && !File.Exists(Path.Combine(VamRoot(), path))) return Fail("no file " + path);
                    SuperController.singleton.Load(path);
                    return Ok(new JObj().Add("loading", IsLoading()));
                }
            case "hud":
                {
                    SuperController sc = SuperController.singleton;
                    if (r["close_all"] != null && r["close_all"].AsBool)
                    {
                        sc.CloseAllWorldUIPanels();
                        sc.DeactivateWorldUI();
                        sc.HideMainHUD();
                    }
                    else if (r["toggle"] != null && r["toggle"].AsBool) sc.ToggleMainHUDAuto();
                    else if (r["visible"] != null) { if (r["visible"].AsBool) sc.ShowMainHUDAuto(); else sc.HideMainHUD(); }
                    return Ok(new JObj().Add("hud_visible", HudVisible()).Add("menus_open", MenusOpen()));
                }
            case "freeze":
                SuperController.singleton.SetFreezeAnimation(r["on"] == null || r["on"].AsBool);
                return Ok(new JObj().Add("freeze", SuperController.singleton.freezeAnimation));
            case "wait_frames":
                pending.waitUntilFrame = Time.frameCount + Math.Max(1, r["n"] != null ? r["n"].AsInt : 1);
                return null;
            case "frame_log":
                return FrameLog(r);
            case "get":
                {
                    object target;
                    MemberInfo member;
                    Resolve(r, "member", out target, out member);
                    return Ok(new JObj().Add("value", Describe(GetValue(target, member))));
                }
            case "set":
                {
                    object target;
                    MemberInfo member;
                    Resolve(r, "member", out target, out member);
                    SetValue(target, member, r["value"]);
                    return Ok(new JObj().Add("value", Describe(GetValue(target, member))));
                }
            case "invoke":
                return Ok(new JObj().Add("result", Describe(Invoke(r))));
            case "log":
                Logger.LogInfo("[test] " + (r["message"] != null ? r["message"].Value : ""));
                return Ok(null);
            case "click":
                return Click(r);
            case "quit":
                Logger.LogInfo("VaM test agent: quit requested");
                Application.Quit();
                return Ok(null);
            default:
                return Fail("unknown cmd \"" + cmd + "\" (ping, status, load_scene, hud, freeze, wait_frames, frame_log, get, set, invoke, click, log, quit)");
        }
    }

    // A scene's own button (its menu, a UIButton atom), found by a label of its text or its
    // object name among the active buttons, pressed by invoking onClick. {"list":true} names them.
    // Labels: any child component with a string "text" property (UI Text, TextMeshPro, ...).
    // {"index":n} presses the n-th of the list instead (order as listed: by height, then left to right).
    private static string Click(JSONNode r)
    {
        string text = r["text"] != null ? r["text"].Value.Trim() : "";
        bool list = r["list"] != null && r["list"].AsBool;
        int index = r["index"] != null ? r["index"].AsInt : -1;
        List<UnityEngine.UI.Button> buttons = new List<UnityEngine.UI.Button>(UnityEngine.Object.FindObjectsOfType<UnityEngine.UI.Button>());
        buttons.Sort(delegate(UnityEngine.UI.Button a, UnityEngine.UI.Button b)
        {
            Vector3 pa = a.transform.position, pb = b.transform.position;
            if (Mathf.Abs(pa.y - pb.y) > 0.01f) return pb.y.CompareTo(pa.y);
            return pa.x.CompareTo(pb.x);
        });
        List<string> labels = new List<string>();
        for (int n = 0; n < buttons.Count; n++)
        {
            UnityEngine.UI.Button b = buttons[n];
            List<string> parts = new List<string>();
            bool match = string.Equals(b.gameObject.name, text, StringComparison.OrdinalIgnoreCase);
            foreach (Component c in b.GetComponentsInChildren<Component>(true))
            {
                if (c == null || c.GetType().Name.IndexOf("Text", StringComparison.Ordinal) < 0) continue;
                PropertyInfo p = c.GetType().GetProperty("text", BindingFlags.Instance | BindingFlags.Public);
                if (p == null || p.PropertyType != typeof(string)) continue;
                string s = p.GetValue(c, null) as string;
                s = s != null ? s.Trim() : "";
                if (s.Length == 0) continue;
                parts.Add(s);
                if (string.Equals(s, text, StringComparison.OrdinalIgnoreCase)) match = true;
            }
            Vector3 at = b.transform.position;
            Transform parent = b.transform.parent;
            string label = n + ": " + (parts.Count > 0 ? string.Join(" / ", parts.ToArray()) : b.gameObject.name) +
                " [" + (parent != null ? parent.name : "-") + " @ " + at.x.ToString("F2", CultureInfo.InvariantCulture) + "," +
                at.y.ToString("F2", CultureInfo.InvariantCulture) + "," + at.z.ToString("F2", CultureInfo.InvariantCulture) + "]";
            labels.Add(label);
            if (!list && b.interactable && (n == index || (index < 0 && text.Length > 0 && match)))
            {
                b.onClick.Invoke();
                return Ok(new JObj().Add("clicked", label).Add("object", b.gameObject.name));
            }
        }
        if (list) return Ok(new JObj().Add("buttons", string.Join(" | ", labels.ToArray())));
        return Fail("no active button \"" + text + "\" (index " + index + ") among " + buttons.Count + ": " + string.Join(" | ", labels.ToArray()));
    }

    private static string VamRoot() { return Path.GetFullPath(Path.Combine(Application.dataPath, "..")); }

    private static bool IsLoading()
    {
        SuperController sc = SuperController.singleton;
        return sc != null && sc.isLoading;
    }

    private static bool HudVisible()
    {
        SuperController sc = SuperController.singleton;
        return sc != null && sc.MainHUDVisible;
    }

    // The DLSS bridge's and the VR menu's test for "VaM's menus are open" (no SR, no frame generation).
    private static bool MenusOpen()
    {
        SuperController sc = SuperController.singleton;
        return sc != null && (sc.MainHUDVisible || sc.worldUIActivated);
    }

    private JObj Status()
    {
        JObj status = new JObj();
        status.Add("frame", Time.frameCount).Add("qpc_s", Qpc()).Add("time_s", Time.realtimeSinceStartup);
        // Frame times over the last FrameWindow frames, and the frame rate over the last second.
        List<float> times = new List<float>();
        double now = Qpc();
        int lastSecond = 0;
        for (int i = 0; i < frameFilled; i++)
        {
            times.Add(frameMs[i]);
            if (now - frameStamp[i] <= 1.0) lastSecond++;
        }
        times.Sort();
        status.Add("fps", lastSecond);
        if (times.Count > 0)
        {
            float median = times[times.Count / 2];
            int longFrames = 0;
            foreach (float t in times) if (t > 2f * median) longFrames++;
            status.Add("frame_ms", new JObj().Add("n", times.Count).Add("p50", median)
                .Add("p95", times[Math.Min(times.Count - 1, (int)(0.95 * (times.Count - 1) + 0.5))])
                .Add("p99", times[Math.Min(times.Count - 1, (int)(0.99 * (times.Count - 1) + 0.5))])
                .Add("max", times[times.Count - 1]).Add("long_frames", longFrames));
        }
        SuperController sc = SuperController.singleton;
        if (sc != null)
        {
            status.Add("loading", sc.isLoading).Add("hud_visible", sc.MainHUDVisible).Add("world_ui", sc.worldUIActivated)
                .Add("menus_open", MenusOpen()).Add("active_ui", sc.activeUI.ToString())
                .Add("freeze", sc.freezeAnimation);
            FieldInfo loaded = typeof(SuperController).GetField("loadedName", BindingFlags.Instance | BindingFlags.NonPublic);
            status.Add("scene", loaded != null ? loaded.GetValue(sc) as string : null);
            try { status.Add("atoms", sc.GetAtoms().Count); } catch (Exception) { }
        }
        else
        {
            status.Add("loading", true).Add("hud_visible", false);
        }
        JObj xr = new JObj();
        xr.Add("enabled", XRSettings.enabled).Add("device", XRSettings.loadedDeviceName).Add("model", XRDevice.model)
            .Add("refresh_hz", XRDevice.refreshRate).Add("eye_w", XRSettings.eyeTextureWidth).Add("eye_h", XRSettings.eyeTextureHeight)
            .Add("resolution_scale", XRSettings.eyeTextureResolutionScale).Add("viewport_scale", XRSettings.renderViewportScale);
        status.Add("xr", xr);
        // The head as Unity sees it (left-handed: euler y+ = turn right, x+ = look down).
        Vector3 headPos = InputTracking.GetLocalPosition(XRNode.Head);
        Vector3 headEuler = InputTracking.GetLocalRotation(XRNode.Head).eulerAngles;
        status.Add("head", new JObj().Add("pos", headPos).Add("euler", headEuler));
        try
        {
            List<object> plugins = new List<object>();
            foreach (string key in BepInEx.Bootstrap.Chainloader.PluginInfos.Keys) plugins.Add(key);
            status.Add("plugins", plugins);
        }
        catch (Exception) { }
        return status;
    }

    private string FrameLog(JSONNode r)
    {
        bool on = r["on"] == null || r["on"].AsBool;
        if (frameLog != null) { frameLog.Close(); frameLog = null; }
        string path = r["path"] != null ? r["path"].Value : Path.Combine(logDir, "vam-frames.csv");
        if (on)
        {
            bool exists = File.Exists(path);
            frameLog = new StreamWriter(path, true, new UTF8Encoding(false));
            if (!exists) frameLog.Write("frame,qpc_s,dt_ms,loading,menus_open,gc\n");
            frameLogRows = 0;
        }
        return Ok(new JObj().Add("on", on).Add("path", path));
    }

    // -------------------------------------------------------------- reflection

    private static Type FindType(string name)
    {
        foreach (Assembly asm in AppDomain.CurrentDomain.GetAssemblies())
        {
            Type type = null;
            try { type = asm.GetType(name, false); } catch (Exception) { }
            if (type != null) return type;
        }
        throw new ArgumentException("no type " + name);
    }

    private static object Instance(Type type)
    {
        const BindingFlags flags = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
        foreach (string name in new string[] { "singleton", "Instance", "instance", "_singleton", "_instance" })
        {
            PropertyInfo p = type.GetProperty(name, flags);
            if (p != null && type.IsAssignableFrom(p.PropertyType)) return p.GetValue(null, null);
            FieldInfo f = type.GetField(name, flags);
            if (f != null && type.IsAssignableFrom(f.FieldType)) return f.GetValue(null);
        }
        if (typeof(UnityEngine.Object).IsAssignableFrom(type)) return UnityEngine.Object.FindObjectOfType(type);
        return null;
    }

    private static MemberInfo FindMember(Type type, string name)
    {
        const BindingFlags flags = BindingFlags.Static | BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        PropertyInfo p = type.GetProperty(name, flags);
        if (p != null) return p;
        FieldInfo f = type.GetField(name, flags);
        if (f != null) return f;
        throw new ArgumentException(type.Name + " has no field or property " + name);
    }

    private static bool IsStatic(MemberInfo member)
    {
        PropertyInfo p = member as PropertyInfo;
        if (p != null) { MethodInfo g = p.GetGetMethod(true); return g != null && g.IsStatic; }
        FieldInfo f = member as FieldInfo;
        return f != null && f.IsStatic;
    }

    private static object GetValue(object target, MemberInfo member)
    {
        PropertyInfo p = member as PropertyInfo;
        if (p != null) return p.GetValue(target, null);
        return ((FieldInfo)member).GetValue(target);
    }

    // type + "a.b.c": every part but the last is followed; the last is returned as the member.
    private static void Resolve(JSONNode r, string key, out object target, out MemberInfo member)
    {
        Type type = FindType(r["type"] != null ? r["type"].Value : "");
        string path = r[key] != null ? r[key].Value : "";
        string[] parts = path.Split('.');
        member = FindMember(type, parts[0]);
        target = IsStatic(member) ? null : Instance(type);
        if (target == null && !IsStatic(member)) throw new ArgumentException("no instance of " + type.Name);
        for (int i = 1; i < parts.Length; i++)
        {
            target = GetValue(target, member);
            if (target == null) throw new ArgumentException(parts[i - 1] + " is null");
            member = FindMember(target.GetType(), parts[i]);
        }
    }

    private static object Convert(JSONNode value, Type type)
    {
        if (value == null) return null;
        if (type == typeof(string)) return value.Value;
        if (type == typeof(bool)) return value.Value == "true" || value.Value == "1" || value.Value == "True";
        if (type == typeof(int)) return value.AsInt;
        if (type == typeof(float)) return value.AsFloat;
        if (type == typeof(double)) return value.AsDouble;
        if (type.IsEnum) return Enum.Parse(type, value.Value, true);
        if (type == typeof(Vector3) && value.AsArray != null && value.Count == 3)
            return new Vector3(value[0].AsFloat, value[1].AsFloat, value[2].AsFloat);
        throw new ArgumentException("cannot pass a " + type.Name + " from JSON");
    }

    private static void SetValue(object target, MemberInfo member, JSONNode value)
    {
        PropertyInfo p = member as PropertyInfo;
        if (p != null) { p.SetValue(target, Convert(value, p.PropertyType), null); return; }
        FieldInfo f = (FieldInfo)member;
        f.SetValue(target, Convert(value, f.FieldType));
    }

    private static object Invoke(JSONNode r)
    {
        Type type = FindType(r["type"] != null ? r["type"].Value : "");
        string path = r["method"] != null ? r["method"].Value : "";
        object target = null;
        int dot = path.LastIndexOf('.');
        string methodName = path;
        if (dot > 0)
        {
            JSONClass sub = new JSONClass();
            sub["type"] = r["type"].Value;
            sub["member"] = path.Substring(0, dot);
            MemberInfo member;
            Resolve(sub, "member", out target, out member);
            target = GetValue(target, member);
            if (target == null) throw new ArgumentException(path.Substring(0, dot) + " is null");
            type = target.GetType();
            methodName = path.Substring(dot + 1);
        }
        JSONNode args = r["args"];
        int count = args != null ? args.Count : 0;
        const BindingFlags flags = BindingFlags.Static | BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        foreach (MethodInfo method in type.GetMethods(flags))
        {
            if (method.Name != methodName || method.GetParameters().Length != count) continue;
            ParameterInfo[] parameters = method.GetParameters();
            object[] values = new object[count];
            for (int i = 0; i < count; i++) values[i] = Convert(args[i], parameters[i].ParameterType);
            if (!method.IsStatic && target == null)
            {
                target = Instance(type);
                if (target == null) throw new ArgumentException("no instance of " + type.Name);
            }
            return method.Invoke(method.IsStatic ? null : target, values);
        }
        throw new ArgumentException(type.Name + " has no method " + methodName + " taking " + count + " argument(s)");
    }

    // ------------------------------------------------------------------- JSON

    private static object Describe(object value)
    {
        if (value == null || value is string || value is bool || value is int || value is float || value is double ||
            value is long || value is Vector3 || value is Vector2) return value;
        if (value is Quaternion) return ((Quaternion)value).eulerAngles;
        if (value is Enum) return value.ToString();
        if (value is UnityEngine.Object) return ((UnityEngine.Object)value).name + " (" + value.GetType().Name + ")";
        IEnumerable list = value as IEnumerable;
        if (list != null)
        {
            List<object> items = new List<object>();
            foreach (object item in list)
            {
                if (items.Count == 50) { items.Add("..."); break; }
                items.Add(Describe(item));
            }
            return items;
        }
        return value.ToString();
    }

    private static string Ok(JObj members)
    {
        StringBuilder sb = new StringBuilder("{\"ok\":true");
        if (members != null)
        {
            foreach (KeyValuePair<string, object> item in members.items)
            {
                sb.Append(',');
                Write(sb, item.Key);
                sb.Append(':');
                Write(sb, item.Value);
            }
        }
        return sb.Append('}').ToString();
    }

    private static string Fail(string error)
    {
        StringBuilder sb = new StringBuilder("{\"ok\":false,\"error\":");
        Write(sb, error);
        return sb.Append('}').ToString();
    }

    private static void Write(StringBuilder sb, object value)
    {
        if (value == null) { sb.Append("null"); return; }
        if (value is bool) { sb.Append((bool)value ? "true" : "false"); return; }
        if (value is int || value is long) { sb.Append(System.Convert.ToInt64(value).ToString(CultureInfo.InvariantCulture)); return; }
        if (value is float || value is double)
        {
            double d = System.Convert.ToDouble(value);
            sb.Append(double.IsNaN(d) || double.IsInfinity(d) ? "null" : d.ToString("R", CultureInfo.InvariantCulture));
            return;
        }
        if (value is Vector3)
        {
            Vector3 v = (Vector3)value;
            sb.Append('[');
            Write(sb, v.x); sb.Append(','); Write(sb, v.y); sb.Append(','); Write(sb, v.z);
            sb.Append(']');
            return;
        }
        if (value is Vector2)
        {
            Vector2 v = (Vector2)value;
            sb.Append('['); Write(sb, v.x); sb.Append(','); Write(sb, v.y); sb.Append(']');
            return;
        }
        JObj obj = value as JObj;
        if (obj != null)
        {
            sb.Append('{');
            for (int i = 0; i < obj.items.Count; i++)
            {
                if (i > 0) sb.Append(',');
                Write(sb, obj.items[i].Key);
                sb.Append(':');
                Write(sb, obj.items[i].Value);
            }
            sb.Append('}');
            return;
        }
        string s = value as string;
        if (s == null)
        {
            IList list = value as IList;
            if (list != null)
            {
                sb.Append('[');
                for (int i = 0; i < list.Count; i++) { if (i > 0) sb.Append(','); Write(sb, list[i]); }
                sb.Append(']');
                return;
            }
            s = value.ToString();
        }
        sb.Append('"');
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
    }
}
