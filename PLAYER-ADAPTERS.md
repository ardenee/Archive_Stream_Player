# Additional player adapters: XMPlay, AIMP, and WACUP

DEV implementation and verification: 13 November 2026.

These integrations reuse the same statically linked ZIP, 7z, RAR, AES, multipart and extraction backend as Winamp and foobar2000. No new archive-related runtime DLLs or external extraction executables are introduced. The official AIMP and XMPlay SDK files are **not bundled** and are needed only when compiling their respective adapters.

## XMPlay 4.1 — native x86 adapter

Download the official XMPlay plugin SDK from https://www.un4seen.com/files/xmp-sdk.zip and extract it outside the repository. Its root contains xmparc.h and xmpfunc.h. The DEV SDK lives at D:\Temp\ArchiveStreamPlayerAdapters\xmplay-sdk.

~~~powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$sdk = 'D:\Temp\ArchiveStreamPlayerAdapters\xmplay-sdk'
& $msbuild 'xmp_archive_stream\xmp_archive_stream.vcxproj' /m /p:Configuration=Release /p:Platform=Win32 "/p:XmplaySdkRoot=$sdk"
& $msbuild 'xmp_archive_stream\xmplay_adapter_test.vcxproj' /m /p:Configuration=Release /p:Platform=Win32 "/p:XmplaySdkRoot=$sdk"
~~~

Copy xmp_archive_stream\Release\xmp-archive-stream.dll into the XMPlay installation directory. XMPlay detects its plugins by filename; the **xmp- prefix is mandatory**. This plugin uses XMPlay's native XMPARC archive interface rather than a Winamp plugin wrapper. It lists supported audio members, exposes encrypted and multipart ZIP/7z/RAR archive support, and handles file names in UTF-8.

XMPARC.DecompressFile takes the requested maximum decoded-byte length, which may be shorter than the entry; the adapter supports such partial probes. It prefers backend decoded-byte streaming, otherwise extracts entirely into memory when at most 50 MiB, otherwise uses a temporary file. Temporary files are removed automatically. The return value is always an **XMPlay-owned decoded byte block**; XMPARC does not provide a way to transfer an ongoing archive stream directly to the host decoder. This is not full zero-buffer streaming at the player level.

The DEV x86 host contract test passed WAV full-entry and first-64-byte parity for ZIP, 7z and RAR, and a 61 MiB solid 7z fixture passed through cache fallback. A real XMPlay 4.1 instance loaded the DLL, enumerated four tracks from a solid 7z, routed solid members to memory and the nonsolid member through the stream. Audible output is not independently certified.

## AIMP 5.40 — native x86 and x64 adapters

Obtain the official AIMP v5.40 SDK at https://aimp.su/?do=download&os=desktop&cat=sdk and extract it outside the repository. SDK must have Sources\Cpp\apiPlugin.h. On DEV it is D:\Temp\ArchiveStreamPlayerAdapters\aimp-sdk.

~~~powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$sdk = 'D:\Temp\ArchiveStreamPlayerAdapters\aimp-sdk'
& $msbuild 'aimp_archive_stream\aimp_archive_stream.vcxproj' /m /p:Configuration=Release /p:Platform=x64 "/p:AimpSdkRoot=$sdk"
& $msbuild 'aimp_archive_stream\aimp_adapter_test.vcxproj' /m /p:Configuration=Release /p:Platform=x64 "/p:AimpSdkRoot=$sdk"
# Repeat with /p:Platform=Win32 for 32-bit AIMP.
~~~

Copy the matching-architecture aimp_archive_stream.dll into an AIMP plugin subfolder, for example AIMP\Plugins\aimp_archive_stream\aimp_archive_stream.dll. AIMP registers the archive extensions *.zip, *.7z, *.rar, *.zip.001 and *.7z.001 via its FileFormat service; its FileExpander generates independently seekable virtual tracks. The AIMP virtual URI follows the SDK's archive:index/path format. Audio decoding remains AIMP's job: the adapter returns **decoded archive-entry bytes** through IAIMPStream, not PCM.

