# VLC native Archive Stream adapter

Optional native VLC 3.0 **x64** input integration. The static
libarchive_stream_plugin.dll implements the ast:// access scheme. The
vlc_native_playlist.exe program enumerates audio members of ZIP, 7z and RAR
archives and creates a native M3U8 with one ast:// URL per audio track.

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
- Native-first launcher passed a 21-entry 7z dry run, and explicit HTTP,
  missing-DLL and one-off CLI preference-override compatibility modes. See
  test_launcher_native.py.

Sound-speaker output and all codecs/encryption layouts are NOT certified. The four
specific direct native encrypted 7z/RAR5 layouts passed correct password with
Save=No, Cancel and wrong-password rejection; Save=Yes remains untested.
The native playlist builder is a separate process from the VLC DLL, so its
header-encryption password may be requested again by the player. Credential
handoff for that scenario is an open integration gap. All PCM tests use a
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
The access module uses VLC's 3_0_0f plugin ABI. VLC 4, x86 VLC and other
untested builds use the HTTP bridge fallback, not this x64 DLL.

## Install

The package may be extracted to **any writable folder**, with no changes to the VLC installation. The launcher temporarily sets VLC_PLUGIN_PATH to the bundled external module directory for its VLC child process, then restores the caller's environment. This route passed real VLC 3.0.24 exact PCM output without a DLL in the player's plugins folder (test_real_vlc_plugin_path.py). The package contains:

- plugins\access\libarchive_stream_plugin.dll: bundled DLL; the launcher can load it from this external location without copying it into VLC.
- vlc_native_playlist.exe: place beside launch-archive.ps1.
- launch-archive.ps1 and configure-adapters.ps1: launcher/settings.
- x64\Release\archive_player_bridge.exe: existing HTTP compatibility route.
- INSTALL.txt and the project's license/third-party notices.

Example after copying, specifying the correct VLC installation:

    & '.\launch-archive.ps1' -Player VLC -PlayerPath 'D:\Apps\VLC\vlc.exe' -Archive 'D:\Audio\Album.7z'

If native components are present and compatible, either alongside the launcher/package or under VLC's own plugins directory, the launcher produces a
local ast:// playlist with NO HTTP server. Explicit -UseHttpBridge always keeps
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
