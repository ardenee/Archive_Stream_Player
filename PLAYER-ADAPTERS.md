# Additional player adapters: XMPlay, AIMP, and WACUP

DEV implementation and verification: 13 November 2026.

These integrations reuse the same statically linked ZIP, 7z, RAR, AES, multipart and extraction backend as Winamp and foobar2000. No new archive-related runtime DLLs or external extraction executables are introduced. The official AIMP and XMPlay SDK files are **not bundled** and are needed only when compiling their respective adapters.

## XMPlay 4.1 — native x86 adapter

Download the official XMPlay plugin SDK from https://www.un4seen.com/files/xmp-sdk.zip and extract it outside the repository. Its root contains xmparc.h and xmpfunc.h. The DEV SDK lives at D:\Temp\ArchiveStreamPlayerAdapters\xmplay-sdk.

~~~powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$sdk = 'D:\Temp\ArchiveStreamPlayerAdapters\xmplay-sdk'
& $msbuild 'xmp_archive_stream\xmp_archive_stream.vcxproj' /m /p:Configuration=Release /p:Platform=Win32 "/p:XmplaySdkRoot=$sdk"
& $msbuild 'xmp_archive_stream\xmplay_adapter_test.vcxproj' /m /p:Configuration=Release /p:Platform=Win32 "/p:XmplaySdkRoot=$sdk"
~~~

Copy xmp_archive_stream\Release\xmp-archive-stream.dll into the XMPlay installation directory. XMPlay detects its plugins by filename; the **xmp- prefix is mandatory**. This plugin uses XMPlay's native XMPARC archive interface rather than a Winamp plugin wrapper. It lists supported audio members, exposes encrypted and multipart ZIP/7z/RAR archive support, and handles file names in UTF-8.

XMPARC.DecompressFile takes the requested maximum decoded-byte length, which may be shorter than the entry; the adapter supports such partial probes. It prefers backend decoded-byte streaming, otherwise extracts entirely into memory when at most 50 MiB, otherwise uses a temporary file. Temporary files are removed automatically. The return value is always an **XMPlay-owned decoded byte block**; XMPARC does not provide a way to transfer an ongoing archive stream directly to the host decoder. This is not full zero-buffer streaming at the player level.

The DEV x86 host contract test passed WAV full-entry and first-64-byte parity for ZIP, 7z and RAR, and a 61 MiB solid 7z fixture passed through cache fallback. A real XMPlay 4.1 instance loaded the DLL, enumerated four tracks from a solid 7z, routed solid members to memory and the nonsolid member through the stream. Audible output is not independently certified.

## AIMP 5.40 — native x86 and x64 adapters

Obtain the official AIMP v5.40 SDK at https://aimp.su/?do=download&os=desktop&cat=sdk and extract it outside the repository. SDK must have Sources\Cpp\apiPlugin.h. On DEV it is D:\Temp\ArchiveStreamPlayerAdapters\aimp-sdk.

~~~powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$sdk = 'D:\Temp\ArchiveStreamPlayerAdapters\aimp-sdk'
& $msbuild 'aimp_archive_stream\aimp_archive_stream.vcxproj' /m /p:Configuration=Release /p:Platform=x64 "/p:AimpSdkRoot=$sdk"
& $msbuild 'aimp_archive_stream\aimp_adapter_test.vcxproj' /m /p:Configuration=Release /p:Platform=x64 "/p:AimpSdkRoot=$sdk"
# Repeat with /p:Platform=Win32 for 32-bit AIMP.
~~~

Copy the matching-architecture aimp_archive_stream.dll into an AIMP plugin subfolder, for example AIMP\Plugins\aimp_archive_stream\aimp_archive_stream.dll. AIMP registers the archive extensions *.zip, *.7z, *.rar, *.zip.001 and *.7z.001 via its FileFormat service; its FileExpander generates independently seekable virtual tracks. The AIMP virtual URI follows the SDK's archive:index/path format. Audio decoding remains AIMP's job: the adapter returns **decoded archive-entry bytes** through IAIMPStream, not PCM.

Each virtual stream uses **stream first → in-memory extraction (50 MiB) → temporary file cache**. Solid 7z and RAR bypass direct streaming; temp files are automatically removed when the stream is released. The official SDK provides the COM interfaces used by the independent host contract test.

AIMP x86 and x64 mock-host tests verified registration of FileFormat and FileExpander, virtual-file URI properties, reads and rewind/end seeking, and a 61 MiB solid 7z cache case. Both actual portable AIMP 5.40 builds accepted archive files in the Add Files dialog and expanded four tracks in a solid 7z. AIMP x64 traced route-to-memory when playback was requested. Audible playback, UI seeking, and interactive encrypted-password prompts are not yet fully verified.

## WACUP (Winamp compatibility)

### WACUP 32-bit — verified on build 1.99.57.25464

Use the existing Winamp Release|Win32 plugin binaries:

- in_archive_stream\Release\in_archive_stream.dll
- gen_archive_stream\Release\gen_archive_stream.dll

Copy them to the WACUP x86 Plugins folder. Portable WACUP blocks third-party plugins by default; enable **Preferences → Advanced → Portable Mode → Allow 3rd-party (non-WACUP and non-native) plug-ins** and restart. WACUP bundles an in_zip.dll that takes over some ZIP/7z routing, creating zip:// playlist records instead of the Archive Stream virtual-file format; **disable its bundled ZIP handler in the test installation** if you want Archive Stream's own list expansion. In WACUP Preferences → Plug-ins → General Purpose, select Archive Stream Playlist Expander and click Configure on an archive playlist item.

WACUP's WM_COPYDATA enqueue call may return zero even when the track was successfully appended. The Winamp general adapter now verifies the actual playlist count increase, and it appends all replacements before removing the original archive entry so failed expansion leaves the original intact.

An isolated WACUP x86 installation loaded both DLLs and expanded a four-entry solid 7z. Winamp-compatible IPC then confirmed that playback time advanced during the WAV track and the player transitioned to FLAC. Logs confirmed solid 7z was routed to memory.

### WACUP 64-bit — binaries compile; the host does not load them

The Winamp plugin Visual Studio projects now also build Release|x64 against the matching static backend libraries, without accidentally copying x64 binaries into the installed 32-bit Winamp directory. **However, that does not mean WACUP x64 supports them**. A separate portable test based on installed WACUP x64 1.99.57.25464 did not load either x64 DLL, even after enabling portable third-party plugins. WACUP's published x64 preview documentation warns of limited third-party Winamp plugin compatibility: https://getwacup.com/preview_x64/index_prior_23410.php

Do **not** replace the installed 64-bit WACUP's plugins with the x64 builds or assume they are accepted. WACUP x86 is the supported compatibility path at this stage.

## Diagnostics and runtime separation

Set the environment variable ARCHIVE_STREAM_ADAPTER_DEBUG=1 **before launching** AIMP or XMPlay to enable trace logs under:

- %LOCALAPPDATA%\AIMPArchiveStream\aimp_archive_stream.log
- %LOCALAPPDATA%\XMPlayArchiveStream\xmplay_archive_stream.log

The adapters use separate player data/password folders. Winamp and WACUP deliberately share WinampArchiveStream configuration. Passwords are not written into logs.

All DEV SDKs, portable test players, compressed WAV fixtures and generated DLLs stay outside the Git repository. No installed WACUP or foobar2000 configuration was overwritten by these tests. Keep testing encryption, Unicode/split archives, cancellation, audible output and player-level seek transitions before declaring comprehensive playback certification.
