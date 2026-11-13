# Archive Stream Player — feature matrix

**Snapshot:** 13 November 2026, updated after real Winamp 5.9 and WACUP x86 password, PCM-output and seek tests, plus the previous native/bridge credential tests. Base matrix published at `96c55b7`. This is a **verification matrix**, not a claim that every function has passed real-player acceptance tests. The [main README](README.md) describes the project; [PLAYER-ADAPTERS.md](PLAYER-ADAPTERS.md) contains implementation detail and test evidence.

## Status legend

| Mark | Meaning |
| --- | --- |
| **H** | Observed in an actual host-player process or its UI. Includes verified archive reads, elapsed playback progression, or decoded output where specified; **does not automatically mean audible speaker output**. |
| **T** | Passed an automated adapter contract, real-player SDK/harness, shared-backend or HTTP-bridge regression. **Not** necessarily tested via the target player's UI. |
| **I** | Implemented/connected, but the specific end-to-end behavior is not yet certified. |
| **L** | Available with an important host limitation or manual refresh/reimport requirement. |
| **?** | Not verified; do not present as working. |
| **—** | Not offered in this integration mode. |

A player can have **H** for obtaining archive audio and **?** for audible output or interactive seeking. Status applies to the *specific column*, not to all codecs or all archive layouts.

## Player integration and playback

| Player / mode | Native archive listing | Tracks queued / expanded | Host requested or decoded audio | Seeking / byte-range tests | Title presets | Update titles already in playlist | Audible speaker output |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **Winamp 5.9 x86** — two native plugins | **H** | **H** | **H** (PCM to silent test sink) | **H** (18s seek + PCM flush/restart) | **T** | **I** (playlist cache refresh requested) | **?** (current full matrix not certified) |
| **foobar2000 x86 / x64** — native component | **H** | **H** | **H** (PCM decode) | **H** (decode + seek) | **H** | **H** via `%archive_stream_title%` | **?** |
| **WACUP x86** — Winamp plugins | **H** | **H** | **H** (PCM to silent test sink) | **H** (18s seek + PCM flush/restart) | **I** (Winamp path) | **I** | **?** |
| **WACUP x64** — local HTTP bridge | **—** | **H** (M3U8) | **H** (playback progress / HTTP reads) | **T** (HTTP ranges; UI seek unverified) | **T** (M3U8 labels) | **L** (reload playlist) | **?** |
| **AIMP 5.40 x86 / x64** — native plugin | **H** | **H** | **H** (archive route requested) | **T** (native COM read/seek, not GUI seek) | **T** (virtual-file metadata) | **I** (host redraw unverified) | **?** |
| **XMPlay x86** — native XMPARC | **H** | **H** | **H** (host invokes extraction) | **T** (partial probes; GUI seek unverified) | **T** (reversible indexed aliases) | **L** (reimport archive tracks) | **?** |
| **XMPlay** — local HTTP bridge | **—** | **H** (M3U8) | **H** (live HTTP archive stream) | **T** (HTTP ranges) | **T** (M3U8 labels) | **L** (reload playlist) | **?** |
| **MusicBee 3.7** — menu plugin + bridge | **—** | **I** (Now Playing queue not certified) | **T** (URLs tested outside complete UI workflow) | **T** (bridge HTTP ranges) | **T** (bridge labels) | **L** (requeue/reload; display host-dependent) | **?** |
| **MediaMonkey 2024** — bridge; Winamp DLLs also load | **I** (native expander not certified) | **H** (bridge playlist /Add) | **H** (archive HTTP request) | **T** (bridge ranges) | **T** (bridge labels) | **L** (reload playlist) | **?** |
| **VLC** — M3U8 + local HTTP bridge | **—** | **H** | **H** (two-track test with dummy audio output) | **T** (bridge ranges, not GUI seek) | **T** (M3U8 labels) | **L** (reload playlist) | **?** (dummy output only) |
| **Windows Media Player 12** — ASX + HTTP bridge | **—** | **T** (ASX generated; full host playback unverified) | **H** (URL requested only) | **T** (bridge ranges; host seek unverified) | **T** (ASX labels) | **L** (reload ASX) | **?** |

