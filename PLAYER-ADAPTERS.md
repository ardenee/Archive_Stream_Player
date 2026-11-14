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

**DEV confirmed:** encrypted RAR5 with data encryption and with encrypted headers each passed x86/x64 in-memory byte parity and memory-limit rejection; both types also served correct HTTP byte ranges on x86/x64 with `ROUTE_COUNTS stream=0 memory=1 cache=0`. New repeatable [`test_encrypted_rar_multipart.py`](archive_backend_test/test_encrypted_rar_multipart.py) builds genuine four-volume stored RAR5 WAV test archives in both encryption modes. Win32/x64 native decoders match complete source SHA-256; bridge byte-range reads match exactly with `memory=1 cache=0` when 8 MB RAM is available, and `memory=0 cache=1` when RAM is intentionally capped at 1 MB (without exceeding the memory budget). Removing the final volume causes a fast, noninteractive failure on both architectures. Separate [`test_encrypted_rar_solid.py`](archive_backend_test/test_encrypted_rar_solid.py) builds **two-file solid RAR5** in both data- and filename-encrypted modes; both entries—including the later solid entry—pass native Win32/x64 byte parity and RAM-only HTTP ranges. The installed WinRAR cannot generate legacy RAR4 archives for this test, so RAR4 was explicitly skipped rather than reported as passing. The normal ZIP/non-solid 7z/non-solid RAR direct stream regressions remain separate.

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

**Track-title scope and refresh:** Winamp's input plugin and general expander use the same shared template for new metadata queries/expanded playlist rows. Saving Winamp's native configuration requests a safe `IPC_REFRESHPLCACHE` title-cache flush. AIMP exposes shared titles through `IAIMPVirtualFile::GetFileInfo()`. The HTTP bridge updates M3U8/ASX playlist labels on a shared-preset change; bridge clients must reload the playlist to repaint existing entries. **foobar2000:** `foo_archive_stream/foobar_title_provider.cpp` implements the native SDK's `%archive_stream_title%` titleformat field and calls `metadb_io::dispatch_refresh()` for affected existing archive playlist handles when the shared preset or native prefix/postfix changes. To display it in a foobar2000 playlist column, use `$if2(%archive_stream_title%,%title%)` as the column title-format expression. The original audio tags are unchanged. Both real portable Win32 and x64 players passed the live-refresh test, which added an archive track and changed its title format without replacing the playlist entry or changing the user's registry. **XMPlay:** when a non-default title preset is selected, `GetFileList()` creates an indexed, reversible display filename that keeps the audio extension; `DecompressFile()` accepts both new aliases and previous original internal filenames even after a preset change. ZIP and solid 7z two-WAV tests pass, including partial reads and old-name compatibility. **XMPlay's XMPARC interface has no playlist-row metadata-refresh callback:** a preset change takes effect for newly imported archive entries; existing rows must be removed/reimported or refreshed manually through XMPlay's own interface. MusicBee's MP3 HTTP stream displays embedded audio metadata. Its tested OGG/Vorbis HTTP stream did not, so the MusicBee adapter now uses a session-local OGG fallback; its embedded title and artist tags appear in real MusicBee. **MusicBee shared title-template is not yet working in the host UI.** In an isolated MusicBee 3.7 SDK test, queuing the bridge's M3U8 playlist did not expose expanded track titles through the native playlist getters. The SDK's `Library_SetFileTag` returned success when passed an archive track and a test title, but the visible playlist and library title getters still returned original embedded metadata even after panel refresh. Synthetic media bytes were unmodified. This failed attempt was not shipped: rewriting file tags or claiming a title change without host evidence would be unsafe. These are host API boundaries, not changes to source archive filenames.

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

