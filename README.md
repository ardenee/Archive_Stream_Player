# Archive Stream Player

Play music directly from **ZIP, 7z and RAR archives** in your favourite Windows music player, without extracting entire albums.

**[Download v0.1.7](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.7)** · [Project website](https://www.arden.ie/projects/archive-stream-player/) · [Feature matrix](FEATURE-MATRIX.md)

**Adapter UI update (November 18, 2026):** Winamp/WACUP's settings dialog has more room for the right-hand tab border and password-help text. MusicBee launches shared settings without a visible PowerShell console and registers archive-file drag/drop handlers on the host's existing drop-enabled controls; a real file drop was confirmed on one MusicBee panel, but other panels failed to accept the archive. The updated handler preserves MusicBee's existing AllowDrop behaviour and tracks panels created after startup; cross-panel drop acceptance is still unverified. MusicBee now additionally registers an archive-only native OLE drop target if the main window has no existing target; existing MusicBee targets are never replaced, ordinary audio files are rejected by our handler, and a standalone COM registration/ZIP-versus-MP3 test passes. MediaMonkey no longer adds four redundant Tools-menu entries: use the player plugin settings and drop archives onto Now Playing. XMPlay now uses the official archive-plugin Options-page callback to appear under **Options → Decoders → Archive Stream Player**, while retaining the separate Config dialog; the embedded page has a narrower layout for XMPlay's settings pane. VLC uses an external archive launcher and has no integrated settings panel; Windows Media Player integration remains experimental.

**Playback source fix (November 18, 2026):** MusicBee was accepting `.ast` archive-descriptor files into its queue but could not open them as playable tracks. The development adapter now queues `.mp3` hard links to the validated native descriptors; the BASS add-on verifies the linked descriptor before opening the archive directly. The C# descriptor-to-BASS PCM/seek regression passes without extracting audio, but playback in the MusicBee application must be checked after installation. XMPlay's embedded Options page now moves to the right of its navigation tree using the tree's measured screen bounds.

**MusicBee playlist persistence (November 18, 2026):** Native archive tracks now use stable, small descriptor files under `%LOCALAPPDATA%\ArchiveStreamPlayer\MusicBee\v`, with zero-copy hard links used as MusicBee-compatible playable paths. Two-process restart tests confirmed identical track paths after the adapter closes for a six-track ZIP plus MP3-containing 7z and RAR archives; all persisted references passed native PCM decoding and seeking. MusicBee distinguishes archive drops into **Tracks** (SDK library import of persistent archive references) and **Playing Tracks** (queue-only); the new destination detection and import routes pass isolated tests but require verification inside the MusicBee UI. The compressed archive must remain at its original path. Compatibility HTTP playback remains session-bound; old playlists referencing deleted temporary descriptors must be re-added once.

**MusicBee library integration (November 18, 2026):** The development adapter now uses the drop target and MusicBee's MainPanel bounds to distinguish library Tracks from Playing Tracks. Persistent native archive aliases are imported through `Library_AddFileToLibrary()` for library drops, whereas Playing Tracks drops only queue. The native BASS add-on advertises these `.mp3` aliases and validates their paired `.ast` descriptors. Tests confirm that ordinary MP3s still decode as MP3, archive aliases still decode/seek directly from ZIP/7z/RAR, and isolated library/queue routing works. MusicBee's first live Tracks drop was incorrectly sent to Playing Tracks because custom MusicBee panels expose no names and the SDK-bound fallback did not identify the destination. The development fix now measures the actual left-to-right Tracks/Playing Tracks child-window panes before the fallback; the measured DEV layout and Win32 child-window regression tests pass. Live library import and double-click playback still need verification after deployment. Unrecognised drop panels continue using the known-working queue route rather than altering the library unexpectedly.

## Features

- ZIP, 7z and RAR/RAR5, including password-protected, solid and multipart archives.
- MP3, FLAC, WAV, OGG Vorbis, AAC, M4A and WMA audio.
- Browse archives, add tracks to playlists, play and seek.
- Stream directly when possible, use memory next, and disk cache only when necessary.
- Shared password, cache, title and logging settings.

**Development update (November 2026):** WACUP 32-bit track titles now stay consistent through play and stop in tested ZIP, RAR and solid 7z playlists. The foobar2000 component (0.2.2 development build) has compact preferences pages for playback/cache, passwords and diagnostics; foobar2000 controls playlist title display. The component also has expanded About information and project links. AIMP and XMPlay now offer native preferences tabs for playback/cache, passwords, diagnostics and track-title presets without opening PowerShell. XMPlay's 32-bit archive plugin v0.2.4 uses a compact, XMPlay-styled Config dialog with native Apply/OK/Cancel controls and Windows version/description information. Development preferences now follow each host's layout: Winamp/WACUP uses a compact, DPI-aware tabbed dialog; AIMP builds all Preferences controls through the official AIMP GUI SDK (native PageControl tabs, labels, text fields, checkboxes, buttons and dropdowns), inheriting AIMP's theme and DPI handling while using AIMP's Apply/Reset actions; foobar2000 retains its compact native Preferences pages with standard keyboard navigation. Shared settings launch without PowerShell console windows in Winamp, WACUP and foobar2000. The installer also uses hidden launchers for its settings and VLC shortcuts. These changes are in development source; the download link above still points to the published v0.1.7 release.

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
