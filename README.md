# Archive Stream Player

**Archive Stream Player** is a shared Windows archive-audio backend with Winamp plugins for browsing and playing audio stored inside ZIP, 7z and RAR archives without requiring the user to extract an entire archive manually. An archive can be expanded into individual virtual Winamp playlist entries; playback selects a streaming, memory or managed-cache path depending on the container and entry.

This repository contains the native C/C++ Winamp plugins, archive backends, Visual Studio projects and bundled third-party source.

## Components

| Project | Output | Role |
| --- | --- | --- |
| `in_archive_stream` | `in_archive_stream.dll` | Winamp input plugin: virtual archive entry playback, metadata and configuration UI |
| `gen_archive_stream` | `gen_archive_stream.dll` | Winamp general-purpose plugin: archive expansion and playlist integration |
| `foo_archive_stream` | `foo_archive_stream.dll` | Experimental foobar2000 File-menu archive inspector (no decoder/playback yet) |
| `archive_backend` | `archive_backend.lib` | Shared archive listing, metadata, memory/file extraction, streaming and cache logic |
| `archive_backend_test` | `archive_backend_test.exe` | Command-line listing, extraction and stream diagnostics |
| `libarchive_static` | `libarchive_static.lib` | Static libarchive build used by archive backends |
| `unrar_static` | `unrar_static.lib` | Static RARLAB UnRAR integration |
| `zlib_static` | `zlib_static.lib` | Static zlib dependency |
| `shared` | Source files | Shared logging and Winamp adapter protocol definitions |
| `third_party` | Source files | Bundled upstream libraries, Winamp headers and codec components |

The full multi-project solution is `in_archive_stream/in_archive_stream.slnx`. The project references express the library dependencies.

## Player-independent core and adapters (refactor in progress)

The long-term design is a reusable archive-audio core with independently built player adapters for Winamp and potentially foobar2000, MediaMonkey and other players. **Only Winamp has a working playback adapter.** The foobar2000 component provides inspection and, on this development branch, native Preferences pages; playback and playlist integration remain unimplemented. Winamp binaries cannot be loaded by foobar2000.

The first refactor step moved Winamp-specific playlist messaging from the former `shared/zip_audio.cpp` into `in_archive_stream/winamp_playlist_bridge.cpp`. Both Winamp DLLs now use `shared/winamp_bridge_protocol.h` for their private window-message protocol; the low-level archive backend no longer declares those Winamp message constants. Obsolete ZIP wrappers and an unused duplicate duration-scanner were removed.

The `archive_backend` public functions remain the shared listing, extraction, and ZIP/7z/RAR stream boundary. Host adapters can now call `ArchiveSetDataDirectoryName()` during initialization to select an isolated folder beneath `%LOCALAPPDATA%`. The generic default is `ArchiveStreamPlayer`; **both Winamp plugins explicitly select `WinampArchiveStream` before loading configuration**, preserving existing settings, password lists and cache paths. Only single-component folder names containing letters, digits, underscores or hyphens are accepted. The setter must be called before password/config/cache use, not during playback.

Windows Media Foundation startup is now headless and reports HRESULT failure status to the caller. The Winamp input adapter displays user-facing errors; background metadata scanning never opens a startup dialog. Startup and shutdown are serialized to avoid concurrent first-use initialization. Generic ZIP streaming now reuses the libarchive implementation already used by Winamp, without switching Winamp's playback route. The shared stream state distinguishes `Ready`, normal `EndOfEntry`, `Error`, `Cancelled` and `NotOpen`. Premature EOF is reported as a read failure. Optional caller-owned `ArchiveStreamCancellationToken` supports cooperative cancellation of generic ZIP/7z/RAR reads and seeks. The legacy Winamp wrapper now rejects attempts to reopen an active context instead of resetting and leaking its handle. `ArchiveReadEntryStreamDetailed()` returns a typed read result and ZIP/RAR backend error text. `ArchiveOpenEntryStreamDetailed()` and `ArchiveSeekEntryStreamDetailed()` add typed lifecycle/error responses without changing the legacy Winamp functions. The move-only `ArchiveStreamOwner` closes streams and exposes all three detailed operations for new adapters. Active Winamp playback cancellation wiring and further stream lifetime work remain future tasks. Preserve the virtual Winamp playlist format throughout.


## Archive and audio support

- **ZIP:** ordinary and first-volume `.zip.001` split archives, with libarchive-backed access. The generic backend exposes ZIP decoded-byte streaming, backward/forward seeking and extract-to-file. ZIP archive and split-volume paths are opened through Windows UTF-16 APIs, including when the archive filename contains Unicode characters. The Winamp ZIP playback route remains unchanged during the adapter migration.
- **7z:** normal and first-volume `.7z.001` split archives using the 7-Zip/LZMA SDK. Non-solid Copy/LZMA2 entries have a streaming route; solid entries can require managed cache.
- **RAR:** RARLAB UnRAR and libarchive-based routines. Non-solid streaming and fallback paths exist; solid entries may need earlier decompression history. Multipart/header-metadata and password-list code exists, but compatibility depends on the archive and needs regression coverage.

