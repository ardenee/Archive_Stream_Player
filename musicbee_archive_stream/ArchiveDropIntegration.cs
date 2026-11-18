// MusicBee's plugin SDK has no archive-specific drag/drop callback.
// Attach only to its existing WinForms drop targets (and main form) so that
// supported archives reuse the same native first, RAM second, disk-last
// queueing pipeline as the Tools > Add Archive Audio command.
using System;
using System.Collections.Generic;
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
        private bool archiveDropChangedRootAllowDrop;
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
            archiveDropRetry.Interval = 1000;
            archiveDropRetry.Tick += RetryArchiveDropIntegration;
            archiveDropRetry.Start();
            TryAttachArchiveDropIntegration();
        }

        private void RetryArchiveDropIntegration(object sender, EventArgs args)
        {
            if (archiveDropClosing || archiveDropRoot != null || ++archiveDropAttempts >= 45)
            {
                if (archiveDropRetry != null)
                    archiveDropRetry.Stop();
                if (archiveDropRoot == null && !archiveDropClosing)
                    Trace("ARCHIVE_DROP_HOOK_UNAVAILABLE reason=musicbee_form_not_found");
                return;
            }

            TryAttachArchiveDropIntegration();
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
                archiveDropChangedRootAllowDrop = !root.AllowDrop;
                if (archiveDropChangedRootAllowDrop)
                    root.AllowDrop = true;
                AttachArchiveDropControls(root, true);
                Trace("ARCHIVE_DROP_HOOK_READY main=" + root.GetType().Name +
                    " targets=" + archiveDropTargets.Count + " observed=" + archiveDropObserved.Count);
                if (archiveDropRetry != null)
                    archiveDropRetry.Stop();
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DROP_HOOK_FAILED type=" + error.GetType().Name);
                DetachArchiveDropControls();
            }
        }

        private void AttachArchiveDropControls(Control control, bool isRoot)
        {
            if (control == null || control.IsDisposed || archiveDropObserved.Contains(control))
                return;

            archiveDropObserved.Add(control);
            control.ControlAdded += ArchiveDropControlAdded;

            // Preserve every existing drop-enabled MusicBee target. Controls
            // without drop support are not changed, apart from the main form.
            if (isRoot || control.AllowDrop)
            {
                control.DragEnter += ArchiveDropDragEnter;
                control.DragOver += ArchiveDropDragEnter;
                control.DragDrop += ArchiveDropDragDrop;
                archiveDropTargets.Add(control);
                Trace("ARCHIVE_DROP_TARGET type=" + control.GetType().Name + " name=" + control.Name);
            }

            foreach (Control child in control.Controls)
                AttachArchiveDropControls(child, false);
        }

        private void ArchiveDropControlAdded(object sender, ControlEventArgs args)
        {
            if (!archiveDropClosing && archiveDropRoot != null && args.Control != null)
                AttachArchiveDropControls(args.Control, false);
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

        private async void ArchiveDropDragDrop(object sender, DragEventArgs args)
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

            if (archiveDropClosing || archives.Length == 0)
                return;

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
            args.Effect = DragDropEffects.Copy;
            Trace("ARCHIVE_DROP_ACCEPTED archives=" + archives.Length);
            foreach (string archive in archives)
            {
                if (archiveDropClosing)
                    break;

                try
                {
                    string[] tracks = await Task.Run(() => StartArchiveSession(archive));
                    if (archiveDropClosing || tracks.Length == 0)
                        continue;

                    bool queued = api.NowPlayingList_QueueFilesLast != null &&
                        api.NowPlayingList_QueueFilesLast(tracks);
                    Trace("ARCHIVE_DROP_QUEUE_RESULT queued=" + (queued ? "1" : "0") +
                        " tracks=" + tracks.Length + " extension=" + Path.GetExtension(archive));
                    if (queued)
                        StartPlaybackDiagnostics();
                    else
                        MessageBox.Show("MusicBee did not accept the archive tracks.",
                            "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Warning);
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

            if (archiveDropChangedRootAllowDrop && archiveDropRoot != null && !archiveDropRoot.IsDisposed)
                archiveDropRoot.AllowDrop = false;

            archiveDropChangedRootAllowDrop = false;
            archiveDropRoot = null;
            archiveDropTargets.Clear();
            archiveDropObserved.Clear();
        }
    }
}
