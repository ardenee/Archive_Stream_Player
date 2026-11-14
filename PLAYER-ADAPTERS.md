# Player adapters: streaming behavior, setup and tests

For a player-by-player status overview with explicit real-host, automated-test and unverified distinctions, see [FEATURE-MATRIX.md](FEATURE-MATRIX.md). This document supplies the implementation details and evidence behind those statuses.

## Stream-first routing and failure recovery (13 November 2026)

Our **player adapters** select the entry route using the shared archive backend: **direct incremental archive stream -> in-memory decompression within the configured buffer -> persistent managed disk cache only when necessary**. The host player is not asked to extract the archive. AIMP's native `IAIMPStream`, foobar2000's virtual-file reader, and the localhost HTTP bridge all expose decoded archive bytes on demand to the player codec. There is no full-archive extraction step on a successful stream route. Solid layouts and unsupported encrypted codecs may require the memory route from the outset.

**Encrypted RAR5 memory fallback:** The shared backend now uses the statically linked UnRAR SDK's `RAR_TEST` / `UCM_PROCESSDATA` callback to capture a selected encrypted entry directly in RAM, with a hard pre-allocation size check and UnRAR's test/CRC verification. It does not create a transient extracted audio file. The same `ArchiveExtractEntryToMemory()` API is used by AIMP, the HTTP bridge and foobar2000; foobar2000 also has a direct native-RAR path to avoid the previously problematic libarchive RAR5 decoder. Entries above the memory limit continue to use the permitted disk fallback.

**Mid-stream recovery:** AIMP and foobar2000 switch from an archive stream to memory, then cache, on a backend read/seek failure, retaining the byte position so the retried read neither skips nor duplicates bytes. A cancellation is not retried. **DEV verification (13 November 2026):** native AIMP passed 8/8 injected read/seek/route scenarios (Win32 and x64), using its official COM interfaces through `aimp_adapter_test.exe`; native foobar2000 passed 8/8 equivalent scenarios within separate, real portable foobar2000 Win32/x64 processes via its SDK-backed archive-file reader. Both verify exact audio byte parity, file position, fault injection and fallback route. The HTTP bridge also recovers from decoder errors after a response has begun, sending only the remaining decoded bytes under the original status and Content-Length; its four injected Win32/x64 memory/cache scenarios passed. A socket-write failure or an archive entry that cannot be fully decoded via fallback still terminates that response. These injected backend failures do **not** replace real-player audible playback checks or tests against actual corrupt input.

**XMPlay exception:** The native `XMPARC` interface returns a completed host-owned memory block, so incremental reads by the plugin are **not** true on-demand host playback. Use the XMPlay HTTP bridge route for genuine archive streaming. This is a host API limitation, not a reason to extract the archive by default.

**Native-player fault regression:** The two independent scripts are `aimp_archive_stream/test_native_recovery.py` and `foo_archive_stream/test_native_recovery.py`. Both generate a temporary stored ZIP containing `C:\audiotest\01.MP3File.mp3`, inject one backend error after an initial 64 KiB read, test both read and seek recovery, compare bytes against the source, and verify the exact fallback and position. AIMP's verifier uses the official `IAIMPStream` COM interface with `ASTNativeFaultTestGetInjectedCount` and `ASTNativeFaultTestGetRecoveredCount` exports **only in the test DLL**. The foobar test runs in an isolated portable process, uses `fb2k::service_new<ArchiveEntryFile>` and its actual SDK read/seek methods, and reports the outcome via an isolated test-only startup hook.

To prepare DEV **test-only DLLs**, build AIMP with `/p:ArchiveNativeFaultTestDefinitions=ARCHIVE_STREAM_AIMP_FAULT_TEST` and foobar2000 with `/p:ArchiveFoobarFaultTestDefinitions=ARCHIVE_STREAM_FOOBAR_FAULT_TEST`, separately for Win32/x64, redirecting each `OutDir`/`IntDir` to scratch folders under `D:\\Temp` (for example, `D:\Temp\ArchiveStreamAimpFaultTest\Win32` and `D:\Temp\ArchiveStreamFoobarFaultTest\x64`). Build the normal AIMP host contract executable as well. The foobar SDK project-reference import libraries must be available in that isolated output folder; the test runtime is a separate copy of the corresponding *portable* foobar installation, with the test DLL in its `components` directory and a fresh `profile`. **Never deploy the injected DLL to an ordinary player install.** The tests do not write to a production player profile or to saved password lists.

~~~powershell
py -3.13 '.\aimp_archive_stream\test_native_recovery.py'

# Test foobar2000 x64 in the isolated portable runtime
py -3.13 '.\foo_archive_stream\test_native_recovery.py'

