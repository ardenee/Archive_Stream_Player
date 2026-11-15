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

## Native PCM corruption prevention

Winamp's output ABI (`third_party/winamp/OUT.H`) requires `Out_Module::Write` to
receive no more than **8,192 bytes per call**. The return value **1** means the
data was not accepted and must be retried; **0** means accepted. Ignoring this
contract can produce initially clean audio followed by scrambling as the host
output queue fills.

The shared source `in_archive_stream/pcm_output_writer.h` now enforces this
contract for native MP3, WAV, FLAC, OGG and MF AAC/M4A/WMA decoding. It also
keeps complete interleaved PCM frames and stops sending stale samples on a
seek/stop. Run `in_archive_stream/test_pcm_output_writer.cpp` on both Win32
and x64 to verify exact received bytes under deliberately rejected writes,
bad capacity and interrupted playback. The real Winamp and WACUP x86 PCM silent sinks verified 367,264 and 367,926
source-identical samples respectively before/after seeking. Real MediaMonkey
ZIP and RAR album tests confirm the first MP3 write is ≤8,192 bytes, with
real player seek and progression. A separate post-fix native encrypted RAR5
MP3 test passed password entry, 37→43-second playback, GUI seek to 223 seconds,
resume to 228 seconds, bounded memory/no HTTP/cache, first accepted PCM write
8192 bytes, and unchanged normal MediaMonkey profile. The test-only
`AST_MM5_QA_AUTOPLAY=1` mode can avoid occasional missing accessibility rows
in a fresh disposable MM5 surface without changing production queueing.

**Real audible listening update (15 November 2026):** Following the PCM output
fix, the user confirmed that playback sounded good for the entire observed
session. Treat this as a **passed user listening check for that playback**, not
as automated loopback capture or proof across all codecs, archives and seeks.
The isolated host tests still run muted, so their clock progress alone is not
audible evidence.

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
  the earlier combined sequence had WAV/AAC continuation-display failures,
  but the post-PCM-fix **seven-track native ZIP run passed all 7/7 GUI seeks**
  with 7 actual input decoder seek requests, all 3 MF seeks successful,
  direct STREAM routing for each track, zero HTTP/cache and isolated cleanup.
  Test: `test_native_portable_codecs.py`, using `AST_MM5_QA_NATIVE=1`.
- `test_native_manifest_generator.py` validates 2-track ZIP/7z/RAR manifest
  files with zero socket or decoded-audio generation.
- `test_native_actions.js` checks native-path scope, malformed/untrusted
  URLs, queue acknowledgement, HTTP compatibility, user cancellation and
  shared settings.

**Encrypted native RAR5 (15 November 2026):** First-use password dialogs and actual playback pass in MediaMonkey 2024 without HTTP for both encrypted MP3 (bounded memory; no cache) and encrypted AAC (managed cache as final fallback). The input DLL runs in `MediaMonkeyEngine.exe`; no password was saved without consent. Since encrypted RAR5 streaming is not available for these members, do not label either case `STREAM`. AAC currently cannot use a Media Foundation memory-backed decoder. Native encrypted RAR5 **MP3** GUI seek/resume is certified (19→223→228s via bounded memory, no cache or HTTP). Native encrypted RAR5 AAC seek **remains open**: the actual maximized MediaMonkey host played the track using managed cache, but the GUI emitted no decoder seek callback and merely showed naturally advancing clock time. It may lack usable duration metadata, but that is not yet established. Native header-encrypted archives and multipart acceptance also remain open. Run the real host test with `AST_MM5_QA_NATIVE=1` `AST_MM5_QA_AES=accept` `AST_MM5_QA_ARCHIVE=rar` and `AST_MM5_QA_AES_CODEC=MP3` or `AAC`.

**Encrypted native 7z (15 November 2026):** Actual MediaMonkey 2024 showed a native archive password dialog hosted by `MediaMonkeyEngine.exe`. After entering a synthetic password, AAC played through direct AES+LZMA2 streaming, not HTTP or disk cache. The shared 7z reader now rejects an initial empty AES password during open, with full Win32/x64 layout regression coverage. **Native encrypted AAC GUI seek/resume now passes twice in the actual host:** after the Windows credential prompt, the test restores the maximized MediaMonkey window and clicks the actual seek bar. Both isolated sessions jumped from 43–44s to 162s, resumed at 167s, logged an input `SEEK_REQUEST`, a Media Foundation `MF_SEEK_DISPATCH` and a successful `SEEK_READER_RETURN`; zero HTTP/cache and no profile changes. The QA now refuses to call elapsed-clock movement a seek without the decoder events. Encrypted RAR5 AAC and untested AES archive layouts remain separate acceptance tasks; explicit HTTP compatibility remains available. QA: `test_native_portable_codecs.py` with `AST_MM5_QA_NATIVE=1` and `AST_MM5_QA_AES=accept`, plus `archive_backend_test/test_7z_encrypted_layouts.py`.

**Still unverified:** all seven codecs across every archive type in native
mode, all encrypted/multipart layouts through the new native host path,
automatic repaint of already-queued native titles, audible speakers and
full PCM-content parity of actual player output. Earlier encryption, title
refresh and seven-codec HTTP integration tests remain valid for the explicit
or automatically selected compatibility route.
