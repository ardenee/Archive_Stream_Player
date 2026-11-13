# Additional player adapters: XMPlay, AIMP, and WACUP

## Shared preferences and password / cache / title parity (13 November 2026)

The newer adapters use a **common settings window**, implemented by [`archive_player_bridge/configure-adapters.ps1`](archive_player_bridge/configure-adapters.ps1). Settings are stored per-user at `HKCU\Software\ArchiveStreamPlayer\Adapters`, and apply to **AIMP x86/x64, XMPlay x86, MusicBee and the playlist/HTTP bridge**. The existing Winamp and foobar2000 native preferences remain separate and unchanged. A settings change applies to newly opened tracks/sessions; restart native plugins after changing the password-list path.

~~~powershell
# Open the shared graphical preferences window
& '.\archive_player_bridge\launch-archive.ps1' -Configure

# Non-GUI status for verification or automation
& '.\archive_player_bridge\launch-archive.ps1' -Configure -NoLaunch
~~~

| Setting | Behavior |
| --- | --- |
| **Memory threshold** | 1–512 MiB (default 50): choose direct stream first; use RAM only for eligible entries within this threshold |
| **Temporary-cache budget** | 0–20,480 MiB (default 2,048): 0 disables temporary-file fallback; AIMP/XMPlay restrict each entry, while the bridge tracks total files in its session |
| **Password file** | Optional Unicode path to a plaintext list; blank uses `%LOCALAPPDATA%\WinampArchiveStream\keeps.txt` for AIMP, XMPlay, MusicBee and bridge clients |
| **Prompt for missing password** | On by default, using native Windows credentials UI; disabled for unattended tests or where dialogs are unwanted |
| **Offer to save password** | On by default; prompts for explicit consent, then the backend appends a **verified** password to the configured plaintext list |
| **Track-title format** | Six familiar presets: filename, archive type + filename, numbered filename, filename with audio type, archive type + numbered filename, or archive name + numbered filename |
| **Temporary-cache controls** | Display current temporary-session count/size, open the temp folder, and **clear folders older than 24 hours** after confirming MusicBee and the bridge are not running |

**Password security:** Saved passwords are **plain text**, not Windows Credential Manager secrets. Do not select a shared/public file or include it in a package; the credentials UI itself does not persist them. A failed password or cancellation is handled by the backend; worker-thread prompts are serialized by the bridge to avoid simultaneous dialogs. There is **no archive-wide decoding preflight**, so opening a large encrypted album is not delayed by processing every track first. The HTTP player may time out if password entry is left unanswered for a long time. Prompt windows require an interactive Windows session and have not yet been manually certified in every player.

**Unicode password paths:** The shared backend now opens password-list files through native Unicode Windows file APIs (with fallback for legacy ANSI Winamp paths). A synthetic encrypted LZMA2 7z was read and sought successfully using a non-ASCII password-file path on both architectures. The repeatable regression is `archive_backend_test/test_unicode_passwords.py` and the temporary-file read/append self-test is `archive_backend_test.exe pwlistselftest`. No saved user password was copied into or committed to the tests. **Known encryption gaps:** a separate synthetic 7z with encrypted file headers could not be enumerated (`SZ_ERROR_UNSUPPORTED`), and an AES+stored coder-layout fixture failed the direct stream graph check. These layouts need their own backend investigation; the new password-path behavior does not establish support for them.

**Cache scope:** AIMP and XMPlay delete their per-track extracted files when the reader is released; the bridge deletes its per-session files at shutdown. These are **not the persistent Winamp/foobar cache**. The shared GUI intentionally does not claim to browse or erase the existing native players' managed caches: use Winamp/foobar's own preferences for that. Stale-file cleanup refuses to run if a bridge or MusicBee process is active, and affects only clearly named temporary session folders older than 24 hours.

**Track-title scope:** AIMP sets its virtual track metadata title, and the bridge formats **M3U8 `#EXTINF` titles and ASX `title` elements** without changing the original audio URLs. The XMPlay archive API uses the internal filename as the extraction key, so its file-list entry names must not be rewritten for presentation. MusicBee currently queues HTTP track URLs; its actual displayed names depend on its URL metadata handling, not solely on the M3U playlist labels. Winamp and foobar2000 continue to use their native title templates.