# Test foobar2000 Win32 in its separate portable runtime
$env:ARCHIVE_FOOBAR_FAULT_PORTABLE_DIR = 'D:\Temp\ArchiveStreamFoobarFaultTest\portable-Win32'
py -3.13 '.\foo_archive_stream\test_native_recovery.py'
Remove-Item Env:\ARCHIVE_FOOBAR_FAULT_PORTABLE_DIR
~~~

The AIMP regression accepts `ARCHIVE_AIMP_FAULT_TEST_BIN_DIR` to select an alternate test-binary root. The foobar regression accepts `ARCHIVE_FOOBAR_FAULT_PORTABLE_DIR`. All test-specific options, counters, environment overrides and automatic startup checks are **compile-time excluded** from ordinary Release builds. Real player audio output, damaged archives, and cancellation should be validated separately.

**Mid-response HTTP fault regression:** `archive_player_bridge/test_midstream_recovery.py` requests a non-zero HTTP byte range, injects a read failure after the first 64 KiB have been delivered, and verifies that the completed response matches the original file byte for byte. It exercises both memory and cache recovery on Win32 and x64 (four scenarios). The injection interface is compiled **only** into a separate test binary with `/p:ArchiveBridgeTestDefinitions=ARCHIVE_STREAM_BRIDGE_FAULT_TEST`; normal Release builds neither accept nor expose the test option. Build that test binary with `OutDir` and `IntDir` redirected outside the repository (DEV: `D:\\Temp\\ArchiveStreamBridgeFaultTest\\<platform>\\`), then run `py -3.13 archive_player_bridge/test_midstream_recovery.py`. Set `ARCHIVE_BRIDGE_FAULT_TEST_BIN_DIR` to override the test-executable root.

The repeatable encrypted-RAR regression uses WinRAR **only to generate small synthetic test archives**, never at playback runtime:

~~~powershell
py -3.13 '.\archive_backend_test\test_encrypted_rar_memory.py'
~~~

**DEV confirmed:** encrypted RAR5 with data encryption and with encrypted headers each passed x86/x64 in-memory byte parity and memory-limit rejection; both types also served correct HTTP byte ranges on x86/x64 with `ROUTE_COUNTS stream=0 memory=1 cache=0`. The installed WinRAR cannot generate legacy RAR4 archives for this test, so RAR4 was explicitly skipped rather than reported as passing. The normal ZIP/non-solid 7z/non-solid RAR direct stream regressions remain separate.

## Shared preferences and password / cache / title parity (13 November 2026)

All adapters now use a **shared playback settings policy** implemented by [`shared/archive_adapter_preferences.h`](shared/archive_adapter_preferences.h) and the common settings window [`archive_player_bridge/configure-adapters.ps1`](archive_player_bridge/configure-adapters.ps1). Settings live at `HKCU\Software\ArchiveStreamPlayer\Adapters` and apply to **Winamp, foobar2000, AIMP, XMPlay, MusicBee and all playlist/HTTP bridge consumers**. Winamp and foobar2000 retain their native settings pages and host-specific logging/title options; the shared settings page is also accessible from those players when installed with `configure-adapters.ps1`. Existing player-specific INI/cfg settings and caches are kept intact, and shared values take precedence once configured. New settings affect newly opened tracks/sessions; restart a player when changing its password list.

~~~powershell
# Open the shared graphical preferences window
& '.\archive_player_bridge\launch-archive.ps1' -Configure

# Non-GUI status for verification or automation
& '.\archive_player_bridge\launch-archive.ps1' -Configure -NoLaunch
~~~

| Setting | Behavior |
| --- | --- |
| **Memory threshold** | 1–512 MiB (default 50): choose direct stream first; use RAM only for eligible entries within this threshold |
| **Managed-cache budget** | 0–20,480 MiB (default 2,048): 0 disables disk fallback; other values bound the reusable shared cache across native adapters and bridge processes |
| **Password file** | Optional Unicode path to a plaintext list; the shared default is `%LOCALAPPDATA%\WinampArchiveStream\keeps.txt`. Existing native player paths are retained until shared settings are saved |
| **Prompt for missing password** | On by default, using native Windows credentials UI; disabled for unattended tests or where dialogs are unwanted |
| **Offer to save password** | On by default; prompts for explicit consent, then the backend appends a **verified** password to the configured plaintext list |
| **Track-title format** | Six shared presets with an interactive preview. Winamp refreshes its playlist-title cache; AIMP exposes virtual-track titles; bridge playlists republish on preset changes. foobar2000 exposes a live-refreshable `%archive_stream_title%` field; XMPlay creates reversible indexed display names for newly enumerated archive tracks |
| **Managed-cache controls** | View or clear shared managed track files, including when a player is open (in-use files remain). Automatic size/age cleanup (1–365 days) runs on player initialization as well as disk fallback; legacy caches are not silently migrated |
| **Diagnostic logging** | Optional shared `DebugLogging` switch and `LogDirectory` path. Winamp input/general plugins, foobar2000, AIMP, XMPlay, MusicBee and the HTTP bridge use separate generated `archive_stream_<host>.log` files, with Unicode folder support. Open the logs directory or clear generated logs from any settings page; unrelated files are preserved. Existing player-specific legacy log options remain available where applicable |

