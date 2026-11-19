# Media Player (2022) — SDK feasibility probes

**Status:** Diagnostic code only. **Not a production Media Player plugin** and not part of the main installer. This is for `Microsoft.ZuneMusic` (the Windows 11 Media Player application), *not* `wmplayer.exe` (Legacy).

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

## Cleanup and safety

The tests use disposable directories under `D:\Temp\AST-MediaPlayer2022-MFQA`. They do not change audio file associations, Windows optional features, real archives or the other player plugins. Cloud Files registration and placeholders are removed by the provider at shutdown. The first version left a read-only test file; this was corrected, and the repeat ZIP test confirmed that its test root was gone.

The Media Foundation test deliberately returns a media-format error to avoid falsely claiming playback. It uses its own test URL scheme rather than registering `ast:` or overriding standard audio extensions. It cleans up its COM registration and temporary playlists even when the test fails.

Next: investigate whether Media Player (2022) accepts a direct seekable transport that avoids filesystem hydration. If not, Cloud Files should only be considered an explicitly disk-backed fallback subject to the shared cache policy; no release integration is approved. Full-album playlists and in-player seek tests remain pending.

Sources: [Microsoft hydration policy warning](https://learn.microsoft.com/en-us/windows/win32/api/cfapi/ne-cfapi-cf_hydration_policy_primary) and [Cloud Files sync-root policy](https://learn.microsoft.com/en-us/windows/win32/api/cfapi/nf-cfapi-cfregistersyncroot).