The settings window is accessible from **AIMP Options > Archive Stream Player**, **XMPlay Archive Stream plugin Config**, and **MusicBee Tools > Archive Stream: Settings...**. For **VLC, WMP, WACUP x64 and MediaMonkey's bridge route**, open `launch-archive.ps1 -Configure`. All these launchers need `configure-adapters.ps1` beside the plugin or launcher as provided by the packages.

### Packaging and verified tests

~~~powershell
& '.\archive_player_bridge\package-native-adapters.ps1' -Player AIMP -Platform Win32
& '.\archive_player_bridge\package-native-adapters.ps1' -Player AIMP -Platform x64
& '.\archive_player_bridge\package-native-adapters.ps1' -Player XMPlay -Platform Win32
& '.\musicbee_archive_stream\package-musicbee.ps1'

py -3.13 '.\archive_player_bridge\test_bridge.py'
py -3.13 '.\archive_player_bridge\test_cache_controls.py'
py -3.13 '.\archive_player_bridge\test_passwords.py' --audiotest-generator-key
~~~

**DEV confirmed:** AIMP x86/x64 and XMPlay x86 compiled, MusicBee compiled; AIMP x86/x64 registered the Options service in live portable player processes; the XMPlay mock confirmed its non-null Config callback, decoded a WAV file, and performed 64-byte partial probes. The shared Windows Forms settings dialog opened with its expected controls and completed registry-save round trips for both enabled and disabled flags. The bridge passed all six M3U/ASX title presets, byte-for-byte reads in all three routes, cache-budget rejection, disabled-prompt behavior, and encrypted 7z regression on both architectures. GUI clicking through every host's settings and actual incorrect-password recovery in each player remain manual acceptance tasks.



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

XMPARC.DecompressFile takes the requested maximum decoded-byte length, which may be shorter than the entry; the adapter supports such partial probes. It prefers backend decoded-byte streaming, otherwise extracts entirely into memory when within the configured memory threshold, otherwise uses a temporary file. Temporary files are removed automatically. The return value is always an **XMPlay-owned decoded byte block**; XMPARC does not provide a way to transfer an ongoing archive stream directly to the host decoder. A large entry can therefore require a large host buffer even when the archive backend reads it incrementally. This is **not on-demand streaming at the player level**. The memory threshold applies to fallback extraction, not the memory block requested by XMPARC.

**XMPlay streaming playback alternative:** XMPlay can receive an M3U8 playlist with decoded archive bytes served on demand by our loopback HTTP bridge. This bypasses XMPARC's full-entry buffer requirement while preserving the required **direct stream → memory fallback → disk cache** order in the shared backend. The player still handles MP3/FLAC/WAV/Ogg decoding; only the compressed archive entries are streamed. Use the following command with the local XMPlay installation (or supply your own `-PlayerPath`):

~~~powershell
& '.\archive_player_bridge\launch-archive.ps1' -Archive 'C:\audiotest\Archives\7Z_NonSolid.7z' -Player XMPlay -PlayerPath 'D:\Temp\ArchiveStreamPlayerAdapters\xmplay\xmplay.exe'
~~~

This bridge alternative provides stream-first playback but **does not give XMPlay native archive drag-and-drop expansion**. The XMPARC plugin remains available for native archive browsing and buffered playback. Keep the bridge launcher open until playback ends. For on-demand streaming inside XMPlay's native archive browsing flow, a different host integration would be required; XMPARC cannot supply it.

The DEV x86 host contract test passed WAV full-entry and first-64-byte parity for ZIP, 7z and RAR, and a 61 MiB solid 7z fixture passed through cache fallback. A real XMPlay 4.1 instance loaded the DLL, enumerated four tracks from a solid 7z, routed solid members to memory and the nonsolid member through the backend stream (then into an XMPlay-owned buffer). Separately, an isolated XMPlay 4.1 instance launched with a generated bridge M3U8 and `-play` requested a live archive-backed HTTP stream: `ROUTE_COUNTS stream=1 memory=0 cache=0`. Audible output and interactive seeking are not independently certified.

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

Each virtual stream uses **stream first → in-memory extraction (default 50 MiB, configurable) → temporary file cache**. Solid 7z and RAR bypass direct streaming; temp files are automatically removed when the stream is released. The official SDK provides the COM interfaces used by the independent host contract test.

