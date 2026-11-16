# Archive Stream Player — installation

## One installer, multiple players

Use **ArchiveStreamPlayer-Setup.exe**, built with Inno Setup 7 from \`installer/build-installer.ps1\`.

1. Start setup and choose the players you want.
2. Installed, compatible players are detected and selected automatically.
3. For a portable player, tick its entry and browse to the folder containing its executable.
4. Close the players before Setup copies their native plugin files.
5. Restart the selected players. Use **Configure Archive Stream Player** from the Start Menu to adjust shared settings.

The installer blocks selecting a folder containing the wrong 32/64-bit player executable.

## Player-specific notes

- **Winamp 32-bit and WACUP 32-bit:** input playback and playlist expansion plugins are installed together.
- **foobar2000 32/64-bit:** installs the matching native component. Choose the correct executable folder, including when running a portable copy.
- **AIMP 32/64-bit:** installs the matching native archive plugin.
- **XMPlay 32-bit:** installs the 7z/RAR archive plugin. XMPlay's built-in ZIP engine operates independently of Archive Stream's shared settings.
- **MusicBee 32-bit:** installs the BASS archive module and Tools menu plugin; do not manually replace MusicBee's own BASS runtime.
- **MediaMonkey 5/2024:** the installer puts **two MMIP packages** in the Archive Stream Player program folder. Open MediaMonkey → Tools → Addons → Add and install **both** packages. The host must approve them; ordinary file copying alone is not sufficient.
- **VLC 3 64-bit:** installs an external archive-access module and **Open an archive in VLC** Start Menu shortcut. VLC's installed program files are not modified.
- **WACUP 64-bit:** no native plugin. Compatibility-playlist mode remains separate.
- **Windows Media Player:** experimental; not part of the selectable native installer.

## Shared controls

When Archive Stream handles the archive, one shared settings page controls password prompts and optional saved passwords, memory allocation, managed encoded-audio cache and cleanup, title templates and logging.

Playback priority is **direct stream → memory → cache**. Cache is the last resort, not the default. Saved passwords are plaintext and require explicit consent.

The plugin works with ZIP, 7z and RAR/RAR5, including common encrypted, solid and multi-part archives. An incorrect password, missing volume, unsupported compression method or a song larger than the allowed RAM/cache may prevent playback.

## For developers

Build each native plugin first, then run:

\`\`\`powershell
.\installer\build-installer.ps1
\`\`\`

Output: \`installer\output\ArchiveStreamPlayer-Setup.exe\`.

The compiler stages the Release binaries, MediaMonkey add-ons and required third-party licence notices into the installer. Runtime archive libraries are statically linked. The build script deliberately does not include any private QA material.

[Project homepage](https://www.arden.ie/projects/archive-stream-player/) · [Feature matrix](FEATURE-MATRIX.md) · [Licensing](LICENSING.md)
