// The words of the in-game menus in the user's language. Compiled into every plugin that shows text
// (manifest/components.toml: shared = ["lang/VamkitLang.cs"]); the launch panel's lang.py reads the same tables.
//
// English is the text in the code: Lang.T("Render scale"). A language is one table,
// <VaM>/vamkit/lang/<code>.json: a flat JSON object, English text -> that language's text, with "@name" the
// language's own name. A text the table does not have stays English, so a table can be started with a few lines.
//
// Which language: <VaM>/vamkit/launcher/profiles/language.json {"language": "<code>"}, written by the launch panel's
// language row; without it, Windows' display language (the primary languages below), and English when that has no
// table. Read once, when the plugin first asks for a text: a change shows with the next start of VaM.
//
// One English word that needs two words in another language carries a context after "##": Lang.T("Off##choice").
// The context is never shown. Lang.N marks an English text kept in data and translated later with Lang.T(variable);
// tools/check_lang.py finds the keys by these three names.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

internal static class Lang
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern uint GetModuleFileNameW(IntPtr module, StringBuilder name, uint size);

    [DllImport("kernel32.dll")]
    private static extern ushort GetUserDefaultUILanguage();

    // <VaM>/vamkit. A test names its own before the first text is asked for.
    internal static string Folder;

    private static readonly object gate = new object();
    private static Dictionary<string, string> table;
    private static string code = "en";

    // The language in use: "en", or the code of the table that was loaded.
    internal static string Code { get { Load(); return code; } }

    internal static string T(string english)
    {
        Load();
        string text;
        if (table.TryGetValue(english, out text)) return text;
        int cut = english.IndexOf("##", StringComparison.Ordinal);
        return cut < 0 ? english : english.Substring(0, cut);
    }

    // T with {0}, {1} ... filled in.
    internal static string F(string english, params object[] values)
    {
        return string.Format(System.Globalization.CultureInfo.InvariantCulture, T(english), values);
    }

    // Marks an English text that is kept as data and shown later through T(variable).
    internal static string N(string english) { return english; }

    private static void Load()
    {
        if (table != null) return;
        lock (gate)
        {
            if (table != null) return;
            Dictionary<string, string> found = new Dictionary<string, string>();
            string chosen = "en";
            try
            {
                if (Folder == null)
                {
                    StringBuilder exe = new StringBuilder(32768);
                    GetModuleFileNameW(IntPtr.Zero, exe, (uint)exe.Capacity);
                    Folder = Path.Combine(Path.GetDirectoryName(exe.ToString()), "vamkit");
                }
                string asked = null;
                string choice = Path.Combine(Path.Combine(Path.Combine(Folder, "launcher"), "profiles"), "language.json");
                if (File.Exists(choice)) Parse(File.ReadAllText(choice, Encoding.UTF8)).TryGetValue("language", out asked);
                if (string.IsNullOrEmpty(asked)) asked = Windows();
                string file = Path.Combine(Path.Combine(Folder, "lang"), asked + ".json");
                if (asked != "en" && Plain(asked) && File.Exists(file))
                {
                    found = Parse(File.ReadAllText(file, Encoding.UTF8));
                    chosen = asked;
                }
            }
            catch (Exception)
            {
                found = new Dictionary<string, string>();       // a table that cannot be read: English
                chosen = "en";
            }
            code = chosen;
            table = found;
        }
    }

    // A language code is a file name: letters, digits, - and _ only.
    private static bool Plain(string text)
    {
        if (text.Length == 0 || text.Length > 24) return false;
        foreach (char c in text)
            if (!(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-' || c == '_')) return false;
        return true;
    }

    // Windows' display language as a table's code: the primary language of its LANGID. lang.py has the same list;
    // a new table for a language that is not here adds its line to both.
    private static string Windows()
    {
        switch (GetUserDefaultUILanguage() & 0x3FF)
        {
            case 0x04: return "zh";
            case 0x07: return "de";
            case 0x0A: return "es";
            case 0x0C: return "fr";
            case 0x10: return "it";
            case 0x11: return "ja";
            case 0x12: return "ko";
            case 0x16: return "pt";
            case 0x19: return "ru";
            default: return "en";
        }
    }

    // A flat JSON object of texts: {"key": "text", ...}. Anything else in it is an error, and the caller stays English.
    internal static Dictionary<string, string> Parse(string json)
    {
        Dictionary<string, string> found = new Dictionary<string, string>();
        int i = 0;
        if (json.Length > 0 && json[0] == '﻿') i = 1;
        Skip(json, ref i);
        Expect(json, ref i, '{');
        Skip(json, ref i);
        if (i < json.Length && json[i] == '}') return found;
        while (true)
        {
            Skip(json, ref i);
            string key = Text(json, ref i);
            Skip(json, ref i);
            Expect(json, ref i, ':');
            Skip(json, ref i);
            found[key] = Text(json, ref i);
            Skip(json, ref i);
            if (i < json.Length && json[i] == ',') { i++; continue; }
            Expect(json, ref i, '}');
            return found;
        }
    }

    private static void Skip(string s, ref int i)
    {
        while (i < s.Length && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    }

    private static void Expect(string s, ref int i, char c)
    {
        if (i >= s.Length || s[i] != c) throw new FormatException("expected " + c + " at " + i);
        i++;
    }

    private static string Text(string s, ref int i)
    {
        Expect(s, ref i, '"');
        StringBuilder b = new StringBuilder();
        while (true)
        {
            if (i >= s.Length) throw new FormatException("a text does not end");
            char c = s[i++];
            if (c == '"') return b.ToString();
            if (c != '\\') { b.Append(c); continue; }
            if (i >= s.Length) throw new FormatException("a text does not end");
            c = s[i++];
            switch (c)
            {
                case 'n': b.Append('\n'); break;
                case 't': b.Append('\t'); break;
                case 'r': b.Append('\r'); break;
                case 'b': b.Append('\b'); break;
                case 'f': b.Append('\f'); break;
                case 'u':
                    if (i + 4 > s.Length) throw new FormatException("a short \\u at " + i);
                    b.Append((char)Convert.ToInt32(s.Substring(i, 4), 16));
                    i += 4;
                    break;
                default: b.Append(c); break;        // \" \\ \/
            }
        }
    }
}