**Password security:** Saved passwords are **plain text**, not Windows Credential Manager secrets. Do not select a shared/public file or include it in a package; the credentials UI itself does not persist them. A failed password or cancellation is handled by the backend; worker-thread prompts are serialized by the bridge to avoid simultaneous dialogs. There is **no archive-wide decoding preflight**. For an encrypted 7z **Copy/Store** track, whether headers are hidden or visible, the backend performs one per-entry, constant-memory CRC pass before accepting the password; AES-CBC alone cannot distinguish correct and incorrect passwords. This delays the first bytes of that track for the sake of data integrity, but does not decompress to disk or allocate the entire track. The CRC pass checks the stream-open cancellation token between 64 KiB reads; the synthetic cancellation regression verifies it returns `Cancelled` with no open handle. The HTTP player may time out if password entry is left unanswered for a long time. Prompt windows require an interactive Windows desktop, but **no manual input was required for the new regression suite**: real portable foobar2000 Win32/x64 passed synthetic AES 7z password entry, declined saving and cancellation; official native AIMP COM Win32/x64 and XMPlay XMPARC Win32 harnesses passed entry/declined-save with full or partial WAV reads; and both bridge architectures passed CredUI entry, declined-save and a 128-byte HTTP range. Each test preserved its password list. These native harness tests do not certify the actual installed AIMP/XMPlay settings or prompt window, and a bridge prompt test does not certify every client player UI. Winamp 5.9/WACUP x86 native password prompts now pass real isolated-host acceptance: synthetic AES 7z WAV password entry and cancellation, decoded PCM through a test-only silent output plugin, and 18-second host IPC seeking with flush/resume. Their original password-list contents were unchanged. These tests do not certify audible speaker output.

### Automated real Winamp / WACUP native PCM and password acceptance

The isolated Winamp and WACUP regression uses a **QA-only Winamp output plugin**, built as a separate `out_archive_stream_test.vcxproj` target and loaded exclusively into a temporary clone of the player installation. It simulates a bounded two-second PCM output buffer and writes only event counts (open, initial write, seek flush, resumed write) to a temporary report. No speaker device is opened, so this test is **not audible-output certification**. It checks actual host-level input-plugin playback, AES+LZMA2 password entry/cancellation and jumping to 18 seconds; original installations, player preferences and saved password lists remain untouched.

~~~powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild '.\in_archive_stream\in_archive_stream.vcxproj' /m /p:Configuration=Release /p:Platform=Win32 /p:RunPostBuildEvent=Never
& $msbuild '.\in_archive_stream\test_support\out_archive_stream_test.vcxproj' /m /p:Configuration=Release /p:Platform=Win32

foreach ($player in @('winamp', 'wacup')) {
    py -3.13 '.\in_archive_stream\test_password_prompt_ui.py' accept $player
    py -3.13 '.\in_archive_stream\test_password_prompt_ui.py' cancel $player
    foreach ($codec in @('mp3', 'flac', 'ogg', 'wma', 'm4a', 'aac')) {
        py -3.13 '.\in_archive_stream\test_password_prompt_ui.py' codec $player $codec
    }
}
~~~

All **16 host scenarios** passed (eight for Winamp 5.9 x86 and eight for WACUP x86): real native password entry/cancellation on encrypted 7z WAV, and six additional codecs inside ZIP. Fourteen playback scenarios verified input-plugin PCM writes followed by an `IPC_JUMPTOTIME` request, an output-plugin `Flush(18000)` and new PCM writes. `in_archive_stream/playback.cpp` now lets pending seeks preempt all five decoder output-write loops (MP3, WAV, FLAC, OGG, Media Foundation) to avoid seek starvation when output buffers are full. `in_archive_stream/dllmain.cpp` accepts the opt-in `ARCHIVE_STREAM_ADAPTER_DEBUG=1` trace override for isolated diagnostics without changing the persisted logging policy. The test sink is **never shipped in Winamp/WACUP packages**. This does not establish bit-for-bit PCM parity, audible output or every archive/encryption layout.

