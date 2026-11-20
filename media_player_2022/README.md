# Media Player (2022) — SDK feasibility probes

**Status:** Native Media Foundation/Cloud Files probes, plus **validated experimental HTTP playback through the shared archive bridge**. **Not a production Media Player plugin** and not part of the main installer. This is for `Microsoft.ZuneMusic` (Windows 11 Media Player), *not* `wmplayer.exe` (Legacy).

See [the full SDK investigation](../MEDIA-PLAYER-2022.md) for results, limitations and acceptance criteria.

## C++ projects

| Project | Purpose |
| --- | --- |
| `mf_scheme_probe.vcxproj` | Disposable COM `IMFSchemeHandler` DLL for a new `astprobe:` URL scheme. Logs activation and deliberately refuses playback |
| `mf_scheme_probe_harness.vcxproj` | Exercises COM activation and `IMFSourceResolver`, without the Media Player application |
| `activate_modern_media_file.vcxproj` | Uses `IApplicationActivationManager::ActivateForFile` for the real Microsoft Store Media Player |
| `cfapi_probe.vcxproj` | Temporary Windows Cloud Files provider using supported **progressive** hydration by default, with explicit unsupported-partial comparison modes. Supports a physical MP3 control or **one MP3 from inside a ZIP** via the static shared archive backend |
| `cfapi_read_probe.vcxproj` | Independent native Win32 test for a 4,096-byte read of a projected file, with optional offset and bounded controller timeout |

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

**No drag-and-drop target, app-specific decoder or installer component has been created.**

## Cleanup and safety

The tests use disposable directories under `D:\Temp\AST-MediaPlayer2022-MFQA`. They do not change audio file associations, Windows optional features, real archives or the other player plugins. Cloud Files registration and placeholders are removed by the provider at shutdown. The first version left a read-only test file; this was corrected, and the repeat ZIP test confirmed that its test root was gone.

The Media Foundation test deliberately returns a media-format error to avoid falsely claiming playback. It uses its own test URL scheme rather than registering `ast:` or overriding standard audio extensions. It cleans up its COM registration and temporary playlists even when the test fails.

Next: evaluate controls on HTTP prefetch (especially solid/encrypted archives), investigate actual non-buffered HTTP seek requests, provide bounded playlist duration/tag metadata and design a secure opt-in loopback strategy. Cloud Files remains an inferior disk-hydrating fallback; no release integration or drop-target work is approved yet.

Sources: [Microsoft hydration policy warning](https://learn.microsoft.com/en-us/windows/win32/api/cfapi/ne-cfapi-cf_hydration_policy_primary) and [Cloud Files sync-root policy](https://learn.microsoft.com/en-us/windows/win32/api/cfapi/nf-cfapi-cfregistersyncroot).
