# Media Player (2022) SDK feasibility and integration decision

**Target:** Windows 11 modern **Media Player** (`Microsoft.ZuneMusic`, `Microsoft.Media.Player.exe`). NOT legacy Windows Media Player (`wmplayer.exe`). **Status: the real modern Media Player plays audio from archive entries using the existing localhost HTTP bridge, and opens a multi-track HTTP M3U8 playlist. The Cloud Files approach is deprioritised. A supported adapter, HTTP seeking under low-buffer conditions, and drag-and-drop are not yet implemented or certified.** Do not mark this adapter supported or add an installer component until the remaining gates pass.

## DEV feasibility results — 19 November 2026

The actual C++ test programs live in [media_player_2022](media_player_2022/README.md), built with Visual Studio 18 and Windows SDK 10.0.26100.0.

| Gate | Outcome | Evidence / limitation |
| --- | --- | --- |
| Diagnostic `IMFSchemeHandler` COM activation | **Pass in standard-user Media Foundation harness** | Isolated `astprobe:` CLSID registered under HKCU. `IMFSourceResolver` invoked `CLASS_FACTORY_REQUESTED` and `BEGIN_CREATE_OBJECT` with a limited user token; elevated DEV shell could not load that HKCU class |
| Modern Media Player opens ordinary M3U8 | **Pass** | Actual `Microsoft.ZuneMusic` activated with `IApplicationActivationManager::ActivateForFile`; playable-looking MP3 control appeared with metadata |
| Modern Media Player invokes `astprobe:` scheme handler | **Not observed** | Custom URL appeared as `track.mp3` but the app did not activate the registered MF handler. Do not use this registration path as the production backend without further evidence |
| Cloud Files API single-file callback / byte parity | **Pass** | Registered a disposable read-only sync root; created 68,336-byte MP3 placeholder; `CF_CALLBACK_TYPE_FETCH_DATA` and `CF_OPERATION_TYPE_TRANSFER_DATA` returned data; first 4,096 bytes matched original file. Initial standalone read took **60.82 seconds**, not acceptable playback latency |
| Modern Media Player loads a CfAPI audio placeholder | **Pass at file activation/byte-delivery level** | Real Media Player accepted the placeholder via `ActivateForFile`; Windows requested bytes through our native provider; the player displayed a 2-second MP3 duration |
| ZIP-to-Media Player via CfAPI + shared archive backend | **Pass at archive byte-delivery level (repeat test)** | With `M:\Music\VA - Subspecies 4 (2024).zip`, `archive_backend` opened independently streamable MP3 entry **index 1**, uncompressed size **20,661,882 bytes**. Media Player accepted the virtual file; Windows called the provider, which supplied **316 blocks** (65536 bytes maximum) and reported **zero archive read errors**. Placeholder ready in **~0.22 s**. No explicit extraction or HTTP bridge |
| Whole-track hydration / disk behaviour | **Confirmed limitation** | Both explicit `PARTIAL` and supported `PROGRESSIVE`, with `STREAMING_ALLOWED`, caused the Media Player process to request the entire **20,661,882-byte** MP3 in one `FETCH_DATA`; both retained **20,661,882 bytes** on disk with **20,709,376 bytes** allocated. The initial `PARTIAL` policy is explicitly marked **unsupported** by Microsoft |
| Independent 4 KB read / seekable-file baseline | **Native test passes promptly, full-file hydration remains** | `cfapi_read_probe.exe` read 4,096 bytes through a 68,336-byte MP3 placeholder in ~105–129 ms overall with both `PROGRESSIVE | STREAMING_ALLOWED` and `PROGRESSIVE | NONE`. Despite the small read, Windows requested and retained the entire **68,336 bytes**. A separate PowerShell-job test had blocked; it is **not** a reliable measure of intrinsic Cloud Files latency |
| Cleanup | **Pass after correction** | The initial test left a read-only 20.7 MB test file behind; provider cleanup now clears read-only on its own QA placeholder and removes file + sync root after disconnect. Repeat archive and native-read tests reported no leftover roots; temporary MF registration and test playlists removed |

