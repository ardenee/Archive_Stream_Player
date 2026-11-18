# Player installation

[Download the installer](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.7), run it and select your installed music players. Portable installations can be chosen manually.

| Player | Installation |
| --- | --- |
| Winamp / WACUP 32-bit | Installs the native input and playlist plugins |
| foobar2000 32/64-bit | Installs the matching native component |
| AIMP 32/64-bit | Installs the matching native plugin |
| XMPlay 32-bit | Installs `xmp-archive-stream.dll`. In **Options → Decoders**, select the **Archive Stream Player** tree page, or use **Archive Stream ZIP/7z/RAR → Config** for the standalone dialog. XMPlay handles ordinary ZIP files itself |
| MusicBee 32-bit | Installs native BASS add-on and Tools menu plugin. MusicBee-compatible `.mp3` descriptor aliases avoid copying compressed audio; archive-only Win32/OLE drop registration extends MusicBee's drop handling without intercepting ordinary audio files; native descriptors now persist under LocalAppData so MusicBee's queued tracks survive restart; drops on Playing Tracks queue; drops on the Music/Tracks main panel use MusicBee's SDK library-import path; panel routing distinguishes Tracks from Playing Tracks, and a real SDK probe confirmed 6/6 archive aliases in MusicBee's library. A delayed refresh is included for the Tracks view; visibility in the active MusicBee view still requires confirmation. Ordinary audio remains MusicBee-owned unless a separately verified native-only format requires fallback. Compatibility bridge tracks remain session-scoped; host playback still requires verification |
| MediaMonkey 5 / 2024 | Installs native playback and Now Playing archive-drop handling; plugin settings replace four redundant Tools menu commands |
| VLC 3 64-bit | Use the **Open an archive in VLC** Start Menu shortcut |

Plugin settings follow the player's own UI: **Winamp/WACUP** uses its input plugin's compact configuration dialog; **foobar2000** uses Preferences → Tools → Archive Stream; **AIMP** uses Preferences → Plugins → Archive Stream Player with AIMP SDK-native tabs and controls; and **XMPlay** uses its embedded Options → Decoders → Archive Stream Player page (with Config still available). All use consistent shared settings underneath, although some players control playlist titles themselves.

The **Configure Archive Stream Player** shortcut also manages shared passwords, cache, titles and logging.

WACUP 64-bit and Windows Media Player are not included as native installer options.

[Features and limitations](FEATURE-MATRIX.md) · [Project website](https://www.arden.ie/projects/archive-stream-player/)