Each virtual stream uses **stream first → in-memory extraction (50 MiB) → temporary file cache**. Solid 7z and RAR bypass direct streaming; temp files are automatically removed when the stream is released. The official SDK provides the COM interfaces used by the independent host contract test.

AIMP x86 and x64 mock-host tests verified registration of FileFormat and FileExpander, virtual-file URI properties, reads and rewind/end seeking, and a 61 MiB solid 7z cache case. Both actual portable AIMP 5.40 builds accepted archive files in the Add Files dialog and expanded four tracks in a solid 7z. AIMP x64 traced route-to-memory when playback was requested. Audible playback, UI seeking, and interactive encrypted-password prompts are not yet fully verified.

## WACUP (Winamp compatibility)

### WACUP 32-bit — verified on build 1.99.57.25464

Use the existing Winamp Release|Win32 plugin binaries:

- in_archive_stream\Release\in_archive_stream.dll
- gen_archive_stream\Release\gen_archive_stream.dll

Copy them to the WACUP x86 Plugins folder. Portable WACUP blocks third-party plugins by default; enable **Preferences → Advanced → Portable Mode → Allow 3rd-party (non-WACUP and non-native) plug-ins** and restart. WACUP bundles an in_zip.dll that takes over some ZIP/7z routing, creating zip:// playlist records instead of the Archive Stream virtual-file format; **disable its bundled ZIP handler in the test installation** if you want Archive Stream's own list expansion. In WACUP Preferences → Plug-ins → General Purpose, select Archive Stream Playlist Expander and click Configure on an archive playlist item.

WACUP's WM_COPYDATA enqueue call may return zero even when the track was successfully appended. The Winamp general adapter now verifies the actual playlist count increase, and it appends all replacements before removing the original archive entry so failed expansion leaves the original intact.

An isolated WACUP x86 installation loaded both DLLs and expanded a four-entry solid 7z. Winamp-compatible IPC then confirmed that playback time advanced during the WAV track and the player transitioned to FLAC. Logs confirmed solid 7z was routed to memory.

### WACUP 64-bit — binaries compile; the host does not load them

The Winamp plugin Visual Studio projects now also build Release|x64 against the matching static backend libraries, without accidentally copying x64 binaries into the installed 32-bit Winamp directory. **However, that does not mean WACUP x64 supports them**. A separate portable test based on installed WACUP x64 1.99.57.25464 did not load either x64 DLL, even after enabling portable third-party plugins. WACUP's published x64 preview documentation warns of limited third-party Winamp plugin compatibility: https://getwacup.com/preview_x64/index_prior_23410.php

Do **not** replace the installed 64-bit WACUP's plugins with the x64 builds or assume they are accepted. WACUP x86 is the supported compatibility path at this stage.

## MediaMonkey, MusicBee, VLC, and Windows Media Player (13 November 2026)

These integrations reuse the **existing statically linked** ZIP/7z/RAR/AES backend. The main addition is an x86/x64 seekable loopback HTTP bridge, plus a native MusicBee C# plugin and a MediaMonkey MMIP packaging project. **VLC and Windows Media Player use supported playlist and HTTP media interfaces; they do not have native archive-format plugins.**

### Seekable localhost playback bridge (Windows x86/x64)

`archive_player_bridge/archive_player_bridge.vcxproj` builds against the existing static archive libraries. The standalone executable binds to **127.0.0.1 only**, uses an unpredictable 128-bit URL token, and serves decoded archive-entry bytes (not decoded PCM) as ordinary audio URLs. It supports GET, HEAD and HTTP Range (206), MIME types and per-entry extensions, M3U8 and ASX playlists, a token-authenticated POST /shutdown, and automatic cleanup of its temporary cache. It does not need any external archive DLL or extraction process.

Entry priority: **direct decoded-byte archive stream -> in-memory extract (default 50 MiB) -> temporary disk-cache file**. Solid 7z/RAR bypass direct stream. The decoder/codec remains the player's responsibility. This is **not a persistent background service**; URLs work only while their bridge process is running and cannot be used as permanent library references.