### Automated Windows credential prompt acceptance

The following tests generate temporary AES-encrypted 7z WAV archives and synthetic passwords, then automate **Windows credential dialogs** in an interactive DEV desktop session. Each test declines storage and verifies the password-list file is unmodified. Run these only when no other foobar2000 instance is active. The foobar2000 tests use isolated portable test installations; AIMP/XMPlay use published native adapter interfaces through the test harness, while HTTP bridge tests validate transport rather than a client player's own UI.

~~~powershell
py -3.13 '.\foo_archive_stream\test_password_prompt_ui.py' x64 --submit-password
py -3.13 '.\foo_archive_stream\test_password_prompt_ui.py' x64
py -3.13 '.\foo_archive_stream\test_password_prompt_ui.py' x86 --submit-password
py -3.13 '.\foo_archive_stream\test_password_prompt_ui.py' x86

py -3.13 '.\aimp_archive_stream\test_password_prompt_ui.py' x64
py -3.13 '.\aimp_archive_stream\test_password_prompt_ui.py' Win32
py -3.13 '.\xmp_archive_stream\test_password_prompt_ui.py'

py -3.13 '.\archive_player_bridge\test_bridge_password_prompt_ui.py' x64
py -3.13 '.\archive_player_bridge\test_bridge_password_prompt_ui.py' Win32
~~~

All nine scenarios passed on DEV. The foobar2000 cancellation cases reject the archive and exit normally; its successful cases decode the synthetic WAV. Separate real-player AES decoding tests certify seeking. Native AIMP/XMPlay harnesses verify decoded audio bytes and seeking/partial reads, and the bridge serves a 128-byte WAV HTTP Range response. Winamp/WACUP x86 native prompting is now real-host verified separately; remaining player-specific end-to-end bridge-client dialogs retain their pending matrix statuses.

**Unicode password paths:** The shared backend now opens password-list files through native Unicode Windows file APIs (with fallback for legacy ANSI Winamp paths). A synthetic encrypted LZMA2 7z was read and sought successfully using a non-ASCII password-file path on both architectures. The repeatable regression is `archive_backend_test/test_unicode_passwords.py` and the temporary-file read/append self-test is `archive_backend_test.exe pwlistselftest`. No saved user password was copied into or committed to the tests. **Resolved 7z encryption gaps (13 November 2026):** Single-AES encoded headers now decode using the in-process LZMA SDK. AES+Copy/Store, AES+Copy+Delta, AES+LZMA2, encrypted headers, split encrypted-header archives and encrypted solid-folder memory/file fallback pass the synthetic x86/x64 regressions in `archive_backend_test/test_7z_encrypted_layouts.py`. Wrong-password attempts are rejected in the file and stream paths; AES+Copy validates the full decoded entry CRC with a 64 KiB buffer before accepting a saved password. The suite also interrupts a large AES+Copy entry during stream-open validation on both Win32 and x64 to verify cancellation. The encrypted solid folder is **not** reported as independently streamable. The regression also checks HTTP byte ranges with `--cache-limit-mb 0`: all seven non-solid variants route `stream=1 memory=0 cache=0` on both architectures, while the encrypted solid folder uses `stream=0 memory=1 cache=0`. Missing-CRC AES+Copy entries are rejected by the direct stream because there is no reliable key-verification evidence. Other exotic 7z coder pipelines have not been certified. **Real foobar2000 verification:** in separate portable Win32 and x64 installations, four five-second WAV fixtures (`AES+Copy+Delta` with hidden or visible headers, `AES+LZMA2` with hidden headers, and multipart `AES+Copy` with hidden headers) each decoded **220,500 PCM frames with zero failures and `seek=PASS`** using foobar2000's built-in Archive Stream decoder verification. Existing encrypted LZMA2 visible/hidden solid/multipart tests also decoded two files per archive on both platforms. These are real player-codec decode and seeking checks, **not audible speaker-output certification**.