The DEV x86 host contract test passed WAV full-entry and first-64-byte parity for ZIP, 7z and RAR, and a 61 MiB solid 7z fixture passed through cache fallback. A separate encrypted-7z XMPARC harness test exercised a real Windows credential prompt, declined saving, and passed full and partial WAV reads without modifying the existing password file. A real XMPlay 4.1 instance loaded the DLL, enumerated four tracks from a solid 7z, routed solid members to memory and the nonsolid member through the backend stream (then into an XMPlay-owned buffer). Separately, an isolated XMPlay 4.1 instance launched with a generated bridge M3U8 and `-play` requested a live archive-backed HTTP stream: `ROUTE_COUNTS stream=1 memory=0 cache=0`. **A subsequent isolated real XMPlay 4.1 bridge session passed manual GUI-seek/resume verification for an MP3 inside a stored ZIP:** the player visibly reported a 05:50 MP3 playing at **00:17.5**, a click on its real seek slider moved the displayed position to **03:23.9**, and the player kept progressing to **04:04.2**. The bridge exit log recorded `ROUTE_COUNTS stream=2 memory=0 cache=0` (two test player instances opened the same archive-backed track); no disk fallback was used. An attempted unattended renderer-pixel regression was *not* added to the project: the DEV desktop locked partway through QA, and nested GDI captures could not sample the skinned window. This is a direct real GUI observation rather than a certified repeatable GUI harness. **Native XMPARC player-level seeking**, other codecs, and **audible speaker output** remain uncertified.

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

**Verified (14 November 2026):** The Win32 C# SDK build passed. A repeatable actual MusicBee 3.7.9796 host test, [`musicbee_archive_stream/test_host_queue_ui.py`](musicbee_archive_stream/test_host_queue_ui.py), creates a temporary portable player profile and synthetic two-track ZIP, opens **Tools > Archive Stream: Add Archive Audio**, selects the archive, and confirms `QUEUE_RESULT accepted=1 tracks=2` from the real MusicBee Now Playing API. The MP3 was selected and played with advancing MusicBee time; a GUI seek produced a genuine nonzero HTTP Range request at byte offset 8,462,728 (subsequent runs may yield a different offset). Session and bridge logs are written to `%LOCALAPPDATA%\ArchiveStreamPlayer\Logs` inside the isolated profile with `ARCHIVE_STREAM_ADAPTER_DEBUG=1`. **OGG/Vorbis fixed for the tested case:** The same OGG failed when queued as the raw archive HTTP URL but played correctly from a local file. The MusicBee adapter now materializes only the host-limited `.ogg`, `.flac`, `.wav`, `.wma` and `.m4a` archive entries through the loopback bridge into `%TEMP%\\ArchiveStreamMusicBee-*\\archive-entry-NNNNNN.<ext>`. Each download checks HTTP content length and only publishes complete files; the shared `CacheLimitMB` bounds the combined local-file bytes per session. A zero cache limit blocks fallback with an explicit error. MP3 and AAC retain their direct archive-backed HTTP URLs. FLAC itself plays over HTTP, but the tested MusicBee 3.7 host repeatedly restarted FLAC from byte zero during GUI seek and never resumed playback; its local file fallback restores seek behavior. The MusicBee Now Playing queue API provides paths/URLs but no in-memory audio source, so the host-specific disk fallback cannot use the shared RAM-preferred route. These temporary files are unencrypted materialized audio, removed when the plugin closes normally; abnormal termination may require shared stale-temp cleanup. The real-player regression now passes OGG embedded title/artist metadata, advancing playback and GUI seek to about 2:02. The OGG-first reflection test [`musicbee_archive_stream/test_ogg_first_session.ps1`](musicbee_archive_stream/test_ogg_first_session.ps1) verifies SHA-256 parity, leaves MP3 HTTP streaming intact, and checks shutdown deletes the OGG file. This does not verify audible speaker output or byte-for-byte PCM decoding; native shared title presets remain host-dependent.

**Expanded codec verification (14 November 2026):** [`musicbee_archive_stream/test_host_codec_matrix.py`](musicbee_archive_stream/test_host_codec_matrix.py) stages an isolated MusicBee 3.7.9796 profile and a seven-codec ZIP, verifies Tools-menu queue acceptance of seven tracks, and selects each track in the host UI. MP3/AAC retain live archive HTTP requests with nonzero Range after GUI seek; OGG/FLAC/WAV/WMA/M4A are placed in complete session-local files. All seven tracks were selected without MusicBee error dialogs following the codec routing change. The native MusicBee SDK host diagnostic `HOST_PLAYBACK` (opt-in only; no HTTP token logging) records current queue index, codec, route, Player_GetPlayState and Player_GetPosition every second. All seven selected tracks report `Playing` and advancing positions. The GUI seek test confirms each track jumps to a later position and keeps advancing: MP3 ~212 s, OGG ~122 s, FLAC ~295 s, WAV ~171 s, WMA ~442 s, M4A ~442 s and AAC ~157 s (test-fixture dependent). MP3 and AAC bridge logs independently show nonzero byte-range requests for their direct-stream seeks. The Win32 reflection test [`test_codec_routes_session.ps1`](musicbee_archive_stream/test_codec_routes_session.ps1) confirms SHA-256 parity for all five local codecs, HTTP preservation for MP3/AAC and removal of materialized audio after normal plugin Close. This establishes **actual host playback-state progression and GUI seek/resume**, not audible speaker output or decoded PCM-content parity. Native shared title-template row display and in-host credential prompts remain uncertified.

