# MediaMonkey 5 / MediaMonkey 2024 — Archive Stream Player

Archive Stream Player now has **native-first playback** for MediaMonkey 5/2024.
MediaMonkey's JavaScript Tools action enumerates ZIP, 7z and RAR archives with
the statically linked archive backend, then queues tiny local `.ast` track
references. The matching x86 Winamp-compatible `in_archive_stream.dll` input
module accepts those references and decodes the compressed archive directly.
No localhost HTTP listener or extracted audio file is needed in this mode.

## Installation

Build both companions from the repo root after compiling the Win32 input DLL
and x64 bridge/listing executable:

```powershell
.\mediamonkey_archive_stream\package-mmip.ps1
.\mediamonkey_archive_stream\package-native-addon.ps1
```

Install both MMIPs through **MediaMonkey → Tools → Addons → Add**:

1. `ArchiveStreamPlayer-MediaMonkey5.mmip` — x86 input decoder registered in
   MediaMonkey's Plugins directory.
2. `ArchiveStreamPlayer-MediaMonkey5-Native.mmip` — Tools menu actions,
   static listing executable and companion hash of the decoder.

Restart MediaMonkey after installation. The add-on checks that the installed
input DLL matches the companion included in its package (SHA-256) before using
native playback. An old, absent, inaccessible or mismatched DLL automatically
retains the previous authenticated loopback HTTP compatibility route. The Tools
menu also has **Add Archive Audio (HTTP compatibility)** as an explicit override.

No Winamp *general-purpose* plugin is installed: MediaMonkey 5 can load Winamp
*input* modules but does not provide Winamp's general-plugin archive expansion.

## Native routing and lifecycle

- The native list-only helper runs `archive_player_bridge.exe --archive <file>
  --native-manifest-dir <session-folder>` and **exits before creating any
  network socket**. It writes one small `entry-<index>.ast` reference per
  supported track and a local `archive-stream-native.m3u8` with shared titles.
- Each reference contains `ARCHIVE_STREAM_NATIVE_TRACK_V1` followed by the
  canonical archive/index/internal-name virtual path. It never contains decoded
  audio, passwords or HTTP tokens. The Winamp-compatible input DLL validates
  the signature, size and archive entry before invoking the existing decoder.
- The decoder retains the shared policy: direct compressed archive stream
  first, bounded RAM when streaming is impossible, managed cache only last.
  Shared settings, logging, password handling and cache controls remain in the
  static backend. Existing archive files and embedded tags are never edited.
- The JavaScript add-on uses `app.player.addTracksAsync` and only accepts local
  native references from the session directory reported by its launcher.
  Previously queued tracks are not rewritten when title preferences change.
- The launcher keeps native references alive while MediaMonkey is running
  (maximum two hours), then deletes its session files. The HTTP compatibility
  bridge retains its previous session handling.

## Actual host verification — 15 November 2026

- ZIP, non-solid 7z and RAR5 each passed **two native MP3/OGG tracks** with real
  MediaMonkey 2024 playing state, SDK seek to ~90 seconds and continuing
  positions, direct archive streams and no HTTP/disk cache. Each used a unique
  portable-profile clone, leaving normal MediaMonkey database and preferences
  unchanged. Test: `test_real_native_manifest_album.py`.
- The complete **packaged** add-on with matching staged input DLL passed
  native ZIP MP3+OGG queue, title visibility, actual mouse-click seek and
  resumed playback. The individual ZIP WAV and AAC native GUI tests also
  passed. A combined seven-codec native ZIP test queued and played all seven;
  five of seven combined GUI seeks passed, while WAV/AAC had intermittent
  continuation-display failures requiring further multi-entry investigation.
  Test: `test_native_portable_codecs.py`, using `AST_MM5_QA_NATIVE=1`.
- `test_native_manifest_generator.py` validates 2-track ZIP/7z/RAR manifest
  files with zero socket or decoded-audio generation.
- `test_native_actions.js` checks native-path scope, malformed/untrusted
  URLs, queue acknowledgement, HTTP compatibility, user cancellation and
  shared settings.

**Encrypted native 7z (15 November 2026):** Actual MediaMonkey 2024 showed a native archive password dialog hosted by `MediaMonkeyEngine.exe`. After entering a synthetic password, AAC played through direct AES+LZMA2 streaming, not HTTP or disk cache. The shared 7z reader now rejects an initial empty AES password during open, with full Win32/x64 layout regression coverage. **Native encrypted AAC GUI seek/resume is still open:** the host requested a ~156-second seek but did not resume advancing afterward. Use the explicit HTTP compatibility menu action for this tested combination; that route has independent real-host seek evidence. QA: `test_native_portable_codecs.py` with `AST_MM5_QA_NATIVE=1` and `AST_MM5_QA_AES=accept`, plus `archive_backend_test/test_7z_encrypted_layouts.py`.

**Still unverified:** all seven codecs across every archive type in native
mode, all encrypted/multipart layouts through the new native host path,
automatic repaint of already-queued native titles, audible speakers and
full PCM-content parity of actual player output. Earlier encryption, title
refresh and seven-codec HTTP integration tests remain valid for the explicit
or automatically selected compatibility route.