**Shared cache and source safety:** [`shared/archive_unified_cache.h`](shared/archive_unified_cache.h) now maintains reusable audio files under `%LOCALAPPDATA%\ArchiveStreamPlayer\ManagedCache` for Winamp, foobar2000, AIMP, XMPlay and bridge consumers. A cache key includes the canonical archive path, archive size/modified timestamp (and contiguous `.001` volumes), entry index, size, declared CRC and internal path. Extraction is published through a private partial file and a per-entry cross-process mutex. Direct streams and eligible RAM decompression still take precedence over cache hits. Existing legacy Winamp/foobar caches remain untouched until the user explicitly clears them. The GUI can open/clear shared cache files; it also shows and safely clears stale bridge/MusicBee temp session folders. The startup age-cleanup regression `archive_player_bridge/test_shared_maintenance.py` passed on Win32 and x64 using an isolated `%LOCALAPPDATA%` profile, without touching the user's cache. **Cache warming:** [`shared/archive_unified_warmer.h`](shared/archive_unified_warmer.h) runs only for solid 7z/RAR entries after disk fallback, prepares eligible remaining tracks with no background password prompts, and supersedes outdated jobs. The synthetic regression [`archive_player_bridge/test_shared_cache.py`](archive_player_bridge/test_shared_cache.py) confirms Win32/x64 HTTP byte parity, warming of an unrequested track, persistent cross-process reuse and source modification invalidation. Decoded cache files are ordinary unencrypted files even when the source archive required a password; clear the shared cache from settings if you do not want decrypted audio left on disk. Neither this test nor a build certifies audible playback in every player.

**Track-title scope and refresh:** Winamp's input plugin and general expander use the same shared template for new metadata queries/expanded playlist rows. Saving Winamp's native configuration requests a safe `IPC_REFRESHPLCACHE` title-cache flush. AIMP exposes shared titles through `IAIMPVirtualFile::GetFileInfo()`. The HTTP bridge updates M3U8/ASX playlist labels on a shared-preset change; bridge clients must reload the playlist to repaint existing entries. **foobar2000:** `foo_archive_stream/foobar_title_provider.cpp` implements the native SDK's `%archive_stream_title%` titleformat field and calls `metadb_io::dispatch_refresh()` for affected existing archive playlist handles when the shared preset or native prefix/postfix changes. To display it in a foobar2000 playlist column, use `$if2(%archive_stream_title%,%title%)` as the column title-format expression. The original audio tags are unchanged. Both real portable Win32 and x64 players passed the live-refresh test, which added an archive track and changed its title format without replacing the playlist entry or changing the user's registry. **XMPlay:** when a non-default title preset is selected, `GetFileList()` creates an indexed, reversible display filename that keeps the audio extension; `DecompressFile()` accepts both new aliases and previous original internal filenames even after a preset change. ZIP and solid 7z two-WAV tests pass, including partial reads and old-name compatibility. **XMPlay's XMPARC interface has no playlist-row metadata-refresh callback:** a preset change takes effect for newly imported archive entries; existing rows must be removed/reimported or refreshed manually through XMPlay's own interface. MusicBee's displayed HTTP track names depend on its URL metadata handling. These are host API boundaries, not silently altered source archive filenames.

The shared settings window's live controls were verified with `archive_player_bridge/test_shared_settings_ui.py` in a real Windows desktop session: cache/password/logging controls, interactive title preview, cache clear and log clear through actual buttons, preserving unrelated files and canceling without registry changes. The test uses an isolated `LOCALAPPDATA` directory. The shared settings window is accessible through **Winamp Input plugin settings > Shared settings**, **foobar2000 Preferences > Tools > Archive Stream > Shared settings**, **AIMP Options > Archive Stream Player**, **XMPlay Archive Stream plugin Config** and **MusicBee Tools > Archive Stream: Settings...**. For **VLC, WMP, WACUP x64 and MediaMonkey's bridge route**, run `launch-archive.ps1 -Configure`. Install `configure-adapters.ps1` next to each plugin DLL, or use the packaging scripts that include it. Winamp's normal Release build stages the script beside the installed plugin; foobar's Release build stages it in the output folder. These settings use Windows PowerShell 5.1 and Windows Forms. The generated log files are under `%LOCALAPPDATA%\ArchiveStreamPlayer\Logs` unless a custom absolute folder is chosen. Logging is opt-in and logs events, filenames, source locations and archive paths; a debug log may therefore contain private filesystem paths. Password values are not intentionally recorded. Log cleanup deletes only recognized generated log names and retains files that cannot be deleted because they are in use. Use `-ShowSettings`, `-SetDebugLogging $true|$false`, `-SetLogDirectory 'D:\Logs'`, or `-ClearLogs` for non-GUI operation.

### Packaging and verified tests

~~~powershell
& '.\archive_player_bridge\package-native-adapters.ps1' -Player AIMP -Platform Win32
& '.\archive_player_bridge\package-native-adapters.ps1' -Player AIMP -Platform x64
& '.\archive_player_bridge\package-native-adapters.ps1' -Player XMPlay -Platform Win32
& '.\foo_archive_stream\package-foobar.ps1' -Platform x64
& '.\foo_archive_stream\package-foobar.ps1' -Platform Win32
& '.\mediamonkey_archive_stream\package-mmip.ps1'
& '.\musicbee_archive_stream\package-musicbee.ps1'

