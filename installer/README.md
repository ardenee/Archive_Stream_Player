# Installer

[Download Archive Stream Player v0.1.7](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.7)

To build it yourself, first compile the plugin Release binaries and the two MediaMonkey MMIP packages. Then:

1. Install [Inno Setup 7](https://jrsoftware.org/isinfo.php).
2. Open **[ArchiveStreamPlayer.iss](ArchiveStreamPlayer.iss)** in Inno Setup.
3. Select **Build → Compile**.

The current DEV build is saved to `installer/output/ArchiveStreamPlayer-Setup-DEV.exe`. This is not the published v0.1.7 release installer.

It automatically detects compatible native players and lets you choose which ones to install into, including portable installations. MediaMonkey is installed directly; the two MMIP packages are included for optional manual installation.

**Media Player (2022) DEV preview (November 20, 2026):** The installer also offers an optional, **unchecked** Media Player (2022) **external Archive Stream drop target**. Unlike the native VLC or AIMP plugins, this is a standalone Win32 window which feeds Microsoft's packaged Media Player via private 127.0.0.1 HTTP playlists. Install both companion files in `{app}/MediaPlayer2022` and launch the Start Menu shortcut **Media Player (2022) - Drop Archives**, or opt into the desktop shortcut task. Drag one ZIP/7z/RAR to the separate drop window or shortcut; dragging directly onto Microsoft's window is **not** supported. If Windows' AppContainer loopback exemption is absent, the launcher requests temporary UAC elevation for an OS helper; that helper removes only its own exemption when the drop process exits. Leave the drop window open until playback is finished. Eager whole-track HTTP prefetch and non-buffered seeking remain uncertified; do not present this DEV component as a released native plugin.

[Project website](https://www.arden.ie/projects/archive-stream-player/)
