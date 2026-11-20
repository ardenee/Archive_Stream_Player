# Media Player (2022) — SDK feasibility probes

**Status:** The native Windows OLE drop-target preview has been built, validated with actual Shell file-drop data and limited-user process execution, and added as an **optional unchecked component of the DEV installer**. It opens archive audio in `Microsoft.ZuneMusic` over our existing localhost HTTP bridge. **This is an external companion window, not a plugin injected into Media Player**, and is not in the published v0.1.7 installer. Legacy `wmplayer.exe` remains unrelated.

See [the full SDK investigation](../MEDIA-PLAYER-2022.md) for results, limitations and acceptance criteria.

## C++ projects

| Project | Purpose |
| --- | --- |
| `mf_scheme_probe.vcxproj` | Disposable COM `IMFSchemeHandler` DLL for a new `astprobe:` URL scheme. Logs activation and deliberately refuses playback |
| `mf_scheme_probe_harness.vcxproj` | Exercises COM activation and `IMFSourceResolver`, without the Media Player application |
| `activate_modern_media_file.vcxproj` | Uses `IApplicationActivationManager::ActivateForFile` for the real Microsoft Store Media Player |
| `cfapi_probe.vcxproj` | Temporary Windows Cloud Files provider using supported **progressive** hydration by default, with explicit unsupported-partial comparison modes. Supports a physical MP3 control or **one MP3 from inside a ZIP** via the static shared archive backend |
| `cfapi_read_probe.vcxproj` | Independent native Win32 test for a 4,096-byte read of a projected file, with optional offset and bounded controller timeout |
| `modern_media_drop.vcxproj` | **Native Win32 OLE ZIP/7z/RAR drop target (DEV preview)**: automatic conditional loopback-helper elevation, session-owned rollback, background bridge, Media Player file activation, session cleanup and single-argument shortcut drops |

All build as **Release|x64** with MSBuild (Visual Studio 18 Community, Windows SDK 10.0.26100.0). The Cloud Files project uses `archive_backend.lib` and the existing statically linked archive dependencies; no extra runtime DLLs are introduced.

## DEV tests

`test-mf-scheme-dispatch.ps1` registers a temporary **HKCU-only** COM CLSID and a new `astprobe:` scheme, tests via a non-elevated scheduled user task, and removes its registrations. The `-TestModernPlayer` option also opens an M3U8 using Media Player (2022). **Result:** the desktop MF resolver loads the handler, but the modern Media Player did not dispatch the scheme.

`test-cfapi-placeholder.ps1` registers a temporary read-only sync root for a 68,336-byte MP3 and verifies that the first 4,096 bytes match the original. The older PowerShell-based read took roughly 60 seconds, but that timing is **not representative of native Win32 reads**. `test-cfapi-small-read.ps1` uses the independent `cfapi_read_probe.exe` with a bounded timeout: a normal native 4 KB read returned in approximately 105–129 ms overall, both with streaming permitted and with disk hydration required.

`test-cfapi-modern-player.ps1` activates the real modern Media Player with that projected MP3. **Result:** app activation, data fetch and transfer succeeded; UI identified the two-second audio file.

`test-cfapi-archive-player.ps1` uses the archive backend to open the first independently streamable MP3 within `M:\Music\VA - Subspecies 4 (2024).zip`, exposes it through a read-only Cloud Files placeholder, and activates modern Media Player. **Result:** first MP3 at entry index 1, 20,661,882 decoded bytes delivered from the archive in 316 transfers, no backend read failures, ~0.22 sec to create placeholder. The test was repeated and cleanup validated.

**Confirmed limitation:** Microsoft's SDK documentation says `CF_HYDRATION_POLICY_PARTIAL` is **not currently supported**. Diagnostic A/B tests using both that policy and supported `CF_HYDRATION_POLICY_PROGRESSIVE` proved that **Media Player (2022) requested the entire 20,661,882-byte track**, even with `STREAMING_ALLOWED` enabled, and **the entire track was retained on disk** (20,709,376 bytes NTFS allocation). The requester PID and process path identified `Microsoft.Media.Player.exe`. The provider delivered all 316 blocks in approximately 297 ms, so backend transfer was not the issue.

Even a native 4 KB read of a 68 KB MP3 placeholder caused Windows to request and store the entire small file. The two supported native-reader configurations completed promptly. **Cloud Files is a possible disk-backed compatibility layer, not a streaming-first solution for the observed player behaviour.** Audible playback, seeking, full-album playlist and metadata, solid/encrypted handling, and standard-user provider lifecycle remain uncertified. Do not install as a supported adapter.

## HTTP stream verification — November 20, 2026

`test-http-range-bridge.ps1` is a **headless, non-UI regression**: it starts the real shared archive bridge, validates nine M3U8 audio URLs, confirms `HEAD` 200 and `GET Range: bytes=0-4095` 206, verifies all 4,096 body bytes and the `HTTP_AUDIO_TRANSFER` logging, then shuts down the temporary server and removes its test playlist. It requires no Media Player app registration or loopback exemption. x64 and Win32 bridge Release builds pass.

Separately, the **actual modern Media Player** was manually tested with the bridge: Open URL launched MP3 entry 1; a nine-entry M3U8 played Neptune and Next advanced to Wonky Control Room. These passed **only** while the packaged player was temporarily allowed localhost access by `CheckNetIsolation LoopbackExempt`. That exemption was removed and its absence verified after each test. We must not silently enable a lasting exception for users.

