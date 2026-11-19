# Archive Stream Player

Play music directly from **ZIP, 7z and RAR archives** in your favourite Windows music player, without extracting entire albums.

**[Download v0.1.7](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.7)** · [Project website](https://www.arden.ie/projects/archive-stream-player/) · [Feature matrix](FEATURE-MATRIX.md)

**Adapter UI update (November 18, 2026):** Winamp/WACUP's settings dialog has more room for the right-hand tab border and password-help text. MusicBee launches shared settings without a visible PowerShell console and registers archive-file drag/drop handlers on the host's existing drop-enabled controls; a real file drop was confirmed on one MusicBee panel, but other panels failed to accept the archive. The updated handler preserves MusicBee's existing AllowDrop behaviour and tracks panels created after startup; cross-panel drop acceptance is still unverified. MusicBee now additionally registers an archive-only native OLE drop target if the main window has no existing target; existing MusicBee targets are never replaced, ordinary audio files are rejected by our handler, and a standalone COM registration/ZIP-versus-MP3 test passes. MediaMonkey no longer adds four redundant Tools-menu entries: use the player plugin settings and drop archives onto Now Playing. XMPlay now uses the official archive-plugin Options-page callback to appear under **Options → Decoders → Archive Stream Player**, while retaining the separate Config dialog; the embedded page has a narrower layout for XMPlay's settings pane. VLC uses an external archive launcher and has no integrated settings panel; Windows Media Player integration remains experimental.

**MusicBee ID3 alias fix (November 19, 2026):** Native playback now accepts tiny ID3v2.3 metadata aliases while keeping the .ast text descriptors unchanged. The BASS addon validates that the ID3 alias contains an exact byte-for-byte copy of its paired manifest before streaming archive audio. Older same-file hard-link aliases remain playable. With Use File Tags enabled, MusicBee reads Title, Artist, Album, Track Number, Genre and Year directly from the alias. Existing rows migrate to versioned paths to invalidate MusicBee cached titles, while their original aliases remain available. DEV verified 9/9 Tauregen RAR titles and direct PCM playback; original descriptor files were byte-for-byte unchanged.

**MusicBee native Archive Player tab (November 19, 2026):** The official MusicBee playlist APIs maintain a native Archive Player tab; archive-only drops append distinct tracks and open it. Startup creates or reuses only the backing playlist and does not force tab navigation (avoiding duplicate tabs and the .mbp file-association prompt). First-run migration happens only once: Clear List is not automatically undone on restart. Tools > Open Archive Player explicitly reopens its tab. Entries are metadata-only references under LocalAppData; no full-album audio extraction is needed to list tracks.

**MusicBee tab implementation:** The Music/Tracks view did not display earlier archive imports even though MusicBee's library query found them and Music Explorer could play them. The native Archive Player playlist tab avoids that view dependency and uses MusicBee's own selection, context menus and playback, rather than a fragile overlay. The `MB_InvokeCommand(Command.NavigateTo, playlist)` route successfully created and activated a real tab with 63 existing tracks on DEV. Native BASS playback events have been observed in the host; newly imported durations may initially display `0:00`.

**MusicBee verification (November 19, 2026):** The supported Playlist_CreatePlaylist, Playlist_QueryFilesEx, Playlist_AppendFiles, Playlist_SetFiles and MB_InvokeCommand delegates manage the playlist. The background metadata migration rechecks the complete list before applying an update, so a Clear List or another drop cannot be overwritten. The SDK test passes creation, deduplication, closed-tab reuse and cleared-list persistence. Real DEV: 9/9 ID3 migrations with visible Title/Artist/Album/Genre, no duplicate title tab, idempotent restart and native direct streaming. An additional user-side Clear List/restart check remains desirable.

## Features

- ZIP, 7z and RAR/RAR5, including password-protected, solid and multipart archives.
- MP3, FLAC, WAV, OGG Vorbis, AAC, M4A and WMA audio.
- Browse archives, add tracks to playlists, play and seek.
- Stream directly when possible, use memory next, and disk cache only when necessary.
- Shared password, cache, title, Use File Tags, and logging settings.

**Shared Use File Tags (November 19, 2026):** The checkbox is off by default. When on, embedded audio fields override the synthetic title template; missing tags use filenames. The shared ID3v2/FLAC/Ogg/Opus prefix probe is read-only, limited to 256 KiB and independently streamable entries. It never decompresses solid archives merely to display tags or calculates archive-content CRCs. MusicBee stores metadata in safe ID3 aliases, AIMP exposes embedded fields, Winamp/WACUP and MediaMonkey have their existing tag readers, foobar2000 and XMPlay can use host decoders, and VLC/bridge can use embedded titles. Title refresh for existing rows may depend on the host.

**Development update (November 2026):**

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
