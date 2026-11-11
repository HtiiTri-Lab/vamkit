// INI text editing for the VaM VR settings menu (C# 3). Values are changed in
// place: every other line, comment, key order and the file's line endings stay
// as the launch panel or the OFXR tray wrote them, so both keep reading their
// own files. Lookups follow GetPrivateProfileString, which the readers use:
// section and key names ignore case, the first section and key of a name win.
// Shared with tests\VRMenuIniTest.cs.
using System;
using System.Collections.Generic;
using System.Text;

public static class VRMenuIni
{
    public sealed class Edit
    {
        public readonly string section, key, value;

        public Edit(string section, string key, string value)
        {
            this.section = section; this.key = key; this.value = value;
        }
    }

    // [section] key's value, trimmed, or null when either is missing.
    public static string Get(string text, string section, string key)
    {
        string[] lines = Lines(text);
        int start, end;
        if (!FindSection(lines, section, out start, out end)) return null;
        for (int i = start; i < end; i++)
        {
            string value;
            if (KeyOf(lines[i], key, out value)) return value;
        }
        return null;
    }

    // `text` with each edit applied. A key missing from its section is added
    // after the section's last entry, a missing section at the end.
    public static string Set(string text, IList<Edit> edits)
    {
        string newline = text.Contains("\r\n") ? "\r\n" : "\n";
        List<string> lines = new List<string>(Lines(text));
        // Keep each existing separator: real profiles may contain both CRLF
        // and LF after edits by different tools. Never normalize other lines.
        List<string> endings = new List<string>();
        string[] raw = text.Split('\n');
        for (int i = 0; i < raw.Length; i++) endings.Add(i == raw.Length - 1 ? "" : raw[i].EndsWith("\r") ? "\r\n" : "\n");
        bool trailing = lines.Count > 0 && lines[lines.Count - 1].Length == 0;
        if (trailing) { lines.RemoveAt(lines.Count - 1); endings.RemoveAt(endings.Count - 1); }
        foreach (Edit edit in edits)
        {
            string[] current = lines.ToArray();
            int start, end;
            if (!FindSection(current, edit.section, out start, out end))
            {
                if (lines.Count > 0 && lines[lines.Count - 1].Trim().Length > 0) { lines.Add(""); endings.Add(newline); }
                lines.Add("[" + edit.section + "]"); endings.Add(newline);
                lines.Add(edit.key + "=" + edit.value); endings.Add(newline);
                continue;
            }
            bool done = false;
            for (int i = start; i < end && !done; i++)
            {
                string old;
                if (!KeyOf(current[i], edit.key, out old)) continue;
                // The key as the file spells it, and any spacing after '='.
                string line = current[i];
                int at = line.IndexOf('=') + 1;
                while (at < line.Length && (line[at] == ' ' || line[at] == '\t')) at++;
                lines[i] = line.Substring(0, at) + edit.value;
                done = true;
            }
            if (done) continue;
            int last = start - 1;                       // the header
            for (int i = start; i < end; i++)
                if (current[i].Trim().Length > 0) last = i;
            lines.Insert(last + 1, edit.key + "=" + edit.value);
            endings.Insert(last + 1, newline);
        }
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < lines.Count; i++)
        {
            b.Append(lines[i]);
            if (i < lines.Count - 1 || trailing) b.Append(endings[i].Length > 0 ? endings[i] : newline);
        }
        return b.ToString();
    }

    // The frames per VaM frame the OFXR layer runs, from its ofxr_bridge.ini:
    // `[ofxr] frame_multiplier` (2 to 4, written by this menu) where the layer
    // reads it - OFXR-DLSSG-v10 and later, most > 0: the most the session can
    // run - else the tray's "3X Frame Gen" switch `[ofxr] triple_frame_gen`,
    // on giving switchValue (3, or 4 in a 4X launch) and off 2. Numbers are
    // read as GetPrivateProfileInt reads them, as the layer does.
    public static int FrameMultiplier(string text, int most, int switchValue)
    {
        if (most > 0)
        {
            int asked = ProfileInt(Get(text, "ofxr", "frame_multiplier"), 0);
            if (asked >= 2 && asked <= 4) return Math.Min(asked, Math.Max(2, most));
        }
        return ProfileInt(Get(text, "ofxr", "triple_frame_gen"), 0) != 0 ? switchValue : 2;
    }

    // The edits that set it: frame_multiplier for the layers that read it,
    // and the switch to match for those that do not.
    public static List<Edit> FrameMultiplierEdits(int multiplier)
    {
        List<Edit> edits = new List<Edit>();
        edits.Add(new Edit("ofxr", "frame_multiplier", multiplier.ToString()));
        edits.Add(new Edit("ofxr", "triple_frame_gen", multiplier > 2 ? "1" : "0"));
        return edits;
    }

    // A value as GetPrivateProfileInt reads it: `missing` for no key, else its
    // leading digits, 0 when there are none and for a negative number.
    public static int ProfileInt(string value, int missing)
    {
        if (value == null) return missing;
        int i = 0;
        bool negative = false;
        if (i < value.Length && (value[i] == '+' || value[i] == '-')) negative = value[i++] == '-';
        long n = 0;
        for (; i < value.Length && value[i] >= '0' && value[i] <= '9'; i++)
            n = Math.Min(int.MaxValue, n * 10 + (value[i] - '0'));
        return negative ? 0 : (int)n;
    }

    // A settings file's text. UTF-8 and ANSI files are taken one byte per
    // char, so the lines the menu leaves alone go back byte for byte whatever
    // their encoding; a UTF-16 file (byte order mark FF FE) stays UTF-16, and
    // a UTF-8 byte order mark stays in front.
    public sealed class Document
    {
        private readonly byte[] mark;
        private readonly bool utf16;
        public readonly string text;

        public Document(byte[] bytes)
        {
            int skip = 0;
            if (bytes.Length >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) { utf16 = true; skip = 2; }
            else if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) skip = 3;
            mark = new byte[skip];
            Array.Copy(bytes, mark, skip);
            if (utf16)
            {
                text = Encoding.Unicode.GetString(bytes, skip, bytes.Length - skip);
                return;
            }
            char[] chars = new char[bytes.Length - skip];
            for (int i = 0; i < chars.Length; i++) chars[i] = (char)bytes[skip + i];
            text = new string(chars);
        }

        // The file's bytes for `updated`, this text with values changed.
        public byte[] Bytes(string updated)
        {
            byte[] body;
            if (utf16) body = Encoding.Unicode.GetBytes(updated);
            else
            {
                body = new byte[updated.Length];
                for (int i = 0; i < updated.Length; i++) body[i] = updated[i] < 256 ? (byte)updated[i] : (byte)'?';
            }
            byte[] all = new byte[mark.Length + body.Length];
            Array.Copy(mark, all, mark.Length);
            Array.Copy(body, 0, all, mark.Length, body.Length);
            return all;
        }
    }

    private static string[] Lines(string text)
    {
        return text.Replace("\r\n", "\n").Split('\n');
    }

    private static string SectionName(string line)
    {
        string t = line.Trim();
        if (!t.StartsWith("[")) return null;
        int close = t.IndexOf(']');
        return close > 0 ? t.Substring(1, close - 1).Trim() : null;
    }

    // Lines start..end-1 are the entries of the first [section].
    private static bool FindSection(string[] lines, string section, out int start, out int end)
    {
        start = end = -1;
        for (int i = 0; i < lines.Length; i++)
        {
            string name = SectionName(lines[i]);
            if (name == null) continue;
            if (start >= 0) { end = i; return true; }
            if (string.Equals(name, section, StringComparison.OrdinalIgnoreCase)) start = i + 1;
        }
        if (start < 0) return false;
        end = lines.Length;
        return true;
    }

    private static bool KeyOf(string line, string key, out string value)
    {
        value = null;
        string t = line.TrimStart();
        if (t.Length == 0 || t[0] == ';' || t[0] == '#') return false;
        int eq = line.IndexOf('=');
        if (eq <= 0 || !string.Equals(line.Substring(0, eq).Trim(), key, StringComparison.OrdinalIgnoreCase)) return false;
        value = line.Substring(eq + 1).Trim();
        return true;
    }
}
