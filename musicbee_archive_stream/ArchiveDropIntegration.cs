// MusicBee's plugin SDK has no archive-specific drag/drop callback.
// Attach only to its existing WinForms drop targets (and main form) so that
// supported archives reuse the same native first, RAM second, disk-last
// queueing pipeline as the Tools > Add Archive Audio command.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        private readonly List<Control> archiveDropObserved = new List<Control>();
        private readonly List<Control> archiveDropTargets = new List<Control>();
        private System.Windows.Forms.Timer archiveDropRetry;
        private Control archiveDropRoot;
        private ArchiveOleDropRegistration archiveOleRoot;
        private bool archiveDropClosing;
        private int archiveDropAttempts;
        private string lastArchiveDropSignature = "";
        private DateTime lastArchiveDropTime = DateTime.MinValue;

        private static bool IsArchivePath(string path)
        {
            if (String.IsNullOrWhiteSpace(path) || !File.Exists(path))
                return false;
            string extension = Path.GetExtension(path);
            return extension.Equals(".zip", StringComparison.OrdinalIgnoreCase) ||
                extension.Equals(".7z", StringComparison.OrdinalIgnoreCase) ||
                extension.Equals(".rar", StringComparison.OrdinalIgnoreCase) ||
                extension.Equals(".001", StringComparison.OrdinalIgnoreCase);
        }

        private static string[] GetArchiveDropFiles(IDataObject data)
        {
            if (data == null || !data.GetDataPresent(DataFormats.FileDrop))
                return new string[0];

            string[] paths = data.GetData(DataFormats.FileDrop) as string[];
            if (paths == null || paths.Length == 0 || paths.Length > 32)
                return new string[0];

            // Do not take ownership of mixed or ordinary audio file drops.
            if (paths.Any(path => !IsArchivePath(path)))
                return new string[0];

            return paths.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        }

        private void StartArchiveDropIntegration()
        {
            if (archiveDropClosing || archiveDropRetry != null)
                return;

            archiveDropAttempts = 0;
            archiveDropRetry = new System.Windows.Forms.Timer();
            archiveDropRetry.Interval = 2500;
            archiveDropRetry.Tick += RetryArchiveDropIntegration;
            archiveDropRetry.Start();
            TryAttachArchiveDropIntegration();
        }

        private void RetryArchiveDropIntegration(object sender, EventArgs args)
        {
            if (archiveDropClosing)
                return;

            if (archiveDropRoot == null)
            {
                if (++archiveDropAttempts >= 45)
                {
                    archiveDropRetry.Stop();
                    Trace("ARCHIVE_DROP_HOOK_UNAVAILABLE reason=musicbee_form_not_found");
                    return;
                }
                TryAttachArchiveDropIntegration();
                return;
            }

            if (archiveDropRoot.IsDisposed)
            {
                DetachArchiveDropControls();
                StartArchiveDropIntegration();
                return;
            }

            // MusicBee creates and replaces panels after its initial startup.
            // Revisit the live control tree, without changing AllowDrop on any
            // host-owned control. Only register listeners where it is enabled.
            try
            {
                AttachArchiveDropControls(archiveDropRoot, true);
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_RESCAN_FAILED type=" + error.GetType().Name);
            }
        }

        private void TryAttachArchiveDropIntegration()
        {
            if (archiveDropClosing || archiveDropRoot != null || api.MB_GetWindowHandle == null)
                return;

            IntPtr window = api.MB_GetWindowHandle();
            if (window == IntPtr.Zero)
                return;

            Control root = Control.FromHandle(window);
            if (root == null || root.IsDisposed)
                return;

            if (root.InvokeRequired)
            {
                root.BeginInvoke((Action)TryAttachArchiveDropIntegration);
                return;
            }

            archiveDropRoot = root;
            try
            {
                // Never override MusicBee's own OLE drop target. Changing
                // AllowDrop on the main form can break normal MusicBee drops.
                AttachArchiveDropControls(root, true);
                // Register only where MusicBee has not already registered an
                // OLE drop target. Its existing targets and normal audio drops
                // are never revoked, replaced, or redirected.
                archiveOleRoot = ArchiveOleDropRegistration.TryRegister(root.Handle,
                    IsArchivePath, QueueArchiveDropFiles, Trace);
                Trace("ARCHIVE_DROP_HOOK_READY main=" + root.GetType().Name +
                    " targets=" + archiveDropTargets.Count + " observed=" + archiveDropObserved.Count);
                ScheduleArchivePlayerStartup();
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_HOOK_FAILED type=" + error.GetType().Name);
                DetachArchiveDropControls();
            }
        }

        private void AttachArchiveDropControls(Control control, bool isRoot)
        {
            if (control == null || control.IsDisposed)
                return;

            if (!archiveDropObserved.Contains(control))
            {
                archiveDropObserved.Add(control);
                control.ControlAdded += ArchiveDropControlAdded;
            }

            if (control.AllowDrop && !archiveDropTargets.Contains(control))
            {
                control.DragEnter += ArchiveDropDragEnter;
                control.DragOver += ArchiveDropDragEnter;
                control.DragDrop += ArchiveDropDragDrop;
                archiveDropTargets.Add(control);
                Trace("ARCHIVE_DROP_TARGET_ADDED type=" + control.GetType().Name);
            }
            else if (!control.AllowDrop && archiveDropTargets.Contains(control))
            {
                control.DragEnter -= ArchiveDropDragEnter;
                control.DragOver -= ArchiveDropDragEnter;
                control.DragDrop -= ArchiveDropDragDrop;
                archiveDropTargets.Remove(control);
            }

            // Newly created and newly enabled controls may not have existed
            // during startup. Recheck their current drop registration.
            foreach (Control child in control.Controls.Cast<Control>().ToArray())
                AttachArchiveDropControls(child, false);
        }

        private void ArchiveDropControlAdded(object sender, ControlEventArgs args)
        {
            if (archiveDropClosing || archiveDropRoot == null || args.Control == null)
                return;

            try
            {
                AttachArchiveDropControls(args.Control, false);
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_NEW_CONTROL_FAILED type=" + error.GetType().Name);
            }
        }

        private void ArchiveDropDragEnter(object sender, DragEventArgs args)
        {
            try
            {
                if (GetArchiveDropFiles(args.Data).Length != 0)
                    args.Effect = DragDropEffects.Copy;
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_ENTER_FAILED type=" + error.GetType().Name);
            }
        }

        private void ArchiveDropDragDrop(object sender, DragEventArgs args)
        {
            string[] archives;
            try
            {
                archives = GetArchiveDropFiles(args.Data);
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_READ_FAILED type=" + error.GetType().Name);
                return;
            }

            if (archiveDropClosing)
                return;
            if (archives.Length == 0)
            {
                if (args.Data != null && args.Data.GetDataPresent(DataFormats.FileDrop))
                    Trace("ARCHIVE_DROP_IGNORED reason=not_archive_or_mixed_file_drop");
                return;
            }

            args.Effect = DragDropEffects.Copy;
            QueueArchiveDropFiles(archives, new Point(args.X, args.Y), sender as Control);
        }

        // The OLE root target has no WinForms sender; resolve its screen point
        // against MusicBee's current child control and SDK panel bounds.
        private void QueueArchiveDropFiles(string[] archives, Point dropPoint)
        {
            QueueArchiveDropFiles(archives, dropPoint, null);
        }

        private async void QueueArchiveDropFiles(string[] archives, Point dropPoint, Control origin)
        {
            if (archiveDropClosing || archives == null || archives.Length == 0)
                return;
            ArchiveDropDestination destination = ResolveArchiveDropDestination(origin, dropPoint);

            // Multiple WinForms handlers and the root OLE handler may both see
            // the same user gesture; never enqueue an archive twice.
            string signature = String.Join("|", archives);
            DateTime now = DateTime.UtcNow;
            if (signature.Equals(lastArchiveDropSignature, StringComparison.OrdinalIgnoreCase) &&
                (now - lastArchiveDropTime).TotalMilliseconds < 1500)
            {
                Trace("ARCHIVE_DROP_DUPLICATE_IGNORED files=" + archives.Length);
                return;
            }

            lastArchiveDropSignature = signature;
            lastArchiveDropTime = now;
            Trace("ARCHIVE_DROP_ACCEPTED archives=" + archives.Length + " destination=" + destination);
            foreach (string archive in archives)
            {
                if (archiveDropClosing)
                    break;

                try
                {
                    string[] tracks = await Task.Run(() => StartArchiveSession(archive));
                    if (archiveDropClosing || tracks.Length == 0)
                        continue;

                    // Archive files always open the dedicated, persistent
                    // MusicBee playlist tab, regardless of which host pane
                    // accepted the original archive drop.
                    if (OpenArchivePlayerTab(tracks))
                    {
                        Trace("ARCHIVE_DROP_TAB_RESULT success=1 tracks=" + tracks.Length +
                            " extension=" + Path.GetExtension(archive));
                    }
                    else
                    {
                        // Keep the previously working native queue as a
                        // compatibility fallback if a host lacks playlist APIs.
                        bool queued = api.NowPlayingList_QueueFilesLast != null &&
                            api.NowPlayingList_QueueFilesLast(tracks);
                        Trace("ARCHIVE_DROP_FALLBACK_QUEUE queued=" + (queued ? "1" : "0") +
                            " tracks=" + tracks.Length + " extension=" + Path.GetExtension(archive) +
                            " original_destination=" + destination);
                        if (queued)
                            StartPlaybackDiagnostics();
                        else
                            MessageBox.Show("MusicBee could not add these archive tracks.",
                                "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    }
                }
                catch (Exception error)
                {
                    Trace("ARCHIVE_DROP_QUEUE_FAILED extension=" + Path.GetExtension(archive) +
                        " type=" + error.GetType().Name);
                    MessageBox.Show(error.Message, "Archive Stream Player",
                        MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
            }
        }

        private void DetachArchiveDropControls()
        {
            if (archiveOleRoot != null)
            {
                archiveOleRoot.Dispose();
                archiveOleRoot = null;
            }
            if (archiveDropRetry != null)
            {
                archiveDropRetry.Stop();
                archiveDropRetry.Tick -= RetryArchiveDropIntegration;
                archiveDropRetry.Dispose();
                archiveDropRetry = null;
            }

            foreach (Control target in archiveDropTargets)
            {
                if (target == null || target.IsDisposed)
                    continue;
                target.DragEnter -= ArchiveDropDragEnter;
                target.DragOver -= ArchiveDropDragEnter;
                target.DragDrop -= ArchiveDropDragDrop;
            }

            foreach (Control observed in archiveDropObserved)
            {
                if (observed != null && !observed.IsDisposed)
                    observed.ControlAdded -= ArchiveDropControlAdded;
            }

            archiveDropRoot = null;
            archiveDropTargets.Clear();
            archiveDropObserved.Clear();
        }
    }
}
