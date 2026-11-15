# VLC native Archive Stream adapter

Optional VLC 3.0 **x64** native input integration. The statically linked
DLL v1.1 implements the `ast://` access scheme, including an **in-process**
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
- Two WAV tracks from each ZIP/7z/RAR archive played via the generated
  native M3U8, recording two access opens, two closes and two stream routes,
  with no HTTP. See test_real_vlc_playlist.py.
- Four fresh synthetic encrypted WAV archive layouts passed real VLC 3.0.24 Windows credentials: 7z AES+LZMA2 data encryption, 7z encrypted headers, RAR5 encrypted data, and RAR5 encrypted headers. One prompt plus Save=No and exact 176,400-frame PCM parity in each, no managed disk cache, configured password list hash unchanged. Native 7z decrypts via `ROUTE_STREAM`; encrypted RAR5 bypasses libarchive's unsupported encrypted-data stream and immediately uses bounded `ROUTE_MEMORY`, avoiding a previous duplicate prompt. Cancel and incorrect-password cases pass fail-closed in all four layouts (no decoded audio or cache). GUI regression: `test_real_vlc_password.py [--layout=7z|7z-hidden|rar|rar-headers] [--cancel|--wrong]` (requires pywin32 and pywinauto). This certifies direct `ast://` player tracks only, not the separate playlist-builder prompt lifecycle.
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
- The launcher regression checks DLL v1.1 detection, older unversioned DLL
  fallback to its original 21-entry native playlist builder, native 1.1
  without that builder, missing-DLL and explicit HTTP/CLI overrides
  (`test_launcher_native.py`).

Sound-speaker output and all codecs/encryption layouts are NOT certified. The four
specific direct native encrypted 7z/RAR5 layouts passed correct password with
Save=No, Cancel and wrong-password rejection; Save=Yes remains untested.
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
The access module uses VLC's 3_0_0f plugin ABI and Windows DLL FileVersion 1.1.0.0 for native album-feature detection. VLC 4, x86 VLC and other
untested builds use the HTTP bridge fallback, not this x64 DLL.

## Install

The package may be extracted to **any writable folder**, with no changes to the VLC installation. The launcher temporarily sets VLC_PLUGIN_PATH to the bundled external module directory for its VLC child process, then restores the caller's environment. This route passed real VLC 3.0.24 exact PCM output without a DLL in the player's plugins folder (test_real_vlc_plugin_path.py). The package contains:

- plugins\access\libarchive_stream_plugin.dll: bundled DLL; the launcher can load it from this external location without copying it into VLC.
- vlc_native_playlist.exe: optional compatibility builder for older DLLs;
  versioned v1.1 doesn't require it.
- launch-archive.ps1 and configure-adapters.ps1: launcher/settings.
- x64\Release\archive_player_bridge.exe: existing HTTP compatibility route.
- INSTALL.txt and the project's license/third-party notices.

Example after copying, specifying the correct VLC installation:

    & '.\launch-archive.ps1' -Player VLC -PlayerPath 'D:\Apps\VLC\vlc.exe' -Archive 'D:\Audio\Album.7z'

With a compatible v1.1 DLL the launcher passes an in-process
`ast://<hex-path>/list/album.m3u8` URL directly to VLC; no HTTP or M3U8
file is produced. Older unversioned DLLs use the included external builder. Explicit -UseHttpBridge always keeps
the prior route. Per-launch -MemoryMB, -CacheLimitMB, -TitleTemplate and
-PasswordList overrides use HTTP to preserve their semantics. Shared native
settings can be changed through -Configure. Native playlists contain only
source paths, indices and titles, not decoded audio or password values.
Old generated VLC native playlists are removed after seven days.

## Licensing

Original project code is MIT-licensed. The official VLC SDK has LGPL-2.1+
license terms and is never redistributed here. The backend's 7z/ZIP/RAR
libraries retain their upstream licenses. See LICENSE, LICENSING.md and
third_party/README.md in the repository/package. This package does not contain
VLC binaries or copyrighted VLC SDK header files.
