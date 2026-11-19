# Player installation

[Download the installer](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.7), run it and select your installed music players. Portable installations can be chosen manually.

| Player | Installation |
| --- | --- |
| Winamp / WACUP 32-bit | Installs the native input and playlist plugins |
| foobar2000 32/64-bit | Installs the matching native component |
| AIMP 32/64-bit | Installs the matching native plugin |
| XMPlay 32-bit | Installs `xmp-archive-stream.dll`. In **Options → Decoders**, select the **Archive Stream Player** tree page, or use **Archive Stream ZIP/7z/RAR → Config** for the standalone dialog. XMPlay handles ordinary ZIP files itself |
| MusicBee 32-bit | Native BASS addon and MusicBee Tools plugin. Archive Player is a MusicBee-native playlist found via the SDK, not a fixed tab number. Archive ZIP/7z/RAR entries have tiny, ID3v2.3 **v4** metadata aliases backed by exact .ast manifests; no audio extraction is required to list them. The v4 aliases omit internal ID3 PRIV/TXXX/COMM frames, which could cause BASS to reject otherwise valid tracks (fixed and live-verified against Elementis ZIP track 2). Shared Use File Tags and title preset changes refresh the dedicated playlist live; embedded track numbers determine order. After an archive track first plays, a background-priority scan measures verified PCM durations from the .ast references for that album only, caches them, and refreshes its TLEN aliases; the built-in MusicBee playlist Time column can still show 0:00 (no SDK Duration setter). Ordinary files and Music library remain MusicBee-owned; select the archive tab manually if closed. |
| MediaMonkey 5 / 2024 | Installs native playback and Now Playing archive-drop handling; plugin settings replace four redundant Tools menu commands |
| VLC 3 64-bit | Use the **Open an archive in VLC** Start Menu shortcut |

Plugin settings follow the player's own UI: **Winamp/WACUP** uses its input plugin's compact configuration dialog; **foobar2000** uses Preferences → Tools → Archive Stream; **AIMP** uses Preferences → Plugins → Archive Stream Player with AIMP SDK-native tabs and controls; and **XMPlay** uses its embedded Options → Decoders → Archive Stream Player page (with Config still available). All use consistent shared settings underneath, although some players control playlist titles themselves.

The **Configure Archive Stream Player** shortcut manages shared passwords, cache, titles, **Use File Tags** (off by default; overrides title presets), and logging. MusicBee uses ID3 aliases; AIMP/VLC can use a bounded direct-stream tag probe; foobar2000/XMPlay use native host metadata decoding.

WACUP 64-bit and Windows Media Player are not included as native installer options.

[Features and limitations](FEATURE-MATRIX.md) · [Project website](https://www.arden.ie/projects/archive-stream-player/)