~~~powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild 'archive_player_bridge\archive_player_bridge.vcxproj' /m /p:Configuration=Release /p:Platform=Win32
& $msbuild 'archive_player_bridge\archive_player_bridge.vcxproj' /m /p:Configuration=Release /p:Platform=x64

# Choose a player: VLC, WMP, MusicBee, or MediaMonkey
& '.\archive_player_bridge\launch-archive.ps1' -Archive 'C:\audiotest\Archives\7Z_NonSolid.7z' -Player VLC
~~~

The launcher starts its own temporary bridge, passes the corresponding M3U8 or ASX playlist to the player, and stays alive until the user closes the launcher. Do not close it during playback. `archive_player_bridge.exe --archive "C:\audiotest\Archives\7Z_NonSolid.7z"` also works on its own and prints the generated playlist paths and URLs; optional flags are `--port`, `--playlist-dir`, `--memory-mb`, `--lifetime` and `--password-list`.

**Testing:** The self-contained `archive_player_bridge/test_bridge.py` builds small ZIP and solid 7z fixtures from `C:\audiotest\01.MP3File.mp3` and `C:\audiotest\02.OGGFile.ogg`, using installed 7z.exe **for generating test data only**. DEV Win32/x64 tests passed every mid-track 2,000-byte HTTP range/byte-parity read, HEAD size, rejected unknown endpoint, and clean shutdown. Both ZIP cases used 2/2 streams; solid 7z cases used 2/2 memory extractions with the default buffer, and **2/2 file-cache reads with the memory limit forced to 1 MiB**. All three routes passed byte-for-byte parity on both architectures. Run after building the executables:

~~~powershell
py -3.13 '.\archive_player_bridge\test_bridge.py'
py -3.13 '.\archive_player_bridge\test_passwords.py' --audiotest-generator-key
~~~

The encrypted test successfully enumerated **21 audio entries** and fetched the first 256 decoded bytes from both `C:\audiotest\Archives\7Z_Password_NonSolid.7z` and `7Z_Password_Solid.7z` on **Win32 and x64**. Nonsolid used AES streaming; solid used memory. The runtime defaults to the user's existing `%LOCALAPPDATA%\WinampArchiveStream\keeps.txt`, or accepts an explicit `--password-list`. **The three saved candidates in that existing file do not contain the password configured by `C:\audiotest\make_test_archives.cmd`**. The passing encrypted test used an **automatically removed temporary test-only password list** from the fixture-generator script (normal file removal, not a secure disk erase). The original Winamp keeps.txt was not changed, read out to logs, or committed.

### MusicBee 3.7.9796 — native C# menu adapter plus bridge

DEV installed player: `C:\Program Files (x86)\MusicBee`. Download the official SDK source `MusicBeeInterface.cs` from https://getmusicbee.com/download/plugins/MusicBeeInterface.cs and store it outside Git (DEV: `D:\Temp\ArchiveStreamNewPlayers`).

~~~powershell
& $msbuild 'musicbee_archive_stream\musicbee_archive_stream.csproj' /p:Configuration=Release /p:Platform=AnyCPU /p:MusicBeeSdkRoot='D:\Temp\ArchiveStreamNewPlayers'
~~~

Run `musicbee_archive_stream/package-musicbee.ps1` to make a self-contained x86 plugin ZIP, or copy **both** `musicbee_archive_stream\Release\MB_ArchiveStream.dll` and the **Win32** `archive_player_bridge\Release\archive_player_bridge.exe` into MusicBee's `Plugins` folder, then restart MusicBee. The native .NET 4.8 plugin registers **Tools > Archive Stream: Add Archive Audio...**. The command selects an archive, starts a loopback bridge, and queues its individually playable track URLs via MusicBee's official Now Playing API. MusicBee handles decoding and DSP. Sessions are closed when the plugin unloads, with a 24-hour maximum lifetime.

**Verified:** x86 SDK build passed, real MusicBee initialized the plugin and reported menu registration, and a managed test opened a bridge session, obtained two track URLs, requested 16 valid bytes from each and performed cleanup. **An actual GUI menu action, successful Now Playing queue insertion and audible playback in MusicBee still require a manual live test.** Set `ARCHIVE_STREAM_ADAPTER_DEBUG=1` before launching MusicBee to enable `%LOCALAPPDATA%\MusicBeeArchiveStream\adapter.log`.

