# VLC native Archive Stream adapter

Optional VLC 3.0 **x64** native input integration. The statically linked
DLL v1.3 implements the `ast://` access scheme, including an **in-process**
album at `ast://<hex-path>/list/album.m3u8`. VLC reads the virtual M3U8 and
all tracks in the same process, allowing verified per-archive passwords to
remain in process. No HTTP server, separate builder or on-disk playlist.
The legacy `vlc_native_playlist.exe` is retained for older unversioned DLLs.

Each entry is read by the project's existing statically linked archive
backend, never via a localhost web server. The routing policy is incremental
direct archive stream first, bounded RAM fallback second, managed disk cache
only last. Shared adapter preferences, passwords, title templates and logging
are reused. No extra archive-related runtime DLL is introduced.

## Real VLC 3.0.24 x64 verification

- ZIP, non-solid 7z/LZMA2 and single-volume RAR5 each passed 264,600-frame
  44.1 kHz WAV decoding with exact original PCM byte parity, stream=1 and
  no managed disk cache. See test_real_vlc_native.py.
- Seven synthetic audio codecs (MP3, OGG, FLAC, WAV, WMA, M4A, AAC) were encoded once and played both directly and from ZIP, non-solid 7z/LZMA2 and single-volume RAR5 in the real VLC 3.0.24 x64 host. All 21 source-vs-native-archive decoded-PCM samples matched byte-for-byte, including the lossy codecs compared against their *same compressed source files*. All 21 used ROUTE_STREAM, with zero RAM extraction, managed disk cache and HTTP. This uses the silent audio-file sink, not speakers. The test reads actual archive member order (7z can reorder entries): `test_real_vlc_seven_codecs.py`.
- Two WAV tracks from each ZIP/7z/RAR archive played via the generated
  native M3U8, recording two access opens, two closes and two stream routes,
  with no HTTP. See test_real_vlc_playlist.py.
- Four fresh synthetic encrypted WAV archive layouts passed real VLC 3.0.24 Windows credentials: 7z AES+LZMA2 data encryption, 7z encrypted headers, RAR5 encrypted data, and RAR5 encrypted headers. One prompt plus Save=No and exact 176,400-frame PCM parity in each, no managed disk cache, configured password list hash unchanged. Native 7z decrypts via `ROUTE_STREAM`; encrypted RAR5 bypasses libarchive's unsupported encrypted-data stream and immediately uses bounded `ROUTE_MEMORY`, avoiding a previous duplicate prompt. Cancel and incorrect-password cases pass fail-closed in all four layouts (no decoded audio or cache). GUI regression: `test_real_vlc_password.py [--layout=7z|7z-hidden|rar|rar-headers] [--cancel|--wrong]` (requires pywin32 and pywinauto). This certifies direct `ast://` player tracks only, not the separate playlist-builder prompt lifecycle.
- **Real password Save=Yes verification:** `test_real_vlc_password_save.py` uses the
  real libVLC 3.0.24 x64 SDK with the production access DLL. It independently
  tests data- and header-encrypted 7z/RAR5 tracks in three modes: approved
  correct password (one saved plaintext entry, exact 132,300-frame WAV PCM),
  approved incorrect password (no saved entry and no decoded audio), and
  cancellation (no saved entry or decoded audio). All **12 cases** pass,
  including a consecutive one-process test. A process-local Windows registry
  override maps HKCU onto a disposable QA subtree for each case; the original
  user's settings and configured password file are untouched, its digest
  remains unchanged, and temporary QA keys are removed. Run from the repo
  root with `py -3.13 -B vlc_archive_stream/test_real_vlc_password_save.py`;
  optional single case: `...py rar-headers wrong`. This verifies direct
  native `ast://` playback; the actual PowerShell launcher also passes Save=Yes
  separately through the new v1.2 child-process password-list override.