AIMP x86 and x64 mock-host tests verified registration of FileFormat and FileExpander, virtual-file URI properties, reads and rewind/end seeking, and a 61 MiB solid 7z cache case. Six additional x86/x64 host-contract runs across ZIP, nonsolid 7z and RAR WAV archives passed read/seek/rewind tests, with direct streaming verified through the plugin's route logging. Both actual portable AIMP 5.40 builds accepted archive files in the Add Files dialog and expanded four tracks in a solid 7z. AIMP x64 traced route-to-memory when playback was requested. Audible playback, UI seeking, and interactive encrypted-password prompts are not yet fully verified.

## WACUP (Winamp compatibility)

### WACUP 32-bit — verified on build 1.99.57.25464

Use the existing Winamp Release|Win32 plugin binaries:

- in_archive_stream\Release\in_archive_stream.dll
- gen_archive_stream\Release\gen_archive_stream.dll

Copy them to the WACUP x86 Plugins folder. Portable WACUP blocks third-party plugins by default; enable **Preferences → Advanced → Portable Mode → Allow 3rd-party (non-WACUP and non-native) plug-ins** and restart. WACUP bundles an in_zip.dll that takes over some ZIP/7z routing, creating zip:// playlist records instead of the Archive Stream virtual-file format; **disable its bundled ZIP handler in the test installation** if you want Archive Stream's own list expansion. In WACUP Preferences → Plug-ins → General Purpose, select Archive Stream Playlist Expander and click Configure on an archive playlist item.

WACUP's WM_COPYDATA enqueue call may return zero even when the track was successfully appended. The Winamp general adapter now verifies the actual playlist count increase, and it appends all replacements before removing the original archive entry so failed expansion leaves the original intact.

An isolated WACUP x86 installation loaded both DLLs and expanded a four-entry solid 7z. Winamp-compatible IPC then confirmed that playback time advanced during the WAV track and the player transitioned to FLAC. Logs confirmed solid 7z was routed to memory.

### WACUP 64-bit — native third-party plugins blocked; bridge playback verified

**WACUP x64 does not currently support loading third-party Winamp-compatible plugins.** This is a limitation of the WACUP x64 preview host, **not a failure in our archive backend**. WACUP's official [current x64 preview page](https://getwacup.com/preview_x64/) explicitly says to use WACUP x86 for third-party Winamp plugins. The current preview tested is **1.99.57.25464 (6 November 2026)**. The developer has not promised that the restriction is permanent; it must be reviewed again if WACUP adds a public third-party x64 plugin interface.

Both of our Winamp plugins compile as x64, but this **does not make them compatible with WACUP x64's loader**. In a separate, SHA256-verified and extracted 25464 x64 test installation, `in_archive_stream.dll` and `gen_archive_stream.dll` were staged in `Plugins`, but neither appeared in WACUP's loaded module list; its own `in_url.dll` and `in_what.dll` did load. An earlier **1.99.47.23960** isolated x64 test likewise failed to load our DLLs with the portable third-party setting enabled. Do not copy the x64 Winamp DLLs to an installed WACUP as a supposed fix. The **WACUP x86 plugin integration** above remains the native choice.

**WACUP x64 can play audio from archives through our independently built local playback bridge**, which uses WACUP's own M3U8 playlist parser and HTTP/codec support instead of trying to load a plugin. Build the x64 bridge and run:

~~~powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild 'archive_player_bridge\archive_player_bridge.vcxproj' /m /p:Configuration=Release /p:Platform=x64
& '.\archive_player_bridge\launch-archive.ps1' -Archive 'C:\audiotest\Archives\7Z_NonSolid.7z' -Player WACUPx64
~~~

The launcher opens the temporary archive playlist with WACUP's `/ENQPLAY` command, then keeps the bridge running while WACUP reads its localhost track URLs. Press Enter **after playback finishes** to shut down the bridge and remove temporary files. For a portable or nonstandard WACUP installation, supply `-PlayerPath 'D:\Path\To\WACUP\wacup.exe'`. Password candidates default to `%LOCALAPPDATA%\WinampArchiveStream\keeps.txt`. The original archives remain untouched, and no external archive decoder is required.

