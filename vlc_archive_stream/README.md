# Archive Stream Player for VLC 3 (64-bit)

**Open ZIP, 7z and RAR audio albums directly in VLC** with the native VLC 3 SDK access plugin (v1.5.1). Use VLC's ordinary **Media > Open File** (select All Files if needed) or drag an archive into VLC. No PowerShell, HTTP server, launcher, or album extraction is required.

## Install and use

1. Install Archive Stream Player and select the **VLC 3 64-bit** component. The installer places `libarchive_stream_plugin.dll` in VLC's `plugins/access` directory and regenerates its plugin cache.
2. Restart VLC if it was running.
3. Open or drop a ZIP, 7z or RAR audio archive.
4. The album expands in VLC's native playlist. Play, seek, and select tracks as usual. VLC's own decoders handle the audio.

Normal MP3, WAV, FLAC, videos, and other ordinary media remain owned by VLC's original access modules.

## Native VLC settings

Open **Tools > Preferences** (`Ctrl+P`), select **Show settings: All** at the bottom, then **Input / Codecs > Access modules > Archive Stream Player**.

Settings include the memory threshold (MiB), managed cache size/age and cleanup, password prompts and verified-password saving, password-list path, **Use File Tags**, six title formats, prefix/postfix, debug logging and log directory. VLC stores these preferences in its normal configuration.

**Use VLC-specific Archive Stream settings** is off by default. Off means the shared Archive Stream Player settings continue to govern VLC as well as other adapters. Turn it on to use the values in VLC's own preferences page **for VLC only**; saving them does not overwrite other players' settings. Reopen the archive after changing options. The previously installed shared configuration utility remains available.

## Metadata and durations before playback

Version 1.5.1 uses VLC's native directory-input API rather than a minimal M3U playlist. During archive listing it creates native VLC playlist items and supplies any available **title, artist, album, album artist, genre, date/year and track number** from bounded header/tag probes. The title preset controls the displayed title; **Use File Tags** only determines whether the embedded title overrides that preset. Other tag fields populate independently of that checkbox.

For duration, validated MP3 Info/Xing (with available LAME gapless correction), FLAC STREAMINFO and WAV RIFF headers are used. Only directly streamable, unencrypted, non-solid entries are inspected before playback, with a bounded 256 KiB tag prefix. There is **no full-archive extraction or CRC recalculation** to obtain playlist metadata. For solid archives, encrypted entries, unsupported codecs and unusually large or unrecognised headers, some metadata remains unknown until VLC plays that track. Embedded cover art is not preloaded.

The older in-memory XSPF virtual-playlist path remains as a compatibility fallback; normal Open File/drag-and-drop uses the direct native directory API.

## Verify which plugin is active

Open **Tools > Messages**, choose verbosity **1** or **2**, and reopen the archive. Look for **Archive Stream Player: native tagged archive directory opened** and **Archive Stream Player: native archive audio stream opened**. The audio tracks use `ast://` virtual addresses, not HTTP. With shared or VLC diagnostic logging enabled, check `%LOCALAPPDATA%\ArchiveStreamPlayer\Logs\archive_stream_vlc.log` for `LOCAL_ARCHIVE_REDIRECT`, `NATIVE_DIRECTORY_TAGS`, `NATIVE_DIRECTORY_OPEN` and `ROUTE_STREAM`/`ROUTE_MEMORY`/`ROUTE_CACHE`.

## Routing, validation and limitations

Archive Stream uses **direct streaming first**, then a bounded memory buffer, then managed disk cache as a last resort. Shared password handling supports encrypted archives when the backend can decode them, but may require memory/cache fallback. The original archive is never modified. The shared password-list loader accepts UTF-8 with or without a BOM at the start of the file; without the fix, the first password from a BOM-bearing Windows text file was misread and a valid encrypted-header 7z failed authentication.

DEV validation on VLC **3.0.24 x64**: all nine Subspecies 4 ZIP tracks were listed with artist, album, genre, year, title and verified durations prior to playback; generated 7z and RAR WAV cases opened through the native directory and streamed successfully. A normal MP3 remained on VLC's built-in filesystem module. The native directory test is `test-native-directory-metadata.ps1`. `test-native-preferences.ps1` validates saved VLC-only settings against shared-mode fallback using a disposable VLC config. `test-native-password-list-bom.ps1` creates a disposable encrypted-header 7z and verifies playback with both BOM and BOM-free UTF-8 password lists; no user passwords or shared preferences are changed.

Build `vlc_archive_stream.vcxproj` in **Release|x64** against VLC 3 SDK. Inno Setup 7 assembles the main Windows installer. VLC 4, VLC 32-bit, and native VLC cover-art preloading are not currently supported.

See the [feature matrix](../FEATURE-MATRIX.md) and [licensing information](../LICENSING.md).