- **21/21 native playback seek/resume and PCM-content checks:** `test_real_vlc_seven_codec_seek.py` tests ZIP, non-solid 7z and single-volume RAR5 for WAV/MP3/OGG/FLAC/WMA/M4A/AAC using real libVLC 3.0.24 x64 player API and a paced, silent libVLC decoded-audio callback. Each jumps near 95 seconds of a synthetic 120-second three-tone track, resumes, and emits the expected final 880 Hz 16-bit PCM rather than pre-seek audio. All cases stream directly, with zero managed RAM extraction, disk cache or HTTP. MP3/WAV/FLAC/WMA/M4A/AAC use `libvlc_media_player_set_time`; OGG uses `libvlc_media_player_set_position`. On the same source .ogg, the time API ended playback but position API succeeded, so the regression tests both modes explicitly (`--baseline --only ogg --ogg-time` reproduces the failure). Many small/streamable fixtures use VLC read-ahead and do not invoke the plugin's ACCESS_SEEK callback; the separate long-distance 7z/WAV test below confirms that path. No GUI click or speakers are involved.
- **Real Qt GUI seek bar, 21/21:** `test_real_vlc_gui_seek.py` ran actual VLC 3.0.24 x64 player windows for all WAV/MP3/OGG/FLAC/WMA/M4A/AAC tracks inside ZIP, non-solid 7z and single-volume RAR5. It clicked the real accessible seek slider near 70%, verified the visible elapsed-time label jumped from ~00:04 to ~01:24–01:27, and confirmed time continued advancing. All cases logged `ACCESS_OPEN_OK` and `ROUTE_STREAM`, with no HTTP, memory extraction or disk cache. The suite disabled first-run metadata-network/update boxes, used a disposable profile and removed only its own VLC processes. The test uses `--aout=dummy` (no speaker), not PCM-content analysis; separate SDK decoded-PCM and long-distance ACCESS_SEEK tests remain necessary. Run `py -3.13 -B vlc_archive_stream/test_real_vlc_gui_seek.py`; filter with `--kind 7z --only ogg`.
- **Real GUI custom album titles (ZIP/7z/RAR5):** `test_real_vlc_gui_playlist_titles.py` verifies two consecutive tracks appear in the VLC Qt window as `1. track1.wav` and `2. track2.wav` with the shared `<#>. <filename>` title preset. Each actual native in-process album used two direct streams, no HTTP/RAM/disk fallback and a disposable isolated profile with metadata network/update disabled. This is generated-title display, **not** live repaint after changes to already queued tracks.
- Native seeking was confirmed using the actual libVLC player API and a
  20-minute 7z WAV, beyond VLC's read-ahead buffer. VLC resumed at ~14:10;
  our access module logged ACCESS_SEEK offset=74970044; the test audio output
  ended in the intended later PCM segment. See test_real_vlc_seek.py.
- A two-track solid 7z with known PCM values passed exact host decoded output
  parity for both members, with two bounded RAM routes, zero disk-cache routes.
  See test_real_vlc_solid.py.
- Real native in-process ZIP album: two WAV entries, two access opens and
  direct streams, no HTTP (`test_real_vlc_inprocess_playlist.py`).
- Real in-process encrypted-filename 7z and RAR5 albums: two tracks each,
  **one credential prompt and Save=No per entire album**, exact final-track
  PCM in silent-file outputs, 7z stream=2 or RAR5 RAM=2, cache=0
  (`test_real_vlc_inprocess_encrypted.py`).
- The **actual PowerShell launcher** loaded the DLL from outside VLC and
  played two silent ZIP tracks; additional actual-launcher tests exercised
  the two header-encrypted 7z/RAR5 two-track albums with one prompt each,
  zero cache, configured password list unchanged and disposable GUI cleanup
  (`test_real_vlc_launcher_inprocess.py`,
  `test_real_vlc_launcher_encrypted.py`).
- **Native v1.3 playback settings:** The existing PowerShell launcher accepts
  `-MemoryMB` (1–512), `-CacheLimitMB` (0–20480; zero disables disk), and
  `-TitleTemplate` (one of the six shared presets) without HTTP when the
  v1.3 DLL is available. It passes each value only to the VLC child process
  and restores any prior environment value; HKCU is not modified. Example:
  `powershell -File launch-archive.ps1 -Player VLC -PlayerPath "D:\Apps\VLC\vlc.exe" -Archive "D:\Audio\album.7z" -MemoryMB 64 -CacheLimitMB 512 -TitleTemplate '<#>. <filename>'`.
  Actual real-host QA in `test_real_vlc_launcher_preferences.py` passed
  two correct ZIP album `#EXTINF` titles, two direct ZIP streams and zero
  HTTP/cache; a 1 MiB RAM cap with cache=0 refused >1 MiB solid 7z entries
  without creating a disk file, whereas cache=5 MiB gave byte-identical
  managed disk fallback. Live repaint of existing VLC rows remains untested.
- **Actual PowerShell launcher Save=Yes:** v1.2 accepts a child-process-only
  password-list path through the existing `-PasswordList` option without HTTP:
  `powershell -File launch-archive.ps1 -Player VLC -PlayerPath "D:\Apps\VLC\vlc.exe" -Archive "D:\Audio\album.7z" -PasswordList "D:\Audio\passwords\keeps.txt"`.
  The password-list parent folder must exist; the file can be created upon a
  verified password save. The launcher temporarily sets the environment
  variable `ARCHIVE_STREAM_VLC_PASSWORD_LIST` only for the VLC child and
  restores its previous value afterward, never changing HKCU. Native DLL
  v1.1 and earlier retain HTTP fallback for `-PasswordList`. Real GUI regression
  `test_real_vlc_launcher_password_save.py` passes **6/6**: encrypted-header
  7z/RAR5 × Save=Yes (exactly one verified list entry), wrong password+Yes
  (zero writes) and cancellation (zero writes). Both successful layouts
  opened two tracks without HTTP or managed disk; an inherited ambient
  environment password-list decoy and the user's configured list stayed
  unchanged. Two extra GUI Save=Yes cases also passed a Unicode password-list
  directory for encrypted-header 7z and RAR5, selected with the QA-only
  `AST_VLC_QA_UNICODE_PASSWORD_LIST=1` environment flag.
  Saved passwords are **plaintext** and require explicit consent.
