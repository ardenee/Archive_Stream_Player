// Register an archive-only OLE IDropTarget on an unregistered MusicBee host
// window. Do not replace or revoke any drop target owned by MusicBee.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;

namespace MusicBeePlugin
{
    internal sealed class ArchiveOleDropRegistration : IDisposable
    {
        private const int DragDropAlreadyRegistered = unchecked((int)0x80040101);
        private const int DragDropNotRegistered = unchecked((int)0x80040100);
        private const int Succeeded = 0;
        private const int DropEffectNone = 0;
        private const int DropEffectCopy = 1;
        private const uint QueryFileCount = 0xFFFFFFFFu;
        private const short ClipboardFileDrop = 15;
        private const int MaxFiles = 32;
        private readonly IntPtr window;
        private readonly NativeDropTarget target;
        private readonly Action<string> trace;
        private bool owned;

        [StructLayout(LayoutKind.Sequential)]
        private struct PointL
        {
            public int X;
            public int Y;
        }

        [ComVisible(true)]
        [Guid("00000122-0000-0000-C000-000000000046")]
        [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IOleDropTarget
        {
            [PreserveSig]
            int DragEnter([MarshalAs(UnmanagedType.Interface)] System.Runtime.InteropServices.ComTypes.IDataObject data, int keys, PointL point, ref int effect);

            [PreserveSig]
            int DragOver(int keys, PointL point, ref int effect);

            [PreserveSig]
            int DragLeave();

            [PreserveSig]
            int Drop([MarshalAs(UnmanagedType.Interface)] System.Runtime.InteropServices.ComTypes.IDataObject data, int keys, PointL point, ref int effect);
        }

        [DllImport("ole32.dll", PreserveSig = true)]
        private static extern int RegisterDragDrop(IntPtr hwnd, [MarshalAs(UnmanagedType.Interface)] IOleDropTarget dropTarget);

        [DllImport("ole32.dll", PreserveSig = true)]
        private static extern int RevokeDragDrop(IntPtr hwnd);

        [DllImport("ole32.dll")]
        private static extern void ReleaseStgMedium(ref STGMEDIUM medium);

        [DllImport("shell32.dll", CharSet = CharSet.Unicode, EntryPoint = "DragQueryFileW")]
        private static extern uint DragQueryFile(IntPtr dropHandle, uint index, StringBuilder path, uint capacity);

        private static string[] GetArchiveFiles(System.Runtime.InteropServices.ComTypes.IDataObject data, Func<string, bool> isArchive)
        {
            if (data == null)
                return null;

            var request = new FORMATETC
            {
                cfFormat = ClipboardFileDrop,
                dwAspect = DVASPECT.DVASPECT_CONTENT,
                lindex = -1,
                tymed = TYMED.TYMED_HGLOBAL
            };
            if (data.QueryGetData(ref request) != 0)
                return null;

            STGMEDIUM medium;
            data.GetData(ref request, out medium);
            try
            {
                if (medium.tymed != TYMED.TYMED_HGLOBAL || medium.unionmember == IntPtr.Zero)
                    return null;

                uint count = DragQueryFile(medium.unionmember, QueryFileCount, null, 0);
                if (count == 0 || count > MaxFiles)
                    return null;

                var paths = new List<string>((int)count);
                var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                for (uint i = 0; i < count; ++i)
                {
                    uint length = DragQueryFile(medium.unionmember, i, null, 0);
                    if (length == 0 || length > 32767)
                        return null;

                    var buffer = new StringBuilder((int)length + 1);
                    if (DragQueryFile(medium.unionmember, i, buffer, (uint)buffer.Capacity) == 0)
                        return null;

                    string path = buffer.ToString();
                    // A mixed drop belongs to MusicBee. Never claim or remove
                    // ordinary audio files, directories, or unsupported types.
                    if (!isArchive(path))
                        return null;

                    if (seen.Add(path))
                        paths.Add(path);
                }
                return paths.ToArray();
            }
            finally
            {
                ReleaseStgMedium(ref medium);
            }
        }

        [ComVisible(true)]
        [ClassInterface(ClassInterfaceType.None)]
        private sealed class NativeDropTarget : IOleDropTarget
        {
            private readonly Func<string, bool> isArchive;
            private readonly Action<string[], Point> accept;
            private readonly Action<string> trace;
            private bool validDrag;

            public NativeDropTarget(Func<string, bool> isArchive, Action<string[], Point> accept, Action<string> trace)
            {
                this.isArchive = isArchive;
                this.accept = accept;
                this.trace = trace;
            }

            public int DragEnter(System.Runtime.InteropServices.ComTypes.IDataObject data, int keys, PointL point, ref int effect)
            {
                validDrag = false;
                try
                {
                    var files = GetArchiveFiles(data, isArchive);
                    validDrag = files != null && files.Length != 0;
                    if (validDrag)
                        trace("ARCHIVE_OLE_ENTER accepted=1 count=" + files.Length);
                }
                catch (Exception error)
                {
                    trace("ARCHIVE_OLE_ENTER_FAILED type=" + error.GetType().Name);
                }
                effect = validDrag && (effect & DropEffectCopy) != 0 ? DropEffectCopy : DropEffectNone;
                return Succeeded;
            }

            public int DragOver(int keys, PointL point, ref int effect)
            {
                effect = validDrag && (effect & DropEffectCopy) != 0 ? DropEffectCopy : DropEffectNone;
                return Succeeded;
            }

            public int DragLeave()
            {
                validDrag = false;
                return Succeeded;
            }

            public int Drop(System.Runtime.InteropServices.ComTypes.IDataObject data, int keys, PointL point, ref int effect)
            {
                bool accepted = validDrag && (effect & DropEffectCopy) != 0;
                validDrag = false;
                if (!accepted)
                {
                    effect = DropEffectNone;
                    return Succeeded;
                }
                try
                {
                    var archives = GetArchiveFiles(data, isArchive);
                    if (archives == null || archives.Length == 0)
                    {
                        effect = DropEffectNone;
                        return Succeeded;
                    }

                    effect = DropEffectCopy;
                    trace("ARCHIVE_OLE_DROP accepted=1 archives=" + archives.Length);
                    accept(archives, new Point(point.X, point.Y));
                }
                catch (Exception error)
                {
                    effect = DropEffectNone;
                    trace("ARCHIVE_OLE_DROP_FAILED type=" + error.GetType().Name);
                }
                return Succeeded;
            }
        }

        private ArchiveOleDropRegistration(IntPtr hwnd, NativeDropTarget native, Action<string> onTrace)
        {
            window = hwnd;
            target = native;
            trace = onTrace;
        }

        public static ArchiveOleDropRegistration TryRegister(IntPtr window, Func<string, bool> isArchive, Action<string[], Point> onArchive, Action<string> trace)
        {
            if (window == IntPtr.Zero)
                return null;

            var registration = new ArchiveOleDropRegistration(window, new NativeDropTarget(isArchive, onArchive, trace), trace);
            int result = RegisterDragDrop(window, registration.target);
            if (result == Succeeded)
            {
                registration.owned = true;
                trace("ARCHIVE_OLE_ROOT_REGISTERED status=ok");
                return registration;
            }

            if (result == DragDropAlreadyRegistered)
                trace("ARCHIVE_OLE_ROOT_SKIPPED reason=musicbee_already_owns_drop_target");
            else
                trace("ARCHIVE_OLE_ROOT_REGISTRATION_FAILED hresult=0x" + result.ToString("X8"));
            return null;
        }

        public void Dispose()
        {
            if (!owned)
                return;

            owned = false;
            int result = RevokeDragDrop(window);
            if (result != Succeeded && result != DragDropNotRegistered)
                trace("ARCHIVE_OLE_ROOT_REVOKE_FAILED hresult=0x" + result.ToString("X8"));
        }
    }
}