**Important:** WMP's observed URL request is **not** successful music playback: its COM control remained in a transition state, even for a plain MP3. MediaMonkey's DLL load is **not** proof that its native archive expander is called. XMPlay's XMPARC returns a host-owned decoded-byte buffer, **not** true on-demand playback streaming; use its HTTP bridge mode for that.

## Winamp-compatible host codec acceptance (silent PCM)

The following **H** results came from actual isolated Winamp 5.9 x86 and WACUP x86 processes, not merely a backend decoder harness. The host loaded the Archive Stream input plugin, handed decoded PCM to a **test-only, realtime simulated output plugin**, and its IPC seek to 18 seconds triggered an output flush followed by more PCM writes. The output plugin never opens speakers. This is **not** audible-output certification or byte-for-byte PCM-content parity; it demonstrates decoder-to-output delivery and working host-level seek/flush for the specified fixtures.

| Audio track / source | Winamp 5.9 x86 | WACUP x86 | Verified path |
| --- | --- | --- | --- |
| WAV in encrypted AES+LZMA2 7z | **H** | **H** | Native password entry, PCM writes, 18-second seek, cancellation stops |
| MP3 in unencrypted ZIP | **H** | **H** | PCM writes, 18-second seek, output flush/resume |
| FLAC in unencrypted ZIP | **H** | **H** | PCM writes, 18-second seek, output flush/resume |
| OGG in unencrypted ZIP | **H** | **H** | PCM writes, 18-second seek, output flush/resume |
| WMA in unencrypted ZIP | **H** | **H** | Media Foundation PCM writes, 18-second seek, output flush/resume |
| M4A in unencrypted ZIP | **H** | **H** | Media Foundation PCM writes, 18-second seek, output flush/resume |
| AAC in unencrypted ZIP | **H** | **H** | Media Foundation PCM writes, 18-second seek, output flush/resume |

**Seek reliability fix:** Pending seek requests now preempt the audio output-write loops for MP3/WAV/FLAC/OGG/Media Foundation in `in_archive_stream/playback.cpp`. Before the fix, a failing DirectSound device or a stalled output queue could prevent the decoder from reaching its seek branch even when Winamp IPC accepted the command. The DEV acceptance uses an isolated, device-independent sink to test decoding and seek behavior; it does not assert that real speaker hardware is configured.

## Archive handling and routing

Every adapter uses the **same static archive backend**, but host formats and recovery routes are verified to different depths. This table distinguishes direct native tests from a shared backend/bridge capability.

| Player / mode | ZIP | 7z | RAR | Encrypted / password archives | Split / multipart and solid | Direct incremental archive stream | RAM fallback | Reusable managed disk cache | Solid-track background preparation |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Winamp x86 native | **H** | **H** | **H** | **H** (AES 7z native WAV playback/seek + prompt) | **H** / **I** (varies by case) | **T** | **T** | **T** | **T** |
| foobar2000 x86 / x64 native | **T** | **H** | **T** | **H** (encrypted 7z decode/seek); **T** (other backend) | **H** (encrypted multipart 7z); **T** (other cases) | **T** | **T** | **T** | **T** |
| WACUP x86 native | **H** (ZIP with six codecs) | **H** | **I** (RAR not retested) | **H** (AES 7z native WAV playback/seek + prompt) | **H** (solid 7z); **I** (other) | **T** | **H** (solid 7z) | **T** | **T** |
| WACUP x64 bridge | **H** | **H** | **T** (shared bridge) | **T** (shared bridge) | **H** (solid 7z); **T** (other) | **H** (ZIP) | **H** (solid 7z) | **T** | **T** |
| AIMP x86 / x64 native | **T** | **H** | **T** | **T** (encrypted 7z native COM read/seek + prompt) | **H** (solid 7z); **T** (other) | **T** | **T** | **T** | **T** |
| XMPlay x86 native | **T** | **H** | **T** | **T** (encrypted 7z XMPARC full/partial reads + prompt) | **H** (solid 7z); **T** (other) | **L** (internal stream, then host memory block) | **T** | **T** | **T** |
| XMPlay bridge | **H** | **T** | **T** | **T** (shared bridge) | **T** | **H** (HTTP) | **T** | **T** | **T** |
| MusicBee bridge | **T** | **T** | **T** | **T** (bridge only) | **T** (bridge only) | **T** (transport only) | **T** | **T** | **T** |
| MediaMonkey bridge | **H** | **T** | **T** | **T** (bridge only) | **T** (bridge only) | **H** (HTTP request) | **T** | **T** | **T** |
| VLC bridge | **H** | **T** | **T** | **T** (bridge only) | **T** (bridge only) | **H** (HTTP request) | **T** | **T** | **T** |
| WMP bridge | **T** | **T** | **T** | **T** (bridge only) | **T** (bridge only) | **T** (transport; not confirmed playing) | **T** | **T** | **T** |

