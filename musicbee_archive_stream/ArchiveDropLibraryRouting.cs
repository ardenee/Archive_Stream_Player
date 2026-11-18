// Route supported archive drops according to the MusicBee destination.
// Existing Playing Tracks drops remain queue-only. Music/Tracks imports the
// same persistent native zero-copy references into MusicBee's library.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        private enum ArchiveDropDestination
        {
            PlayingTracks,
            MusicLibrary
        }

        [DllImport("user32.dll", EntryPoint = "WindowFromPoint")]
        private static extern IntPtr ArchiveDropWindowFromPoint(Point screenPoint);

        [DllImport("user32.dll", EntryPoint = "EnumChildWindows")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumArchiveChildWindows(IntPtr parent, ArchiveWindowCallback callback, IntPtr state);

        [DllImport("user32.dll", EntryPoint = "GetParent")]
        private static extern IntPtr ArchiveGetParent(IntPtr window);

        [DllImport("user32.dll", EntryPoint = "IsWindowVisible")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool ArchiveIsWindowVisible(IntPtr window);

        [DllImport("user32.dll", EntryPoint = "GetWindowRect")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool ArchiveGetWindowRect(IntPtr window, out ArchiveWindowRect bounds);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate bool ArchiveWindowCallback(IntPtr window, IntPtr state);

        [StructLayout(LayoutKind.Sequential)]
        private struct ArchiveWindowRect
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
            public Rectangle ToRectangle()
            {
                return Rectangle.FromLTRB(Left, Top, Right, Bottom);
            }
        }

        // MusicBee uses custom WinForms track panels with empty window names
        // and no accessibility labels. Identify the actual side-by-side content
        // panes from their direct-child bounds, independently of SDK MainPanel.
        private static ArchiveDropDestination? SplitPaneDestination(Rectangle host, Rectangle[] panels, Point point)
        {
            if (!host.Contains(point) || panels == null)
                return null;

            var candidates = new List<Rectangle>();
            foreach (Rectangle pane in panels)
            {
                if (pane.Width < host.Width / 4 || pane.Height < host.Height / 2 ||
                    !host.Contains(pane))
                    continue;
                candidates.Add(pane);
            }
            candidates.Sort((a, b) => a.Left.CompareTo(b.Left));

            for (int i = 0; i < candidates.Count; ++i)
            {
                Rectangle tracks = candidates[i];
                // The navigator sits to the left, outside the track pane.
                if (tracks.Left < host.Left + host.Width / 10)
                    continue;

                for (int j = i + 1; j < candidates.Count; ++j)
                {
                    Rectangle playing = candidates[j];
                    int gap = playing.Left - tracks.Right;
                    int overlappingHeight = Math.Min(tracks.Bottom, playing.Bottom) -
                        Math.Max(tracks.Top, playing.Top);
                    if (gap < -8 || gap > host.Width / 20 ||
                        overlappingHeight < Math.Min(tracks.Height, playing.Height) / 2)
                        continue;

                    if (tracks.Contains(point))
                        return ArchiveDropDestination.MusicLibrary;
                    if (playing.Contains(point))
                        return ArchiveDropDestination.PlayingTracks;
                }
            }
            return null;
        }

        private ArchiveDropDestination? DirectChildPaneDestination(Point screenPoint)
        {
            if (archiveDropRoot == null || archiveDropRoot.IsDisposed ||
                !archiveDropRoot.IsHandleCreated)
                return null;

            var panels = new List<Rectangle>();
            IntPtr hostHandle = archiveDropRoot.Handle;
            Rectangle host = archiveDropRoot.RectangleToScreen(archiveDropRoot.ClientRectangle);
            ArchiveWindowCallback visit = (window, unused) =>
            {
                if (ArchiveGetParent(window) != hostHandle || !ArchiveIsWindowVisible(window))
                    return true;
                ArchiveWindowRect bounds;
                if (ArchiveGetWindowRect(window, out bounds))
                    panels.Add(bounds.ToRectangle());
                return true;
            };
            EnumArchiveChildWindows(hostHandle, visit, IntPtr.Zero);
            ArchiveDropDestination? result = SplitPaneDestination(host, panels.ToArray(), screenPoint);
            if (result.HasValue)
                Trace("ARCHIVE_DROP_DESTINATION method=win32_panes value=" + result.Value);
            else
                Trace("ARCHIVE_DROP_PANE_UNRESOLVED direct_children=" + panels.Count);
            return result;
        }

        private static ArchiveDropDestination? NamedDropDestination(Control control, Control root)
        {
            // Inspect the event target's own ancestor chain, not unrelated
            // labels elsewhere in MusicBee's main window.
            for (Control current = control; current != null && current != root; current = current.Parent)
            {
                string[] names = new string[3];
                try
                {
                    names[0] = current.AccessibleName;
                    names[1] = current.Name;
                    names[2] = current.Text;
                }
                catch (Exception)
                {
                    continue;
                }

                foreach (string raw in names)
                {
                    string label = (raw ?? String.Empty).Trim();
                    if (label.Equals("Playing Tracks", StringComparison.OrdinalIgnoreCase) ||
                        label.Equals("Now Playing", StringComparison.OrdinalIgnoreCase))
                        return ArchiveDropDestination.PlayingTracks;
                    if (label.Equals("Tracks", StringComparison.OrdinalIgnoreCase) ||
                        label.Equals("Music", StringComparison.OrdinalIgnoreCase) ||
                        label.Equals("Music Library", StringComparison.OrdinalIgnoreCase))
                        return ArchiveDropDestination.MusicLibrary;
                }
            }
            return null;
        }

        private ArchiveDropDestination ResolveArchiveDropDestination(Control origin, Point screenPoint)
        {
            if (archiveDropRoot == null || archiveDropRoot.IsDisposed)
                return ArchiveDropDestination.PlayingTracks;

            Control control = origin;
            try
            {
                IntPtr window = ArchiveDropWindowFromPoint(screenPoint);
                Control hit = window == IntPtr.Zero ? null : Control.FromChildHandle(window);
                if (hit != null && (hit == archiveDropRoot || archiveDropRoot.Contains(hit)))
                    control = hit;
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_HITTEST_FAIL type=" + error.GetType().Name);
            }

            ArchiveDropDestination? named = NamedDropDestination(control, archiveDropRoot);
            if (named.HasValue)
            {
                Trace("ARCHIVE_DROP_DESTINATION method=ancestor value=" + named.Value);
                return named.Value;
            }

            // The observed MusicBee layout uses two adjacent custom WinForms
            // content panes without names: Tracks is the central pane and
            // Playing Tracks is to its right. Resolve their real bounds before
            // attempting the optional SDK panel bounds mapping.
            try
            {
                ArchiveDropDestination? pane = DirectChildPaneDestination(screenPoint);
                if (pane.HasValue)
                    return pane.Value;
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_PANE_DETECT_FAILED type=" + error.GetType().Name);
            }

            // SDK MainPanel is the library's Tracks view in MusicBee's normal
            // side-by-side layout. MusicBee may return either client-relative
            // or screen-relative bounds depending on the host revision.
            try
            {
                if (api.MB_GetPanelBounds != null)
                {
                    Rectangle mainBounds = api.MB_GetPanelBounds(PluginPanelDock.MainPanel);
                    Rectangle hostBounds = archiveDropRoot.RectangleToScreen(archiveDropRoot.ClientRectangle);
                    Rectangle asScreen = archiveDropRoot.RectangleToScreen(mainBounds);
                    bool relativeValid = hostBounds.Contains(asScreen) &&
                        asScreen.Width < hostBounds.Width * 9 / 10 &&
                        asScreen.Height > 50;
                    bool absoluteValid = hostBounds.Contains(mainBounds) &&
                        mainBounds.Width < hostBounds.Width * 9 / 10 &&
                        mainBounds.Height > 50;
                    bool inLibrary = (relativeValid && asScreen.Contains(screenPoint)) ||
                        (absoluteValid && mainBounds.Contains(screenPoint));
                    Trace("ARCHIVE_DROP_PANEL_MAP main=" + mainBounds.ToString() +
                        " host=" + hostBounds.ToString() + " hit=" + screenPoint.ToString() +
                        " library=" + (inLibrary ? "1" : "0"));
                    if (inLibrary)
                        return ArchiveDropDestination.MusicLibrary;
                }
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_PANEL_MAP_FAIL type=" + error.GetType().Name);
            }

            // Unknown host view: preserve the already working queue behavior
            // rather than unexpectedly modifying the permanent music library.
            Trace("ARCHIVE_DROP_DESTINATION method=fallback value=PlayingTracks");
            return ArchiveDropDestination.PlayingTracks;
        }

        private static bool IsPersistentNativeLibraryTrack(string track)
        {
            if (String.IsNullOrWhiteSpace(track))
                return false;

            string path;
            try
            {
                path = Path.GetFullPath(track);
            }
            catch (Exception)
            {
                return false;
            }
            string storage = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "ArchiveStreamPlayer", "MusicBee", "v") + Path.DirectorySeparatorChar;
            return path.StartsWith(storage, StringComparison.OrdinalIgnoreCase) &&
                path.EndsWith(".mp3", StringComparison.OrdinalIgnoreCase) &&
                File.Exists(path) && File.Exists(Path.ChangeExtension(path, ".ast"));
        }

        // Library_AddFileToLibrary is the SDK-supported library import route;
        // no direct MusicBee DB writes or temporary/extracted files.
        private int ImportNativeArchiveTracksIntoLibrary(string[] tracks)
        {
            if (tracks == null || tracks.Length == 0 || api.Library_AddFileToLibrary == null)
            {
                Trace("ARCHIVE_LIBRARY_IMPORT_UNAVAILABLE reason=no_tracks_or_sdk");
                return 0;
            }

            int imported = 0;
            foreach (string path in tracks)
            {
                if (!IsPersistentNativeLibraryTrack(path))
                {
                    Trace("ARCHIVE_LIBRARY_IMPORT_SKIP reason=not_persistent_native_descriptor");
                    continue;
                }
                try
                {
                    string result = api.Library_AddFileToLibrary(path, LibraryCategory.Music);
                    bool accepted = !String.IsNullOrWhiteSpace(result);
                    if (accepted)
                        ++imported;
                    Trace("ARCHIVE_LIBRARY_IMPORT_RESULT accepted=" + (accepted ? "1" : "0") +
                        " extension=" + Path.GetExtension(path) +
                        " returned_same_path=" + (String.Equals(path, result, StringComparison.OrdinalIgnoreCase) ? "1" : "0") +
                        " returned_length=" + (result == null ? "null" : result.Length.ToString()));
                }
                catch (Exception error)
                {
                    Trace("ARCHIVE_LIBRARY_IMPORT_FAIL type=" + error.GetType().Name);
                }
            }

            if (imported > 0 && api.MB_RefreshPanels != null)
            {
                api.MB_RefreshPanels();

                // MusicBee may still be indexing the new library entries when
                // the immediate refresh runs. Ask the host to refresh once
                // more after its UI message queue has processed the import.
                if (!archiveDropClosing && archiveDropRoot != null && !archiveDropRoot.IsDisposed)
                {
                    var delayedRefresh = new System.Windows.Forms.Timer();
                    delayedRefresh.Interval = 750;
                    delayedRefresh.Tick += (sender, args) =>
                    {
                        delayedRefresh.Stop();
                        delayedRefresh.Dispose();
                        if (archiveDropClosing)
                            return;

                        try
                        {
                            if (api.MB_RefreshPanels != null)
                                api.MB_RefreshPanels();
                            Trace("ARCHIVE_LIBRARY_REFRESH_DELAYED");
                        }
                        catch (Exception refreshError)
                        {
                            Trace("ARCHIVE_LIBRARY_REFRESH_FAIL type=" + refreshError.GetType().Name);
                        }
                    };
                    delayedRefresh.Start();
                }
            }

            Trace("ARCHIVE_LIBRARY_IMPORT_SUMMARY imported=" + imported + " total=" + tracks.Length);

            // Diagnostics only: MusicBee may acknowledge a path without
            // displaying it in the current Tracks view. Query its actual
            // library and displayed lists once per import, never per track.
            // This potentially large query is disabled in normal operation.
            try
            {
                bool diagnose = false;
                using (var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\ArchiveStreamPlayer\MusicBee", false))
                {
                    diagnose = key != null && Convert.ToInt32(key.GetValue("DiagnosticLogging", 0)) == 1;
                }
                if (diagnose && api.Library_QueryFilesEx != null)
                {
                    var expected = new HashSet<string>(tracks, StringComparer.OrdinalIgnoreCase);
                    string[] allFiles;
                    bool libraryQuery = api.Library_QueryFilesEx("domain=Library", out allFiles);
                    int libraryMatches = 0;
                    if (libraryQuery && allFiles != null)
                    {
                        foreach (string file in allFiles)
                        {
                            if (expected.Contains(file))
                                ++libraryMatches;
                        }
                    }
                    Trace("ARCHIVE_LIBRARY_VERIFY scope=Library query_ok=" + (libraryQuery ? "1" : "0") +
                        " matches=" + libraryMatches + " expected=" + tracks.Length);

                    string[] displayed;
                    bool viewQuery = api.Library_QueryFilesEx("domain=DisplayedFiles", out displayed);
                    int displayedMatches = 0;
                    if (viewQuery && displayed != null)
                    {
                        foreach (string file in displayed)
                        {
                            if (expected.Contains(file))
                                ++displayedMatches;
                        }
                    }
                    Trace("ARCHIVE_LIBRARY_VERIFY scope=DisplayedFiles query_ok=" + (viewQuery ? "1" : "0") +
                        " matches=" + displayedMatches + " expected=" + tracks.Length);
                }
            }
            catch (Exception verifyError)
            {
                Trace("ARCHIVE_LIBRARY_VERIFY_FAIL type=" + verifyError.GetType().Name);
            }
            return imported;
        }
    }
}
