# Archive Stream Player — feature matrix

**Snapshot:** 13 November 2026, after commit `8d8474a`. This is a **verification matrix**, not a claim that every function has passed real-player acceptance tests. The [main README](README.md) describes the project; [PLAYER-ADAPTERS.md](PLAYER-ADAPTERS.md) contains implementation detail and test evidence.

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
| **Winamp 5.9 x86** — two native plugins | **H** | **H** | **H** | **H** (previous native tests) | **T** | **I** (playlist cache refresh requested) | **?** (current full matrix not certified) |
| **foobar2000 x86 / x64** — native component | **H** | **H** | **H** (PCM decode) | **H** (decode + seek) | **H** | **H** via `%archive_stream_title%` | **?** |
| **WACUP x86** — Winamp plugins | **H** | **H** | **H** (playback time and track progression) | **?** (UI seek) | **I** (Winamp path) | **I** | **?** |
| **WACUP x64** — local HTTP bridge | **—** | **H** (M3U8) | **H** (playback progress / HTTP reads) | **T** (HTTP ranges; UI seek unverified) | **T** (M3U8 labels) | **L** (reload playlist) | **?** |
| **AIMP 5.40 x86 / x64** — native plugin | **H** | **H** | **H** (archive route requested) | **T** (native COM read/seek, not GUI seek) | **T** (virtual-file metadata) | **I** (host redraw unverified) | **?** |
| **XMPlay x86** — native XMPARC | **H** | **H** | **H** (host invokes extraction) | **T** (partial probes; GUI seek unverified) | **T** (reversible indexed aliases) | **L** (reimport archive tracks) | **?** |
| **XMPlay** — local HTTP bridge | **—** | **H** (M3U8) | **H** (live HTTP archive stream) | **T** (HTTP ranges) | **T** (M3U8 labels) | **L** (reload playlist) | **?** |
| **MusicBee 3.7** — menu plugin + bridge | **—** | **I** (Now Playing queue not certified) | **T** (URLs tested outside complete UI workflow) | **T** (bridge HTTP ranges) | **T** (bridge labels) | **L** (requeue/reload; display host-dependent) | **?** |
| **MediaMonkey 2024** — bridge; Winamp DLLs also load | **I** (native expander not certified) | **H** (bridge playlist /Add) | **H** (archive HTTP request) | **T** (bridge ranges) | **T** (bridge labels) | **L** (reload playlist) | **?** |
| **VLC** — M3U8 + local HTTP bridge | **—** | **H** | **H** (two-track test with dummy audio output) | **T** (bridge ranges, not GUI seek) | **T** (M3U8 labels) | **L** (reload playlist) | **?** (dummy output only) |
| **Windows Media Player 12** — ASX + HTTP bridge | **—** | **T** (ASX generated; full host playback unverified) | **H** (URL requested only) | **T** (bridge ranges; host seek unverified) | **T** (ASX labels) | **L** (reload ASX) | **?** |

**Important:** WMP's observed URL request is **not** successful music playback: its COM control remained in a transition state, even for a plain MP3. MediaMonkey's DLL load is **not** proof that its native archive expander is called. XMPlay's XMPARC returns a host-owned decoded-byte buffer, **not** true on-demand playback streaming; use its HTTP bridge mode for that.

## Archive handling and routing

Every adapter uses the **same static archive backend**, but host formats and recovery routes are verified to different depths. This table distinguishes direct native tests from a shared backend/bridge capability.