py -3.13 '.\archive_player_bridge\test_bridge.py'
py -3.13 '.\archive_player_bridge\test_cache_controls.py'
py -3.13 '.\archive_player_bridge\test_passwords.py' --audiotest-generator-key
~~~

**DEV confirmed:** AIMP x86/x64 and XMPlay x86 compiled, MusicBee compiled; AIMP x86/x64 registered the Options service in live portable player processes; the XMPlay mock confirmed its non-null Config callback, decoded a WAV file, and performed 64-byte partial probes. The shared Windows Forms settings dialog opened with its expected controls and completed registry-save round trips for both enabled and disabled flags. The bridge passed all six M3U/ASX title presets, byte-for-byte reads in all three routes, cache-budget rejection, disabled-prompt behavior, and encrypted 7z regression on both architectures. GUI clicking through every host's settings and incorrect-password recovery in every individual player remain acceptance tasks. The synthetic successful-password, declined-save and cancellation cases described above are already automated and passing; see [FEATURE-MATRIX.md](FEATURE-MATRIX.md) for the per-host verification level.



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

XMPARC.DecompressFile takes the requested maximum decoded-byte length, which may be shorter than the entry; the adapter supports such partial probes. It prefers backend decoded-byte streaming, otherwise extracts entirely into memory when within the configured memory threshold, otherwise reuses or creates a managed cache file, with explicit cache cleanup controls. The return value is always an **XMPlay-owned decoded byte block**; XMPARC does not provide a way to transfer an ongoing archive stream directly to the host decoder. A large entry can therefore require a large host buffer even when the archive backend reads it incrementally. This is **not on-demand streaming at the player level**. The memory threshold applies to fallback extraction, not the memory block requested by XMPARC.

**XMPlay streaming playback alternative:** XMPlay can receive an M3U8 playlist with decoded archive bytes served on demand by our loopback HTTP bridge. This bypasses XMPARC's full-entry buffer requirement while preserving the required **direct stream → memory fallback → disk cache** order in the shared backend. The player still handles MP3/FLAC/WAV/Ogg decoding; only the compressed archive entries are streamed. Use the following command with the local XMPlay installation (or supply your own `-PlayerPath`):

~~~powershell
& '.\archive_player_bridge\launch-archive.ps1' -Archive 'C:\audiotest\Archives\7Z_NonSolid.7z' -Player XMPlay -PlayerPath 'D:\Temp\ArchiveStreamPlayerAdapters\xmplay\xmplay.exe'
~~~

This bridge alternative provides stream-first playback but **does not give XMPlay native archive drag-and-drop expansion**. The XMPARC plugin remains available for native archive browsing and buffered playback. Keep the bridge launcher open until playback ends. For on-demand streaming inside XMPlay's native archive browsing flow, a different host integration would be required; XMPARC cannot supply it.

The DEV x86 host contract test passed WAV full-entry and first-64-byte parity for ZIP, 7z and RAR, and a 61 MiB solid 7z fixture passed through cache fallback. A separate encrypted-7z XMPARC harness test exercised a real Windows credential prompt, declined saving, and passed full and partial WAV reads without modifying the existing password file. A real XMPlay 4.1 instance loaded the DLL, enumerated four tracks from a solid 7z, routed solid members to memory and the nonsolid member through the backend stream (then into an XMPlay-owned buffer). Separately, an isolated XMPlay 4.1 instance launched with a generated bridge M3U8 and `-play` requested a live archive-backed HTTP stream: `ROUTE_COUNTS stream=1 memory=0 cache=0`. Audible output and interactive seeking are not independently certified.

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

Each virtual stream uses **stream first → in-memory extraction (default 50 MiB, configurable) → reusable managed cache**. Solid 7z and RAR bypass direct streaming; cached entries persist for reuse subject to shared age/size cleanup rather than being removed when the native stream is released. The official SDK provides the COM interfaces used by the independent host contract test.

AIMP x86 and x64 mock-host tests verified registration of FileFormat and FileExpander, virtual-file URI properties, reads and rewind/end seeking, and a 61 MiB solid 7z cache case. Six additional x86/x64 host-contract runs across ZIP, nonsolid 7z and RAR WAV archives passed read/seek/rewind tests, with direct streaming verified through the plugin's route logging. Both actual portable AIMP 5.40 builds accepted archive files in the Add Files dialog and expanded four tracks in a solid 7z. AIMP x64 traced route-to-memory when playback was requested. Native Win32/x64 COM harnesses additionally opened encrypted 7z using actual Windows CredUI, declined saving, and passed stream read/seek verification without changing the password list. Audible playback, player-UI seeking and the prompt inside an actual AIMP instance are still not fully verified.