Byte-counting instrumentation in `archive_player_bridge.cpp` confirmed Media Player fetched **20,661,882**, **19,742,415** and **19,649,377** complete MP3 bytes upon opening the album, plus **23,793,435** for the next track. This is `ROUTE_STREAM`, no managed extraction, but **aggressive prefetch makes it potentially costly for solid/encrypted archives**. Seeking via the app's UI worked within buffered data; **a nonzero-start HTTP Range request from the player was not observed**. Archive-format parity, bridge lifetime, metadata and duration certification remain open. See [the full results](../MEDIA-PLAYER-2022.md).

## Native drop target preview — November 20, 2026

Built `modern_media_drop.vcxproj` as a native x64 Win32 GUI application without any extra runtime archive DLLs. It uses `RegisterDragDrop`/`IDropTarget` to accept exactly **one ZIP, 7z or RAR file per drop**, rejects ordinary audio files, starts the existing static `archive_player_bridge.exe` with a hidden console, and calls the actual modern Media Player using `IApplicationActivationManager::ActivateForFile`.

**How to use (DEV source build):** run `media_player_2022/bin/x64/Release/modern_media_drop.exe`; drop an archive onto its window. Alternatively, drag an archive onto an installed Start Menu/desktop launcher shortcut (the launcher accepts the archive as a single file-path argument). Leave the Archive Stream window open while playing, since it owns the localhost bridge lifetime. Closing the window gracefully shuts down all its album sessions, deletes transient playlists and returns its Windows loopback exception to the previous state.

**How to use (DEV installer):** choose the optional, unchecked `Media Player (2022) - HTTP archive drop target (preview)` component. The Start Menu `Media Player (2022) - Drop Archives` shortcut is installed; a desktop shortcut is optional. The `modern_media_drop.exe` and `archive_player_bridge.exe` files are installed side by side under `{app}/MediaPlayer2022`; that packaging layout passed a real M3U8 activation test.

**Loopback permissions:** Media Player's own Settings screen has no `Allow loopback` option. The launcher checks Windows' `CheckNetIsolation LoopbackExempt -s` for the exact `Microsoft.ZuneMusic_8wekyb3d8bbwe` package (case-insensitive). If not already allowed, it asks Windows for UAC elevation of a **separate helper process**. Only this helper changes the exemption; it monitors the parent GUI process and automatically removes its own permission on exit or abnormal termination. Preexisting exemptions are never removed. The permission gives Media Player localhost access generally while active, not exclusively to our secret URLs. The HTTP server itself listens only on 127.0.0.1 with random per-session URL secrets.

**Validation performed on DEV:** x64 Release build PASS; temporary UAC helper added the missing exemption; 9-entry Subspecies 4 ZIP playlist activated in the real modern player; Windows Shell `IDataObject` CF_HDROP → OLE `DragEnter`/`Drop` contract PASS; direct bare archive argument from a shortcut PASS; normal MP3 file drop rejected without HTTP or loopback change; limited desktop-user scheduled-task OLE drop/activation PASS; existing exemption preserved without launching an elevated helper; closing the GUI stopped the bridge, removed session directories, and restored any exemption created by the session. **Windows Job Object ownership** (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`) was also tested by force-killing the drop GUI: the bridge was terminated automatically, and the elevated helper restored loopback and exited. A forced crash can leave a few small M3U8/ASX/log metadata files; no archive audio files were extracted. The installer compiled using Inno Setup 7; it was **not installed on PRD**.

**Limitations:** You cannot drop directly onto Microsoft's app window using this integration; only our separate drop window or shortcut. Media Player eagerly downloads complete tracks and prefetches upcoming entries; this cannot yet be described as efficient partial HTTP streaming. Independent real-host seeking beyond prebuffered data, solid/encrypted/multipart parity, playlist-wide pre-play metadata and durations are not certified. No archive-content CRC is calculated; backend routing is direct stream → configured memory buffer → managed cache.

Source-controlled test scripts: `test-modern-media-drop.ps1` runs Shell OLE acceptance and MP3 rejection; `test-modern-drop-crash.ps1` force-kills the owner to verify Windows Job Object child-process cleanup and loopback rollback. Both were run successfully on DEV. **No native decoder was inserted into Microsoft's application.**

## Cleanup and safety

The tests use disposable directories under `D:\Temp\AST-MediaPlayer2022-MFQA`. They do not change audio file associations, Windows optional features, real archives or the other player plugins. Cloud Files registration and placeholders are removed by the provider at shutdown. The first version left a read-only test file; this was corrected, and the repeat ZIP test confirmed that its test root was gone.

The Media Foundation test deliberately returns a media-format error to avoid falsely claiming playback. It uses its own test URL scheme rather than registering `ast:` or overriding standard audio extensions. It cleans up its COM registration and temporary playlists even when the test fails.

Next: constrain Media Player's aggressive HTTP prefetch where possible, certify non-buffered seeking, test solid/encrypted/multipart files and bounded playlist metadata/durations, then decide whether to promote the optional DEV drop-target preview to a release component. Cloud Files remains an inferior disk-hydrating fallback. No direct drag-and-drop into Microsoft's own window is supported.

Sources: [Microsoft hydration policy warning](https://learn.microsoft.com/en-us/windows/win32/api/cfapi/ne-cfapi-cf_hydration_policy_primary) and [Cloud Files sync-root policy](https://learn.microsoft.com/en-us/windows/win32/api/cfapi/nf-cfapi-cfregistersyncroot).