### MediaMonkey 2024 — native JavaScript Tools add-on (new)

The supported MediaMonkey 5 integration is a **native JavaScript Tools-menu add-on** with a separate, statically linked x64 Archive Stream playback bridge. It does not attempt to load `gen_archive_stream.dll`: MediaMonkey 5 no longer supports Winamp general-purpose expander plugins. Install the new add-on separately from the older experimental Winamp input-only MMIP.

~~~powershell
& '.\mediamonkey_archive_stream\package-native-addon.ps1'
# MediaMonkey 2024: Tools > Addons > Add
# Select mediamonkey_archive_stream\Release\ArchiveStreamPlayer-MediaMonkey5-Native.mmip
# Restart MediaMonkey if requested.
# Tools > Archive Stream: Add Archive Audio...
# Tools > Archive Stream: Settings...
~~~

**Implementation:** `mediamonkey_archive_stream/native_addon/actions_add.js` registers official MediaMonkey JavaScript Tools actions and opens its native single-archive Windows picker. `launch-mediamonkey.ps1` starts the **same statically linked x64 archive bridge** as standalone integrations; it atomically publishes a private ready manifest pointing to a temporary authenticated M3U8. The JavaScript reads that playlist, checks loopback host/session URL pattern, applies the shared `#EXTINF` display title to ephemeral track objects (without editing audio tags), and queues the archive URLs using **MediaMonkey's own `app.player.addTracksAsync()`**, leaving existing Now Playing entries intact. It writes a queue acknowledgement only after native insertion succeeds; without acknowledgement, the launcher terminates the bridge. With acknowledgement, the bridge serves archive-backed tracks until the player closes (with a grace period) or a two-hour limit. ZIP, 7z, RAR and split 7z remain handled by the shared static backend with **direct stream → memory → managed cache** policy. The Settings action opens the **common preferences GUI**. Addon lookup supports per-user `%APPDATA%\MediaMonkey5\Scripts` and application-local `Scripts`; the actual `getApplicationPath()` returns a directory on the tested 2024 host, so the add-on explicitly resolves `MediaMonkey.exe`. Separate MIT/third-party notices are included in the self-contained MMIP.

**Verified (14 November 2026):** `test_native_actions.js` passes native queue/title handoff, untrusted URL and entry-count rejection, cancel/malformed inputs, failed player queue (no bridge acknowledgement), install-location fallback, launcher failure and shared Settings contracts. `test_native_addon.ps1` validates a self-contained MMIP and bridge-ready manifest; [`test_native_acknowledgement.ps1`](mediamonkey_archive_stream/test_native_acknowledgement.ps1) reads a live 64-byte archive-backed HTTP Range after native queue acknowledgement, then verifies bridge and playlist cleanup. A real MediaMonkey 2024 host opened both native Tools commands, selected a ZIP from the Windows picker and inserted tracks with shared titles. The later repeatable [`test_native_portable_codecs.py`](mediamonkey_archive_stream/test_native_portable_codecs.py) stages a *genuine portable* copy using `Portable/MediaMonkey.ini` with `[Portable] IsPortable=1` and a unique MM5.DB. It cancels the first-run scan wizard rather than scanning the user's Music/Videos and verifies hashes of the real user-profile database, settings and persistent JSON remain unchanged. Only the OS file-picker result is injected in the disposable test addon; its actual JavaScript queue action, packaged shared bridge, real Now Playing, real audio decode/position and actual GUI seek bar are exercised. The initial seven-entry stored ZIP test passed **MP3, OGG, FLAC, WAV, WMA, M4A and AAC** playing with elapsed UI positions, all seven preset titles, and `ROUTE_STREAM` for all entries with no cache or memory fallback. Six codecs (MP3/FLAC/WAV/WMA/M4A/AAC) also passed GUI seek/resume. OGG initially played but showed unknown duration and left the track on seek. Correctly supplying **199 seconds** for the actual Vorbis fixture on the ephemeral MediaMonkey track restored OGG GUI seek/resume (00:04→02:06→02:09) with direct streaming, proving its issue was missing duration rather than an unavoidable disk fallback. Production now obtains the Vorbis duration by reading bounded entry prefix/tail bytes using the *shared stream backend*, only for reasonably sized, non-solid, unencrypted OGG/Vorbis files with a verified EOS page; it writes the rounded seconds to M3U8 `#EXTINF`, which the MediaMonkey addon forwards to `track.songLength`. Unsupported layouts retain an unknown duration and do not trigger a startup password prompt or extraction. The updated Win32/x64 bridge passes ZIP/solid 7z byte parity and stream/memory/cache fallback tests. The **complete seven-codec post-fix production MMIP** passed real MediaMonkey GUI seek/continuation in one isolated run: MP3 00:05→03:40→03:44; OGG 00:04→02:06→02:09; FLAC 00:05→05:03→05:07; WAV 00:05→02:56→03:00; WMA/M4A 00:05→07:35→07:38; AAC 00:04→02:39→02:42. It logged `ROUTE_STREAM` for all seven entries, memory/cache zero, passed the original profile SHA-256 checks, and deleted the disposable test workspace. The integration is a native **host action/queue adapter** to a static archive backend, not a native ZIP/7z/RAR decoder. **This seven-ZIP-codec test does not establish speaker output, PCM-content parity or encrypted/multipart playback; separate encrypted 7z/RAR5 host password tests are documented below.**

