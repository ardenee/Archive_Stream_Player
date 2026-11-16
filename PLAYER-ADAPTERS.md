# Archive Stream Player — player setup

Archive Stream Player offers native player plugins wherever the host supports them, with a compatibility playlist mode for other configurations.

## Choosing an integration

| Player | How it connects |
| --- | --- |
| **Winamp 32-bit** | Native input and archive playlist plugins |
| **foobar2000 32/64-bit** | Native component |
| **WACUP 32-bit** | Winamp-compatible native plugins |
| **WACUP 64-bit** | Compatibility playlist |
| **AIMP 32/64-bit** | Native plugin |
| **XMPlay 32-bit** | Archive plugin for 7z/RAR; XMPlay's own ZIP support for compatible ZIP files |
| **MusicBee 32-bit** | Native archive audio plugin and Tools integration |
| **MediaMonkey 5 / 2024** | Two companion add-ons for native-first playback |
| **VLC 3 64-bit** | Native access package and archive launcher |
| **Windows Media Player** | Experimental compatibility playlist |

## Getting started

Build or install the integration for your player, choose a ZIP, 7z or RAR archive, and add its audio entries to the player's playlist. Compatible native integrations read tracks directly from the archive. Some formats use the compatibility mode automatically.

The following package builders are included with the source:

- **foobar2000:** `foo_archive_stream/package-foobar.ps1`
- **MusicBee:** `musicbee_archive_stream/package-musicbee.ps1`
- **MediaMonkey:** `mediamonkey_archive_stream/package-mmip.ps1` and `mediamonkey_archive_stream/package-native-addon.ps1` (install both)
- **VLC:** `vlc_archive_stream/package-native-vlc.ps1`
- **Other supported players:** `archive_player_bridge/package-native-adapters.ps1` and the appropriate native plugin project

A complete set of public, ready-made installers is not yet available. See the relevant player folder for its source and package scripts.

## Playback and settings

The shared playback order is **direct archive stream → bounded memory → reusable managed cache**. Cache files contain extracted audio entries, not the contents of an entire album. You can set a memory limit, manage or clear the cache, choose title presets, configure passwords and enable optional logging.

Encrypted archives prompt for a password when needed. Saving a password requires your consent. Supported codecs and archive layouts depend on the player.

## Known limitations

- WACUP 64-bit uses the compatibility path rather than the 32-bit Winamp input plugin.
- XMPlay can use its own ZIP handling, which does not use every Archive Stream preference.
- Some hosts cannot change already-queued track titles without readding or reloading the playlist.
- Windows Media Player support is experimental.
- Unsupported archive and codec combinations may use a fallback or be rejected.

See the [feature matrix](FEATURE-MATRIX.md) and [third-party licensing information](third_party/README.md).
