// Test of VamkitLang.cs outside the game: the table reader, the choice file, the fall-back to English and the
// "##" contexts. Argument: the folder of the tables (components/shared/lang). Exit 0 when every check holds.
using System;
using System.Collections.Generic;
using System.IO;

internal static class LangTest
{
    private static int failed;

    private static void Check(bool ok, string what)
    {
        if (!ok) { failed++; Console.WriteLine("FAIL  " + what); }
    }

    private static bool Throws(string json)
    {
        try { Lang.Parse(json); return false; }
        catch (FormatException) { return true; }
    }

    private static int Main(string[] args)
    {
        // The reader: a flat object of texts, with the escapes JSON has.
        Dictionary<string, string> t = Lang.Parse("﻿ { \"a\\n\" : \"b\\u4e2d\\\"\\\\\" ,\r\n \"c\":\"\" }");
        Check(t.Count == 2 && t["a\n"] == "b中\"\\" && t["c"] == "", "texts with escapes are read");
        Check(Lang.Parse("{}").Count == 0, "an empty table is read");
        Check(Throws("") && Throws("[1]") && Throws("{\"a\": 1}") && Throws("{\"a\": \"b\"") && Throws("{\"a\": {\"b\": \"c\"}}"),
            "anything but a flat object of texts is refused");

        // A made-up <VaM>/vamkit: the Chinese table of the repository and the launch panel's choice.
        string folder = Path.Combine(Path.GetTempPath(), "vamkit-lang-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(Path.Combine(folder, "lang"));
            Directory.CreateDirectory(Path.Combine(Path.Combine(folder, "launcher"), "profiles"));
            File.Copy(Path.Combine(args[0], "zh.json"), Path.Combine(Path.Combine(folder, "lang"), "zh.json"));
            File.WriteAllText(Path.Combine(Path.Combine(Path.Combine(folder, "launcher"), "profiles"), "language.json"), "{\"language\": \"zh\"}\n");
            Lang.Folder = folder;
            Check(Lang.Code == "zh", "the language the launch panel chose is used (" + Lang.Code + ")");
            Check(Lang.T("Off") == "关" && Lang.T("Off##choice") == "关闭", "one English word, two words by context");
            Check(Lang.T("not in any table") == "not in any table", "a text the table lacks stays English");
            Check(Lang.T("not in any table##context") == "not in any table", "and its context is never shown");
            Check(Lang.F("Gloss · {0}", "A") == "光泽 · A", "values are filled in");
            Check(Lang.N("Soft beauty") == "Soft beauty" && Lang.T(Lang.N("Soft beauty")) == "柔光美颜", "a marked text is translated when shown");
        }
        finally
        {
            try { Directory.Delete(folder, true); } catch (Exception) { }
        }
        Console.WriteLine(failed == 0 ? "ok" : failed + " check(s) failed");
        return failed == 0 ? 0 : 1;
    }
}
