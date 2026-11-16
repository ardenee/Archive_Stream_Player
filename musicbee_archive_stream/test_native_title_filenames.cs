// Regression: native shared title rendering and Windows-safe temporary .ast names.
// No MusicBee profile, tags, archive data, or registry settings are modified.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

internal static class NativeMusicBeeTitlesTest
{
    [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamFormatNativeTitleW",
        CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
    private static extern int NativeFormatTitle(string virtualTrack, StringBuilder title, uint capacity);

    private static void Require(bool ok, string reason)
    {
        if (!ok)
            throw new InvalidOperationException(reason);
    }

    private static int Main(string[] args)
    {
        if (args.Length != 2)
            return 2;
        var type = Assembly.LoadFrom(args[0]).GetType("MusicBeePlugin.Plugin", true);
        var method = type.GetMethod("SafeNativeTitleFileName", BindingFlags.Static | BindingFlags.NonPublic);
        Require(method != null, "SafeNativeTitleFileName is unavailable");
        var used = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Func<string, int, string> safe = (title, number) =>
            (string)method.Invoke(null, new object[] { title, number, used, args[1] });
        string first = safe("ZIP: 1 - Hello.mp3", 1);
        Require(first == "ZIP- 1 - Hello.mp3.ast", "Shared title not sanitized predictably: " + first);
        string duplicate = safe("ZIP: 1 - Hello.mp3", 2);
        Require(duplicate != first && duplicate.EndsWith(".ast"), "Duplicate track title collided");
        string reserved = safe("CON", 3);
        Require(reserved.StartsWith("_CON", StringComparison.Ordinal), "Reserved DOS filename not protected");
        string invalid = safe("a/b\\c?d*e|f<g>h:\"i", 4);
        Require(invalid.IndexOfAny(Path.GetInvalidFileNameChars()) < 0, "Invalid filename characters remain");
        string unicode = safe("日本語 Café Пример.flac", 5);
        Require(unicode.Contains("Пример") && unicode.Contains("日本語"), "Unicode title corrupted");
        string longTitle = safe(new string('x', 230), 6);
        Require(longTitle.Length < 120, "Long title was not bounded");
        string empty = safe("... ", 7);
        Require(empty.StartsWith("Archive Track 7"), "Empty title fallback missing");
        var native = new StringBuilder(2048);
        string virtualTrack = @"C:\Temp\Sample.zip.archive_stream|1|日本語/Пример.ogg";
        Require(NativeFormatTitle(virtualTrack, native, (uint)native.Capacity) == 1, "Native shared title formatter failed");
        Require(native.ToString().Contains("Пример.ogg"), "Native UTF-16/UTF-8 title lost Unicode");
        Require(!native.ToString().Contains("<filename>"), "Unexpanded title template placeholder");
        foreach (string ext in new[] { ".zip", ".7z", ".rar" })
        {
            native.Clear();
            string input = @"C:\Temp\SharedTitle" + ext + ".archive_stream|1|Café/Пример.ogg";
            Require(NativeFormatTitle(input, native, (uint)native.Capacity) == 1, "Failed title for " + ext);
            Require(native.ToString().Contains("Пример.ogg"), "Unicode title lost in " + ext);
        }
        Console.WriteLine("MUSICBEE_SHARED_TITLE_FILENAME_AND_UNICODE_REGRESSIONS_PASS");
        Console.WriteLine("NATIVE_FORMATTED_TITLE=" + native);
        return 0;
    }
}
