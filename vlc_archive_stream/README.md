# Archive Stream Player for VLC 3 (64-bit)

**Open ZIP, 7z and RAR audio albums directly in VLC.** Version 1.4.1 of the native VLC 3 SDK access plugin supports VLC's normal **Media > Open File** and archive drag-and-drop, without PowerShell, HTTP, an external launcher or extracting an album to a directory.

## How to use

1. Install Archive Stream Player and select the **VLC 3 64-bit** component. This installs `libarchive_stream_plugin.dll` into the VLC `plugins/access` directory and rebuilds VLC's plugin cache.
2. Restart VLC if it is already running.
3. Use **Media > Open File** and choose an audio ZIP/7z/RAR archive, or drag the archive into VLC.
4. VLC lists the archive's supported audio entries as tracks. Choose a track, play and seek normally.

## Confirming the native plugin is used

Open **Tools > Messages** (set verbosity to 1 or 2), then reopen an archive. Look for `Archive Stream Player: native Open File/drag-drop handler active (no HTTP)` and `Archive Stream Player: native archive playlist opened`. When playing a track, the native source is an `ast://` URI, not a `file://` archive path or local HTTP URL. You can also enable shared debug logging and check `ArchiveStreamPlayer/Logs/archive_stream_vlc.log` under your local application-data directory; entries include `LOCAL_ARCHIVE_REDIRECT`, `NATIVE_PLAYLIST_DURATION`, and `ROUTE_STREAM`/`ROUTE_MEMORY`/`ROUTE_CACHE`.

## Track durations

The native playlist now supplies real durations on opening for independently streamable **MP3 with a validated Info/Xing frame, FLAC with STREAMINFO, and WAV with a valid RIFF header**. It probes only bounded headers through the shared backend, never decodes whole albums, recalculates CRCs or extracts solid archives merely to obtain times. Unsupported/untrusted headers, other codecs, and solid or encrypted entries still begin with unknown duration until VLC reads the individual track. In DEV VLC 3.0.24, all nine Subspecies 4 MP3 durations appeared in the playlist before playing those tracks.

VLC's built-in audio decoders decode the audio. Archive Stream supplies the archive container reader and each uncompressed audio byte stream. The route is **direct streaming first**, then memory when it fits the shared configured buffer, then managed disk cache only if necessary. Password prompts, encrypted archives, titles and logging use the shared settings. Archive files stay unchanged; content CRCs are never recalculated for metadata.

Normal MP3/WAV/FLAC files and non-archive media continue through VLC's built-in file access. Unsupported containers and codecs are not automatically made playable. VLC 4 and VLC 32-bit are not supported by this VLC 3 module.

## Build and advanced compatibility

Build `vlc_archive_stream.vcxproj` in **Release|x64** against the official VLC 3 SDK. Inno Setup 7 installs the module into the selected VLC installation. The standalone `package-native-vlc.ps1` also packages the DLL and legacy launcher for optional portable/diagnostic use; **the launcher is not required for regular playback**.

The VLC module contains two SDK access handlers: the `file` handler recognises local archive paths and redirects them internally to `ast://`; the `ast` handler lists tracks in-process and provides seekable audio data. VLC itself decodes the audio. The optional legacy HTTP bridge is not involved in normal VLC 3 x64 operation.

## Limitations

The plugin currently has no dedicated graphical preferences page inside VLC; shared settings are edited using the Archive Stream Player configuration shortcut. Existing titles may require reopening the archive to refresh. Live testing confirms direct-file playback on VLC 3.0.24 x64 for ZIP (nine-track Subspecies 4), generated 7z and RAR; a normal MP3 stayed with VLC's original filesystem access module. The code and installer update are in DEV; a previously published installer such as v0.1.7 does not include this updated module.

See the [feature matrix](../FEATURE-MATRIX.md) and [licensing information](../LICENSING.md).
