# Archive Stream Player — Windows installer

**[Download the Windows preview installer](https://github.com/ardenee/Archive_Stream_Player/releases/download/v0.1.0-preview.1/ArchiveStreamPlayer-Setup.exe)** · [GitHub release page](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.0-preview.1)

**One installer for all selectable native player integrations.**

The installer detects installed players, checks whether each is 32-bit or 64-bit, preselects compatible versions, and lets you install only the plugins you want. Portable players can be selected by browsing to their actual application folder.

## What gets installed

- **Winamp 32-bit and WACUP 32-bit:** native audio and playlist plugins.
- **foobar2000 32/64-bit, AIMP 32/64-bit, XMPlay 32-bit:** matching native plugins.
- **MusicBee 32-bit:** native BASS module and Tools menu integration.
- **MediaMonkey 5 / 2024:** both add-on packages. Open **MediaMonkey → Tools → Addons → Add** and install the two MMIP files shown by setup. MediaMonkey's approval step cannot be skipped.
- **VLC 3 64-bit:** native external archive module plus an **Open an archive in VLC** Start Menu shortcut, without modifying the VLC installation.

**WACUP 64-bit** has a compatibility playlist mode but cannot load the 32-bit native plugin. **Windows Media Player** remains experimental. Neither is presented as a native-plugin checkbox.

The installer creates a **Configure Archive Stream Player** shortcut and a [project homepage](https://www.arden.ie/projects/archive-stream-player/) shortcut. Licences and third-party notices are bundled. Uninstalling does not intentionally erase shared player preferences or password lists. To remove MediaMonkey's installed add-ons, use its own Addons manager.

## Build on DEV

1. Build the project plugins in **Release** for their supported architectures.
2. Install **Inno Setup 7**.
3. From the repository root, run:

\`\`\`powershell
.\installer\build-installer.ps1
\`\`\`

The executable is generated at \`installer\output\ArchiveStreamPlayer-Setup.exe\`.

The build script stages compiled Release binaries and packages the required two MediaMonkey MMIPs. The installer uses statically linked archive backends; there are no extra libarchive/7-Zip/UnRAR runtime DLLs to install. Temporary build payloads and the unsigned installer executable are not committed to the source repository.

**Status:** a public, unsigned preview release is available. A signed stable release is not yet available. Commercial distribution of installers built with Inno Setup 7 may require a separate Inno Setup commercial licence.

## Command-line deployment

Setup supports Inno Setup's standard \`/SILENT\`, \`/VERYSILENT\` and \`/COMPONENTS\` parameters. For deployment to non-standard or portable player directories, supply a path in the form:

\`\`\`text
/COMPONENTS=winamp,foobar64
/PLAYERDIR_WINAMP="D:\Apps\Winamp"
/PLAYERDIR_FOOBAR64="D:\Apps\foobar2000"
\`\`\`

Paths and player executable architecture are rechecked before any files are installed. A wrong-architecture or missing executable stops installation. Silent installations of the MediaMonkey option only place its MMIP packages; complete the add-on approval in MediaMonkey afterward.

See [README](../README.md) and the [feature matrix](../FEATURE-MATRIX.md).
