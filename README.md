# Archive Stream Player

**Archive Stream Player** lets compatible Windows music players browse and play songs stored inside **ZIP, 7z and RAR archives** without unpacking an entire album first. It adds archive support through native plug-ins and shares one archive-reading, playback-routing and settings backend across supported players.

[**Download installer**](https://github.com/ardenee/Archive_Stream_Player/releases/latest) · [Project website](https://www.arden.ie/projects/archive-stream-player/) · [Player setup guide](PLAYER-ADAPTERS.md)

## Features and benefits

- **Play directly from archives:** open a ZIP, 7z or RAR album and select individual tracks in a supported player.
- **Stream-first playback:** access compressed entries directly where possible; fall back to the configured memory buffer, then managed disk cache only when necessary.
- **More archive types:** supports common ZIP, 7z, RAR/RAR5, multipart, solid and password-protected archives, subject to their compression/encryption methods.
- **Audio playback and seeking:** uses either Archive Stream's audio decoders or the player's existing decoders, depending on the integration.
- **Track information:** title formatting, embedded tags and durations where available and supported by the player.
- **Shared preferences:** passwords, memory buffer, cache limits, title options, optional embedded file tags and logging. VLC can use separate overrides.

**Audio formats:** MP3, PCM WAV, FLAC, Ogg Vorbis, AAC, M4A and WMA. Decoder and sample-format support varies by player.

## Supported players

| Player | Supported versions | Integration and limitations |
| --- | --- | --- |
| **Winamp** | 32-bit | Native input/playlist plug-ins. Archive Stream provides audio decoding and shared settings. |
| **WACUP** | 32-bit | Winamp-compatible native plug-ins. **64-bit WACUP is not supported.** |
| **foobar2000** | 32-bit and 64-bit | Native component opens archive entries; foobar2000 must have a decoder for the audio format. Uses host metadata where available. |
| **AIMP** | 32-bit and 64-bit | Native plug-in using AIMP's audio decoders. Embedded title refresh supported; pre-play duration is available from trusted MP3, FLAC or WAV headers, otherwise may be blank until playback. |
| **XMPlay** | 32-bit | Native archive plug-in for 7z/RAR; XMPlay also has its own ZIP handling, which may not follow Archive Stream's ZIP-specific settings. Uses XMPlay's audio decoders. |
| **MusicBee** | 32-bit | Native BASS decoder with an archive-only playlist. Verified durations can populate MusicBee's native Time column for eligible directly streamable MP3s with genuine Info/Xing headers; other tracks may have no native Time value. Automatic playlist-tab selection is not guaranteed. |
| **MediaMonkey** | 5 / 2024 | Native archive playback add-on with shared settings and archive-aware track handling. |
| **VLC** | 3, 64-bit | Native archive access/directory plug-in; open or drag ZIP/7z/RAR into VLC. VLC decodes audio. Verified MP3, FLAC and WAV durations and available tags can appear before playback; other cases may remain unknown until played. **VLC 4 is not supported.** |

## Limitations

- Streaming depends on the archive's compression and encryption method. Solid and encrypted entries may require memory buffering or managed cache; damaged archives, missing multipart volumes and unsupported methods can fail.
- Track titles, metadata refresh, playlist behaviour and reported durations depend on each player's APIs and the audio headers available. Not every player can show complete metadata before playback.
- foobar2000, AIMP, XMPlay and VLC require their own decoders for the music formats being played. Archive Stream does not supply missing codecs to those players.
- The plug-in does not repair archives, recover missing volumes, bypass DRM or extract entire albums into folders by default.
- **Modern Microsoft Media Player (2022)**, **WACUP 64-bit** and **VLC 4** are not supported.

## Installation

Download the [latest Windows installer](https://github.com/ardenee/Archive_Stream_Player/releases/latest) and select the compatible music players installed on your machine. Portable player installations can be selected manually.

For player-specific setup and settings, see the [installation guide](PLAYER-ADAPTERS.md). For more detail on archive handling and differences between players, see the [feature matrix](FEATURE-MATRIX.md).

## Licence

Archive Stream Player project code is released under the [MIT licence](LICENSE). Included third-party components retain their respective [licences](LICENSING.md).