Here **T** in a bridge row can mean **the bridge itself passed a transport/backend regression**, not that the named player has played that exact format. Source archives are read-only. Supported solid layouts use RAM or cache instead of claiming independently seekable archive streams. Encrypted Copy/Store 7z requires a per-entry CRC verification pass before streaming, and exotic coder chains are not universally certified.

## Player settings, titles and diagnostics

| Player / mode | Settings entry | Password-list use | Interactive password prompt | Shared cache controls | Per-host diagnostic logs | Title-change behavior |
| --- | --- | --- | --- | --- | --- | --- |
| Winamp x86 | **I** (native entry wired; common GUI **H** standalone) | **T** | **H** (real host enter/cancel) | **H** (common GUI); native controls wired | **H** (test opt-in native trace) | New titles formatted; `IPC_REFRESHPLCACHE` on save; repaint pending |
| foobar2000 x86 / x64 | **H** (native Preferences); common GUI **H** standalone | **H** (real encrypted 7z tests) | **H** (native credential UI: enter/decline-save/cancel, x86/x64) | **H** (common GUI) | **I** (shared logging connected) | **H** live refresh in existing playlist with `$if2(%archive_stream_title%,%title%)` |
| WACUP x86 | **I** (Winamp-compatible settings) | **T** (backend) | **H** (real host enter/cancel) | **H** (common GUI) | **H** (native plugin trace) | Uses Winamp format/cache path; repaint not certified |
| WACUP x64 | **T** (launcher `-Configure`) | **T** (bridge) | **T** (bridge CredUI Win32/x64; WACUP UI not tested) | **H** (common GUI) | **T** (bridge) | M3U8 reload required |
| AIMP x86 / x64 | **H** (Options service registered); common GUI **H** standalone | **T** (backend) | **T** (Win32/x64 native COM harness + Windows CredUI; in-player UI pending) | **H** (common GUI) | **T** (native log regression) | **T** title in virtual metadata; host row repaint pending |
| XMPlay x86 native | **T** (Config callback); common GUI **H** standalone | **T** (backend) | **T** (Win32 XMPARC harness + Windows CredUI; in-player UI pending) | **H** (common GUI) | **T** (native log regression) | **T** new indexed titles; **L** reimport old rows |
| XMPlay bridge | **T** (launcher) | **T** (bridge) | **T** (bridge CredUI Win32/x64; XMPlay UI not tested) | **H** (common GUI) | **T** (bridge) | Reload playlist |
| MusicBee | **H** (Tools menu registers); dialog launch **I** | **T** (bridge) | **T** (bridge CredUI Win32/x64; MusicBee UI not tested) | **H** (common GUI) | **T** (x86 adapter logging) | Bridge labels; in-player presentation unverified |
| MediaMonkey | **T** (launcher) | **T** (bridge) | **T** (bridge CredUI Win32/x64; MediaMonkey UI not tested) | **H** (common GUI) | **T** (bridge) | Reload playlist |
| VLC | **T** (launcher) | **T** (bridge) | **T** (bridge CredUI Win32/x64; VLC UI not tested) | **H** (common GUI) | **T** (bridge) | Reload playlist |
| WMP | **T** (launcher) | **T** (bridge) | **T** (bridge CredUI Win32/x64; WMP UI not tested) | **H** (common GUI) | **T** (bridge) | Reload ASX |

