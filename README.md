# Archive Stream Player

**Archive Stream Player** is a Windows/Winamp plugin system for browsing and playing audio stored inside ZIP, 7z and RAR archives without requiring the user to extract an entire archive manually. An archive can be expanded into individual virtual Winamp playlist entries; playback selects a streaming, memory or managed-cache path depending on the container and entry.

This repository contains the native C/C++ Winamp plugins, archive backends, Visual Studio projects and bundled third-party source.

## Components

| Project | Output | Role |
| --- | --- | --- |
| `in_archive_stream` | `in_archive_stream.dll` | Winamp input plugin: virtual archive entry playback, metadata and configuration UI |
| `gen_archive_stream` | `gen_archive_stream.dll` | Winamp general-purpose plugin: archive expansion and playlist integration |
| `archive_backend` | `archive_backend.lib` | Shared archive listing, metadata, memory/file extraction, streaming and cache logic |
| `archive_backend_test` | `archive_backend_test.exe` | Command-line listing, extraction and stream diagnostics |
| `libarchive_static` | `libarchive_static.lib` | Static libarchive build used by archive backends |
| `unrar_static` | `unrar_static.lib` | Static RARLAB UnRAR integration |
| `zlib_static` | `zlib_static.lib` | Static zlib dependency |
| `shared` | Source files | Shared logging and Winamp adapter protocol definitions |
| `third_party` | Source files | Bundled upstream libraries, Winamp headers and codec components |

The full multi-project solution is `in_archive_stream/in_archive_stream.slnx`. The project references express the library dependencies.

## Player-independent core and adapters (refactor in progress)

The long-term design is a reusable archive-audio core with independently built player adapters for Winamp and potentially foobar2000, MediaMonkey and other players. **Only the Winamp adapter is implemented today.** Do not assume a Winamp binary will load in another player without compatibility testing.

The first refactor step moved Winamp-specific playlist messaging from the former `shared/zip_audio.cpp` into `in_archive_stream/winamp_playlist_bridge.cpp`. Both Winamp DLLs now use `shared/winamp_bridge_protocol.h` for their private window-message protocol; the low-level archive backend no longer declares those Winamp message constants. Obsolete ZIP wrappers and an unused duplicate duration-scanner were removed.

The `archive_backend` public functions remain the shared listing, extraction, and 7z/RAR stream boundary. Host adapters can now call `ArchiveSetDataDirectoryName()` during initialization to select an isolated folder beneath `%LOCALAPPDATA%`. The generic default is `ArchiveStreamPlayer`; **both Winamp plugins explicitly select `WinampArchiveStream` before loading configuration**, preserving existing settings, password lists and cache paths. Only single-component folder names containing letters, digits, underscores or hyphens are accepted. The setter must be called before password/config/cache use, not during playback.

Windows Media Foundation startup is now headless and reports HRESULT failure status to the caller. The Winamp input adapter displays user-facing errors; background metadata scanning never opens a startup dialog. Startup and shutdown are serialized to avoid concurrent first-use initialization. Remaining work includes adding ZIP streaming to the generic backend and improving structured errors, cancellation and stream lifetimes. Preserve the virtual Winamp playlist format throughout.


## Archive and audio support

- **ZIP:** ordinary and first-volume `.zip.001` split archives, with libarchive-backed access. Streaming and cache choices vary by entry.
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

The backend test utility can be used independently of Winamp:

```powershell
.\archive_backend_test\Release\archive_backend_test.exe "D:\Audio\Album.7z"
.\archive_backend_test\Release\archive_backend_test.exe "D:\Audio\Album.rar" 0
.\archive_backend_test\Release\archive_backend_test.exe "D:\Audio\Album.7z" 0 stream
.\archive_backend_test\Release\archive_backend_test.exe "D:\Audio\Album.rar" 0 file "D:\Temp\extracted-track.flac"
```

The console tests enumerate entries, test memory extraction, exercise eligible archive streams and can extract RAR/7z entries to files. They **do not** replace a Winamp playback regression test; support depends on the specific entry, compression method and archive layout.

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