**Important:** Actual Media Player opened the projected ZIP-backed file and consumed its bytes, but the prototype does not yet certify audible playback, seek/restart timing, or correct album metadata across multiple tracks. **Crucially, Cloud Files does not satisfy our no-full-track-hydration goal in the observed Media Player behaviour.** It may be useful only as an explicitly disk-hydrated compatibility fallback, not our primary direct-streaming route.

### Policy A/B instrumentation

The diagnostic callback logs the actual requesting process ID/path, required/optional ranges, per-block transfer timing and the file's `CF_PLACEHOLDER_STANDARD_INFO.OnDiskDataSize` plus `FILE_STANDARD_INFO.AllocationSize`. In both 20.7 MB MP3 runs, the request came from `Microsoft.Media.Player.exe`, `RequiredFileOffset=0`, `RequiredLength=20661882`, and all 316 `TRANSFER_DATA` blocks completed in approximately **297 ms**. The measured on-disk data was **20661882 bytes**; allocated file space was **20709376 bytes**. Neither hydration policy reduced the amount read or retained.

The read-only native `cfapi_read_probe` test also shows that even a **4096-byte** application read led to a **68336-byte** fetch and full on-disk retention. Native read completion was fast with both streaming modifier choices; the old PowerShell `Start-Job` timing result must not be cited as normal application startup latency. All disposable sync roots are cleaned after each completed native probe.

## HTTP compatibility proof — 20 November 2026

**Result: core HTTP playback and album navigation PASS in actual Media Player (2022). This is not a native plug-in and is not yet a production adapter.** No drag-and-drop integration was added.

The existing `archive_player_bridge/archive_player_bridge.cpp` process was used without an extraction directory. It binds **only** to `127.0.0.1`, exposes per-session secret URLs, writes a local M3U8 file with nine `http://127.0.0.1:<port>/<secret>/entry/<index>.mp3` URLs for `M:\Music\VA - Subspecies 4 (2024).zip`, and retains existing backend routing **stream → memory → managed cache**. Media Player itself performs audio decoding. The tests used `Microsoft.Media.Player.exe` from `Microsoft.ZuneMusic` 11.2609.11.0.

| Test | Result | Evidence / limitation |
| --- | --- | --- |
| HTTP bridge HEAD and byte-range request | **Pass** | `HEAD` returned 200; `Range: bytes=0-4095` returned **206** with `Content-Range: bytes 0-4095/20661882`, and the new `HTTP_AUDIO_TRANSFER` log confirmed exactly 4096 payload bytes |
| Direct HTTP track via **Media Player → Home → Open URL** | **Pass only with a temporary loopback exemption** | Media Player displayed and started **Neptune — Cosmic Bazaar; Subspecies 4**, with Pause enabled. Bridge recorded `ROUTE_STREAM index=1`, 20,661,882-byte response |
| Full album M3U8 in modern Media Player | **Pass with same exemption** | The app opened a nine-entry HTTP M3U8; initial Neptune played, and its Next button advanced to **Wonky Control Room**. Individual playback of all nine entries is not yet certified. The bridge logged subsequent direct-stream requests |
| Native seek UI | **Partial pass** | Seeking in the player advanced its seek slider (e.g. to ~72.8%) while playback remained active, but **no nonzero-start HTTP Range request was observed from the app**. The app already had buffered data; do not claim HTTP range-seek certification |
| Actual bytes sent to Media Player | **High prefetch cost** | Repeated test with byte-counting logs showed complete HTTP transfers for entry 1 (**20,661,882 bytes**), 2 (**19,742,415 bytes**) and 3 (**19,649,377 bytes**) on album opening (~60 MB), plus entry 4 (**23,793,435 bytes**) after Next. These were **actual payload byte counts**, not only requested response lengths. All four used `ROUTE_STREAM`, without managed disk extraction |
| Default Windows loopback isolation | **Blocks playback** | Media Player accepted the HTTP URL / playlist but did not contact `127.0.0.1` until temporarily using `CheckNetIsolation LoopbackExempt -a -n=Microsoft.ZuneMusic_8wekyb3d8bbwe`. Tests removed the exemption with `-d` and verified it was absent afterward. A production integration must not silently weaken the app's network isolation |
| Playlist metadata/duration | **Incomplete** | Embedded MP3 title/album appeared after fetching, but the bridge M3U8 currently writes `#EXTINF:-1` where durations have not been probed; pre-play durations for all tracks and full artist/album metadata still need work |