### MediaMonkey 2024 — Winamp DLL compatibility and playlist bridge

DEV installed player: `C:\Program Files (x86)\MediaMonkey 5` (2024.2.2). Its documented MMIP native plugin packaging format supports Winamp 2.x DLLs: https://www.mediamonkey.com/wiki/Getting_Started_(Addons).

~~~powershell
& '.\mediamonkey_archive_stream\package-mmip.ps1'
# In MediaMonkey: Tools > Addons > Add
# Select mediamonkey_archive_stream\Release\ArchiveStreamPlayer-MediaMonkey5.mmip
~~~

The MMIP packages the Release/Win32 `in_archive_stream.dll` and `gen_archive_stream.dll`. In the DEV MediaMonkey playback-engine process, **both DLLs loaded** without a process crash during the stability test. That is **binary-load confirmation, not proof that MediaMonkey invokes the Winamp general-purpose expansion callback**. The in-player list-expansion UI still requires validation, and the standard Winamp `.archive_stream|index|entry` playlist format must not be presumed fully supported in MediaMonkey.

The verified playback alternative is the bridge and MediaMonkey's official `/NoSplash /Add` playlist command:

~~~powershell
& '.\archive_player_bridge\launch-archive.ps1' -Archive 'C:\audiotest\Archives\7Z_NonSolid.7z' -Player MediaMonkey
~~~

MediaMonkey successfully accepted `/Add` and `/Play` in the DEV test, and the bridge recorded an actual direct-stream request (stream=1). Audible output and the native playlist expander remain to be certified separately.

### VLC — M3U8/HTTP playback

Use `launch-archive.ps1 -Player VLC`. VLC receives the archive's per-track URLs through an M3U8 playlist; byte-range seeking is serviced by the same static backend. **It is not a VLC access-module plugin** and VLC does not open the original ZIP/7z/RAR file directly.

A real DEV VLC headless run with dummy audio output successfully consumed the two-track ZIP playlist, exited normally, and the bridge recorded **two live direct-stream requests**. HTTP seeking and byte parity passed in independent tests; audible device output was intentionally bypassed.

### Windows Media Player Legacy 12 — ASX/HTTP playback

Use `launch-archive.ps1 -Player WMP`. The launcher passes a Windows Media Player compatible **ASX playlist** of localhost per-track audio URLs. **This is not a decoder/input-source plugin** for WMP.

The DEV WMP COM control is available (12.0.26100.9818), and an individual bridge track URL generated a live audio request. **Playback was not confirmed**: the COM control stayed in play state **9 (transitioning)** even with the original MP3 opened directly from `C:\audiotest`. A live WMP GUI/output-device test is required before certifying playback.

### Runtime and security notes

The bridge does not expose any network interface other than loopback. The session token protects individual tracks and its shutdown endpoint from blind connections; local users who can access the tokenized playlist can access those tracks during the session. Bridge URLs are ephemeral, not permanent library addresses; the bridge never stores password values in its logs or source. Its temporary files are removed on normal shutdown; forced process termination may leave temporary files and should be followed by a cleanup. Audio codec compatibility remains determined by each player.

## Diagnostics and runtime separation

Set the environment variable ARCHIVE_STREAM_ADAPTER_DEBUG=1 **before launching** AIMP or XMPlay to enable trace logs under:

- %LOCALAPPDATA%\AIMPArchiveStream\aimp_archive_stream.log
- %LOCALAPPDATA%\XMPlayArchiveStream\xmplay_archive_stream.log

The adapters use separate player data/password folders. Winamp and WACUP deliberately share WinampArchiveStream configuration. Passwords are not written into logs.

All DEV SDKs, portable test players, compressed WAV fixtures and generated DLLs stay outside the Git repository. No installed WACUP or foobar2000 configuration was overwritten by these tests. Keep testing encryption, Unicode/split archives, cancellation, audible output and player-level seek transitions before declaring comprehensive playback certification.
