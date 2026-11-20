# Installer

[Download Archive Stream Player v0.1.11](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.11)

To build it yourself, first compile the plugin Release binaries and the two MediaMonkey MMIP packages. Then:

1. Install [Inno Setup 7](https://jrsoftware.org/isinfo.php).
2. Open **[ArchiveStreamPlayer.iss](ArchiveStreamPlayer.iss)** in Inno Setup.
3. Select **Build → Compile**.

The v0.1.11 release build is saved to `installer/output/ArchiveStreamPlayer-Setup.exe`. The published installer is attached to the GitHub release.

It automatically detects compatible native players and lets you choose which ones to install into, including portable installations. MediaMonkey is installed directly; the two MMIP packages are included for optional manual installation.

[Project website](https://www.arden.ie/projects/archive-stream-player/)
