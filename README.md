# Archive Stream Player

Play music directly from **ZIP, 7z and RAR archives** in your favourite Windows music player, without extracting entire albums.

**[Download v0.1.7](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.7)** · [Project website](https://www.arden.ie/projects/archive-stream-player/) · [Feature matrix](FEATURE-MATRIX.md)

## Features

- ZIP, 7z and RAR/RAR5, including password-protected, solid and multipart archives.
- MP3, FLAC, WAV, OGG Vorbis, AAC, M4A and WMA audio.
- Browse archives, add tracks to playlists, play and seek.
- Stream directly when possible, use memory next, and disk cache only when necessary.
- Shared password, cache, title and logging settings.

**Development update (November 2026):** WACUP 32-bit track titles now stay consistent through play and stop in tested ZIP, RAR and solid 7z playlists. The foobar2000 component (0.2.2 development build) has compact preferences pages for playback/cache, passwords and diagnostics; foobar2000 controls playlist title display. The component also has expanded About information and project links. AIMP now provides its own native preferences tabs for playback/cache, passwords, diagnostics and track-title presets (no external settings script required). Shared settings launch without PowerShell console windows in Winamp, WACUP and foobar2000. The installer also uses hidden launchers for its settings and VLC shortcuts. These changes are in development source; the download link above still points to the published v0.1.7 release.

## Supported players

| Player | Supported versions |
| --- | --- |
| Winamp / WACUP | 32-bit |
| foobar2000 | 32-bit and 64-bit |
| AIMP | 32-bit and 64-bit |
| XMPlay | 32-bit |
| MusicBee | 32-bit |
| MediaMonkey | 5 / 2024 |
| VLC | 3, 64-bit |

The installer detects supported players and lets you choose which plugins to install. **VLC** includes a shortcut to open archives.

WACUP 64-bit has no native plugin. Windows Media Player integration remains experimental and is not included in the installer.

Audio playback uses Archive Stream's decoders in Winamp, MusicBee and MediaMonkey; 
foobar2000, AIMP, XMPlay and VLC use their own audio decoders. 
See the [feature matrix](FEATURE-MATRIX.md) for differences and limitations.

## Build

Build the plugin Release binaries and MediaMonkey add-on packages, then open [installer/ArchiveStreamPlayer.iss](installer/ArchiveStreamPlayer.iss) in **Inno Setup 7** and select **Build → Compile**.

## Licence

The original project code is [MIT licensed](LICENSE). Third-party components retain their own [licences](LICENSING.md).
