// Off-line test of VRMenuIni.cs, the VaM VR menu's INI editing (C# 3, no VaM
// needed). Usage: VRMenuIniTest.exe [ini file ...]. Each file named is only
// read: a few of its values are changed in memory, and the result must differ
// from the file in exactly those lines and give the file back byte for byte
// when the old values are put back.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class VRMenuIniTest
{
    private static int checks, failures;

    private static void Check(bool ok, string what)
    {
        checks++;
        if (ok) return;
        failures++;
        Console.WriteLine("FAIL: " + what);
    }

    private static void Same(string expected, string actual, string what)
    {
        Check(expected == actual, what + "\n  expected " + Show(expected) + "\n  actual   " + Show(actual));
    }

    private static void SameBytes(byte[] expected, byte[] actual, string what)
    {
        bool ok = expected.Length == actual.Length;
        for (int i = 0; ok && i < expected.Length; i++) ok = expected[i] == actual[i];
        Check(ok, what + " (" + expected.Length + " bytes expected, " + actual.Length + " written)");
    }

    private static string Show(string s)
    {
        return s == null ? "(null)" : "\"" + s.Replace("\r", "\\r").Replace("\n", "\\n") + "\"";
    }

    // section, key, value, section, key, value, ...
    private static List<VRMenuIni.Edit> Edits(params string[] parts)
    {
        List<VRMenuIni.Edit> list = new List<VRMenuIni.Edit>();
        for (int i = 0; i + 2 < parts.Length; i += 3) list.Add(new VRMenuIni.Edit(parts[i], parts[i + 1], parts[i + 2]));
        return list;
    }

    private static byte[] Join(byte[] a, byte[] b)
    {
        byte[] all = new byte[a.Length + b.Length];
        Array.Copy(a, all, a.Length);
        Array.Copy(b, 0, all, a.Length, b.Length);
        return all;
    }

    public static int Main(string[] args)
    {
        // As LaunchPanel.py writes it (CRLF on Windows).
        string panel = "; VaM launch panel: per-run settings; no registry writes.\r\n[Performance]\r\nEnabled=1\r\n" +
            "Upscaler=DLSS\r\nRenderScale=50\r\nDLSSPreset=Default\r\nDLSSHairBias=0\r\n\r\n[NR]\r\nEnabled=1\r\nStyle=0\r\n";

        Same("DLSS", VRMenuIni.Get(panel, "Performance", "Upscaler"), "get");
        Same("DLSS", VRMenuIni.Get(panel, "performance", "UPSCALER"), "get ignores case");
        Same("1", VRMenuIni.Get(panel, "NR", "Enabled"), "get stays in its section");
        Same(null, VRMenuIni.Get(panel, "NR", "Upscaler"), "get: key of another section");
        Same(null, VRMenuIni.Get(panel, "overlay", "position"), "get: missing section");
        Same("x y", VRMenuIni.Get("[a]\n  k  =  x y  \n", "a", "k"), "get trims");
        Same(null, VRMenuIni.Get("[a]\n;k=1\n#k=2\n", "a", "k"), "get skips comments");
        Same("1", VRMenuIni.Get("[a]\nk=1\nk=2\n[a]\nk=3\n", "a", "k"), "get: first key of the first section");
        Same("", VRMenuIni.Get("[a]\nk=\n", "a", "k"), "get: empty value");
        Same("v", VRMenuIni.Get("[ a ]\nk=v", "a", "k"), "get: spaces in brackets, no final newline");
        Same("u", VRMenuIni.Get("x=1\n[a]\nk=u\n", "a", "k"), "get: lines before the first section");

        Same(panel.Replace("DLSSHairBias=0", "DLSSHairBias=30"),
            VRMenuIni.Set(panel, Edits("Performance", "DLSSHairBias", "30")), "set: one line changes, CRLF stays");
        Same(panel.Replace("Style=0", "Style=2").Replace("DLSSPreset=Default", "DLSSPreset=K"),
            VRMenuIni.Set(panel, Edits("NR", "Style", "2", "performance", "dlsspreset", "K")), "set: two sections in one call");
        Same("[a]\ndlsspreset=J\n", VRMenuIni.Set("[a]\ndlsspreset=K\n", Edits("a", "DLSSPreset", "J")), "set keeps the key's spelling");
        Same("[a]\nk = 5\n", VRMenuIni.Set("[a]\nk = 1\n", Edits("a", "k", "5")), "set keeps the spacing after =");
        Same("[a]\n;k=1\nk=5\n", VRMenuIni.Set("[a]\n;k=1\nk=2\n", Edits("a", "k", "5")), "set leaves comments");
        Same("[a]\nk=5\n[a]\nk=3\n", VRMenuIni.Set("[a]\nk=1\n[a]\nk=3\n", Edits("a", "k", "5")), "set: the first of two sections");
        Same("[a]\nx=1\nk=5\n\n[b]\ny=2\n", VRMenuIni.Set("[a]\nx=1\n\n[b]\ny=2\n", Edits("a", "k", "5")), "set adds a missing key after the section's entries");
        Same("[a]\nk=5\n\n[b]\n", VRMenuIni.Set("[a]\n\n[b]\n", Edits("a", "k", "5")), "set: key into an empty section");
        Same("[a]\nx=1\n\n[c]\nk=5\n", VRMenuIni.Set("[a]\nx=1\n", Edits("c", "k", "5")), "set adds a missing section at the end");
        Same("[a]\r\nx=1\r\n\r\n[c]\r\nk=5", VRMenuIni.Set("[a]\r\nx=1", Edits("c", "k", "5")), "set: no final newline stays so");
        Same("[c]\nk=6\n", VRMenuIni.Set("", Edits("c", "k", "5", "c", "k", "6")), "set: a new section, then its key again");
        Same(panel, VRMenuIni.Set(panel, Edits()), "set without edits");
        string mixed = "[Performance]\r\nRenderScale=35\nDLSSPreset=RR\n\r\n[NR]\r\nEnabled=1\r\n";
        Same(mixed.Replace("RenderScale=35", "RenderScale=50"),
            VRMenuIni.Set(mixed, Edits("Performance", "RenderScale", "50")), "mixed line endings kept");
        string changed = VRMenuIni.Set(panel, Edits("Performance", "DLSSHairBias", "30", "NR", "Style", "1"));
        Same(panel, VRMenuIni.Set(changed, Edits("Performance", "DLSSHairBias", "0", "NR", "Style", "0")), "set back gives the text back");

        // Bytes: what the menu does not change comes back unchanged.
        byte[] utf8 = Encoding.UTF8.GetBytes("; 中文注释\r\n[ofxr]\r\ntriple_frame_gen=1\r\n");
        VRMenuIni.Document d = new VRMenuIni.Document(utf8);
        Same("1", VRMenuIni.Get(d.text, "ofxr", "triple_frame_gen"), "utf-8: value read");
        SameBytes(Encoding.UTF8.GetBytes("; 中文注释\r\n[ofxr]\r\ntriple_frame_gen=0\r\n"),
            d.Bytes(VRMenuIni.Set(d.text, Edits("ofxr", "triple_frame_gen", "0"))), "utf-8: other bytes unchanged");
        SameBytes(utf8, d.Bytes(d.text), "utf-8: unchanged text gives the same bytes");
        byte[] gbk = Join(new byte[] { (byte)';', 0xD6, 0xD0, 0xCE, 0xC4, (byte)'\n' }, Encoding.ASCII.GetBytes("[a]\nk=1\n"));
        d = new VRMenuIni.Document(gbk);
        SameBytes(Join(new byte[] { (byte)';', 0xD6, 0xD0, 0xCE, 0xC4, (byte)'\n' }, Encoding.ASCII.GetBytes("[a]\nk=2\n")),
            d.Bytes(VRMenuIni.Set(d.text, Edits("a", "k", "2"))), "ANSI (GBK) bytes unchanged");
        byte[] mark = new byte[] { 0xEF, 0xBB, 0xBF };
        d = new VRMenuIni.Document(Join(mark, Encoding.ASCII.GetBytes("[a]\nk=1\n")));
        Same("1", VRMenuIni.Get(d.text, "a", "k"), "utf-8 byte order mark: first section found");
        SameBytes(Join(mark, Encoding.ASCII.GetBytes("[a]\nk=2\n")), d.Bytes(VRMenuIni.Set(d.text, Edits("a", "k", "2"))),
            "utf-8 byte order mark kept");
        byte[] wide = new byte[] { 0xFF, 0xFE };
        d = new VRMenuIni.Document(Join(wide, Encoding.Unicode.GetBytes("[a]\r\nk=1\r\n")));
        Same("1", VRMenuIni.Get(d.text, "a", "k"), "utf-16: value read");
        SameBytes(Join(wide, Encoding.Unicode.GetBytes("[a]\r\nk=2\r\n")), d.Bytes(VRMenuIni.Set(d.text, Edits("a", "k", "2"))),
            "utf-16 stays utf-16");

        // The OFXR layer's frame multiplier: frame_multiplier over the tray's switch.
        string tray = "[ofxr]\r\nbackend=nvidia\r\ntriple_frame_gen=1\r\nexcluded_processes=\r\n\r\n[overlay]\r\nposition=off\r\n";
        Check(VRMenuIni.FrameMultiplier(tray, 4, 3) == 3, "multiplier: switch on, 3X launch");
        Check(VRMenuIni.FrameMultiplier(tray, 4, 4) == 4, "multiplier: switch on, 4X launch");
        Check(VRMenuIni.FrameMultiplier(tray.Replace("triple_frame_gen=1", "triple_frame_gen=0"), 4, 3) == 2, "multiplier: switch off");
        string menu = VRMenuIni.Set(tray, VRMenuIni.FrameMultiplierEdits(4));
        Same(tray.Replace("excluded_processes=\r\n", "excluded_processes=\r\nframe_multiplier=4\r\n"), menu,
            "multiplier edits: one key added at the end of [ofxr], the switch left on");
        Check(VRMenuIni.FrameMultiplier(menu, 4, 3) == 4, "multiplier: frame_multiplier over the switch");
        Check(VRMenuIni.FrameMultiplier(menu, 3, 3) == 3, "multiplier: no more than the session can run");
        Check(VRMenuIni.FrameMultiplier(menu, 0, 3) == 3, "multiplier: an older layer follows the switch only");
        string two = VRMenuIni.Set(menu, VRMenuIni.FrameMultiplierEdits(2));
        Same("2", VRMenuIni.Get(two, "ofxr", "frame_multiplier"), "multiplier edits: 2X");
        Same("0", VRMenuIni.Get(two, "ofxr", "triple_frame_gen"), "multiplier edits: 2X turns the switch off");
        Check(VRMenuIni.FrameMultiplier(two, 4, 4) == 2, "multiplier: 2X read back");
        string three = VRMenuIni.Set(two, VRMenuIni.FrameMultiplierEdits(3));
        Check(VRMenuIni.FrameMultiplier(three, 4, 4) == 3 && VRMenuIni.Get(three, "ofxr", "triple_frame_gen") == "1",
            "multiplier: 3X in a 4X launch, the switch on");
        Check(VRMenuIni.FrameMultiplier(tray, 4, 3) == VRMenuIni.FrameMultiplier(tray.Replace("\r\n", "\n"), 4, 3), "multiplier: LF file");
        Check(VRMenuIni.FrameMultiplier("[ofxr]\nframe_multiplier=5\ntriple_frame_gen=0\n", 4, 3) == 2, "multiplier: 5 is ignored");
        Check(VRMenuIni.FrameMultiplier("[ofxr]\nframe_multiplier=1\ntriple_frame_gen=1\n", 4, 3) == 3, "multiplier: 1 is ignored");
        Check(VRMenuIni.FrameMultiplier("[ofxr]\nframe_multiplier=-3\ntriple_frame_gen=1\n", 4, 4) == 4, "multiplier: negative is ignored");
        Check(VRMenuIni.FrameMultiplier("[ofxr]\nframe_multiplier=\ntriple_frame_gen=0\n", 4, 3) == 2, "multiplier: empty is ignored");
        Check(VRMenuIni.FrameMultiplier("[ofxr]\nframe_multiplier=3x\n", 4, 4) == 3, "multiplier: leading digits, as GetPrivateProfileInt");
        Check(VRMenuIni.FrameMultiplier("[ofxr]\nFRAME_MULTIPLIER = 4\n", 4, 3) == 4, "multiplier: key without case, spaces");
        Check(VRMenuIni.FrameMultiplier("[overlay]\nframe_multiplier=4\n", 4, 3) == 2, "multiplier: only in [ofxr]");
        Check(VRMenuIni.FrameMultiplier("", 4, 3) == 2, "multiplier: empty file");
        Check(VRMenuIni.ProfileInt(null, 7) == 7, "profile int: missing");
        Check(VRMenuIni.ProfileInt("12ab", 0) == 12, "profile int: leading digits");
        Check(VRMenuIni.ProfileInt("x", 5) == 0, "profile int: no digits");
        Check(VRMenuIni.ProfileInt("-4", 5) == 0, "profile int: negative");

        foreach (string path in args) CheckFile(path);
        Console.WriteLine((failures == 0 ? "RESULT: PASS" : "RESULT: FAIL") + " (" + checks + " checks, " + failures + " failed)");
        return failures == 0 ? 0 : 1;
    }

    // A real file: the launch panel's performance.ini or OFXR's ofxr_bridge.ini.
    private static void CheckFile(string path)
    {
        byte[] bytes = File.ReadAllBytes(path);
        VRMenuIni.Document d = new VRMenuIni.Document(bytes);
        string[][] keys;
        if (VRMenuIni.Get(d.text, "Performance", "Upscaler") != null)
            keys = new string[][] {
                new string[] { "Performance", "DLSSHairBias" }, new string[] { "Performance", "FFRFullRateRadius" },
                new string[] { "Performance", "DLSSPreset" }, new string[] { "NR", "Intensity" }, new string[] { "NR", "Enabled" } };
        else if (VRMenuIni.Get(d.text, "ofxr", "triple_frame_gen") != null)
            keys = new string[][] { new string[] { "ofxr", "triple_frame_gen" }, new string[] { "overlay", "position" } };
        else
        {
            Check(false, path + ": neither a launch panel performance.ini nor an ofxr_bridge.ini");
            return;
        }
        List<VRMenuIni.Edit> forward = new List<VRMenuIni.Edit>(), back = new List<VRMenuIni.Edit>();
        List<string> newValues = new List<string>();
        foreach (string[] k in keys)
        {
            string old = VRMenuIni.Get(d.text, k[0], k[1]);
            Check(old != null, path + ": has [" + k[0] + "] " + k[1]);
            if (old == null) continue;
            string value = old == "7" ? "8" : "7";
            forward.Add(new VRMenuIni.Edit(k[0], k[1], value));
            back.Add(new VRMenuIni.Edit(k[0], k[1], old));
            newValues.Add(value);
        }
        string updated = VRMenuIni.Set(d.text, forward);
        for (int i = 0; i < forward.Count; i++)
            Same(newValues[i], VRMenuIni.Get(updated, forward[i].section, forward[i].key), path + ": [" + forward[i].section + "] " + forward[i].key + " written");
        string[] a = d.text.Split('\n'), b = updated.Split('\n');
        Check(a.Length == b.Length, path + ": same number of lines");
        int differ = 0;
        for (int i = 0; i < a.Length && i < b.Length; i++) if (a[i] != b[i]) differ++;
        Check(differ == forward.Count, path + ": " + differ + " lines differ, " + forward.Count + " edited");
        SameBytes(bytes, d.Bytes(VRMenuIni.Set(updated, back)), path + ": old values give the file back");
        if (keys[0][0] == "ofxr")
        {
            // The in-game multiplier: one line added (none where the tab wrote
            // the key before), the switch line at most changed.
            bool had = VRMenuIni.Get(d.text, "ofxr", "frame_multiplier") != null;
            foreach (int m in new int[] { 2, 3, 4 })
            {
                string multiplied = VRMenuIni.Set(d.text, VRMenuIni.FrameMultiplierEdits(m));
                Check(VRMenuIni.FrameMultiplier(multiplied, 4, 3) == m, path + ": frame_multiplier=" + m + " read back");
                string[] c = multiplied.Split('\n');
                Check(c.Length == a.Length + (had ? 0 : 1),
                    path + ": frame_multiplier=" + m + (had ? " keeps the line count" : " adds one line"));
                int moved = 0;
                foreach (string line in c) if (Array.IndexOf(a, line) < 0) moved++;
                Check(moved <= 2, path + ": frame_multiplier=" + m + " changes " + moved + " lines");
            }
        }
        Console.WriteLine(path + ": " + forward.Count + " values changed and put back");
    }
}