**Shared GUI verification:** `archive_player_bridge/test_shared_settings_ui.py` opened Windows Forms on DEV, checked controls, changed the preview, clicked **Clear managed cache** and **Clear all logs**, confirmed unrelated files were preserved, and confirmed **Cancel** did not modify user registry settings. This validates the **shared GUI**, not the button that launches it in every individual host.

**Password security and verification:** Saved candidate passwords are stored in a plaintext `keeps.txt` list, only after consent and successful verification. **foobar2000 x86/x64 has real-player Windows credential UI tests:** entering a synthetic AES 7z password and declining save decodes successfully; cancelling fails closed without decoding; the configured list remains unchanged. **AIMP Win32/x64 and XMPlay Win32** passed the same password-entry/decline-save checks through their native SDK/COM test harnesses, including actual audio-byte reads and seeking or partial probes. **HTTP bridge Win32/x64** passed a Windows credential prompt followed by an encrypted 7z HTTP range read; its empty test list remained empty. These **T** tests validate the shared bridge path for WACUP x64, MusicBee, MediaMonkey, VLC, WMP and XMPlay bridge consumers, **not** the prompt inside each named player's own UI. **Winamp 5.9/WACUP x86 native prompts are now verified in isolated real-host processes** for both synthetic AES 7z password entry and cancellation, with source PCM output and seeking after acceptance. Their configured password lists remained unchanged; this does not certify all encryption modes. Decrypted cached audio may persist on disk until managed cleanup; cache cleanup is not secure erasure.

## Known host limitations

- **WACUP x64:** Its tested preview does not load third-party Winamp plugin DLLs. Use the bridge; this is not a permanent claim about future WACUP versions.
- **XMPlay native:** `XMPARC` hands a decoded memory block to XMPlay, so actual host-on-demand streaming requires the optional HTTP bridge. Indexed display aliases retain decoding compatibility, but existing playlist row labels cannot be repainted by the archive plugin's API.
- **foobar2000:** The live title field is `%archive_stream_title%`. An existing playlist column must use that field (for example `$if2(%archive_stream_title%,%title%)`); the plugin intentionally does not rewrite `%title%` or file tags.
- **MusicBee / MediaMonkey:** Native plugin load/menu registration is not the same as an end-to-end in-player queue and playback certification. Their loopback bridge functionality has stronger backend coverage than their native UI workflow.
- **VLC / WMP:** Both use playlists/HTTP rather than native archive decoder plugins. VLC passed a dummy audio-output test; WMP full playback has not passed.
- **All players:** Audible-speaker output, all codecs, every encryption layout, GUI seek behavior, bridge-client/player-level prompt handling and automatic title repaint still need per-host verification where marked **?** or **I**. Native foobar2000 password cancellation is now verified; do not extrapolate it to other hosts.

## Repeatable evidence