**Interpretation:** Unlike Cloud Files, the HTTP route feeds our shared archive stream directly to the player and does not hydrate Windows cloud placeholders. However, **Media Player downloads whole tracks and prefetches upcoming entries aggressively**. For solid/encrypted archives this may trigger unnecessary memory/cache preparation; the experimental external drop target now exists, but prefetch must be measured and bounded before a non-preview release. It is premature to claim low-I/O streaming, efficient seek, or general archive parity from these ZIP tests alone.

**Production gates remaining:** (1) improve or bound the aggressive prefetch, particularly solid archives; (2) validate actual HTTP seeking when not already buffered; (3) test ZIP, 7z, RAR, multipart and encrypted routes; (4) correct playlist metadata and durations; (5) assess session lifetime, app restarts and installer uninstall behaviour for general release. A separate, tested preview OLE drop target now exists; it deliberately does not intercept Microsoft's private drag/drop controls or claim to be a native SDK plug-in.

## Native drag-and-drop preview — 20 November 2026

**Implemented in `media_player_2022/modern_media_drop.cpp`**, as `modern_media_drop.vcxproj` (Release|x64). This is a **standalone OLE drop target**, not an in-process Media Player decoder or hook. It does **not** accept drops directly onto Microsoft's window. It accepts Windows Shell `CF_HDROP` for exactly one existing ZIP/7z/RAR archive, rejects non-archives, and can also accept a bare archive filepath passed by dragging onto its shortcut.

**User flow:** open the Archive Stream → Media Player drop window from the Start Menu; drop one archive, or drag the archive onto its optional desktop shortcut. The companion starts `archive_player_bridge.exe` hidden and opens the generated `archive-stream.m3u8` using the documented Windows `IApplicationActivationManager::ActivateForFile` interface. The HTTP server binds exclusively to 127.0.0.1 with a per-session random secret; the existing backend handles streaming, bounded memory fallback and managed cache. **Keep the drop window open** while listening; on close it sends /shutdown to all managed bridge sessions, deletes transient playlists and exits. Each bridge process is assigned to a Windows Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so force-killing the launcher also terminates its bridge. The elevated helper separately observes its owner's process handle and rolls back only its own loopback exemption even after abnormal termination. A forced crash can leave small temporary playlist and log metadata files (not extracted audio), which were manually cleaned after the DEV crash test.

**Automatic loopback (user requested):** the Microsoft Media Player Settings screen has **no** Allow loopback toggle. This is Windows AppContainer isolation. The launcher checks `CheckNetIsolation LoopbackExempt -s` for `Microsoft.ZuneMusic_8wekyb3d8bbwe` case-insensitively. If the exemption is missing, it requests a single Windows UAC elevation to run **only a separate privileged helper**. The helper enables the exemption and waits on the owning drop-window process handle, removing only the exemption it created when the owner exits, including after unexpected termination. An existing exemption is retained without spawning an elevated helper. Windows grants the packaged app access to loopback as a whole for this period; the exemption is **not restricted to Archive Stream URLs**. No persistent exception is installed by setup.

