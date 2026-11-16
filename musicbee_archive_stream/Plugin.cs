// Official MusicBee v3.1+ plugin SDK partial class. No MusicBee SDK source is bundled.
// Native BASS streams archive bytes directly; HTTP/local compatibility is retained.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        private MusicBeeApiInterface api;
        private readonly List<Process> processes = new List<Process>();
        private readonly List<string> directories = new List<string>();
        private readonly List<string> shutdownUrls = new List<string>();
        private readonly object stateLock = new object();
        private System.Windows.Forms.Timer playbackTraceTimer;
        private bool nativeMp3Loaded;
        private const uint BassUnicode = 0x80000000u;

        [DllImport("bass.dll", EntryPoint = "BASS_PluginLoad", CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern uint BassPluginLoad([MarshalAs(UnmanagedType.LPWStr)] string path, uint flags);

        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamWriteNativeSevenCodecsW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int WriteNativeSevenCodecs(string archive, string folder);

        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamWriteNativeBufferedTracksW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int WriteNativeBufferedTracks(string archive, string folder);

        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamWriteNativeDiskLastTracksW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int WriteNativeDiskLastTracks(string archive, string folder);

        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamWriteNativeDiskPcmTracksW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int WriteNativeDiskPcmTracks(string archive, string folder);

        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamWriteNativeDiskMp3TracksW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int WriteNativeDiskMp3Tracks(string archive, string folder);

        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamWriteNativeDiskOggTracksW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int WriteNativeDiskOggTracks(string archive, string folder);

        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamFormatNativeTitleW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int FormatNativeTitle(string virtualPath, StringBuilder output, uint capacity);


        private static readonly object traceLock = new object();

        private static void Trace(string message)
        {
            try
            {
                bool enabled = Environment.GetEnvironmentVariable("ARCHIVE_STREAM_ADAPTER_DEBUG") == "1";
                string configuredFolder = "";
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\ArchiveStreamPlayer\Adapters", false))
                {
                    if (key != null)
                    {
                        object stored = key.GetValue("DebugLogging", 0);
                        if (stored is int && (int)stored == 1)
                            enabled = true;
                        configuredFolder = key.GetValue("LogDirectory", "") as string ?? "";
                    }
                }
                if (!enabled)
                    return;

                string local = Environment.GetEnvironmentVariable("LOCALAPPDATA");
                if (String.IsNullOrWhiteSpace(local))
                    local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                string folder = String.IsNullOrWhiteSpace(configuredFolder)
                    ? Path.Combine(local, "ArchiveStreamPlayer", "Logs") : configuredFolder;
                Directory.CreateDirectory(folder);

                // One log record per line. Never record a prompted password.
                string safe = (message ?? "").Replace('\r', ' ').Replace('\n', ' ');
                string line = DateTime.UtcNow.ToString("o") + " | INFO | MUSICBEE | " + safe + Environment.NewLine;
                lock (traceLock)
                {
                    File.AppendAllText(Path.Combine(folder, "archive_stream_musicbee.log"), line);
                }
            }
            catch { }
        }

        private void StartPlaybackDiagnostics()
        {
            if (playbackTraceTimer != null)
                return;
            bool enabled = Environment.GetEnvironmentVariable("ARCHIVE_STREAM_ADAPTER_DEBUG") == "1";
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\ArchiveStreamPlayer\Adapters", false))
            {
                if (key != null && key.GetValue("DebugLogging", 0) is int && (int)key.GetValue("DebugLogging", 0) == 1)
                    enabled = true;
            }
            if (!enabled)
                return;

            // A WinForms timer calls MusicBee's native API on the host UI thread.
            // Observe only our own archive URLs/local files; never log bridge tokens.
            playbackTraceTimer = new System.Windows.Forms.Timer();
            playbackTraceTimer.Interval = 1000;
            playbackTraceTimer.Tick += (sender, args) =>
            {
                try
                {
                    string source = api.NowPlaying_GetFileUrl == null ? "" : (api.NowPlaying_GetFileUrl() ?? "");
                    Uri uri;
                    bool http = Uri.TryCreate(source, UriKind.Absolute, out uri) &&
                        uri.Scheme == Uri.UriSchemeHttp && uri.IsLoopback &&
                        uri.AbsolutePath.IndexOf("/entry/", StringComparison.Ordinal) >= 0;
                    bool local = source.IndexOf("ArchiveStreamMusicBee-", StringComparison.OrdinalIgnoreCase) >= 0;
                    if (!http && !local)
                        return;
                    string codec = Path.GetExtension(http ? uri.AbsolutePath : source).ToLowerInvariant();
                    int queueIndex = api.NowPlayingList_GetCurrentIndex == null ? -1 : api.NowPlayingList_GetCurrentIndex();
                    int position = api.Player_GetPosition == null ? -1 : api.Player_GetPosition();
                    string state = api.Player_GetPlayState == null ? "unknown" : api.Player_GetPlayState().ToString();
                    Trace("HOST_PLAYBACK index=" + queueIndex + " codec=" + codec + " route=" +
                        (http ? "HTTP" : (source.EndsWith(".ast", StringComparison.OrdinalIgnoreCase) ? "NATIVE" : "LOCAL")) +
                        " state=" + state + " position_ms=" + position);
                }
                catch (Exception e)
                {
                    Trace("HOST_PLAYBACK_DIAGNOSTIC_FAILED type=" + e.GetType().Name);
                }
            };
            playbackTraceTimer.Start();
            Trace("HOST_DIAGNOSTICS_TIMER_START");
        }

        public PluginInfo Initialise(IntPtr apiInterfacePtr)
        {
            api = new MusicBeeApiInterface();
            api.Initialise(apiInterfacePtr);
            Trace("PLUGIN_INITIALISED");
            try
            {
                string folder = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                string nativeDll = Path.GetFullPath(Path.Combine(folder, "..", "bass_archive_stream.dll"));
                if (File.Exists(nativeDll))
                    nativeMp3Loaded = BassPluginLoad(nativeDll, BassUnicode) != 0;
                Trace("NATIVE_BASS_PLUGIN loaded=" + (nativeMp3Loaded ? "1" : "0"));
            }
            catch (Exception e)
            {
                Trace("NATIVE_BASS_PLUGIN_ERROR type=" + e.GetType().Name);
                nativeMp3Loaded = false;
            }
            PluginInfo info = new PluginInfo();
            info.PluginInfoVersion = PluginInfoVersion;
            info.Type = PluginType.General;
            info.Name = "Archive Stream Player";
            info.Description = "Play ZIP, 7z and RAR audio tracks through the static Archive Stream backend.";
            info.Author = "Archive Stream Player";
            info.TargetApplication = "";
            info.VersionMajor = 1;
            info.VersionMinor = 12;
            info.Revision = 0;
            info.MinInterfaceVersion = MinInterfaceVersion;
            info.MinApiRevision = MinApiRevision;
            info.ReceiveNotifications = ReceiveNotificationFlags.StartupOnly;
            info.ConfigurationPanelHeight = 0;
            if (api.MB_AddMenuItem != null)
            {
                var item = api.MB_AddMenuItem("mnuTools/Archive Stream: Add Archive Audio...",
                    "Add audio entries from a ZIP, 7z or RAR archive", OnAddArchive);
                Trace(item == null ? "MENU_ITEM_FAILED" : "MENU_ITEM_REGISTERED");
                api.MB_AddMenuItem("mnuTools/Archive Stream: Settings...",
                    "Configure shared titles, cache cleanup, passwords and diagnostic logging", OnShowSettings);
            }
            return info;
        }

        private async void OnAddArchive(object sender, EventArgs args)
        {
            Trace("ADD_ARCHIVE_INVOKED");
            using (var dialog = new OpenFileDialog())
            {
                dialog.Title = "Archive Stream: choose an audio archive";
                dialog.Filter = "Archives (*.zip;*.7z;*.rar;*.001)|*.zip;*.7z;*.rar;*.001|All files (*.*)|*.*";
                dialog.CheckFileExists = true;
                dialog.Multiselect = false;
                if (dialog.ShowDialog() != DialogResult.OK)
                    return;

                try
                {
                    string[] tracks = await Task.Run(() => StartArchiveSession(dialog.FileName));
                    bool queued = tracks.Length > 0 && api.NowPlayingList_QueueFilesLast != null &&
                        api.NowPlayingList_QueueFilesLast(tracks);
                    Trace("QUEUE_RESULT accepted=" + (queued ? "1" : "0") + " tracks=" + tracks.Length);
                    if (queued)
                        StartPlaybackDiagnostics();
                    if (!queued)
                    {
                        MessageBox.Show("MusicBee did not accept the archive audio tracks.",
                            "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    }
                }
                catch (Exception e)
                {
                    MessageBox.Show(e.Message, "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }

        private static string[] PrepareMusicBeeTracks(string[] urls, string sessionFolder)
        {
            // MusicBee 3.7 rejects some codec containers over HTTP despite accepting
            // their bytes from local files. Keep MP3/AAC archive HTTP URLs intact.
            // The host queue API accepts paths/URLs, not an in-memory media stream.
            int limitMb = 2048;
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\ArchiveStreamPlayer\Adapters", false))
            {
                if (key != null)
                {
                    object configured = key.GetValue("CacheLimitMB", limitMb);
                    if (configured is int && (int)configured >= 0 && (int)configured <= 20480)
                        limitMb = (int)configured;
                }
            }

            string[] prepared = (string[])urls.Clone();
            long remaining = (long)limitMb * 1024 * 1024;
            for (int index = 0; index < urls.Length; ++index)
            {
                Uri url;
                if (!Uri.TryCreate(urls[index], UriKind.Absolute, out url) ||
                    url.Scheme != Uri.UriSchemeHttp || !url.IsLoopback)
                    throw new IOException("Archive Stream returned an unexpected media URL.");
                string extension = Path.GetExtension(url.AbsolutePath).ToLowerInvariant();
                if (extension != ".ogg" && extension != ".flac" && extension != ".wav" && extension != ".wma" && extension != ".m4a")
                    continue;
                if (remaining == 0)
                    throw new IOException("MusicBee needs temporary local media for this archive codec. Enable a nonzero managed-cache limit in Archive Stream settings.");

                string target = Path.Combine(sessionFolder, "archive-entry-" + index.ToString("D6") + extension);
                string partial = target + ".partial";
                var request = (System.Net.HttpWebRequest)System.Net.WebRequest.Create(url);
                request.Method = "GET";
                request.Proxy = null;
                request.AllowAutoRedirect = false;
                request.KeepAlive = false;
                request.Timeout = 60000;
                request.ReadWriteTimeout = 60000;
                long written = 0;
                using (var response = (System.Net.HttpWebResponse)request.GetResponse())
                {
                    if (response.StatusCode != System.Net.HttpStatusCode.OK ||
                        response.ContentLength <= 0 || response.ContentLength > remaining)
                        throw new IOException("Archive media entry exceeds MusicBee's configured temporary cache budget or returned an invalid HTTP response.");
                    using (var stream = response.GetResponseStream())
                    using (var output = new FileStream(partial, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    {
                        byte[] buffer = new byte[65536];
                        int count;
                        while ((count = stream.Read(buffer, 0, buffer.Length)) > 0)
                        {
                            written += count;
                            if (written > response.ContentLength)
                                throw new IOException("Archive media download exceeded its declared size.");
                            output.Write(buffer, 0, count);
                        }
                    }
                    if (written != response.ContentLength)
                        throw new IOException("Archive media download ended prematurely.");
                }
                File.Move(partial, target);
                remaining -= written;
                prepared[index] = target;
                Trace(extension == ".ogg" ? "OGG_LOCAL_READY index=" + index + " bytes=" + written : "LOCAL_CODEC_READY index=" + index + " codec=" + extension + " bytes=" + written);
            }
            return prepared;
        }

        private static string SafeNativeTitleFileName(string title, int number, HashSet<string> used, string folder)
        {
            // MusicBee displays the basename of local temporary .ast tracks.
            // Preserve the preset wherever Windows filenames permit; preserve
            // playlist order separately from filename sorting.
            var clean = new StringBuilder();
            char[] invalid = Path.GetInvalidFileNameChars();
            foreach (char c in title)
            {
                if (Char.IsControl(c) || Array.IndexOf(invalid, c) >= 0)
                    clean.Append('-');
                else
                    clean.Append(c);
            }
            string basename = clean.ToString().Trim().TrimEnd('.');
            if (basename.Length == 0)
                basename = "Archive Track " + number;
            if (basename.Length > 96)
            {
                basename = basename.Substring(0, 96).TrimEnd(' ', '.');
                if (basename.Length > 0 && Char.IsHighSurrogate(basename[basename.Length - 1]))
                    basename = basename.Substring(0, basename.Length - 1);
            }
            string firstPart = basename.Split('.')[0];
            string upper = firstPart.ToUpperInvariant();
            if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
                (upper.Length == 4 && (upper.StartsWith("COM") || upper.StartsWith("LPT")) && upper[3] >= '1' && upper[3] <= '9'))
                basename = "_" + basename;

            string filename = basename + ".ast";
            int duplicate = 0;
            while (used.Contains(filename) || File.Exists(Path.Combine(folder, filename)))
            {
                ++duplicate;
                filename = basename + " (" + number.ToString("D4") + "-" + duplicate + ").ast";
            }
            used.Add(filename);
            return filename;
        }

        private static void ApplyNativeSharedTitles(string folder, List<string> tracks, List<string> virtualPaths)
        {
            var used = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            int renamed = 0;
            for (int i = 0; i < tracks.Count; ++i)
            {
                try
                {
                    var title = new StringBuilder(2048);
                    if (FormatNativeTitle(virtualPaths[i], title, (uint)title.Capacity) != 1 || title.Length == 0)
                        continue;
                    string destination = Path.Combine(folder, SafeNativeTitleFileName(title.ToString(), i + 1, used, folder));
                    File.Move(tracks[i], destination);
                    tracks[i] = destination;
                    ++renamed;
                }
                catch (EntryPointNotFoundException)
                {
                    Trace("NATIVE_TITLE_SKIP reason=old_addon");
                    break; // older BASS DLLs retain their original native route
                }
                catch (Exception e)
                {
                    Trace("NATIVE_TITLE_SKIP index=" + i + " reason=" + e.GetType().Name);
                }
            }
            Trace("NATIVE_TITLE_READY renamed=" + renamed + " total=" + tracks.Count + " preset=SHARED");
        }

        private string[] TryStartNativeAudioSession(string archive, string sessionFolder)
        {
            // Validated MP3/WAV/FLAC/OGG/AAC/M4A/WMA from streamable ZIP/7z/RAR.
            // Supported unencrypted solid MP3/Vorbis can use bounded RAM; other
            // solid, encrypted and unsupported layouts retain the bridge.
            if (!nativeMp3Loaded ||
                !(archive.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) ||
                  archive.EndsWith(".7z", StringComparison.OrdinalIgnoreCase) ||
                  archive.EndsWith(".rar", StringComparison.OrdinalIgnoreCase)))
                return null;

            string folder = Path.Combine(sessionFolder, "Native");
            try
            {
                // The statically-linked native BASS DLL enumerates the archive in
                // this MusicBee process. No bridge executable, HTTP or playlist.
                int count;
                try
                {
                    try
                    {
                        try
                        {
                            try
                            {
                                try
                                {
                                    count = WriteNativeDiskOggTracks(archive, folder);
                                }
                                catch (EntryPointNotFoundException)
                                {
                                    count = WriteNativeDiskMp3Tracks(archive, folder);
                                }
                            }
                            catch (EntryPointNotFoundException)
                            {
                                count = WriteNativeDiskPcmTracks(archive, folder);
                            }
                        }
                        catch (EntryPointNotFoundException)
                        {
                            count = WriteNativeDiskLastTracks(archive, folder);
                        }
                    }
                    catch (EntryPointNotFoundException)
                    {
                        count = WriteNativeBufferedTracks(archive, folder);
                    }
                }
                catch (EntryPointNotFoundException)
                {
                    count = WriteNativeSevenCodecs(archive, folder);
                }
                if (count <= 0 || count > 10000)
                    return null;

                string allowed = Path.GetFullPath(folder).TrimEnd('\\') + Path.DirectorySeparatorChar;
                string[] manifests = Directory.GetFiles(folder, "entry-*.ast", SearchOption.TopDirectoryOnly)
                    .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
                    .ToArray();
                if (manifests.Length != count)
                    return null;

                var tracks = new List<string>();
                var virtualPaths = new List<string>();
                foreach (string path in manifests)
                {
                    string target = Path.GetFullPath(path);
                    if (!target.StartsWith(allowed, StringComparison.OrdinalIgnoreCase))
                        return null;
                    string[] lines = File.ReadAllLines(target, Encoding.UTF8);
                    if (lines.Length != 2 || lines[0] != "ARCHIVE_STREAM_NATIVE_TRACK_V1" ||
                        !lines[1].StartsWith(archive + ".archive_stream|", StringComparison.OrdinalIgnoreCase) ||
                        !(lines[1].EndsWith(".mp3", StringComparison.OrdinalIgnoreCase) ||
                          lines[1].EndsWith(".wav", StringComparison.OrdinalIgnoreCase) ||
                          lines[1].EndsWith(".flac", StringComparison.OrdinalIgnoreCase) ||
                          lines[1].EndsWith(".ogg", StringComparison.OrdinalIgnoreCase) ||
                          lines[1].EndsWith(".aac", StringComparison.OrdinalIgnoreCase) ||
                          lines[1].EndsWith(".m4a", StringComparison.OrdinalIgnoreCase) ||
                          lines[1].EndsWith(".wma", StringComparison.OrdinalIgnoreCase)))
                        return null;
                    tracks.Add(target);
                    virtualPaths.Add(lines[1]);
                }
                if (tracks.Count == 0)
                    return null;
                ApplyNativeSharedTitles(folder, tracks, virtualPaths);
                Trace("NATIVE_MANIFEST_READY mode=AUDIO archive=" + Path.GetExtension(archive).ToLowerInvariant() + " tracks=" + tracks.Count + " helper=DLL http=0 cache=0");
                return tracks.ToArray();
            }
            catch (Exception e)
            {
                Trace("NATIVE_MANIFEST_FAIL type=" + e.GetType().Name);
                return null;
            }
        }

        private string[] StartArchiveSession(string archive)
        {
            Trace("SESSION_REQUEST archive=" + archive);
            string baseDir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            lock (stateLock)
            {
                if (processes.Count >= 8)
                    throw new InvalidOperationException("Too many archive sessions are active. Restart MusicBee to close old sessions.");
            }
            string folder = Path.Combine(Path.GetTempPath(), "ArchiveStreamMusicBee-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
            string[] nativeTracks = TryStartNativeAudioSession(archive, folder);
            if (nativeTracks != null)
            {
                lock (stateLock)
                    directories.Add(folder);
                Trace("SESSION_READY route=NATIVE tracks=" + nativeTracks.Length);
                return nativeTracks;
            }
            string bridge = Path.Combine(baseDir, "archive_player_bridge.exe");
            if (!File.Exists(bridge))
                throw new FileNotFoundException("The Archive Stream compatibility bridge is missing from the MusicBee Plugins folder.", bridge);
            string arguments = "--archive \"" + archive.Replace("\"", "") + "\" --playlist-dir \"" +
                folder + "\" --lifetime 86400";
            ProcessStartInfo startup = new ProcessStartInfo(bridge, arguments);
            startup.UseShellExecute = false;
            startup.CreateNoWindow = true;
            startup.RedirectStandardOutput = true;
            startup.WorkingDirectory = baseDir;
            Process process = null;
            try
            {
                process = Process.Start(startup);
                string playlist = null;
                bool ready = false;
                // The bridge emits a bounded set of startup lines, including
                // the playlist filename and READY marker, before serving media.
                for (int i = 0; i < 24; ++i)
                {
                    string line = process.StandardOutput.ReadLine();
                    if (line == null)
                        break;
                    if (line.StartsWith("PLAYLIST_M3U8=", StringComparison.Ordinal))
                        playlist = line.Substring("PLAYLIST_M3U8=".Length);
                    if (line == "BRIDGE_READY=1")
                    {
                        ready = true;
                        break;
                    }
                }
                if (!ready || String.IsNullOrEmpty(playlist) || !File.Exists(playlist))
                    throw new IOException("Archive Stream could not open the archive. Check that it contains audio and the saved password list is correct.");
                string[] transportUrls = File.ReadAllLines(playlist)
                    .Where(s => s.StartsWith("http://127.0.0.1:", StringComparison.Ordinal))
                    .ToArray();
                if (transportUrls.Length == 0)
                    throw new IOException("No audio entries were found in the selected archive.");
                string[] tracks = PrepareMusicBeeTracks(transportUrls, folder);
                lock (stateLock)
                {
                    processes.Add(process);
                    directories.Add(folder);
                    int entryPosition = transportUrls[0].IndexOf("/entry/", StringComparison.Ordinal);
                    shutdownUrls.Add(entryPosition > 0 ? transportUrls[0].Substring(0, entryPosition) + "/shutdown" : "");
                }
                Trace("SESSION_READY tracks=" + tracks.Length);
                return tracks;
            }
            catch
            {
                if (process != null)
                {
                    try { if (!process.HasExited) process.Kill(); } catch {}
                    process.Dispose();
                }
                try { Directory.Delete(folder, true); } catch {}
                throw;
            }
        }

        private void OnShowSettings(object sender, EventArgs args)
        {
            Trace("SETTINGS_OPEN_REQUEST");
            Configure(IntPtr.Zero);
        }

        public bool Configure(IntPtr panelHandle)
        {
            string script = Path.Combine(Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location), "configure-adapters.ps1");
            if (!File.Exists(script))
            {
                MessageBox.Show("The configuration script is missing from the MusicBee Plugins folder.",
                    "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }
            var info = new ProcessStartInfo("powershell.exe", "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\"");
            info.UseShellExecute = false;
            info.CreateNoWindow = false;
            Process.Start(info);
            return true;
        }

        public void SaveSettings() {}
        public void Uninstall() {}
        public void ReceiveNotification(string sourceFileUrl, NotificationType type) {}

        public void Close(PluginCloseReason reason)
        {
            if (playbackTraceTimer != null)
            {
                playbackTraceTimer.Stop();
                playbackTraceTimer.Dispose();
                playbackTraceTimer = null;
            }
            lock (stateLock)
            {
                for (int i = 0; i < processes.Count; ++i)
                {
                    Process process = processes[i];
                    try
                    {
                        if (!process.HasExited && !String.IsNullOrEmpty(shutdownUrls[i]))
                        {
                            var request = (System.Net.HttpWebRequest)System.Net.WebRequest.Create(shutdownUrls[i]);
                            request.Method = "POST";
                            request.Timeout = 1000;
                            using (var reply = request.GetResponse()) {}
                        }
                        if (!process.WaitForExit(1500))
                            process.Kill();
                    }
                    catch
                    {
                        try { if (!process.HasExited) process.Kill(); } catch {}
                    }
                    process.Dispose();
                }
                processes.Clear();
                shutdownUrls.Clear();
                foreach (string folder in directories)
                {
                    try { if (Directory.Exists(folder)) Directory.Delete(folder, true); } catch {}
                }
                directories.Clear();
            }
        }
    }
}