**Real-host verification on WACUP x64 25464:** two entries in a ZIP-derived playlist were enqueued; WACUP reported active playback and progressing time beyond four seconds, and the bridge counted a genuine decoded-byte **stream** request. A solid 7z playlist likewise loaded both tracks, advanced through active playback, and used the bridge's **memory** route. The end-to-end `launch-archive.ps1 -Player WACUPx64` path was separately exercised against both archive types: two tracks, active playback and increasing elapsed time, with normal launcher exit. Output-device audibility and player UI seeking across every codec are not fully certified.

**Scope:** WACUP x64 supports **archive-backed playlist/HTTP playback**, **not native ZIP/7z/RAR drag-and-drop expansion or an Archive Stream settings page inside WACUP**. The temporary URLs work only while the bridge runs. For a fully integrated Winamp-style playlist expander, use **WACUP x86** until WACUP's x64 host gains public third-party plugin support.

## MediaMonkey, MusicBee, VLC, and Windows Media Player (13 November 2026)

These integrations reuse the **existing statically linked** ZIP/7z/RAR/AES backend. The main addition is an x86/x64 seekable loopback HTTP bridge, plus a native MusicBee C# plugin and a MediaMonkey MMIP packaging project. **VLC and Windows Media Player use supported playlist and HTTP media interfaces; they do not have native archive-format plugins.**

### Seekable localhost playback bridge (Windows x86/x64)

`archive_player_bridge/archive_player_bridge.vcxproj` builds against the existing static archive libraries. The standalone executable binds to **127.0.0.1 only**, uses an unpredictable 128-bit URL token, and serves decoded archive-entry bytes (not decoded PCM) as ordinary audio URLs. It supports GET, HEAD and HTTP Range (206), MIME types and per-entry extensions, M3U8 and ASX playlists, a token-authenticated POST /shutdown, and automatic cleanup of its temporary cache. It does not need any external archive DLL or extraction process.

Entry priority: **direct decoded-byte archive stream -> in-memory extract (default 50 MiB) -> temporary disk-cache file**. Solid 7z/RAR bypass direct stream. The decoder/codec remains the player's responsibility. This is **not a persistent background service**; URLs work only while their bridge process is running and cannot be used as permanent library references.

~~~powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild 'archive_player_bridge\archive_player_bridge.vcxproj' /m /p:Configuration=Release /p:Platform=Win32
& $msbuild 'archive_player_bridge\archive_player_bridge.vcxproj' /m /p:Configuration=Release /p:Platform=x64

# Choose a player: VLC, WMP, MusicBee, MediaMonkey, WACUPx64, or XMPlay
& '.\archive_player_bridge\launch-archive.ps1' -Archive 'C:\audiotest\Archives\7Z_NonSolid.7z' -Player VLC
~~~

The launcher starts its own temporary bridge, passes the corresponding M3U8 or ASX playlist to the player, and stays alive until the user closes the launcher. Do not close it during playback. `archive_player_bridge.exe --archive "C:\audiotest\Archives\7Z_NonSolid.7z"` also works on its own and prints the generated playlist paths and URLs; optional flags are `--port`, `--playlist-dir`, `--memory-mb`, `--lifetime` and `--password-list`.

**Testing:** The self-contained `archive_player_bridge/test_bridge.py` builds small ZIP and solid 7z fixtures from `C:\audiotest\01.MP3File.mp3` and `C:\audiotest\02.OGGFile.ogg`, using installed 7z.exe **for generating test data only**. DEV Win32/x64 tests passed every mid-track 2,000-byte HTTP range/byte-parity read, HEAD size, rejected unknown endpoint, and clean shutdown. Both ZIP cases used 2/2 streams; solid 7z cases used 2/2 memory extractions with the default buffer, and **2/2 file-cache reads with the memory limit forced to 1 MiB**. All three routes passed byte-for-byte parity on both architectures. An additional stream-priority regression (`test_stream_priority.py`) checks ZIP, nonsolid 7z and nonsolid RAR HTTP range requests on both architectures with disk caching disabled. All six cases returned `ROUTE_COUNTS stream=1 memory=0 cache=0`. Run after building the executables:

~~~powershell
py -3.13 '.\archive_player_bridge\test_bridge.py'
py -3.13 '.\archive_player_bridge\test_stream_priority.py'
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