| Area | Test / source | Coverage |
| --- | --- | --- |
| Shared Win32/x64 HTTP transport and routes | [`archive_player_bridge/test_bridge.py`](archive_player_bridge/test_bridge.py), [`test_stream_priority.py`](archive_player_bridge/test_stream_priority.py) | ZIP/7z/RAR stream, memory/cache fallback, byte-for-byte HTTP ranges |
| Passwords/encrypted 7z | [`archive_backend_test/test_7z_encrypted_layouts.py`](archive_backend_test/test_7z_encrypted_layouts.py), [`archive_player_bridge/test_passwords.py`](archive_player_bridge/test_passwords.py) | AES 7z layouts, split/solid fallback, wrong passwords and cancellation |
| Real Winamp 5.9 / WACUP x86 password, PCM and seek | [`in_archive_stream/test_password_prompt_ui.py`](in_archive_stream/test_password_prompt_ui.py), [`test-only silent output SDK plugin`](in_archive_stream/test_support/out_archive_stream_test.cpp) | Real isolated hosts: synthetic AES 7z WAV prompt acceptance/cancel; ZIP MP3/FLAC/OGG/WMA/M4A/AAC decoded PCM delivered to silent output; 18-second host IPC seek, PCM flush and resumed writes; no user configuration changes |
| Real foobar2000 native password UI | [`foo_archive_stream/test_password_prompt_ui.py`](foo_archive_stream/test_password_prompt_ui.py) | Portable foobar2000 x86/x64: successful synthetic password, declined storage, cancellation and no user password-list changes |
| AIMP/XMPlay native adapter password UI | [`aimp_archive_stream/test_password_prompt_ui.py`](aimp_archive_stream/test_password_prompt_ui.py), [`xmp_archive_stream/test_password_prompt_ui.py`](xmp_archive_stream/test_password_prompt_ui.py) | Actual Windows CredUI within official native AIMP COM and XMPlay XMPARC test harnesses; encrypted 7z read/seek/partial parity, declined storage |
| HTTP bridge password UI | [`archive_player_bridge/test_bridge_password_prompt_ui.py`](archive_player_bridge/test_bridge_password_prompt_ui.py) | Win32/x64 CredUI, encrypted 7z byte-range WAV decoding, declined password storage; applies to bridge itself, not each client's GUI |
| Encrypted RAR memory route | [`archive_backend_test/test_encrypted_rar_memory.py`](archive_backend_test/test_encrypted_rar_memory.py) | RAR5 data/header encryption, Win32/x64 byte parity |
| Native AIMP recovery | [`aimp_archive_stream/test_native_recovery.py`](aimp_archive_stream/test_native_recovery.py) | Official AIMP stream contract; injected read/seek fallback |
| Native foobar2000 decoding and recovery | [`foo_archive_stream/test_native_recovery.py`](foo_archive_stream/test_native_recovery.py) and DEV real-player encrypted 7z test | SDK read/seek recovery and real Win32/x64 PCM decoder validation |
| Foobar2000 live title refresh | [`foo_archive_stream/test_live_title_refresh.py`](foo_archive_stream/test_live_title_refresh.py) | Real portable x86/x64 player, existing archive track, field refresh, settings retained |
| XMPlay reversible title aliases | [`xmp_archive_stream/test_title_aliases.py`](xmp_archive_stream/test_title_aliases.py) | ZIP and solid 7z two-track, full and partial WAV reads, old-name compatibility |
| Shared settings UI | [`archive_player_bridge/test_shared_settings_ui.py`](archive_player_bridge/test_shared_settings_ui.py) | Real GUI controls, title preview, cache/log clear and Cancel with isolated files |
| Shared logging and cache policy | [`test_shared_logging.py`](archive_player_bridge/test_shared_logging.py), [`test_shared_maintenance.py`](archive_player_bridge/test_shared_maintenance.py), [`test_shared_cache.py`](archive_player_bridge/test_shared_cache.py) | Per-adapter logs, Unicode paths, safe cleanup and cross-process cache reuse |
| Player-specific findings | [`PLAYER-ADAPTERS.md`](PLAYER-ADAPTERS.md) | Exact AIMP, WACUP, XMPlay, MusicBee, MediaMonkey, VLC and WMP DEV host observations |

**Maintenance rule:** Update the cell's verification level only after a relevant test or real-player observation passes. Keep untested capabilities as **I** or **?**, even if the shared backend supports the same format elsewhere.