- The launcher regression checks DLL v1.1+ native album support and v1.2
  native `-PasswordList` detection, older unversioned DLL
  fallback to its original 21-entry native playlist builder, native 1.2
  without that builder, missing-DLL and explicit HTTP/CLI overrides
  (`test_launcher_native.py`).

Sound-speaker output and additional codecs/encryption layouts are NOT certified beyond the seven unencrypted codec × three archive combinations above and separately tested encrypted WAV layouts. The four
specific direct native encrypted 7z/RAR5 layouts passed correct password with
Save=No, Cancel and wrong-password rejection; direct native Save=Yes passes all four layouts. The production v1.2 launcher now passes six separate Save=Yes/wrong+Yes/Cancel encrypted-header 7z/RAR5 album tests.
For the versioned v1.1 module, the native album is enumerated **inside VLC**,
so its verified password remains available for subsequent track opens.
Only older unversioned DLLs use the separate playlist builder and may need
another encrypted-header prompt. All PCM tests use a
silent audio-file sink and disposable isolated VLC/profile folders.

## Build

The official VLC 3 SDK headers are NOT bundled or copied into the project.
Obtain them from https://github.com/videolan/vlc-3.0. DEV's external SDK source
is at D:\Temp\AST-VLC-3.0-SDK (Git revision e30973a).

Use Visual Studio/MSBuild (v145 on DEV):

    $msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild '.\vlc_archive_stream\vlc_archive_stream.vcxproj' /m:1 /p:Configuration=Release /p:Platform=x64 /p:VlcSdkRoot='D:\Temp\AST-VLC-3.0-SDK'
    & $msbuild '.\vlc_archive_stream\vlc_native_playlist.vcxproj' /m:1 /p:Configuration=Release /p:Platform=x64
    & '.\vlc_archive_stream\package-native-vlc.ps1'

The packaged output is vlc_archive_stream\Release\ArchiveStreamPlayer-VLC3-x64.zip.
The access module uses VLC's 3_0_0f plugin ABI and Windows DLL FileVersion **1.3.0.0** for native album and per-launch password-list/memory/cache/title overrides (album listing introduced in v1.1, native password-list in v1.2). VLC 4, x86 VLC and other
untested builds use the HTTP bridge fallback, not this x64 DLL.

## Install

The package may be extracted to **any writable folder**, with no changes to the VLC installation. The launcher temporarily sets VLC_PLUGIN_PATH to the bundled external module directory for its VLC child process, then restores the caller's environment. This route passed real VLC 3.0.24 exact PCM output without a DLL in the player's plugins folder (test_real_vlc_plugin_path.py). The package contains:

- plugins\access\libarchive_stream_plugin.dll: bundled DLL; the launcher can load it from this external location without copying it into VLC.
- vlc_native_playlist.exe: optional compatibility builder for older DLLs;
  versioned v1.1+ doesn't require it.
- launch-archive.ps1 and configure-adapters.ps1: launcher/settings.
- x64\Release\archive_player_bridge.exe: existing HTTP compatibility route.
- INSTALL.txt and the project's license/third-party notices.

Example after copying, specifying the correct VLC installation:

    & '.\launch-archive.ps1' -Player VLC -PlayerPath 'D:\Apps\VLC\vlc.exe' -Archive 'D:\Audio\Album.7z'

With a compatible v1.1+ DLL the launcher passes an in-process
`ast://<hex-path>/list/album.m3u8` URL directly to VLC; no HTTP or M3U8
file is produced. Older unversioned DLLs use the included external builder. Explicit -UseHttpBridge always keeps
the prior route. The v1.3 native DLL accepts per-launch -MemoryMB,
-CacheLimitMB, -TitleTemplate and -PasswordList overrides directly without
HTTP; older DLL versions may fall back to the bridge for overrides they do
not support. Shared native settings can be changed through -Configure. Native playlists contain only
source paths, indices and titles, not decoded audio or password values.
Old generated VLC native playlists are removed after seven days.

## Licensing

Original project code is MIT-licensed. The official VLC SDK has LGPL-2.1+
license terms and is never redistributed here. The backend's 7z/ZIP/RAR
libraries retain their upstream licenses. See LICENSE, LICENSING.md and
third_party/README.md in the repository/package. This package does not contain
VLC binaries or copyrighted VLC SDK header files.