| Player / mode | ZIP | 7z | RAR | Encrypted / password archives | Split / multipart and solid | Direct incremental archive stream | RAM fallback | Reusable managed disk cache | Solid-track background preparation |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Winamp x86 native | **H** | **H** | **H** | **I** (backend tested) | **H** / **I** (varies by case) | **T** | **T** | **T** | **T** |
| foobar2000 x86 / x64 native | **T** | **H** | **T** | **H** (encrypted 7z decode/seek); **T** (other backend) | **H** (encrypted multipart 7z); **T** (other cases) | **T** | **T** | **T** | **T** |
| WACUP x86 native | **I** | **H** | **I** | **I** | **H** (solid 7z); **I** (other) | **T** | **H** (solid 7z) | **T** | **T** |
| WACUP x64 bridge | **H** | **H** | **T** (shared bridge) | **T** (shared bridge) | **H** (solid 7z); **T** (other) | **H** (ZIP) | **H** (solid 7z) | **T** | **T** |
| AIMP x86 / x64 native | **T** | **H** | **T** | **I** (backend tested) | **H** (solid 7z); **T** (other) | **T** | **T** | **T** | **T** |
| XMPlay x86 native | **T** | **H** | **T** | **I** (backend tested) | **H** (solid 7z); **T** (other) | **L** (internal stream, then host memory block) | **T** | **T** | **T** |
| XMPlay bridge | **H** | **T** | **T** | **T** (shared bridge) | **T** | **H** (HTTP) | **T** | **T** | **T** |
| MusicBee bridge | **T** | **T** | **T** | **T** (bridge only) | **T** (bridge only) | **T** (transport only) | **T** | **T** | **T** |
| MediaMonkey bridge | **H** | **T** | **T** | **T** (bridge only) | **T** (bridge only) | **H** (HTTP request) | **T** | **T** | **T** |
| VLC bridge | **H** | **T** | **T** | **T** (bridge only) | **T** (bridge only) | **H** (HTTP request) | **T** | **T** | **T** |
| WMP bridge | **T** | **T** | **T** | **T** (bridge only) | **T** (bridge only) | **T** (transport; not confirmed playing) | **T** | **T** | **T** |

Here **T** in a bridge row can mean **the bridge itself passed a transport/backend regression**, not that the named player has played that exact format. Source archives are read-only. Supported solid layouts use RAM or cache instead of claiming independently seekable archive streams. Encrypted Copy/Store 7z requires a per-entry CRC verification pass before streaming, and exotic coder chains are not universally certified.

## Player settings, titles and diagnostics

| Player / mode | Settings entry | Password-list use | Interactive password prompt | Shared cache controls | Per-host diagnostic logs | Title-change behavior |
| --- | --- | --- | --- | --- | --- | --- |
| Winamp x86 | **I** (native entry wired; common GUI **H** standalone) | **T** | **I** (dialog not fully accepted) | **H** (common GUI); native controls wired | **I** (shared logging connected) | New titles formatted; `IPC_REFRESHPLCACHE` on save; repaint pending |
| foobar2000 x86 / x64 | **H** (native Preferences); common GUI **H** standalone | **H** (real encrypted 7z tests) | **I** | **H** (common GUI) | **I** (shared logging connected) | **H** live refresh in existing playlist with `$if2(%archive_stream_title%,%title%)` |
| WACUP x86 | **I** (Winamp-compatible settings) | **T** (backend) | **I** | **H** (common GUI) | **I** | Uses Winamp format/cache path; repaint not certified |
| WACUP x64 | **T** (launcher `-Configure`) | **T** (bridge) | **I** | **H** (common GUI) | **T** (bridge) | M3U8 reload required |
| AIMP x86 / x64 | **H** (Options service registered); common GUI **H** standalone | **T** (backend) | **I** | **H** (common GUI) | **T** (native log regression) | **T** title in virtual metadata; host row repaint pending |
| XMPlay x86 native | **T** (Config callback); common GUI **H** standalone | **T** (backend) | **I** | **H** (common GUI) | **T** (native log regression) | **T** new indexed titles; **L** reimport old rows |
| XMPlay bridge | **T** (launcher) | **T** (bridge) | **I** | **H** (common GUI) | **T** (bridge) | Reload playlist |
| MusicBee | **H** (Tools menu registers); dialog launch **I** | **T** (bridge) | **I** | **H** (common GUI) | **T** (x86 adapter logging) | Bridge labels; in-player presentation unverified |
| MediaMonkey | **T** (launcher) | **T** (bridge) | **I** | **H** (common GUI) | **T** (bridge) | Reload playlist |
| VLC | **T** (launcher) | **T** (bridge) | **I** | **H** (common GUI) | **T** (bridge) | Reload playlist |
| WMP | **T** (launcher) | **T** (bridge) | **I** | **H** (common GUI) | **T** (bridge) | Reload ASX |

**Shared GUI verification:** `archive_player_bridge/test_shared_settings_ui.py` opened Windows Forms on DEV, checked controls, changed the preview, clicked **Clear managed cache** and **Clear all logs**, confirmed unrelated files were preserved, and confirmed **Cancel** did not modify user registry settings. This validates the **shared GUI**, not the button that launches it in every individual host.