## WACUP (Winamp compatibility)

### WACUP 32-bit — verified on build 1.99.57.25464

Use the existing Winamp Release|Win32 plugin binaries:

- in_archive_stream\Release\in_archive_stream.dll
- gen_archive_stream\Release\gen_archive_stream.dll

Copy them to the WACUP x86 Plugins folder. Portable WACUP blocks third-party plugins by default; enable **Preferences → Advanced → Portable Mode → Allow 3rd-party (non-WACUP and non-native) plug-ins** and restart. WACUP bundles an in_zip.dll that takes over some ZIP/7z routing, creating zip:// playlist records instead of the Archive Stream virtual-file format; **disable its bundled ZIP handler in the test installation** if you want Archive Stream's own list expansion. In WACUP Preferences → Plug-ins → General Purpose, select Archive Stream Playlist Expander and click Configure on an archive playlist item.

WACUP's WM_COPYDATA enqueue call may return zero even when the track was successfully appended. The Winamp general adapter now verifies the actual playlist count increase, and it appends all replacements before removing the original archive entry so failed expansion leaves the original intact.

An isolated WACUP x86 installation loaded both DLLs and expanded a four-entry solid 7z. Winamp-compatible IPC then confirmed that playback time advanced during the WAV track and the player transitioned to FLAC. Logs confirmed solid 7z was routed to memory. **Additional November 13 native-host acceptance:** the isolated WACUP x86 preview loaded the updated Winamp input plugin, opened an AES 7z WAV and displayed its own Archive Password dialog; synthetic entry produced PCM into a silent QA sink, and cancelling stopped playback without decoding. Winamp IPC jump to 18 seconds triggered a decoder seek, an output flush and resumed PCM writes. Six separate ZIP codec tests (MP3/FLAC/OGG/WMA/M4A/AAC) passed the same PCM + seek checks. These tests cannot establish audible speaker output.

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

## MediaMonkey, MusicBee, VLC, and Windows Media Player (13–14 November 2026)

These integrations reuse the **existing statically linked** ZIP/7z/RAR/AES backend. The main addition is an x86/x64 seekable loopback HTTP bridge, plus a native MusicBee C# plugin and a MediaMonkey MMIP packaging project. **VLC and Windows Media Player use supported playlist and HTTP media interfaces; they do not have native archive-format plugins.**

### Seekable localhost playback bridge (Windows x86/x64)

`archive_player_bridge/archive_player_bridge.vcxproj` builds against the existing static archive libraries. The standalone executable binds to **127.0.0.1 only**, uses an unpredictable 128-bit URL token, and serves decoded archive-entry bytes (not decoded PCM) as ordinary audio URLs. It supports GET, HEAD and HTTP Range (206), MIME types and per-entry extensions, M3U8 and ASX playlists, a token-authenticated POST /shutdown, and automatic cleanup of its temporary playlist/session files; decoded fallback audio lives in the reusable shared managed cache. It does not need any external archive DLL or extraction process.

Entry priority: **direct decoded-byte archive stream -> in-memory extract (default 50 MiB) -> reusable managed disk-cache file**. Solid 7z/RAR bypass direct stream. The decoder/codec remains the player's responsibility. This is **not a persistent background service**; URLs work only while their bridge process is running and cannot be used as permanent library references.

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

**Verified (14 November 2026):** The Win32 C# SDK build passed. A repeatable actual MusicBee 3.7.9796 host test, [`musicbee_archive_stream/test_host_queue_ui.py`](musicbee_archive_stream/test_host_queue_ui.py), creates a temporary portable player profile and synthetic two-track ZIP, opens **Tools > Archive Stream: Add Archive Audio**, selects the archive, and confirms `QUEUE_RESULT accepted=1 tracks=2` from the real MusicBee Now Playing API. The MP3 was selected and played with advancing MusicBee time; a GUI seek produced a genuine nonzero HTTP Range request at byte offset 8,462,728 (subsequent runs may yield a different offset). Session and bridge logs are written to `%LOCALAPPDATA%\ArchiveStreamPlayer\Logs` inside the isolated profile with `ARCHIVE_STREAM_ADAPTER_DEBUG=1`. **Open issue:** The second OGG URL appears in the queue as `1` with an error indicator, and MusicBee reports its source file cannot be found when selected, even though the bridge serves its bytes in direct tests. MusicBee has not passed an OGG host-playback check. These tests do not verify audible speaker output or every codec; native title presets remain host-dependent.

