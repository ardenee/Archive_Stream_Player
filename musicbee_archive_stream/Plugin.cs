// Official MusicBee v3.1+ plugin SDK partial class. No MusicBee SDK source is bundled.
// Native bridge provides decoded archive bytes via loopback HTTP range reads.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
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

        public PluginInfo Initialise(IntPtr apiInterfacePtr)
        {
            api = new MusicBeeApiInterface();
            api.Initialise(apiInterfacePtr);
            Trace("PLUGIN_INITIALISED");
            PluginInfo info = new PluginInfo();
            info.PluginInfoVersion = PluginInfoVersion;
            info.Type = PluginType.General;
            info.Name = "Archive Stream Player";
            info.Description = "Play ZIP, 7z and RAR audio tracks through the static Archive Stream backend.";
            info.Author = "Archive Stream Player";
            info.TargetApplication = "";
            info.VersionMajor = 1;
            info.VersionMinor = 0;
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
                    if (!queued)
                    {
                        MessageBox.Show("MusicBee did not accept the decoded archive track URLs.",
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
            // their bytes from local files. Keep MP3/FLAC/AAC archive HTTP URLs intact.
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
                if (extension != ".ogg" && extension != ".wav" && extension != ".wma" && extension != ".m4a")
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

        private string[] StartArchiveSession(string archive)
        {
            Trace("SESSION_REQUEST archive=" + archive);
            string baseDir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            string bridge = Path.Combine(baseDir, "archive_player_bridge.exe");
            if (!File.Exists(bridge))
                throw new FileNotFoundException("The Archive Stream bridge executable must be in the MusicBee Plugins folder.", bridge);
            lock (stateLock)
            {
                if (processes.Count >= 8)
                    throw new InvalidOperationException("Too many archive sessions are active. Restart MusicBee to close old sessions.");
            }
            string folder = Path.Combine(Path.GetTempPath(), "ArchiveStreamMusicBee-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
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
