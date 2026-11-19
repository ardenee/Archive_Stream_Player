# Media Player (2022) — SDK feasibility probes

**Status:** Diagnostic code only. **Not a production Media Player plugin** and not part of the main installer. This is for `Microsoft.ZuneMusic` (the Windows 11 Media Player application), *not* `wmplayer.exe` (Legacy).

See [the full SDK investigation](../MEDIA-PLAYER-2022.md) for results, limitations and acceptance criteria.

## C++ projects

| Project | Purpose |
| --- | --- |
| `mf_scheme_probe.vcxproj` | Disposable COM `IMFSchemeHandler` DLL for a new `astprobe:` URL scheme. Logs activation and deliberately refuses playback |
| `mf_scheme_probe_harness.vcxproj` | Exercises COM activation and `IMFSourceResolver`, without the Media Player application |
| `activate_modern_media_file.vcxproj` | Uses `IApplicationActivationManager::ActivateForFile` for the real Microsoft Store Media Player |
| `cfapi_probe.vcxproj` | A temporary Windows Cloud Files provider using partial hydration with streaming allowed. Supports an ordinary MP3 control or **one MP3 from inside a ZIP** via the static shared archive backend |

All build as **Release|x64** with MSBuild (Visual Studio 18 Community, Windows SDK 10.0.26100.0). The Cloud Files project uses `archive_backend.lib` and the existing statically linked archive dependencies; no extra runtime DLLs are introduced.

## DEV tests

`test-mf-scheme-dispatch.ps1` registers a temporary **HKCU-only** COM CLSID and a new `astprobe:` scheme, tests via a non-elevated scheduled user task, and removes its registrations. The `-TestModernPlayer` option also opens an M3U8 using Media Player (2022). **Result:** the desktop MF resolver loads the handler, but the modern Media Player did not dispatch the scheme.

`test-cfapi-placeholder.ps1` registers a temporary read-only sync root for a 68,336-byte ordinary MP3 and verifies that the first 4,096 bytes read through the placeholder equal the original file, with actual Cloud Files data fetch callbacks. **Result:** byte parity passed; initial standalone read took about 60 seconds and needs further performance work.

`test-cfapi-modern-player.ps1` activates the real modern Media Player with that projected MP3. **Result:** app activation, data fetch and transfer succeeded; UI identified the two-second audio file.

`test-cfapi-archive-player.ps1` uses the archive backend to open the first independently streamable MP3 within `M:\Music\VA - Subspecies 4 (2024).zip`, exposes it through a read-only Cloud Files placeholder, and activates modern Media Player. **Result:** first MP3 at entry index 1, 20,661,882 decoded bytes delivered from the archive in 316 transfers, no backend read failures, ~0.22 sec to create placeholder. The test was repeated and cleanup validated.

**Critical limitation:** although `CF_HYDRATION_POLICY_PARTIAL | CF_HYDRATION_POLICY_MODIFIER_STREAMING_ALLOWED` is set, Windows requested the **entire 20.7 MB track at once**. This is *not* an acceptance test for bounded streaming or low disk use. The tests do not yet certify audible playback, seeking, full-album playlist or metadata, solid/encrypted handling, or standard-user provider lifecycle. Do not install as a supported adapter.

## Cleanup and safety

The tests use disposable directories under `D:\Temp\AST-MediaPlayer2022-MFQA`. They do not change audio file associations, Windows optional features, real archives or the other player plugins. Cloud Files registration and placeholders are removed by the provider at shutdown. The first version left a read-only test file; this was corrected, and the repeat ZIP test confirmed that its test root was gone.

The Media Foundation test deliberately returns a media-format error to avoid falsely claiming playback. It uses its own test URL scheme rather than registering `ast:` or overriding standard audio extensions. It cleans up its COM registration and temporary playlists even when the test fails.

Next: investigate whole-track fetch/hydration, measure physical disk allocations and seek latency, then test a full nine-track virtual playlist. Only then consider production architecture or installer registration.
