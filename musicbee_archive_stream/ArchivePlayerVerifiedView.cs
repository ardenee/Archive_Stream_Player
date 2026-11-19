// Archive Stream Player / MusicBee independent verified-time window.
// MusicBee's native FilePropertyType.Duration is read-only and the host
// does not derive Time from ID3 TLEN on our audio-free aliases. This view
// instead displays exactly verified duration-cache values alongside the
// actual ID3 metadata, without touching the Music library or the native
// Archive Player playlist. It is opt-in to avoid duplicate tabs.
using System;
using System.Drawing;
using System.IO;
using System.Text;
using System.Windows.Forms;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        private Form archiveVerifiedView;
        private DataGridView archiveVerifiedGrid;
        private Label archiveVerifiedSummary;

        private static string ReadArchiveAliasTag(byte[] bytes, string target, int tagLength)
        {
            int pos = 10;
            while (pos + 10 <= tagLength)
            {
                string type = Encoding.ASCII.GetString(bytes, pos, 4);
                int size = (bytes[pos + 4] << 24) | (bytes[pos + 5] << 16) |
                    (bytes[pos + 6] << 8) | bytes[pos + 7];
                if (size < 1 || pos + 10 + size > tagLength)
                    break;
                if (type == target)
                {
                    int start = pos + 10;
                    if (bytes[start] == 1 && size >= 3)
                    {
                        int skip = 1;
                        if (bytes[start + 1] == 0xff && bytes[start + 2] == 0xfe)
                            skip = 3;
                        return Encoding.Unicode.GetString(bytes, start + skip, size - skip)
                            .TrimEnd('\0').Trim();
                    }
                    if (bytes[start] == 0 && size > 1)
                        return Encoding.GetEncoding(28591).GetString(bytes, start + 1, size - 1)
                            .TrimEnd('\0').Trim();
                    return String.Empty;
                }
                pos += 10 + size;
            }
            return String.Empty;
        }

        private static string[] ReadArchiveAliasDisplayFields(string alias)
        {
            string[] output = { "", "", "", "", "" }; // title, artist, album, number, genre
            try
            {
                var file = new FileInfo(alias);
                if (!file.Exists || file.Length < 42 || file.Length > 65536)
                    return output;
                byte[] bytes = File.ReadAllBytes(alias);
                if (Encoding.ASCII.GetString(bytes, 0, 3) != "ID3")
                    return output;
                int length = 10 + (bytes[6] << 21) + (bytes[7] << 14) +
                    (bytes[8] << 7) + bytes[9];
                if (length > bytes.Length || length > 16384)
                    return output;
                output[0] = ReadArchiveAliasTag(bytes, "TIT2", length);
                output[1] = ReadArchiveAliasTag(bytes, "TPE1", length);
                output[2] = ReadArchiveAliasTag(bytes, "TALB", length);
                output[3] = ReadArchiveAliasTag(bytes, "TRCK", length);
                output[4] = ReadArchiveAliasTag(bytes, "TCON", length);
            }
            catch (Exception)
            {
                // Unsupported or damaged aliases never break the view.
            }
            return output;
        }

        private static string FormatVerifiedArchiveDuration(long milliseconds)
        {
            if (milliseconds <= 0)
                return "—";
            long seconds = milliseconds / 1000;
            long minutes = seconds / 60;
            return minutes.ToString() + ":" + (seconds % 60).ToString("D2");
        }

        private Color ArchiveSkinColour(SkinElement element, ElementState state,
            ElementComponent component, Color fallback)
        {
            if (api.Setting_GetSkinElementColour == null)
                return fallback;
            try
            {
                int argb = api.Setting_GetSkinElementColour(element, state, component);
                // SDK colour values are System.Drawing ARGB. A zero/transparent
                // return means this skin has not defined that particular value.
                Color colour = Color.FromArgb(argb);
                return colour.A == 255 ? colour : fallback;
            }
            catch (Exception)
            {
                return fallback;
            }
        }

        private void ShowArchiveVerifiedView(object sender, EventArgs args)
        {
            try
            {
                if (archiveVerifiedView != null && !archiveVerifiedView.IsDisposed)
                {
                    RefreshArchiveVerifiedView();
                    if (archiveVerifiedView.WindowState == FormWindowState.Minimized)
                        archiveVerifiedView.WindowState = FormWindowState.Normal;
                    archiveVerifiedView.Activate();
                    return;
                }

                // MusicBee's SDK MainPanel dock is shared by all tabs and
                // obscures every library view. TabControl dock accepts a panel
                // but provides no visible tab with the current host. A
                // separate modeless window is the safe, opt-in solution:
                // it never parents into or changes another MusicBee tab.
                Color back = ArchiveSkinColour(SkinElement.SkinInputPanel,
                    ElementState.ElementStateDefault, ElementComponent.ComponentBackground, Color.White);
                Color text = ArchiveSkinColour(SkinElement.SkinInputPanelLabel,
                    ElementState.ElementStateDefault, ElementComponent.ComponentForeground, Color.Black);
                if (back.ToArgb() == text.ToArgb())
                    text = back.GetBrightness() > 0.5f ? Color.Black : Color.White;
                Color headerBack = ArchiveSkinColour(SkinElement.SkinElementPanel,
                    ElementState.ElementStateDefault, ElementComponent.ComponentBackground, back);
                Color selection = ArchiveSkinColour(SkinElement.SkinInputPanel,
                    ElementState.ElementStateHighlight, ElementComponent.ComponentBackground,
                    back.GetBrightness() > 0.5f ? Color.FromArgb(255, 241, 176) : Color.FromArgb(85, 87, 112));
                if (selection.ToArgb() == back.ToArgb())
                    selection = back.GetBrightness() > 0.5f ? Color.FromArgb(255, 241, 176) : Color.FromArgb(85, 87, 112);
                Color alt = back.GetBrightness() > 0.5f
                    ? Color.FromArgb(Math.Max(0, back.R - 7), Math.Max(0, back.G - 7), Math.Max(0, back.B - 7))
                    : Color.FromArgb(Math.Min(255, back.R + 8), Math.Min(255, back.G + 8), Math.Min(255, back.B + 8));
                Font skinFont = api.Setting_GetDefaultFont == null ? SystemFonts.MessageBoxFont :
                    (api.Setting_GetDefaultFont() ?? SystemFonts.MessageBoxFont);
                var window = new Form
                {
                    Name = "ArchiveStreamVerifiedDurations",
                    Text = "Archive Player - Verified Durations",
                    StartPosition = FormStartPosition.CenterScreen,
                    FormBorderStyle = FormBorderStyle.Sizable,
                    MinimumSize = new Size(700, 340),
                    Size = new Size(1000, 570),
                    ShowInTaskbar = true,
                    TopMost = false,
                    Font = skinFont,
                    BackColor = back,
                    ForeColor = text
                };
                window.FormClosed += (s, e) =>
                {
                    archiveVerifiedView = null;
                    archiveVerifiedGrid = null;
                    archiveVerifiedSummary = null;
                    Trace("ARCHIVE_VERIFIED_VIEW_CLOSED");
                };
                var header = new Panel { Dock = DockStyle.Top, Height = 74, Padding = new Padding(14, 7, 14, 7),
                    BackColor = headerBack, ForeColor = text };
                var title = new Label
                {
                    Dock = DockStyle.Top, Height = 33,
                    Text = "Archive Player",
                    Font = new Font(skinFont.FontFamily, skinFont.Size + 8, FontStyle.Bold),
                    BackColor = headerBack, ForeColor = text
                };
                var summary = new Label
                {
                    Dock = DockStyle.Bottom, Height = 25,
                    Text = "Loading archive track durations...",
                    BackColor = headerBack, ForeColor = text
                };
                header.Controls.Add(title);
                header.Controls.Add(summary);
                var table = new DataGridView
                {
                    Dock = DockStyle.Fill,
                    AllowUserToAddRows = false,
                    AllowUserToDeleteRows = false,
                    AllowUserToResizeRows = false,
                    AutoGenerateColumns = false,
                    ReadOnly = true,
                    RowHeadersVisible = false,
                    MultiSelect = false,
                    SelectionMode = DataGridViewSelectionMode.FullRowSelect,
                    BackgroundColor = back,
                    ForeColor = text,
                    BorderStyle = BorderStyle.None,
                    CellBorderStyle = DataGridViewCellBorderStyle.None,
                    ColumnHeadersBorderStyle = DataGridViewHeaderBorderStyle.Single,
                    ColumnHeadersHeightSizeMode = DataGridViewColumnHeadersHeightSizeMode.DisableResizing,
                    ColumnHeadersHeight = 28,
                    EnableHeadersVisualStyles = false,
                    RowTemplate = { Height = 24 },
                    DefaultCellStyle = new DataGridViewCellStyle
                    {
                        BackColor = back, ForeColor = text,
                        SelectionBackColor = selection, SelectionForeColor = text,
                        Padding = new Padding(3, 1, 3, 1)
                    },
                    AlternatingRowsDefaultCellStyle = new DataGridViewCellStyle
                    {
                        BackColor = alt, ForeColor = text,
                        SelectionBackColor = selection, SelectionForeColor = text
                    },
                    ColumnHeadersDefaultCellStyle = new DataGridViewCellStyle
                    {
                        BackColor = headerBack, ForeColor = text,
                        SelectionBackColor = headerBack, SelectionForeColor = text,
                        Padding = new Padding(3, 2, 3, 2)
                    }
                };
                table.Columns.Add(new DataGridViewTextBoxColumn
                {
                    Name = "Number", HeaderText = "#", Width = 52,
                    SortMode = DataGridViewColumnSortMode.NotSortable
                });
                table.Columns.Add(new DataGridViewTextBoxColumn
                {
                    Name = "Artist", HeaderText = "Artist", Width = 155,
                    SortMode = DataGridViewColumnSortMode.NotSortable
                });
                table.Columns.Add(new DataGridViewTextBoxColumn
                {
                    Name = "Title", HeaderText = "Title",
                    AutoSizeMode = DataGridViewAutoSizeColumnMode.Fill,
                    SortMode = DataGridViewColumnSortMode.NotSortable
                });
                table.Columns.Add(new DataGridViewTextBoxColumn
                {
                    Name = "Album", HeaderText = "Album", Width = 165,
                    SortMode = DataGridViewColumnSortMode.NotSortable
                });
                table.Columns.Add(new DataGridViewTextBoxColumn
                {
                    Name = "Genre", HeaderText = "Genre", Width = 120,
                    SortMode = DataGridViewColumnSortMode.NotSortable
                });
                table.Columns.Add(new DataGridViewTextBoxColumn
                {
                    Name = "Duration", HeaderText = "Time", Width = 75,
                    SortMode = DataGridViewColumnSortMode.NotSortable,
                    DefaultCellStyle = new DataGridViewCellStyle
                    {
                        Alignment = DataGridViewContentAlignment.MiddleRight,
                        BackColor = back, ForeColor = text,
                        SelectionBackColor = selection, SelectionForeColor = text
                    }
                });
                // Keep verified Time visible even when the MusicBee window
                // is narrow. Column storage order is unchanged; DisplayIndex
                // controls only presentation, avoiding shifted tag values.
                table.Columns["Duration"].DisplayIndex = 1;
                table.AllowUserToOrderColumns = true;
                table.CellDoubleClick += (s, e) =>
                {
                    if (e.RowIndex < 0 || e.RowIndex >= table.Rows.Count ||
                        api.NowPlayingList_PlayNow == null)
                        return;
                    string url = table.Rows[e.RowIndex].Tag as string;
                    if (!String.IsNullOrEmpty(url))
                        api.NowPlayingList_PlayNow(url);
                };
                window.Controls.Add(table);
                window.Controls.Add(header);
                archiveVerifiedView = window;
                archiveVerifiedGrid = table;
                archiveVerifiedSummary = summary;
                RefreshArchiveVerifiedView();
                window.Show();
                Trace("ARCHIVE_VERIFIED_VIEW_CREATED method=independent_window skin=musicbee columns=6");
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_VERIFIED_VIEW_FAILED type=" + error.GetType().Name);
            }
        }

        private void RefreshArchiveVerifiedView()
        {
            if (archiveDropClosing || archiveVerifiedGrid == null ||
                archiveVerifiedGrid.IsDisposed || api.Playlist_QueryFilesEx == null)
                return;
            if (archiveVerifiedGrid.InvokeRequired)
            {
                archiveVerifiedGrid.BeginInvoke((Action)RefreshArchiveVerifiedView);
                return;
            }
            string playlist = FindArchivePlayerPlaylist();
            string[] tracks;
            if (String.IsNullOrWhiteSpace(playlist) ||
                !api.Playlist_QueryFilesEx(playlist, out tracks) || tracks == null)
                return;
            // Never change the host's original playlist or selected Music tab.
            archiveVerifiedGrid.Rows.Clear();
            long total = 0;
            int verified = 0;
            int number = 0;
            foreach (string track in tracks)
            {
                if (!IsArchivePlayerOwnedUrl(track))
                    continue;
                ++number;
                string[] tags = ReadArchiveAliasDisplayFields(track);
                long length = QueryArchiveDurationMs(track, false); // cache only, NEVER decode
                if (length > 0) { total += length; ++verified; }
                string title = String.IsNullOrWhiteSpace(tags[0]) ?
                    Path.GetFileNameWithoutExtension(track) : tags[0];
                int index = archiveVerifiedGrid.Rows.Add(
                    String.IsNullOrWhiteSpace(tags[3]) ? number.ToString() : tags[3],
                    tags[1], title, tags[2], tags[4], FormatVerifiedArchiveDuration(length));
                archiveVerifiedGrid.Rows[index].Tag = track;
            }
            archiveVerifiedGrid.ClearSelection();
            archiveVerifiedSummary.Text = number + " tracks  |  " + verified +
                " verified durations  |  Total: " + FormatVerifiedArchiveDuration(total);
            Trace("ARCHIVE_VERIFIED_VIEW_REFRESH tracks=" + number +
                " verified=" + verified + " total_ms=" + total);
        }

        private void CloseArchiveVerifiedView()
        {
            Form view = archiveVerifiedView;
            if (view != null && !view.IsDisposed)
            {
                if (view.InvokeRequired)
                    view.BeginInvoke((Action)(() => view.Close()));
                else
                    view.Close();
            }
            archiveVerifiedView = null;
            archiveVerifiedGrid = null;
            archiveVerifiedSummary = null;
        }
    }
}