Native audio decoding targets **MP3, WAV, FLAC and OGG/Vorbis**. A Windows **Media Foundation** path is used for **M4A**, and for **AAC/WMA** when Windows provides the required codecs. The plugin does not depend on separate Winamp input plugins for Media Foundation decoding.

A virtual playlist item has the form:

```text
C:\Music\Album.7z.archive_stream|3|Disc1\Track.flac
```

The `in_archive_stream` plugin recognizes the virtual entry; the `gen_archive_stream` plugin provides playlist expansion.

## Build on Windows

1. Clone the repository with its `third_party` files, keeping all project directories as siblings.
2. Install **Visual Studio 2026**, the C++ desktop toolchain (MSVC **v145**) and a Windows 10/11 SDK.
3. Open `in_archive_stream/in_archive_stream.slnx`.
4. Select **Release** and **x86** (the 32-bit Winamp target) and build the solution.
5. Check `in_archive_stream/Release/in_archive_stream.dll` and `gen_archive_stream/Release/gen_archive_stream.dll`.

The existing DLL projects have post-build commands that copy the DLLs to `C:\Program Files (x86)\Winamp\Plugins`. Ensure Winamp is installed at that location, or adjust those post-build paths before building. The project include/library paths are relative to each `.vcxproj` rather than to the original developer's Windows profile.

### foobar2000 inspection component (separate build)

Download the official foobar2000 SDK **2026-10-01** from https://www.foobar2000.org/SDK and extract it outside this repository. The SDK root contains `foobar2000/`, `pfc/`, `libPPUI/` and its license. On DEV it is stored at `D:\Temp\ArchiveStreamFoobarSDK`, not committed to GitHub.

Build **Release|Win32** (32-bit) or **Release|x64** (64-bit, v145) directly from `foo_archive_stream/foo_archive_stream.vcxproj`, independently of the Winamp solution:

```powershell
$env:FOOBAR_SDK_ROOT = 'D:\Temp\ArchiveStreamFoobarSDK'
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  'foo_archive_stream\foo_archive_stream.vcxproj' /t:Build `
  /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145 `
  /p:FoobarSdkRoot=$env:FOOBAR_SDK_ROOT /m:2 /v:minimal
```

To build native x64 instead, change the command above to `/p:Platform=x64`. This builds the x64 backend, libarchive, UnRAR and zlib libraries independently of the Winamp/x86 output. The 64-bit component is `foo_archive_stream/x64/Release/foo_archive_stream.dll`; the 64-bit console tests are at `archive_backend_test/x64/Release/archive_backend_test.exe`.

The 32-bit component remains at `foo_archive_stream/Release/foo_archive_stream.dll`. This is an independent **foobar2000 SDK component**, not a Winamp DLL. It registers **File → Archive Stream: Inspect archive...** for ZIP/7z/RAR and adds two native Preferences pages: **Tools → Archive Stream** (buffer, cache size, age/size cleanup, cache-folder view/clear, password-list path, debug-log settings, and Open Log Folder) and **Tools → Archive Stream → Title Display** (prefix, postfix, template, and all six Winamp template presets). Values are stored in foobar2000's own configuration and do not modify Winamp's INI; backend cache/password/log paths use `%LOCALAPPDATA%\FoobarArchiveStream` by default. Buffer/title options are stored for future playback but are not yet active. No decoder or virtual playlist entry registration exists. The Preferences implementation and backend regressions **pass Windows Release/Win32 CI** against the official foobar2000 SDK 2026-10-01. Runtime validation on 2026-11-12 loaded the DLL in an **isolated 32-bit portable foobar2000 2.26.2** installation on DEV and confirmed both Preferences pages, six title presets, Apply, persistence across a full player restart, Reset-preview and Cancel semantics. Native x64 builds are now supported. On 2026-11-12, the x64 DLL was loaded in a **separate 64-bit portable foobar2000 2.26.2**; its Preferences registration, six presets, Apply, Cancel, Reset, and full restart persistence passed. The normally installed 64-bit foobar2000 remains unmodified; audio playback is not implemented. Audio playback is not implemented.

Both Release|Win32 and Release|x64 configurations now compile with independent architecture-specific backend and static libraries. DEV's x64 console regressions passed on ZIP, 7z, RAR, split ZIP, and Unicode ZIP fixtures. Archive inspection currently runs on the UI thread; production support must move long scans into cancellable background work. foobar2000 Preferences use host-native Apply/Reset rather than the Winamp dialog. Native password/log path editing still inherits the backend's legacy ANSI file I/O limitations. The foobar2000 SDK itself remains outside version control.


