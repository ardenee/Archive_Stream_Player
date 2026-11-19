// Official MusicBee v3.1+ plugin SDK partial class. No MusicBee SDK source is bundled.
// Native BASS streams archive bytes directly; HTTP/local compatibility is retained.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
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
        private static readonly object persistentTrackLock = new object();
        private System.Windows.Forms.Timer playbackTraceTimer;
        private bool nativeMp3Loaded;
        private const uint BassUnicode = 0x80000000u;

        // MusicBee cannot queue our private .ast descriptor extension as an
        // audio source. A Win32 hard link gives it a recognised .mp3 filename
        // without copying or extracting compressed archive bytes.
        [DllImport("kernel32.dll", EntryPoint = "CreateHardLinkW", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateNativeDescriptorHardLink(string alias, string descriptor, IntPtr securityAttributes);

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

        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamWriteNativeDiskMfTracksW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int WriteNativeDiskMfTracks(string archive, string folder);

        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamWriteNativeEncryptedTracksW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int WriteNativeEncryptedTracks(string archive, string folder);

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
                // Enable focused MusicBee diagnostics without turning on
                // verbose logging for every other player adapter.
                using (RegistryKey diagnostic = Registry.CurrentUser.OpenSubKey(@"Software\ArchiveStreamPlayer\MusicBee", false))
                {
                    if (diagnostic != null && Convert.ToInt32(diagnostic.GetValue("DiagnosticLogging", 0)) == 1)
                        enabled = true;
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
                    bool persistentNative = IsPersistentNativeLibraryTrack(source);
                    bool local = source.IndexOf("ArchiveStreamMusicBee-", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        persistentNative;
                    if (!http && !local)
                        return;
                    string codec = Path.GetExtension(http ? uri.AbsolutePath : source).ToLowerInvariant();
                    int queueIndex = api.NowPlayingList_GetCurrentIndex == null ? -1 : api.NowPlayingList_GetCurrentIndex();
                    int position = api.Player_GetPosition == null ? -1 : api.Player_GetPosition();
                    string state = api.Player_GetPlayState == null ? "unknown" : api.Player_GetPlayState().ToString();
                    Trace("HOST_PLAYBACK index=" + queueIndex + " codec=" + codec + " route=" +
                        (http ? "HTTP" : (persistentNative ? "NATIVE_PERSISTENT" :
                            (source.EndsWith(".ast", StringComparison.OrdinalIgnoreCase) ? "NATIVE" : "LOCAL"))) +
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
            info.VersionMinor = 17;
            info.Revision = 0;
            info.MinInterfaceVersion = MinInterfaceVersion;
            info.MinApiRevision = MinApiRevision;
            // Library double-clicks and Play commands need to reach the same
            // BASS archive decoder as Playing Tracks. Observe host playback
            // events so we can distinguish a library open from a decoder fail.
            info.ReceiveNotifications = ReceiveNotificationFlags.PlayerEvents;
            info.ConfigurationPanelHeight = 0;
            if (api.MB_AddMenuItem != null)
            {
                var item = api.MB_AddMenuItem("mnuTools/Archive Stream: Add Archive Audio...",
                    "Add audio entries from a ZIP, 7z or RAR archive", OnAddArchive);
                Trace(item == null ? "MENU_ITEM_FAILED" : "MENU_ITEM_REGISTERED");
                api.MB_AddMenuItem("mnuTools/Archive Stream: Settings...",
                    "Configure shared titles, cache cleanup, passwords and diagnostic logging", OnShowSettings);
                api.MB_AddMenuItem("mnuTools/Archive Stream: Open Archive Player",
                    "Open or restore the Archive Player tab", OnOpenArchivePlayer);
                api.MB_AddMenuItem("mnuTools/Archive Stream: Verified Durations",
                    "Open a separate MusicBee-styled window with verified archive track times",
                    ShowArchiveVerifiedView);
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
                    if (tracks.Length == 0)
                    {
                        Trace("NATIVE_PASSWORD_NO_QUEUE bridge=0");
                        return;
                    }
                    bool opened = OpenArchivePlayerTab(tracks);
                    Trace("ADD_ARCHIVE_PLAYLIST_RESULT updated=" + (opened ? "1" : "0") +
                        " tracks=" + tracks.Length + " tab_navigation=host_owned");
                    if (!opened)
                    {
                        bool queued = api.NowPlayingList_QueueFilesLast != null &&
                            api.NowPlayingList_QueueFilesLast(tracks);
                        Trace("ADD_ARCHIVE_FALLBACK_QUEUE queued=" + (queued ? "1" : "0") +
                            " tracks=" + tracks.Length);
                        if (queued)
                            StartPlaybackDiagnostics();
                        else
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

        private static string[] CreateMusicBeePlayableAliases(List<string> descriptors)
        {
            var aliases = new List<string>(descriptors.Count);
            bool nativeId3 = NativeSupportsId3Alias();
            bool useTags = UseArchiveFileTags();
            int id3Count = 0;
            foreach (string descriptor in descriptors)
            {
                if (!descriptor.EndsWith(".ast", StringComparison.OrdinalIgnoreCase) || !File.Exists(descriptor))
                {
                    Trace("NATIVE_ALIAS_FAIL reason=missing_descriptor");
                    return null;
                }

                string alias = Path.ChangeExtension(descriptor, ".mp3");
                if (File.Exists(alias))
                {
                    Trace("NATIVE_ALIAS_FAIL reason=alias_already_exists");
                    return null;
                }

                bool created = false;
                if (nativeId3)
                {
                    try
                    {
                        CreateId3ManifestAlias(descriptor, alias, useTags);
                        created = true;
                        ++id3Count;
                    }
                    catch (Exception error)
                    {
                        Trace("NATIVE_ID3_ALIAS_FALLBACK reason=" + error.GetType().Name);
                        if (File.Exists(alias))
                            File.Delete(alias);
                    }
                }
                if (!created && !CreateNativeDescriptorHardLink(alias, descriptor, IntPtr.Zero))
                {
                    Trace("NATIVE_ALIAS_FAIL reason=metadata_alias_unavailable win32=" + Marshal.GetLastWin32Error());
                    return null;
                }
                aliases.Add(alias);
            }

            Trace("NATIVE_ALIAS_READY count=" + aliases.Count + " id3=" + id3Count +
                " fallback_hardlinks=" + (aliases.Count - id3Count) +
                " use_tags=" + (useTags ? "1" : "0") + " stream=1");
            return aliases.ToArray();
        }

        // MusicBee saves the paths in Playing Tracks across launches. Native
        // descriptors and their zero-copy aliases must not live in TEMP.
        // Only descriptor metadata is persisted: audio stays in the archive.
        private static string[] PersistNativePlayableTracks(string archive, string[] temporaryAliases)
        {
            var source = new FileInfo(Path.GetFullPath(archive));
            if (!source.Exists)
                throw new FileNotFoundException("Archive no longer exists.", source.FullName);

            // Hash only the archive's identity and metadata, never audio bytes
            // or a recalculated content CRC from the compressed entries.
            string identity = source.FullName.ToUpperInvariant() + "\n" +
                source.Length.ToString(System.Globalization.CultureInfo.InvariantCulture) + "\n" +
                source.LastWriteTimeUtc.Ticks.ToString(System.Globalization.CultureInfo.InvariantCulture);
            string key;
            using (SHA256 algorithm = SHA256.Create())
            {
                byte[] hash = algorithm.ComputeHash(Encoding.UTF8.GetBytes(identity));
                key = BitConverter.ToString(hash).Replace("-", "").Substring(0, 24).ToLowerInvariant();
            }

            string appData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (String.IsNullOrWhiteSpace(appData))
                throw new IOException("The local application data location is unavailable.");

            // Use a compact on-disk key: long audio titles and Win32 MAX_PATH
            // must still have room for a temporary metadata filename.
            string stableFolder = Path.Combine(appData, "ArchiveStreamPlayer", "MusicBee", "v", key);
            var paths = new List<string>(temporaryAliases.Length);
            lock (persistentTrackLock)
            {
                Directory.CreateDirectory(stableFolder);
                foreach (string temporaryAlias in temporaryAliases)
                {
                    string temporaryDescriptor = Path.ChangeExtension(temporaryAlias, ".ast");
                    if (!File.Exists(temporaryDescriptor))
                        throw new FileNotFoundException("The temporary archive descriptor is missing.", temporaryDescriptor);

                    string persistentDescriptor = Path.Combine(stableFolder, Path.GetFileName(temporaryDescriptor));
                    string persistentAlias = Path.ChangeExtension(persistentDescriptor, ".mp3");
                    if (File.Exists(persistentDescriptor))
                    {
                        // Keep existing playlist paths bound to their original
                        // entry rather than silently retargeting on a reimport.
                        if (!String.Equals(File.ReadAllText(persistentDescriptor, Encoding.UTF8),
                                File.ReadAllText(temporaryDescriptor, Encoding.UTF8), StringComparison.Ordinal))
                            throw new IOException("An existing persistent archive descriptor has different content.");
                    }
                    else
                    {
                        string pending = persistentDescriptor + "~" + Guid.NewGuid().ToString("N").Substring(0, 8);
                        try
                        {
                            File.Copy(temporaryDescriptor, pending, false);
                            File.Move(pending, persistentDescriptor);
                        }
                        finally
                        {
                            if (File.Exists(pending))
                                File.Delete(pending);
                        }
                    }

                    // The alias now contains a valid ID3 metadata block and
                    // the exact original .ast text as a verified trailing
                    // reference. Never overwrite the .ast through a hard link.
                    if (IsId3Alias(temporaryAlias))
                    {
                        // Fresh MusicBee URLs let its own ID3 reader populate
                        // Title / Artist / Album / Track No. Legacy URLs and
                        // playback queues remain valid alongside the new ones.
                        string versioned = VersionedMusicBeeAlias(persistentDescriptor,
                            UseArchiveFileTags(), temporaryAlias);
                        paths.Add(versioned);
                    }
                    else
                    {
                        PersistMusicBeeAlias(temporaryAlias, persistentAlias);
                        if (!File.Exists(persistentAlias))
                            throw new IOException("Failed to persist a native archive alias.");
                        paths.Add(persistentAlias);
                    }
                }
            }
            Trace("NATIVE_PERSIST_READY root=LOCALAPPDATA tracks=" + paths.Count + " source=" + Path.GetExtension(archive));
            return paths.ToArray();
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
                                    try
                                    {
                                        try
                                        {
                                            count = WriteNativeEncryptedTracks(archive, folder);
                                        }
                                        catch (EntryPointNotFoundException)
                                        {
                                            count = WriteNativeDiskMfTracks(archive, folder);
                                        }
                                    }
                                    catch (EntryPointNotFoundException)
                                    {
                                        count = WriteNativeDiskOggTracks(archive, folder);
                                    }
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
                if (count == -2)
                {
                    Trace("NATIVE_PASSWORD_CANCELLED bridge=0");
                    return new string[0];
                }
                if (count == -3)
                {
                    Trace("NATIVE_PASSWORD_REJECTED bridge=0");
                    return new string[0];
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
                string[] playable = CreateMusicBeePlayableAliases(tracks);
                if (playable == null)
                {
                    Trace("NATIVE_ALIAS_FALLBACK reason=musicbee_requires_audio_extension");
                    return null;
                }
                try
                {
                    string[] stableTracks = PersistNativePlayableTracks(archive, playable);
                    // Archive container order is not album track order.
                    // Sort just this newly imported archive; never reorder a
                    // different user's MusicBee playlist or previously added album.
                    stableTracks = OrderArchiveTracks(stableTracks);
                    Trace("NATIVE_MANIFEST_READY mode=AUDIO archive=" + Path.GetExtension(archive).ToLowerInvariant() + " tracks=" + stableTracks.Length + " ordering=track_tags_then_filename helper=DLL http=0 persistence=LOCALAPPDATA");
                    return stableTracks;
                }
                catch (Exception persistenceError)
                {
                    // An empty (not null) result prevents the compatibility
                    // route from silently adding another ephemeral path.
                    Trace("NATIVE_PERSIST_FAIL type=" + persistenceError.GetType().Name);
                    return new string[0];
                }
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
                // The native queue uses stable aliases. The temporary folder
                // contains only enumeration staging files, never playlist URLs.
                try
                {
                    Directory.Delete(folder, true);
                }
                catch (Exception cleanupError)
                {
                    Trace("NATIVE_STAGING_CLEANUP_SKIP type=" + cleanupError.GetType().Name);
                }
                Trace("SESSION_READY route=NATIVE tracks=" + nativeTracks.Length + " persistent=1");
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
            var info = new ProcessStartInfo("powershell.exe", "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" + script + "\"");
            info.UseShellExecute = false;
            info.CreateNoWindow = true;
            info.WindowStyle = ProcessWindowStyle.Hidden;
            Process.Start(info);
            return true;
        }

        public void SaveSettings() {}
        public void Uninstall() {}
        public void ReceiveNotification(string sourceFileUrl, NotificationType type)
        {
            if (type == NotificationType.PluginStartup || type == NotificationType.MusicBeeStarted)
            {
                StartArchiveDropIntegration();
                return;
            }

            if (type == NotificationType.PlaylistUpdated || type == NotificationType.PlaylistCreated)
            {
                Control root = archiveDropRoot;
                if (!archiveDropClosing && root != null && !root.IsDisposed && root.IsHandleCreated)
                {
                    try
                    {
                        // PlaylistUpdated may be raised synchronously while
                        // MusicBee is still committing the list. Defer the
                        // guard until the current UI operation has returned.
                        root.BeginInvoke((Action)GuardArchivePlayerPlaylist);
                    }
                    catch (InvalidOperationException) { }
                }
                return;
            }

            // MusicBee's normal Tracks double-click sends a player event.
            // The persisted .mp3 hard link is opened by our native BASS addon;
            // do not hijack normal MP3 playback or enqueue a second copy.
            if (type == NotificationType.TrackChanging ||
                type == NotificationType.TrackChanged ||
                type == NotificationType.PlayStateChanged)
            {
                string source = sourceFileUrl ?? String.Empty;
                if (source.Length == 0 && api.NowPlaying_GetFileUrl != null)
                    source = api.NowPlaying_GetFileUrl() ?? String.Empty;
                if (!IsPersistentNativeLibraryTrack(source))
                    return;

                string state = api.Player_GetPlayState == null ? "unknown" : api.Player_GetPlayState().ToString();
                Trace("NATIVE_LIBRARY_PLAYBACK_EVENT event=" + type + " state=" + state +
                    " source=LOCALAPPDATA_ALIAS decoder=BASS_ARCHIVE");
                if (type == NotificationType.TrackChanged)
                {
                    try
                    {
                        int reportedMs = api.NowPlaying_GetDuration == null ? -1 : api.NowPlaying_GetDuration();
                        string stored = api.Library_GetFileProperty == null ? "" :
                            api.Library_GetFileProperty(source, FilePropertyType.Duration);
                        Trace("ARCHIVE_DURATION_HOST_PROPERTY decoder_ms=" + reportedMs +
                            " library_duration=" + stored + " api=READ_ONLY");
                    }
                    catch (Exception error)
                    {
                        Trace("ARCHIVE_DURATION_HOST_PROPERTY_FAIL type=" + error.GetType().Name);
                    }
                }
                // Metadata appears immediately. Start the low-priority
                // duration verification only when real playback begins,
                // and only for this archive's tracks in Archive Player.
                if (type == NotificationType.TrackChanged ||
                    (type == NotificationType.PlayStateChanged &&
                     api.Player_GetPlayState != null &&
                     api.Player_GetPlayState() == PlayState.Playing))
                    ScheduleArchiveDurationScanOnPlayback(source);

                StartPlaybackDiagnostics();
            }
        }

        public void Close(PluginCloseReason reason)
        {
            archiveDropClosing = true;
            try
            {
                if (archiveDropRoot != null && archiveDropRoot.InvokeRequired)
                    archiveDropRoot.Invoke((Action)DetachArchiveDropControls);
                else
                    DetachArchiveDropControls();
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_HOOK_REMOVE_FAILED type=" + error.GetType().Name);
            }
            // The optional verified-time view is a MusicBee SDK-owned panel.
            // Remove only that panel, never other user tabs or playlists.
            try { CloseArchiveVerifiedView(); }
            catch (Exception error)
            {
                Trace("ARCHIVE_VERIFIED_VIEW_CLOSE_FAIL type=" + error.GetType().Name);
            }
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
