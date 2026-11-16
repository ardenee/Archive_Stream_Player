# Archive Stream Player — acceptance evidence

**Date:** 16 November 2026
**Environment:** DEV `game-amd`, isolated portable player instances and disposable test files.
**Meaning:** **H** = verified inside a real player; **T** = verified by an adapter/backend/transport test harness. Neither means physical-speaker audibility. No user archives, real password lists, or normal player profiles were used by these tests.

## Interrupted-stream recovery

| Component | Scope | Acceptance outcome | Level |
| --- | --- | --- | --- |
| MusicBee 3.7 x86 BASS | Five ZIP compressed-read faults: WAV/FLAC/MP3 stream → RAM; OGG stream → cache; oversized WAV stream → cache | **5/5 PASS**: recovery trace, native PCM, sustained real-player playback, HTTP=0 | H |
| MusicBee 3.7 x86 BASS | FLAC/non-solid 7z → RAM, OGG/non-solid RAR → cache, oversized WAV/non-solid 7z → cache | **3/3 PASS**: same real-player recovery gates, HTTP=0 | H |
| MusicBee GUI after recovery | Separate ZIP WAV and FLAC fault-injection runs | **WAV and FLAC PASS**, including a second FLAC rerun: actual GUI seek, BASS `SEEK_OK`, playback resumes. Intermittent GUI-click reliability remains a caveat; other host recovery cases are not counted as GUI-seek proof | H |
| AIMP 5.40 x86/x64 | Real native plugin, injected compressed **read and seek** failure, fallback to RAM or managed cache | **8/8 PASS**: `STREAM_RECOVER_OK` with expected route; actual GUI seek and resumed playback, HTTP=0 | H |
| foobar2000 x86/x64 | Dedicated test-only native component, injected read/seek failures with RAM/cache routes | **8/8 PASS**: recovered bytes match reference and seek works in isolated actual foobar2000 processes | H |
| AIMP IAIMPStream contract x86/x64 | Read/seek injection × RAM/cache | **8/8 PASS**: source-byte parity and seeking | T |
| HTTP bridge Win32/x64 | Interrupted-range read → RAM/cache | **4/4 PASS**: exact requested HTTP range bytes and expected route counters; not proof of any bridge player's audio decoder | T |
| Media Foundation shared backend | AAC/M4A/WMA × ZIP/non-solid 7z/non-solid RAR × RAM/cache; plus blocked fallback test | **18/18 standalone recovery PASS**, with a separate fail-closed negative case (documented in `musicbee_archive_stream/test-native-mf-midstream-recovery.ps1`) | T |

**Important MF distinction:** A separate attempt to force a *real MusicBee* AAC IMFByteStream read fault **after** initial playback did not hit another compressed read, even with a longer ADTS fixture. The decoder continued playing prefetched data. It is **not** an in-player recovery pass. That exploratory test-only build was discarded rather than shipped. MusicBee's native WAV/FLAC/MP3/OGG host fault tests above did reach the fault and pass.

## Broader real-player acceptance

| Player | Real-host acceptance on 16 November 2026 |
| --- | --- |
| VLC 3.0.24 x64 native | **21/21 PASS**: exact VLC source-vs-archive decoded-PCM parity, seven codecs × ZIP/7z/RAR; direct archive streaming, HTTP=0. A separate RAR/M4A actual GUI seek/resume also passed. Earlier recorded 21/21 GUI-seek coverage remains historical evidence, not a second rerun today |
| MediaMonkey 2024 portable | **7/7 PASS**: seven ZIP codecs queued with correct titles, played and genuinely GUI-sought; native streaming, HTTP/cache=0. Separate first-use encrypted-header RAR5 AAC: real password prompt, bounded RAM playback, GUI/decoder seek and declined save passed |
| AIMP x86/x64 | **2/2 normal GUI PASS**: ZIP MP3 native stream and seek/resume; additionally the eight real-host injected read/seek recoveries above |
| XMPlay 4.1 x86 | **2/2 PASS**: native non-solid 7z MP3 and encrypted AES-7z MP3 with real password dialog and GUI seek/resume, no managed cache; XMPARC delivers a host-owned decoded buffer, not an ongoing player stream |
| foobar2000 x86/x64 | **2/2 PASS**: live title-field refresh for already-loaded archive playlist entries; plus eight fault-injection tests above |
| MusicBee 3.7 x86 | The eight native in-player interrupted read recovery cases above, following previous seven-codec ZIP/7z/RAR GUI and encrypted archive certification |

**Previously tested, not retested in this session:** native Winamp 5.9 and WACUP x86 playback; WACUP x64 bridge. **Still unverified:** Windows Media Player actual playback through its bridge; physical-speaker audibility across host/codec combinations; externally unplugged/corrupted archive media behavior; actual real-host Media Foundation post-PCM read interruption; unattended stability/long-run tests. These are not marked as passed.

## Reproducible DEV checks

- MusicBee: `musicbee_archive_stream/test-real-native-midstream-gui.ps1`, `-Case all -ArchiveKind zip`; separately `-Case flac -ArchiveKind 7z`, `-Case ogg -ArchiveKind rar`, `-Case wav-cache -ArchiveKind 7z`. Use `-RequireGuiSeek` to add a strict mouse-driven seek gate. Builds a **test-only** DLL outside Release.
- AIMP: `aimp_archive_stream/test_real_player_gui_seek.py`. Real-host fault options `AST_AIMP_GUI_FAULT_ROUTE=memory|cache`, `AST_AIMP_GUI_FAULT_MODE=read|seek`, and `ARCHIVE_AIMP_FAULT_TEST_BIN_DIR` pointing to separately built `ARCHIVE_STREAM_AIMP_FAULT_TEST` DLLs. The normal test uses Release DLLs when fault options are absent.
- AIMP contract: `aimp_archive_stream/test_native_recovery.py`.
- foobar2000: `foo_archive_stream/test_native_recovery.py` with the isolated `ARCHIVE_FOOBAR_FAULT_PORTABLE_DIR` for x86 and x64; `foo_archive_stream/test_live_title_refresh.py x86|x64`.
- Bridge: `archive_player_bridge/test_midstream_recovery.py` with dedicated non-Release fault-test bridge executables.
- VLC: `vlc_archive_stream/test_real_vlc_seven_codecs.py --kind all`; `vlc_archive_stream/test_real_vlc_gui_seek.py --kind rar --only m4a`.
- MediaMonkey: `mediamonkey_archive_stream/test_native_portable_codecs.py` with `AST_MM5_QA_NATIVE=1`, `AST_MM5_VERIFY_GUI_SEEK=1`; encrypted RAR5 AAC adds `AST_MM5_QA_AES=accept`, `AST_MM5_QA_ARCHIVE=rar`, `AST_MM5_QA_RAR_HEADERS=1`, `AST_MM5_QA_ONLY=AAC`, `AST_MM5_QA_AUTOPLAY=1`.
- XMPlay: `xmp_archive_stream/test_real_native_gui_seek.py` and `xmp_archive_stream/test_real_native_password_gui.py`.

The production routing policy remains **direct archive stream → configured bounded memory → shared managed encoded cache (last resort)**. Test-only fault hooks are never enabled in the release binaries.