The backend test utility can be used independently of Winamp:

```powershell
.\archive_backend_test\Release\archive_backend_test.exe "D:\Audio\Album.7z"
.\archive_backend_test\Release\archive_backend_test.exe "D:\Audio\Album.rar" 0
.\archive_backend_test\Release\archive_backend_test.exe "D:\Audio\Album.7z" 0 stream
.\archive_backend_test\Release\archive_backend_test.exe "D:\Audio\Album.rar" 0 file "D:\Temp\extracted-track.flac"
```

The console tests enumerate entries, test memory extraction, exercise eligible archive streams and can extract ZIP/RAR/7z entries to files. Run `archive_backend_test.exe canceltest "D:\Audio\Album.zip"` and `archive_backend_test.exe legacytest "D:\Audio\Album.zip"` to test cooperative cancellation and legacy handle ownership. Those two commands also accept supported 7z and RAR fixtures. Run `archive_backend_test.exe ziptest "D:\Audio\Album.zip"` to check the generic ZIP reader against byte-for-byte memory extraction and verify seeking, EOF, reopening, and invalid-index handling. The `ziptest` parity command is intended for small test fixtures (maximum 1 MiB per file); this is a test-only limit, not a streaming limit. They **do not** replace a Winamp playback regression test; support depends on the specific entry, compression method and archive layout.

## Stream cancellation and ownership

`ArchiveOpenEntryStream(archivePath, index, stream, error, &token)` accepts an **optional** caller-owned `ArchiveStreamCancellationToken`. A worker or UI thread can call `token.RequestCancellation()`; the read/seek owner detects it via an atomic signal and sets `stream.readStatus` to `Cancelled`. The generic API and the ZIP/RAR/7z decoded-discard seek loops check cancellation between operations. Existing Winamp callers omit the optional parameter, so their playback and stop routing is not changed by this batch.

**Lifetime contract:** The token must outlive the stream, including any in-flight I/O. Do not `Reset()` a token while its stream is open. A cancellation request is not a concurrent close: the owner must wait until I/O returns and then call `ArchiveCloseEntryStream()`. Stream read/seek/close methods are **not reentrant** or safe to use simultaneously on the same stream. Cancellation may not interrupt an individual blocking decoder call, password dialog or archive-open operation immediately. The generic stream handle remains caller-owned; avoid copying its raw handle into another owning instance.

The legacy `ArchiveStreamContext` now also refuses a second open or validated-open while its ZIP or backend handle remains active, protecting Winamp's existing stream ownership. Its existing ZIP path is still in use until live playback regression tests justify retiring it.

New player adapters should use `archive_backend/archive_stream_owner.h`. `ArchiveStreamOwner` is noncopyable, move-only, and closes its handle on destruction. Its `OpenDetailed()`, `ReadDetailed()` and `SeekDetailed()` methods expose typed `ArchiveStreamErrorKind` results and diagnostic messages. ZIP/RAR read errors are preserved where available; backend seek failures currently produce a general category with details in logs. Cooperative cancellation checks occur between decoder operations, not inside blocking library calls. The existing Winamp byte-count/bool API remains compatible. Run `archive_backend_test.exe diagtest path.zip` for open/seek status regressions.

## Cache, metadata and logging

The plugin prefers direct streaming when possible, uses memory for appropriate entries and falls back to a managed extraction cache when streaming is unsafe or unavailable. The configuration UI exposes cache controls; runtime diagnostics use shared debug logging.

**CRC policy:** CRC metadata must be read from archive headers only, never computed by reading/decompressing every audio entry during listing. If the header does not provide an applicable CRC, represent it as unavailable. Header CRC reporting, particularly for ZIP and multipart RAR variants, should be verified in regression tests before changing cache identity or deduplication behavior.

## Dependencies and licensing

Bundled sources include **7-Zip LZMA SDK**, **RARLAB UnRAR**, **libarchive**, **zlib**, **dr_libs**, **stb_vorbis** and **Winamp plugin SDK headers**. Media Foundation is supplied by Windows. Review each upstream component's notices before redistributing the DLLs. In particular, **UnRAR is extraction-oriented and has restrictions**; it is not a general-purpose permissive RAR compressor license.

## Development status — 12 November 2026

