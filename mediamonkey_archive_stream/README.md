# Archive Stream Player for MediaMonkey 5 / 2024

**Add songs directly from ZIP, 7z and RAR archives to MediaMonkey's playlist.**

MediaMonkey uses a Tools-menu add-on together with a companion audio input plugin. Native playback is preferred; compatibility mode remains available for unsupported configurations.

## Features

- Browse an archive and add individual tracks to Now Playing.
- Play supported MP3, FLAC, WAV, OGG, AAC, M4A and WMA entries.
- Seek within tracks and use shared track-title presets.
- Open supported encrypted, solid and multipart archives.
- Configure shared passwords, memory, cache cleanup and logging.
- Preserve the original archive and audio tags.

## Install

Create the two MediaMonkey packages from the repository root:

```powershell
.\mediamonkey_archive_stream\package-mmip.ps1
.\mediamonkey_archive_stream\package-native-addon.ps1
```

In **MediaMonkey → Tools → Addons → Add**, install both:

1. `ArchiveStreamPlayer-MediaMonkey5.mmip` — audio input plugin.
2. `ArchiveStreamPlayer-MediaMonkey5-Native.mmip` — archive actions and companion tools.

Restart MediaMonkey, then use the Archive Stream command in its Tools menu to select an archive. An explicit compatibility option is available.

## Status

Native-first archive playback is working for the supported formats. Some layouts require fallback handling. Shared title presets apply when tracks are added; updating titles for an existing multi-track playlist may require readding it.

See the [project feature matrix](../FEATURE-MATRIX.md) and [licensing](../LICENSING.md).
