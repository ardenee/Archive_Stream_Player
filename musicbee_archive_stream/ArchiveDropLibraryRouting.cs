// Route supported archive drops according to the MusicBee destination.
// Existing Playing Tracks drops remain queue-only. Music/Tracks imports the
// same persistent native zero-copy references into MusicBee's library.
using System;
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
                        " extension=" + Path.GetExtension(path));
                }
                catch (Exception error)
                {
                    Trace("ARCHIVE_LIBRARY_IMPORT_FAIL type=" + error.GetType().Name);
                }
            }

            if (imported > 0 && api.MB_RefreshPanels != null)
                api.MB_RefreshPanels();

            Trace("ARCHIVE_LIBRARY_IMPORT_SUMMARY imported=" + imported + " total=" + tracks.Length);
            return imported;
        }
    }
}
