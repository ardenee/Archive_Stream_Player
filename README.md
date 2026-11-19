# Archive Stream Player

Play music directly from **ZIP, 7z and RAR archives** in your favourite Windows music player, without extracting entire albums.

**[Download v0.1.7](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.7)** · [Project website](https://www.arden.ie/projects/archive-stream-player/) · [Feature matrix](FEATURE-MATRIX.md)

**Adapter UI update (November 18, 2026):** Winamp/WACUP's settings dialog has more room for the right-hand tab border and password-help text. MusicBee launches shared settings without a visible PowerShell console and registers archive-file drag/drop handlers on the host's existing drop-enabled controls; a real file drop was confirmed on one MusicBee panel, but other panels failed to accept the archive. The updated handler preserves MusicBee's existing AllowDrop behaviour and tracks panels created after startup; cross-panel drop acceptance is still unverified. MusicBee now additionally registers an archive-only native OLE drop target if the main window has no existing target; existing MusicBee targets are never replaced, ordinary audio files are rejected by our handler, and a standalone COM registration/ZIP-versus-MP3 test passes. MediaMonkey no longer adds four redundant Tools-menu entries: use the player plugin settings and drop archives onto Now Playing. XMPlay now uses the official archive-plugin Options-page callback to appear under **Options → Decoders → Archive Stream Player**, while retaining the separate Config dialog; the embedded page has a narrower layout for XMPlay's settings pane. VLC uses an external archive launcher and has no integrated settings panel; Windows Media Player integration remains experimental.

**MusicBee ID3 alias fix (November 19, 2026):** Native playback now accepts tiny ID3v2.3 metadata aliases while keeping the .ast text descriptors unchanged. The BASS addon validates that the ID3 alias contains an exact byte-for-byte copy of its paired manifest before streaming archive audio. Older same-file hard-link aliases remain playable. With Use File Tags enabled, MusicBee reads Title, Artist, Album, Track Number, Genre and Year directly from the alias. Existing rows migrate to versioned paths to invalidate MusicBee cached titles, while their original aliases remain available. DEV verified 9/9 Tauregen RAR titles and direct PCM playback; original descriptor files were byte-for-byte unchanged.

**MusicBee native Time column — V5 MPEG Info/Xing fix (November 19, 2026):** MusicBee's Duration SDK property is read-only; its tag setter does not update it, and ID3 TLEN alone did not populate the native Time column. For directly streamable MP3s, Archive Stream now reads one genuine first MPEG Layer III Info/Xing frame from the compressed entry only after independent PCM verification confirms the duration. The alias consists of ordinary ID3v2.3 metadata, that single real frame and the exact original .ast manifest. MusicBee extracts duration from the MPEG header, while Archive Stream's BASS addon validates the manifest and routes playback, reads and seeking to the **full original archived audio**. The change does not fabricate frames, calculate audio-content CRCs, rewrite archives or force disk extraction. Titles appear immediately; verification and alias refresh run at low priority after first playback. If the header is unavailable, untrusted or disagrees with PCM length, or the archive is not directly streamable, the plugin keeps its safe metadata-only alias. Live DEV: the original Archive Player tab showed **37:00** for 9 Broken Silence MP3s. MusicBee's own Duration API returned values for **9/9 with zero blanks** after restart, and native BASS open, PCM read, seek and further PCM read passed 9/9 (exact total **36:59.954**). All five Elementis tracks also passed isolated playback tests.

**MusicBee Elementis MP3 playback regression (November 19, 2026):** A five-track Elementis ZIP exposed a BASS format error 41 on Whitebear - Fever Dreams: internal PRIV/TXXX/COMM marker frames prevented BASS forwarding metadata-only aliases to our addon. The ID3 V4 correction restricted aliases to ordinary song tags and versioned paths, preserving the paired .ast and original archives. Five of five opened and decoded, including the formerly failing track 2. The subsequent V5 MPEG Info/Xing change is described above.

**MusicBee optional verified-duration window (November 19, 2026):** MusicBee's SDK MainPanel dock obscured other tabs, and TabControl docking did not yield a usable tab. Tools > Archive Stream: Verified Durations therefore opens an optional, independently movable MusicBee-skinned window showing cached times, album total and tag fields, with double-click playback. It never opens automatically or changes other tabs. With V5, eligible directly streamable MP3s now have native Time values; this separate view remains a fallback for other formats and archives where MusicBee cannot obtain a validated MPEG Info/Xing header.

**MusicBee playback-triggered duration verification (November 19, 2026):** Archive Player displays ID3 metadata and embedded track numbers immediately. First playback triggers one low-CPU/low-I/O background PCM verifier for that archive, prioritising the playing track and reusing validated cached lengths. The worker checks the playlist and settings snapshots before committing, so Clear List or settings changes cannot restore deleted rows. V5 also adds a validated genuine Info/Xing MPEG frame to the new alias when eligible, letting MusicBee populate its native Time column for that MP3. No full archive extraction or content CRCs. Other codecs keep their safe alias and the optional duration window.

**MusicBee playlist isolation fix (November 19, 2026):** Archive Player is a dedicated MusicBee-native playlist with validated ID3 aliases. The plugin discovers the playlist through MusicBee's SDK by its owned folder and playlist name/URL, not a numeric tab or FolderCategory value: category 1/13 were DEV-only values used once to repair a previously overwritten Music tab and are never reserved for other users. Archive drops append only archive-backed entries; ordinary MP3s accidentally inserted by MusicBee's own drop handlers are removed from Archive Player without touching their source files, library, or Now Playing. Command.NavigateTo with an .mbp file path is disabled after causing a Windows file-association prompt and hijacking the Music tab. Automatic tab selection is not yet certified: select Archive Player manually or through MusicBee's Playlists navigator.

**MusicBee tab implementation:** The Archive Player tab uses the MusicBee playlist APIs for creating, appending and querying the archive-only playlist. MusicBee manages and restores its tab UI; a .mbp pathname must **never** be passed as NavigateTo input. Earlier tests misinterpreted NavigateTo returning success: the command actually changed the Music tab destination and sometimes launched a Windows file-association picker. The dedicated playlist remains playable; safe automatic tab selection is pending a confirmed host-supported navigation method. No user-defined playlist, ordinary audio file, or general Music library record is deleted by the archive-only guard.

**MusicBee verification (November 19, 2026):** On DEV, persistent archive-only playlists, embedded tags, one-time TRACKNUMBER ordering, settings refresh without restart, Clear List persistence and native audio routing passed. The Duration SDK property has no setter; tests with a temporary playlist confirmed that setting tag field 16 did not update Duration. V5 instead provides a real verified MPEG Info/Xing frame for supported MP3s. Broken Silence passed 9/9 decoded playback and seeking; its original Archive Player playlist now displays a 37:00 total and its MusicBee SDK returned individual Duration values for all nine tracks after restart. MP3s without a valid first Info/Xing frame, nonstreamable entries and non-MP3 codecs are not claimed fixed.

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