**DEV verification:**
- Release|x64 native GUI build PASS; Windows Shell `IDataObject` with `CF_HDROP` passed the actual `IDropTarget::DragEnter`/`Drop` contract; the application then started the HTTP bridge and activated the nine-entry Subspecies 4 ZIP playlist in the actual Media Player.
- Ordinary MP3 rejected at `DragEnter` with **no** HTTP backend process and **no** loopback modification.
- Missing loopback permission automatically enabled via a UAC helper; both normal close and later clean-up removed the helper-created exemption. A failed early implementation incorrectly treated the package name case-sensitively and left an exception behind; it was cleaned immediately and the fixed code verified.
- Pre-existing loopback permission preserved on exit; the launcher started **zero** unnecessary elevated helpers. A forced parent-process kill was also verified: **bridge terminated, loopback exemption removed, elevated helper exited**. The isolated QA playlist directory left by the forced kill was identified and cleaned on DEV.
- A *limited, normal desktop-user* interactive scheduled-task launch passed OLE drop and Media Player activation; the task, bridge and temporary OS permission were cleaned afterward.
- Portable/installed-file layout of `modern_media_drop.exe` plus `archive_player_bridge.exe` side-by-side passed a direct archive argument and real app activation. The unselected **Media Player (2022) — drop target (preview)** component and Start Menu / optional desktop shortcuts compile in Inno Setup 7; DEV installer file was produced without installing it on PRD.
- Automated, source-controlled tests: `media_player_2022/test-modern-media-drop.ps1` confirms Shell OLE drop → app activation → process cleanup, ZIP acceptance and plain-MP3 rejection; `media_player_2022/test-modern-drop-crash.ps1` confirms forced owner termination cannot orphan the bridge or leave a loopback exemption. It may display UAC when Windows has not previously granted the exemption.

**Not certified / remaining:** Media Player eagerly fetches full tracks (around 60 MB for the first three MP3s), HTTP non-buffered forward/backward seeking, solid/encrypted/multipart archive parity and playlist-wide pre-play duration/tag metadata. The external drop UI is currently a **DEV preview**; not the published release and **not** a native SDK plug-in. Do not assert that drag-and-drop into the Microsoft Media Player window itself works.

## Media Player file extensions and codec registration — 20 November 2026

**Investigation triggered by Media Player's File Open supported-types list.** On DEV, installed `Microsoft.ZuneMusic` **11.2609.11.0** declares **58 supported extensions** in its signed `AppxManifest.xml`, specifically `uap:Extension Category="windows.fileTypeAssociation" -> uap:FileTypeAssociation -> uap:SupportedFileTypes`. `.mp3`, `.wav`, `.flac`, `.ogg`, `.m3u` and `.m3u8` are present; **`.zip`, `.7z` and `.rar` are absent**. Windows file associations for ZIP/RAR/7z on DEV remain owned by WinRAR and were not changed.

There are **three different extension mechanisms** and they must not be conflated:

1. **App package file types:** the app declares the extensions associated with *its own package*. We cannot extend Microsoft's manifest through our separate installer or a generic Shell file association. This is a package-specific Windows declaration, not an external decoder hook. [Microsoft `SupportedFileTypes` reference](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-uap-filetypeassociation-supportedfiletypes).
2. **Media Foundation byte-stream handlers:** standalone COM `IMFByteStreamHandler` classes can be registered under `HKCU\Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\.<ext>` or `HKLM` (or a MIME type). DEV contains the native WAV `{42C9B9F5-16FC-47ef-AF22-DA05F7C842E3}`, MP3 `{A82E50BA-8E92-41eb-9DF2-433F50EC2993}` and FLAC `{0E41CFB8-0506-40F4-A516-77CC23642D91}` handlers; **no archive handlers**. These are consulted only when an application actually uses the relevant Media Foundation source resolver. Our earlier custom *scheme* handler test proved modern Media Player did **not** invoke the registered diagnostic handler via M3U8. [Microsoft byte-stream handler registration](https://learn.microsoft.com/en-us/windows/win32/medfound/scheme-handlers-and-byte-stream-handlers).
3. **Windows Runtime `MediaExtensionManager`:** app-local parser/codec registration. Microsoft documents that registrations are local to the application, not exported to other applications. Our companion cannot call `RegisterByteStreamHandler` to change Microsoft's app. [Microsoft MediaExtensionManager](https://learn.microsoft.com/en-us/uwp/api/windows.media.mediaextensionmanager).

**Legacy WMP must be kept separate.** Microsoft's old `WMPlayer\Extensions\.<ext>` registry API is specifically for legacy `wmplayer.exe`; it does **not** establish an integration hook for the new `Microsoft.ZuneMusic` app. [Legacy extension registry documentation](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/wmp/file-name-extension-registry-settings).

**Reversible DEV test:** created uniquely named test audio files with valid MPEG audio / 30-second WAV payloads and nonstandard `.astmp3`/`.astwav` extensions; temporarily registered `Content Type=audio/mpeg` or `audio/wav` and `PerceivedType=audio` under their own `HKCU\Software\Classes` extension keys. `IApplicationActivationManager::ActivateForFile` returned `S_OK` both **before** and **after** registration, and Media Player displayed the test WAV filenames in both conditions. Therefore successful file activation **does not demonstrate extension acceptance by the native Open File filter or OLE drag target**, and a generic MIME/perceived-type registry registration has not proved a route to direct archive drop. Every temporary registry key was removed; ZIP/7z/RAR file associations and the packaged app were untouched.

An automated OLE drop simulation targeting Media Player's window did not complete even for the supported WAV control and was forcibly stopped; it was **inconclusive**, so no claims of positive/negative direct-drop acceptance derive from it. The existing companion OLE target remains verified separately. A subsequent test of the native File Open and real drag/drop filter must compare an isolated test extension with and without registration, and then a dedicated temporary *byte-stream handler* before attempting any installation-wide archive association. **Merely making ZIP a permitted extension would still not tell Media Player to enumerate tracks or invoke our HTTP playlist bridge.**

Read-only reproducible inventory command: `media_player_2022/inventory-media-extensions.ps1`. **No production extension registration or drag-and-drop hook was added.**

## DEV inventory — 19 November 2026

- Installed Media Player: `Microsoft.ZuneMusic` 11.2609.11.0 x64, package family `Microsoft.ZuneMusic_8wekyb3d8bbwe`.
- Its manifest advertises .m3u/.m3u8 and ordinary audio/video formats, but **not** .zip, .rar or .7z. App extensions exposed in the manifest: file associations, protocol activation, autoplay, and execution alias. **No third-party host plug-in page/activation contract is advertised.**
- Windows SDK `10.0.26100.0`: `mfidl.h`, `mfapi.h`, `mfobjects.h`, `mfmediaengine.h`, `cfapi.h`, `projectedfslib.h` and the matching `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib`, `CldApi.lib`, `ProjectedFSLib.lib` are installed.
- `CldFlt` Windows Cloud Files filter driver **Running**, automatic start; built-in `cldapi.dll` available. No Windows feature enablement is necessary to start an initial Cloud Files feasibility experiment.
- Windows `Client-ProjFS` optional feature **Disabled**. It was NOT enabled or modified; enabling ProjFS requires deliberate approval and can require reboot.
- Existing `archive_player_bridge/launch-archive.ps1 -Player WMP` invokes `wmplayer.exe` (Legacy) with HTTP ASX. This is **not** support for the modern app.
- Unrelated MusicBee working-tree changes must remain untouched.

## SDK review — what the interfaces actually offer

| SDK | Interfaces / callbacks | Supports archive-backed on-demand data? | Compatibility caveat |
| --- | --- | --- | --- |
| **Media Foundation scheme handler** | `IMFSchemeHandler`, `IMFSourceResolver`; optionally `IMFByteStream`, `IMFByteStreamHandler`, `IMFMediaSource` | Yes. A `ast:` URL could map to shared archive streams without exposing physical audio files. | Can be registered per user under `HKCU\Software\Microsoft\Windows Media Foundation\SchemeHandlers`; **the packaged Media Player app is not documented to invoke third-party registered handlers**. Test actual host dispatch before implementing a full decoder/source. |
| **Media Foundation byte-stream handler** | `IMFByteStreamHandler`, `IMFByteStream` and media source resolution | Yes, including known sizes and random seeks. | Registration by extension/MIME; no guarantee the modern app consults that registry or accepts new extensions. Don't override `.zip`, `.rar`, `.7z` or native audio file types. |
| **Cloud Files API (CfAPI)** | `CfRegisterSyncRoot`, `CfConnectSyncRoot`, `CfCreatePlaceholders`, `CF_CALLBACK_TYPE_FETCH_DATA`, `CfExecute` + `CF_OPERATION_TYPE_TRANSFER_DATA` | **Yes**, exposes actual read-only audio file paths and serves requested ranges from our existing backend. | Needs a provider service/process and a registered sync root. The system may cache/hydrate data on disk and displays cloud integration UX. |
| **Projected File System (ProjFS)** | `PrjStartVirtualizing`, `PRJ_GET_DIRECTORY_ENUMERATION_CB`, `PRJ_GET_PLACEHOLDER_INFO_CB`, `PRJ_GET_FILE_DATA_CB`, `PrjWriteFileData` | **Yes**, exposes projected read-only ordinary file paths. | Optional Windows feature currently disabled, possible restart, and files are cached on local storage after reads. |
| **WinRT MediaExtensionManager** | `Windows.Media.MediaExtensionManager` | Can register a decoder/parser within the calling app. | **App-local registration**; cannot register an arbitrary module into Microsoft's separate Media Player process. |
| **MediaEngineExtension** | `IMFMediaEngineExtension` | Can hook a media engine in an application we control. | Host must itself pass `MF_MEDIA_ENGINE_EXTENSION`; no API to force Microsoft's app to do so. |
| **Legacy WMP SDK / DirectShow** | `IWMPPlugin` and older UI/DSP interfaces; DirectShow source filters | Possible for **Legacy WMP**. | Not a documented modern Media Player extensibility route. |
| **Shell property handler** | `IInitializeWithStream`, `IPropertyStore`; optionally `IMFGetService` | Tags and durations can be published without decoding content in a controlled source. | Does **not** solve playback or make Media Player invoke a custom protocol. |
| **Windows App SDK MediaPlayerElement** | `Windows.Media.Playback.MediaPlayer`, `MediaPlayerElement` | Yes, for a new app built by us. | Not a way to attach controls/plug-ins to the Microsoft Media Player app. |

Some third-party packaged decoder extensions depend on the restricted `inProcessMediaExtension` capability. That is **not a generally available ordinary third-party plug-in mechanism**; do not build the solution assuming access.

## Strongest filesystem option: Cloud Files API

Choose a private, read-only, user-scoped `Archive Stream\Mounted Albums\<stable archive id>\` directory. Create per-entry audio placeholders with each file's uncompressed length and original format extension, plus a generated album .m3u8. The placeholder identity maps to archive path, entry index and header metadata. The real archive remains untouched; do not calculate content CRCs.

Microsoft's `CF_HYDRATION_POLICY_PRIMARY` documentation explicitly warns that **PARTIAL is not currently supported**. The proof therefore now defaults to **PROGRESSIVE**; `run-partial` and `run-archive-partial` remain only to reproduce the earlier comparison. `CF_HYDRATION_POLICY_MODIFIER_STREAMING_ALLOWED` permits (but does not require) Windows to avoid disk retention. Our tests proved it **did not** prevent full on-disk hydration for the actual modern player or for a small independent read. Do not combine this modifier with `VALIDATION_REQUIRED`. Do not advertise direct streaming/no extraction on the basis of Cloud Files transfers.

A user-mode provider handles `CF_CALLBACK_TYPE_FETCH_DATA` and responds with `CF_OPERATION_TYPE_TRANSFER_DATA`, forwarding offsets and lengths to the existing shared archive backend. The required fallback order remains: **stream entry directly → configured memory buffer → managed cache only when needed**. Solid, encrypted, non-seekable and multipart cases need explicit tests. Ensure no unintended background archive scans and no recursive provider I/O.

Media Player must see the projected media as ordinary files, receive a normal M3U8, and pass pause/seek/next/previous track tests. The native Media Player UI and audio decoders remain untouched. An Archive Stream shell context menu (not an in-app plug-in) should eventually open the mounted album; no user-facing PowerShell window or HTTP bridge.

Important: a Cloud Files provider has operational lifecycle requirements (start/stop/restart, sync-root registration/unregistration, process shutdown, stale placeholders, ACLs, cleanup, mount stability and availability). Cloud-provider UX may be distracting; evaluate this before selecting it for production.

## Remaining acceptance work after initial proof

1. **Direct-streaming gate FAILED for the tested Cloud Files modes**: actual Media Player requested, and the platform retained, the entire 20.7 MB MP3 using both PARTIAL (documented unsupported) and PROGRESSIVE. Even independent 4 KB reads fully hydrated a 68 KB file. **Do not implement Cloud Files as the primary streaming route**; consider it only as an optional managed disk-hydrating compatibility fallback. Continue evaluating non-filesystem paths accepted by the modern app without assuming an in-process plug-in API.
2. **Multi-track album**: create 9 stable placeholders for Subspecies 4 and 10 for Natural Order 2, with original codec extensions, exact lengths and bounded metadata. Build M3U8; verify actual Media Player playlist, pause, skip, seek (forward/backward), metadata and restart. No independent extraction or content CRC calculation.
3. **Shared routing integration**: expand the diagnostic one-entry direct-stream mode to ZIP/7z/RAR with strict **stream → configured memory → managed cache** fallbacks for solid/encrypted/nonseekable entries, user-scoped credentials, cancellation and concurrency. Do not treat OS hydration as our managed cache.
4. **Provider lifecycle**: standard-user permission test; crash/restart; uninstall cleanup; private user-specific sync root; read-only security; stale placeholders, archive disappearance and thread-safe cancellations. A shell menu should open archives without PowerShell or HTTP.
5. **ProjFS fallback**: only if CfAPI fails requirements and enabling the optional feature is acceptable. We have NOT enabled it. Do not silently install system features.
6. **Release gate**: the DEV installer has a selectable, unchecked **external drop-target preview**. Only promote it as fully supported after playback, unbuffered seek, metadata and all archive routing gates pass. This is a companion transport adapter, not a native plugin.

## Sources reviewed

- https://learn.microsoft.com/en-us/windows/win32/medfound/scheme-handlers-and-byte-stream-handlers
- https://learn.microsoft.com/en-us/windows/win32/medfound/source-resolver
- https://learn.microsoft.com/en-us/uwp/api/windows.media.mediaextensionmanager
- https://learn.microsoft.com/en-us/windows/win32/api/cfapi/nf-cfapi-cfregistersyncroot
- https://learn.microsoft.com/en-us/windows/win32/api/cfapi/nf-cfapi-cfconnectsyncroot
- https://learn.microsoft.com/en-us/windows/win32/api/cfapi/nf-cfapi-cfcreateplaceholders
- https://learn.microsoft.com/en-us/windows/win32/cfapi/build-a-cloud-file-sync-engine
- https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/CloudMirror
- https://learn.microsoft.com/en-us/windows/win32/projfs/projected-file-system
- https://learn.microsoft.com/en-us/windows/win32/projfs/enabling-windows-projected-file-system
- https://learn.microsoft.com/en-us/windows/win32/medfound/custom-metadata-providers-for-media-files
- https://learn.microsoft.com/en-us/answers/questions/5769519/how-to-develop-custom-decoder-media-foundation-tra