**Encrypted archive host acceptance (14 November 2026):** The repeatable [`test_native_portable_codecs.py`](mediamonkey_archive_stream/test_native_portable_codecs.py) also supports `AST_MM5_QA_AES=accept|cancel|wrong`, `AST_MM5_QA_ARCHIVE=7z|rar` and optional `AST_MM5_VERIFY_GUI_SEEK=1`. It generates its own random synthetic AES password, creates a **non-solid encrypted 7z or RAR5 AAC archive** using local 7z/WinRAR solely for test fixtures (with optional `AST_MM5_QA_RAR_HEADERS=1` for encrypted filenames), and patches only the **disposable** package launcher to use an empty isolated password list with zero disk-cache allowance. Its password handler interacts exclusively with credential dialogs owned by the disposable bridge process. A real portable MediaMonkey 2024 process passed synthetic AES 7z **password entry, direct-stream AAC playback, GUI seek/resume and cancellation**. After repairing the shared native RAR backend, a synthetic encrypted RAR5 AAC passed **password entry, direct-stream attempt → RAM decode, GUI seek/resume, cancellation and incorrect-password rejection**. The RAR5 success showed `ROUTE_COUNTS` equivalent to `stream=0 memory=2 cache=0` (initial open plus post-seek). Rejected/cancelled passwords showed zero decoded routes, no playback progress, and no password saved. Original MediaMonkey settings/database hashes remained unchanged; disposable test profiles and extracted audio files were removed. The same portable host also passed **first-use encrypted RAR5 filenames** with no saved candidate: the Windows password dialog appeared before playlist creation, then the selected AAC track was queued and played, GUI seek advanced 00:15→02:45→02:51, and the shared bridge routed through RAM twice with no disk cache. A separate **four-volume filename-encrypted RAR5** hosted an AAC track spanning the volume boundaries: MediaMonkey queued `.part1.rar`, prompted once, played and sought 00:14→02:45→02:52, routing to RAM without disk cache; the profile and password list remained unchanged. The two-entry solid-RAR **backend and HTTP** tests also pass, but a real portable-player solid-RAR run is **not certified**: attempts failed during the test MediaMonkey's first-run wizard or main-window startup before archive playback was reached. Separate cancel and incorrect-password runs did not publish playlists or queue audio. In both Win32 and x64, `archive_player_bridge/test_bridge_password_prompt_ui.py Win32|x64 rar-headers` passed the genuine Windows first-use prompt, 128-byte HTTP Range parity, and declined password persistence. All profiles and test files were isolated and cleaned up. These are **actual host UI and archive-backed transport** tests, not PCM parity or audible output tests.

