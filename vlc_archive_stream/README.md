# Archive Stream Player for VLC

**Open ZIP, 7z and RAR audio archives in VLC 3 (64-bit).**

The VLC integration offers native archive playback, a playlist for an archive's tracks, seeking, password prompts and shared playback preferences. A compatibility option is available when native integration cannot be used.

## Features

- Play individual audio tracks from supported archives.
- Browse an archive as a VLC playlist.
- Stream directly where possible, with memory and managed-cache fallbacks.
- Use password prompts, title presets and shared playback settings.
- Keep the original archive unchanged.

## Build and install

Build the VLC package with `vlc_archive_stream/package-native-vlc.ps1`. The package includes the access plugin and an archive launcher. The VLC SDK is required for building but is not included in this repository.

Extract the package to a folder of your choice. You do not need to copy its plugin into VLC's installation folder.

Example:

```powershell
.\launch-archive.ps1 -Player VLC -PlayerPath 'C:\Path\To\VLC\vlc.exe' -Archive 'C:\Music\Album.7z'
```

The launcher also offers memory, cache, password-list and title options.

## Status

Native playback is available for **VLC 3 64-bit**. Other VLC versions and architectures may use the compatibility mode instead. Updating titles already shown in a playlist may require reopening the album.

See the [feature matrix](../FEATURE-MATRIX.md) and [licensing information](../LICENSING.md).
