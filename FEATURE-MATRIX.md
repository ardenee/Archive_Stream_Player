# Archive Stream Player — features and status

This is an overview of available functions, not a guarantee that every archive, encryption method and audio format works in every host.

**Working** means the integration supports normal archive playback in its stated configuration. **Limited** means a function depends on the player, archive layout or compatibility route. **Experimental** means the player integration is not fully established.

## Player features

| Player | Archive playback and seek | Encrypted archives | Settings and cache | Track titles | Status |
| --- | --- | --- | --- | --- | --- |
| **Winamp 32-bit** | Native | Supported | Shared controls | Title presets and refresh | Working |
| **foobar2000 32/64-bit** | Native | Supported | Shared controls and native preferences | Live playlist title updates | Working |
| **WACUP 32-bit** | Native | Supported | Shared controls | Title presets | Working |
| **WACUP 64-bit** | Compatibility playlist | Limited by route | Shared controls | Reload playlist for changes | Limited |
| **AIMP 32/64-bit** | Native | Supported layouts | Shared controls | New-track titles; existing-row refresh limited | Working |
| **XMPlay 32-bit** | Native 7z/RAR; built-in ZIP | Limited by route | Shared controls on Archive Stream routes | Reimport for title changes | Working |
| **MusicBee 32-bit** | Native, with fallback | Supported layouts | Shared controls | New-track titles; requeue for changes | Working |
| **MediaMonkey 5 / 2024** | Native-first, with fallback | Supported layouts | Shared controls | Title presets; existing-row changes limited | Working |
| **VLC 3 64-bit** | Native, with fallback | Supported layouts | Shared controls | Titles on newly added tracks | Working |
| **Windows Media Player** | Compatibility playlist | Limited | Bridge settings | Playlist titles | Experimental |

## Shared capabilities

| Function | Status |
| --- | --- |
| Browse ZIP, 7z and RAR audio entries | Available |
| Add individual tracks and play without unpacking whole albums | Available |
| Play MP3, WAV, FLAC, OGG, AAC, M4A and WMA | Available where the player supports the codec |
| Solid and multipart archives | Available for supported layouts |
| Direct archive streaming | Preferred |
| Configurable memory fallback | Available |
| Shared managed cache with cleanup controls | Available when needed |
| Password prompts and optional saved passwords | Available for supported encrypted layouts |
| Track title templates | Available; live updates vary by player |
| Shared settings and optional diagnostics | Available |

**Project status:** Core functionality is available. Installer and distribution work remains. Some host SDK limitations and unsupported archive layouts require the documented fallback modes.

See [README](README.md), [player setup](PLAYER-ADAPTERS.md) and [licensing](LICENSING.md).