### MediaMonkey 2024 — Winamp input-module limit and verified playlist bridge

DEV installed player: `C:\Program Files (x86)\MediaMonkey 5` (2024.2.2). Its documented MMIP native plugin packaging format supports Winamp 2.x DLLs: https://www.mediamonkey.com/wiki/Getting_Started_(Addons).

~~~powershell
& '.\mediamonkey_archive_stream\package-mmip.ps1'
# In MediaMonkey: Tools > Addons > Add
# Select mediamonkey_archive_stream\Release\ArchiveStreamPlayer-MediaMonkey5.mmip
~~~

**Verified compatibility limit:** MediaMonkey's own [What's New in MediaMonkey 5](https://www.mediamonkey.com/wiki/WebHelp:What's_New/5.0) states that Winamp input/output plugins remain supported but **general-purpose Winamp plugins are no longer supported**. The DEV player previously loaded both DLLs without a crash, but this is not evidence that MediaMonkey ever invokes `gen_archive_stream.dll`. There is no general-purpose expansion callback to certify in MM5. The MMIP therefore packages **only the experimental Win32 `in_archive_stream.dll` and shared settings script**; it does not pretend to add archive playlist expansion. The MediaMonkey 5 installer may leave an older `gen_archive_stream.dll` behind from a previous MMIP; remove it manually only after confirming it came from this project. MediaMonkey's verified, user-facing archive playback mode remains the bridge. Native in-player archive expansion requires a new dedicated MediaMonkey 5 JavaScript add-on using its documented add-on API; the Winamp virtual-path scheme is not automatically supported.

The verified playback alternative is the bridge and MediaMonkey's official `/NoSplash /Add` playlist command:

~~~powershell
& '.\archive_player_bridge\launch-archive.ps1' -Archive 'C:\audiotest\Archives\7Z_NonSolid.7z' -Player MediaMonkey
~~~

MediaMonkey successfully accepted `/Add` and `/Play` in the DEV test, and the bridge recorded an actual direct-stream request (stream=1). Audible output remains uncertified. In-player archive expansion is **not provided by the old Winamp general plugin in MediaMonkey 5**; a dedicated supported host add-on must be implemented and tested separately.

### VLC — M3U8/HTTP playback

Use `launch-archive.ps1 -Player VLC`. VLC receives the archive's per-track URLs through an M3U8 playlist; byte-range seeking is serviced by the same static backend. **It is not a VLC access-module plugin** and VLC does not open the original ZIP/7z/RAR file directly.

A real DEV VLC headless run with dummy audio output successfully consumed the two-track ZIP playlist, exited normally, and the bridge recorded **two live direct-stream requests**. HTTP seeking and byte parity passed in independent tests; audible device output was intentionally bypassed.

### Windows Media Player Legacy 12 — ASX/HTTP playback

Use `launch-archive.ps1 -Player WMP`. The launcher passes a Windows Media Player compatible **ASX playlist** of localhost per-track audio URLs. **This is not a decoder/input-source plugin** for WMP.

The DEV WMP COM control is available (12.0.26100.9818), and an individual bridge track URL generated a live audio request. **Playback was not confirmed**: the COM control stayed in play state **9 (transitioning)** even with the original MP3 opened directly from `C:\audiotest`. A live WMP GUI/output-device test is required before certifying playback.

### Runtime and security notes

The bridge does not expose any network interface other than loopback. The session token protects individual tracks and its shutdown endpoint from blind connections; local users who can access the tokenized playlist can access those tracks during the session. Bridge URLs are ephemeral, not permanent library addresses; the bridge never stores password values in its logs or source. Temporary playlist/session files are removed on normal shutdown; decoded shared-cache tracks persist under the configured cache policy. Forced process termination may leave temporary playlist/session files, removable from the settings page. Audio codec compatibility remains determined by each player.

## Diagnostics and runtime separation

Set the environment variable ARCHIVE_STREAM_ADAPTER_DEBUG=1 **before launching** AIMP or XMPlay to enable trace logs under:

- %LOCALAPPDATA%\AIMPArchiveStream\aimp_archive_stream.log
- %LOCALAPPDATA%\XMPlayArchiveStream\xmplay_archive_stream.log

The adapters use separate player data/password folders. Winamp and WACUP deliberately share WinampArchiveStream configuration. Passwords are not written into logs.

All DEV SDKs, portable test players, compressed WAV fixtures and generated DLLs stay outside the Git repository. No installed WACUP or foobar2000 configuration was overwritten by these tests. Keep testing encryption, Unicode/split archives, cancellation, audible output and player-level seek transitions before declaring comprehensive playback certification.
