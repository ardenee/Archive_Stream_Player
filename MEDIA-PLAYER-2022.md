# Media Player (2022) SDK feasibility and integration decision

**Target:** Windows 11 modern **Media Player** (`Microsoft.ZuneMusic`, `Microsoft.Media.Player.exe`). NOT legacy Windows Media Player (`wmplayer.exe`). **Status: SDK feasibility prototypes have passed basic Cloud Files activation and archive byte-transfer tests, but no production adapter or fully verified playback/seeking is implemented.** Do not mark this adapter supported or add an installer component until the remaining gates pass.

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
| Whole-track hydration / disk behaviour | **Major unresolved limitation** | Despite `CF_HYDRATION_POLICY_PARTIAL | STREAMING_ALLOWED`, Windows requested **all 20,661,882 bytes** in one `FETCH_DATA`. Do NOT claim the implementation avoids whole-track reads or disk hydration; the OS may materialize the file |
| Cleanup | **Pass after correction** | The initial test left a read-only 20.7 MB test file behind; provider cleanup now clears read-only on its own QA placeholder and removes file + sync root after disconnect. Repeat archive test reported no leftover root; temporary MF registration and test playlists removed |

**Important:** Actual Media Player opened the projected ZIP-backed file and consumed its bytes, but the prototype does not yet certify audible playback, seek/restart timing, correct album metadata across multiple tracks, or acceptable cache/disk use. CfAPI is therefore **feasible but not yet acceptable for release** under our streaming-first policy.

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

Use `CF_HYDRATION_POLICY_PARTIAL` rather than `FULL` or `PROGRESSIVE`: PARTIAL does not continue to fetch the rest of a track in the background. Evaluate `CF_HYDRATION_POLICY_MODIFIER_STREAMING_ALLOWED`: this explicitly permits Windows **not** to store returned data on disk, but **does not promise zero caching**. Do not combine this modifier with `VALIDATION_REQUIRED`. Log bytes served, callbacks, latency, resulting placeholder hydration state, real disk writes, and cache pressure before advertising direct stream/no extraction.

A user-mode provider handles `CF_CALLBACK_TYPE_FETCH_DATA` and responds with `CF_OPERATION_TYPE_TRANSFER_DATA`, forwarding offsets and lengths to the existing shared archive backend. The required fallback order remains: **stream entry directly → configured memory buffer → managed cache only when needed**. Solid, encrypted, non-seekable and multipart cases need explicit tests. Ensure no unintended background archive scans and no recursive provider I/O.

Media Player must see the projected media as ordinary files, receive a normal M3U8, and pass pause/seek/next/previous track tests. The native Media Player UI and audio decoders remain untouched. An Archive Stream shell context menu (not an in-app plug-in) should eventually open the mounted album; no user-facing PowerShell window or HTTP bridge.

Important: a Cloud Files provider has operational lifecycle requirements (start/stop/restart, sync-root registration/unregistration, process shutdown, stale placeholders, ACLs, cleanup, mount stability and availability). Cloud-provider UX may be distracting; evaluate this before selecting it for production.

## Remaining acceptance work after initial proof

1. **Measure and mitigate whole-track fetches**: the first real MP3 received a single 20.7 MB `FETCH_DATA` request, even with partial hydration/streaming allowed. Measure actual NTFS bytes allocated, callback duration, and whether Media Player can seek or start audio before a full fetch completes. If this cannot respect bounded resource use, Cloud Files is not yet suitable for production.
2. **Multi-track album**: create 9 stable placeholders for Subspecies 4 and 10 for Natural Order 2, with original codec extensions, exact lengths and bounded metadata. Build M3U8; verify actual Media Player playlist, pause, skip, seek (forward/backward), metadata and restart. No independent extraction or content CRC calculation.
3. **Shared routing integration**: expand the diagnostic one-entry direct-stream mode to ZIP/7z/RAR with strict **stream → configured memory → managed cache** fallbacks for solid/encrypted/nonseekable entries, user-scoped credentials, cancellation and concurrency. Do not treat OS hydration as our managed cache.
4. **Provider lifecycle**: standard-user permission test; crash/restart; uninstall cleanup; private user-specific sync root; read-only security; stale placeholders, archive disappearance and thread-safe cancellations. A shell menu should open archives without PowerShell or HTTP.
5. **ProjFS fallback**: only if CfAPI fails requirements and enabling the optional feature is acceptable. We have NOT enabled it. Do not silently install system features.
6. **Release gate**: ship a selectable installer option and update supported-feature matrix only after real-audio playback, seeking and all routing gates have passed. The current source is a PoC, not a full player adapter.

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