**Shared backend RAR correction:** `archive_backend/archive_rar_native_encrypted.cpp` now tries verified archive-session passwords and configured candidates before prompting once on an interactive thread through the common password callback. A newly entered candidate is promoted to a runtime password only after UnRAR verifies the selected entry; it is written to the plaintext password list only after **both** successful verification and user consent. A hard memory limit fails *before* requesting a password. The HTTP bridge probes encrypted RAR streaming without displaying a prompt first, then allows its native in-RAM fallback to prompt if streaming is unsupported, preventing duplicate first-use prompts for the tested RAR5 layout. `archive_backend_test/test_encrypted_rar_memory.py` passes Win32/x64 RAR5 data/header encrypted byte parity and hard memory limits; Win32/x64 bridge tests also preserve ZIP/7z/RAR stream-first behavior and HTTP transport. RAR4 fixture generation was not supported by the installed WinRAR and was not certified. **First-use header-encrypted RAR5 without saved candidates is now accepted:** the shared reader detects the encrypted-header flag, tries archive-session and configured passwords, then prompts exactly once on an explicitly interactive enumeration thread, not on background metadata threads. A successfully decrypted header listing places its candidate in this archive's process-local password set so the selected entry can be decoded; choosing to save cannot write `keeps.txt` until a later UnRAR decoded-entry verification. The runtime password is never printed or written to the temporary playlist. Cancelling or supplying an incorrect header password fails listing before any queue is published. This is **H** in isolated real MediaMonkey and **T** for independent Win32/x64 bridge credential UI / 128-byte RAR5 HTTP Range parity. **Affirmative save consent has now passed real MediaMonkey 2024 UI acceptance for first-use encrypted RAR5 filenames.** With `AST_MM5_QA_AES=accept`, `AST_MM5_QA_RAR_HEADERS=1`, and `AST_MM5_QA_SAVE=1`, the isolated bridge displayed the Windows Yes/No save question and the test selected Yes. The independently scoped password list remained **empty after header enumeration and queue acknowledgement**, then contained **exactly one synthetic key after successful AAC data decode**; playback and actual GUI seek (00:15→02:45→02:51) passed using RAM without disk cache. The test verified unchanged SHA-256 hashes of the normal MediaMonkey profile and deleted its disposable password list. This verifies the *affirmative* and previously certified *declined* cases for the tested RAR5 layout; no broader claim about every encryption mode or secure key storage is implied.

### MediaMonkey 2024 — legacy Winamp input-module limitation and standalone playlist bridge

DEV installed player: `C:\Program Files (x86)\MediaMonkey 5` (2024.2.2). Its documented MMIP native plugin packaging format supports Winamp 2.x DLLs: https://www.mediamonkey.com/wiki/Getting_Started_(Addons).

~~~powershell
& '.\mediamonkey_archive_stream\package-mmip.ps1'
# In MediaMonkey: Tools > Addons > Add
# Select mediamonkey_archive_stream\Release\ArchiveStreamPlayer-MediaMonkey5.mmip
~~~

**Verified compatibility limit:** MediaMonkey's own [What's New in MediaMonkey 5](https://www.mediamonkey.com/wiki/WebHelp:What's_New/5.0) states that Winamp input/output plugins remain supported but **general-purpose Winamp plugins are no longer supported**. The DEV player previously loaded both DLLs without a crash, but this is not evidence that MediaMonkey ever invokes `gen_archive_stream.dll`. There is no general-purpose expansion callback to certify in MM5. The MMIP therefore packages **only the experimental Win32 `in_archive_stream.dll` and shared settings script**; it does not pretend to add archive playlist expansion. The MediaMonkey 5 installer may leave an older `gen_archive_stream.dll` behind from a previous MMIP; remove it manually only after confirming it came from this project. MediaMonkey's verified, user-facing archive playback mode remains the bridge. The legacy input-only MMIP does not provide archive expansion; the dedicated JavaScript Tools add-on above now provides an in-player picker, a verified native queue handoff and **all seven ZIP codec playback/seek tests** using the shared archive stream. The Winamp virtual-path scheme is not automatically supported.

The verified playback alternative is the bridge and MediaMonkey's official `/NoSplash /Add` playlist command:

~~~powershell
& '.\archive_player_bridge\launch-archive.ps1' -Archive 'C:\audiotest\Archives\7Z_NonSolid.7z' -Player MediaMonkey
~~~

MediaMonkey successfully accepted `/Add` and `/Play` in the DEV test, and the bridge recorded an actual direct-stream request (stream=1). Audible output remains uncertified. In-player archive expansion is **not provided by the old Winamp general plugin in MediaMonkey 5**; use the separate native JavaScript Tools add-on above for the verified archive selection, queue and seven ZIP codec playback/seek path, or this standalone bridge for external playlist imports.

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
