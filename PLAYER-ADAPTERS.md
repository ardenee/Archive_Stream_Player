# Player installation

[Download the installer](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.7), run it and select your installed music players. Portable installations can be chosen manually.

| Player | Installation |
| --- | --- |
| Winamp / WACUP 32-bit | Installs the native input and playlist plugins |
| foobar2000 32/64-bit | Installs the matching native component |
| AIMP 32/64-bit | Installs the matching native plugin |
| XMPlay 32-bit | Installs `xmp-archive-stream.dll`. In **Options → Decoders**, select the **Archive Stream Player** tree page, or use **Archive Stream ZIP/7z/RAR → Config** for the standalone dialog. XMPlay handles ordinary ZIP files itself |
| MusicBee 32-bit | Native BASS addon and MusicBee Tools plugin; persistent archive-only playlist is identified via SDK folder/name/URL, not tab number. ID3v2.3 v4 aliases preserve .ast manifests and omit problematic PRIV/TXXX/COMM frames. Shared Use File Tags and title presets refresh live. Playback triggers low-priority PCM duration verification of its archive; exact lengths are cached and added to TLEN aliases. Native MusicBee Time stays blank because its Duration file property is read-only. **Tools > Archive Stream: Verified Durations** opens an optional independent window using the MusicBee skin/font, showing Artist, Title, Album, Genre and accurate Time/total; double-click to play. The previous host-docked overlay was removed; nothing covers or modifies other user tabs. Ordinary MP3s stay MusicBee-owned. |
| MediaMonkey 5 / 2024 | Installs native playback and Now Playing archive-drop handling; plugin settings replace four redundant Tools menu commands |
| VLC 3 64-bit | Use the **Open an archive in VLC** Start Menu shortcut |

Plugin settings follow the player's own UI: **Winamp/WACUP** uses its input plugin's compact configuration dialog; **foobar2000** uses Preferences → Tools → Archive Stream; **AIMP** uses Preferences → Plugins → Archive Stream Player with AIMP SDK-native tabs and controls; and **XMPlay** uses its embedded Options → Decoders → Archive Stream Player page (with Config still available). All use consistent shared settings underneath, although some players control playlist titles themselves.

The **Configure Archive Stream Player** shortcut manages shared passwords, cache, titles, **Use File Tags** (off by default; overrides title presets), and logging. MusicBee uses ID3 aliases; AIMP/VLC can use a bounded direct-stream tag probe; foobar2000/XMPlay use native host metadata decoding.

WACUP 64-bit and Windows Media Player are not included as native installer options.

[Features and limitations](FEATURE-MATRIX.md) · [Project website](https://www.arden.ie/projects/archive-stream-player/)