- GitHub and local `main` were compared before this README refresh.
- The complete `Release|x86` solution built successfully on DEV using Visual Studio 2026 / MSVC v145.
- The previously omitted `zlib_static` project was restored to version control, and Visual Studio project references and include/library directories were made independent of the DEV user's absolute checkout path.
- The backend passed local smoke tests using newly generated ZIP, non-solid 7z, solid 7z and RAR archives containing Unicode filenames. Tests covered listing, memory extraction, eligible streaming/seek, and RAR extract-to-file with a byte-for-byte match.
- ZIP entries currently display `HasCRC=NO` in the console listing despite being valid ZIP entries. This is a header-metadata reporting gap to investigate, **not** a reason to calculate CRC by decompressing content.
- Third-party compiler warnings remain; they are not currently treated as evidence of a failed build.
- Initial adapter cleanup: moved Winamp IPC into a dedicated input-plugin bridge and removed unused ZIP audio helper code without changing the existing expansion protocol.
- Added null/empty-path validation to the public archive API and reject reopening an already-live opaque stream rather than leaking its handle. New `archive_backend_test.exe selftest` checks these error paths.
- The stream smoke test now treats a short or oversized read as a failure instead of reporting success with a warning.
- After the initial adapter refactor, the Release/x86 solution rebuilt and ZIP, solid/non-solid 7z and RAR smoke cases passed (listing, memory extraction and eligible streaming/seek).
- The shared backend now supports per-host data directory names with adapter-selected legacy Winamp paths. API tests cover folder-name validation and truncated output paths.
- Media Foundation startup no longer calls Win32 message boxes from the backend; the Winamp playback adapter owns its user-facing error messages, while metadata readers remain headless.
- Generic ZIP streaming and ZIP extract-to-file now dispatch through the existing libarchive-backed ZIP reader. A new `ziptest` command checks decoded-byte parity and stream lifecycle. ZIP source paths were corrected to use UTF-16 Win32 file APIs, including multipart archives with Unicode paths. Stream reads expose `ArchiveStreamReadStatus` to distinguish EOF from premature termination and errors. To avoid changing Winamp's route-selection decisions, ZIP entries' existing `streamCandidate` listing flag was deliberately left unchanged.
- This refactor passed 26 API self-tests; 14 ZIP entry-stream checks across normal, three-part, Unicode-path and empty-entry fixtures; 12 byte-parity ZIP extract-to-file checks; and ZIP/7z/RAR stream smoke tests. A deliberately corrupted ZIP entry failed streaming and extraction, and the file-extraction path removed its partial output. Tests use byte comparisons, not CRC recalculation.
- Added optional cross-thread cancellation tokens to the generic stream API. Tests verified pre-cancelled open rejection, cancelled reads/seeks, cleanup and reopening with a fresh token for ZIP, 7z and RAR. The old Winamp context now refuses double-open and validated double-open, preserves the original stream, supports close/reopen, and safely rejects overlong archive paths.
- Added a move-only RAII `ArchiveStreamOwner` and a detailed read result (`ArchiveReadEntryStreamDetailed`) without changing the Winamp API. The `ownertest` regression covers valid ZIP, 7z, RAR and corrupt ZIP, including decoder error propagation, invalid reads, moves, cancellation and close/reopen.
- Created the independent foobar2000 native inspection component `foo_archive_stream` using the official SDK (2026-10-01). Release|Win32 builds on DEV. The component and its native Preferences pages loaded and passed Apply, Reset, Cancel and restart-persistence tests in isolated portable foobar2000 2.26.2 x86; audio decoding and native x64 support are not implemented.
- Stream API closeout: typed open/read/seek results, cancellation-aware byte accounting, move-only stream ownership, and the `diagtest` regression pass Windows Release/Win32 CI. Native decoder-specific details for some 7z failures still appear in logs; individual blocking decompression calls remain noninterruptible.
- foobar2000 settings parity: native Preferences pages under Tools, including cache sizing/cleanup, password and debug-log paths, cache folder actions, and all six Winamp title presets. Values persist in foobar's host-native configuration, separated from Winamp. CI and DEV x86 builds passed; page registration, settings Apply, Reset/Cancel and restart persistence were runtime-tested in a clean portable x86 player. Future validation: archive inspection UI, long-running list cancellation, and eventual audio playback.
- Actual Winamp playback remains unverified after these changes.

### Next validation and development priorities

1. Run real **Winamp** playback, seeking, playlist expansion and track transitions across MP3/FLAC/OGG/WAV/M4A examples.
2. Build a repeatable test set for **ZIP/7z/RAR**, including normal, split, solid, non-solid, Unicode-named and password-protected cases.
3. Verify **archive-header CRC metadata** and cache identities without introducing CRC calculation passes.
4. Check error handling, concurrency, abort/stop, cache cleanup and recovery across failed or interrupted playback.
5. Update the in-plugin About/help text as implementation support is confirmed; some descriptions predate newer multipart/password paths.

**Current status is a verified compiling baseline, not a declaration that every archive or playback scenario has passed runtime tests.**

## Development workflow

Make and test source changes locally on the DEV Windows machine, then commit and push to this GitHub repository. Keep source, build configuration, tests and this README in sync. Exclude generated Visual Studio output and local caches from commits.
