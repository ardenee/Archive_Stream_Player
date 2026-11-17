# Installer

[Download Archive Stream Player v0.1.2](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.2)

To build it yourself, first compile the plugin Release binaries and the two MediaMonkey MMIP packages. Then:

1. Install [Inno Setup 7](https://jrsoftware.org/isinfo.php).
2. Open **[ArchiveStreamPlayer.iss](ArchiveStreamPlayer.iss)** in Inno Setup.
3. Select **Build → Compile**.

The installer is saved to `installer/output/ArchiveStreamPlayer-Setup.exe`.

It automatically detects compatible players and lets you choose which ones to install into, including portable installations. MediaMonkey requires its two add-ons to be approved through its own Addons menu.

[Project website](https://www.arden.ie/projects/archive-stream-player/)
