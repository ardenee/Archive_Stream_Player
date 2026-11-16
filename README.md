# Archive Stream Player

**Play music directly from ZIP, 7z and RAR archives in Windows music players.**

Archive Stream Player lets you browse the songs inside an archive, add individual tracks to a playlist and play them without unpacking an entire album first. It uses one shared archive engine with integrations for several players.

## Features

- **Archives:** ZIP, 7z and RAR, including supported solid and multipart archives.
- **Audio:** MP3, FLAC, WAV, Ogg Vorbis, AAC, M4A and WMA, subject to the player's decoder support.
- **Playback:** Direct streaming from the archive where possible, a configurable memory buffer when necessary, and a reusable managed disk cache only as a last resort.
- **Encrypted archives:** Password prompts, reusable password lists and optional saving with your consent.
- **Playlists:** Browse archives, queue individual songs, seek within tracks and use shared track-title presets.
- **Settings:** Configure memory, cache size and cleanup, password handling, titles and optional logging.
- **Source protection:** Your original archives and audio metadata are not modified.

Feature availability depends on the player and on the archive and audio format.

## Supported players

| Player | Integration | Project status |
| --- | --- | --- |
| **Winamp** | Native 32-bit plugins | Working |
| **foobar2000** | Native 32-bit and 64-bit component | Working |
| **WACUP** | Native 32-bit plugins; compatibility mode for 64-bit | Working within those modes |
| **AIMP** | Native 32-bit and 64-bit plugin | Working |
| **XMPlay** | Native 7z/RAR integration; XMPlay's own ZIP support; compatibility mode for other layouts | Working with format limitations |
| **MusicBee** | Native 32-bit playback, with compatibility fallback | Working |
| **MediaMonkey 5 / 2024** | Native-first add-on and input plugin, with compatibility fallback | Working |
| **VLC 3** | Native 64-bit archive access, with compatibility fallback | Working |
| **Windows Media Player (classic)** | Playlist/HTTP compatibility | Experimental |

See the [feature matrix](FEATURE-MATRIX.md) for a concise overview of what each integration provides.

## Project status

**Core archive playback and player integrations are implemented and usable in the supported configurations.** Work remains on packaging and distribution. Not every archive, encryption method, or host-specific interface supports every feature.

Some players require you to re-add tracks after changing title presets; player SDK limitations prevent updating every existing playlist row in place. Windows Media Player support remains experimental.

## Installation and settings

The project is available as source code; a complete set of ready-to-install releases has not yet been published.

See [player setup and usage](PLAYER-ADAPTERS.md) for the available packaging options and how to open archives. Shared settings let you control passwords, cache usage, titles and logging.

## Licence and credits

The original Archive Stream Player code uses the [MIT License](LICENSE). Third-party libraries and player SDK components retain their own licences and redistribution conditions. See [licensing](LICENSING.md) and [third-party components](third_party/README.md).