**Password security:** Saved candidate passwords are stored in a plaintext `keeps.txt` list, only after consent and successful verification. Password prompting in each specific player needs interactive acceptance. Decrypted cached audio is stored on disk until managed cleanup; cache behavior is not equivalent to securely erasing extracted content.

## Known host limitations

- **WACUP x64:** Its tested preview does not load third-party Winamp plugin DLLs. Use the bridge; this is not a permanent claim about future WACUP versions.
- **XMPlay native:** `XMPARC` hands a decoded memory block to XMPlay, so actual host-on-demand streaming requires the optional HTTP bridge. Indexed display aliases retain decoding compatibility, but existing playlist row labels cannot be repainted by the archive plugin's API.
- **foobar2000:** The live title field is `%archive_stream_title%`. An existing playlist column must use that field (for example `$if2(%archive_stream_title%,%title%)`); the plugin intentionally does not rewrite `%title%` or file tags.
- **MusicBee / MediaMonkey:** Native plugin load/menu registration is not the same as an end-to-end in-player queue and playback certification. Their loopback bridge functionality has stronger backend coverage than their native UI workflow.
- **VLC / WMP:** Both use playlists/HTTP rather than native archive decoder plugins. VLC passed a dummy audio-output test; WMP full playback has not passed.
- **All players:** Audible-speaker output, all codecs, every encryption layout, GUI seek behavior, password prompt rejection/cancellation and automatic title repaint need per-host verification where marked **?** or **I**.

## Repeatable evidence

| Area | Test / source | Coverage |
| --- | --- | --- |
| Shared Win32/x64 HTTP transport and routes | [`archive_player_bridge/test_bridge.py`](archive_player_bridge/test_bridge.py), [`test_stream_priority.py`](archive_player_bridge/test_stream_priority.py) | ZIP/7z/RAR stream, memory/cache fallback, byte-for-byte HTTP ranges |
| Passwords/encrypted 7z | [`archive_backend_test/test_7z_encrypted_layouts.py`](archive_backend_test/test_7z_encrypted_layouts.py), [`archive_player_bridge/test_passwords.py`](archive_player_bridge/test_passwords.py) | AES 7z layouts, split/solid fallback, wrong passwords and cancellation |
| Encrypted RAR memory route | [`archive_backend_test/test_encrypted_rar_memory.py`](archive_backend_test/test_encrypted_rar_memory.py) | RAR5 data/header encryption, Win32/x64 byte parity |
| Native AIMP recovery | [`aimp_archive_stream/test_native_recovery.py`](aimp_archive_stream/test_native_recovery.py) | Official AIMP stream contract; injected read/seek fallback |
| Native foobar2000 decoding and recovery | [`foo_archive_stream/test_native_recovery.py`](foo_archive_stream/test_native_recovery.py) and DEV real-player encrypted 7z test | SDK read/seek recovery and real Win32/x64 PCM decoder validation |
| Foobar2000 live title refresh | [`foo_archive_stream/test_live_title_refresh.py`](foo_archive_stream/test_live_title_refresh.py) | Real portable x86/x64 player, existing archive track, field refresh, settings retained |
| XMPlay reversible title aliases | [`xmp_archive_stream/test_title_aliases.py`](xmp_archive_stream/test_title_aliases.py) | ZIP and solid 7z two-track, full and partial WAV reads, old-name compatibility |
| Shared settings UI | [`archive_player_bridge/test_shared_settings_ui.py`](archive_player_bridge/test_shared_settings_ui.py) | Real GUI controls, title preview, cache/log clear and Cancel with isolated files |
| Shared logging and cache policy | [`test_shared_logging.py`](archive_player_bridge/test_shared_logging.py), [`test_shared_maintenance.py`](archive_player_bridge/test_shared_maintenance.py), [`test_shared_cache.py`](archive_player_bridge/test_shared_cache.py) | Per-adapter logs, Unicode paths, safe cleanup and cross-process cache reuse |
| Player-specific findings | [`PLAYER-ADAPTERS.md`](PLAYER-ADAPTERS.md) | Exact AIMP, WACUP, XMPlay, MusicBee, MediaMonkey, VLC and WMP DEV host observations |

**Maintenance rule:** Update the cell's verification level only after a relevant test or real-player observation passes. Keep untested capabilities as **I** or **?**, even if the shared backend supports the same format elsewhere.
