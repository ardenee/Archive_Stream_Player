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

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        private MusicBeeApiInterface api;
        private readonly List<Process> processes = new List<Process>();
        private readonly List<string> directories = new List<string>();
        private readonly List<string> shutdownUrls = new List<string>();
        private readonly object stateLock = new object();

        private static void Trace(string message)
        {
            if (Environment.GetEnvironmentVariable("ARCHIVE_STREAM_ADAPTER_DEBUG") != "1")
                return;
            try
            {
                string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MusicBeeArchiveStream");
                Directory.CreateDirectory(folder);
                File.AppendAllText(Path.Combine(folder, "adapter.log"), DateTime.UtcNow.ToString("o") + " " + message + Environment.NewLine);
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
                    if (tracks.Length == 0 || api.NowPlayingList_QueueFilesLast == null ||
                        !api.NowPlayingList_QueueFilesLast(tracks))
                        MessageBox.Show("MusicBee did not accept the decoded archive track URLs.",
                            "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
                catch (Exception e)
                {
                    MessageBox.Show(e.Message, "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }

        private string[] StartArchiveSession(string archive)
        {
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
                string[] tracks = File.ReadAllLines(playlist)
                    .Where(s => s.StartsWith("http://127.0.0.1:", StringComparison.Ordinal))
                    .ToArray();
                if (tracks.Length == 0)
                    throw new IOException("No audio entries were found in the selected archive.");
                lock (stateLock)
                {
                    processes.Add(process);
                    directories.Add(folder);
                    int entryPosition = tracks[0].IndexOf("/entry/", StringComparison.Ordinal);
                    shutdownUrls.Add(entryPosition > 0 ? tracks[0].Substring(0, entryPosition) + "/shutdown" : "");
                }
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

        public bool Configure(IntPtr panelHandle)
        {
            MessageBox.Show("Tools > Archive Stream: Add Archive Audio... opens ZIP, 7z or RAR audio.\n" +
                "The bridge uses the shared Winamp saved-password file under LocalAppData\\WinampArchiveStream\\keeps.txt.\n" +
                "Streams are available only while MusicBee and the bridge are running.",
                "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Information);
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
