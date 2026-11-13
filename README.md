# Archive Stream Player

**Play music directly from ZIP, 7z and RAR archives in your favourite Windows music player.**

Archive Stream Player lets supported players browse the audio tracks inside an archive and play them as individual songs. You don't have to manually unpack an entire album first. The source is published here and the project provides a shared archive engine with several player-specific integrations.

## What it can do

- **Open ZIP, 7z and RAR archives** containing music, including supported split/multipart and solid archives.
- **Display and queue individual songs** from archives in players that offer native archive or playlist integration.
- **Play common music formats**, including MP3, FLAC, WAV and Ogg Vorbis. AAC, M4A and WMA depend on the player's available decoders.
- **Read encrypted archives** using a shared password list and interactive prompts in supported native and bridge players. Passwords are only saved after explicit consent and successful verification.
- **Seek within supported tracks**, so playback does not have to start from the beginning.
- **Use stream-first playback:** decompress directly from an archive where possible; if streaming fails or is unavailable, use the configured RAM buffer, then a reusable managed disk cache only as a last resort. Cached tracks can be reused by different players without repeating extraction.
- **Configure shared playback preferences** across adapters: memory limits, managed-cache size and age, password lists/prompts, background solid-archive preparation, track-title presets and optional per-player diagnostic logs. Open or clear managed cache and generated logs from the common settings page. Winamp and foobar2000 also retain their native preferences.
- **Work without separate archive-extraction programs or runtime archive DLLs.** Archive reading is built into the project's components.
- **Keep archives intact.** Playing a track does not rewrite or modify the original ZIP, 7z or RAR file.

Not every feature above has been verified in every player or with every archive. Availability depends on the player adapter, archive format, codec and encryption method.

## Supported players and current progress

| Player | Integration | Verified so far |
| --- | --- | --- |
| **Winamp** | Native 32-bit input and playlist plugins | Archive playback and expansion implemented; wider live playback regression remains |
| **foobar2000** | Native 32/64-bit component | Playlist integration, audio decoding and seeking verified |
| **WACUP** | Native plugins (32-bit); playlist bridge (64-bit) | x86 expansion and playback progression verified; x64 archive playlist and playback progression verified; native x64 plugins unsupported |
| **AIMP** | Native 32/64-bit plugin | Native ZIP, non-solid 7z and RAR streaming and seeking pass x86/x64 host-contract tests; real-player expansion verified; audible playback still to verify |
| **XMPlay** | Native 32-bit archive plugin or streaming playlist bridge | Archive callback returns buffered bytes; isolated XMPlay process requested a live archive-backed HTTP stream (no cache); audible playback and player-level seeking still to verify |
| **MusicBee** | Native 32-bit menu plugin with playback bridge | Plugin registers and serves playable track URLs; complete interactive playback still to verify |
| **MediaMonkey** | Winamp-compatible DLLs and playlist bridge | DLLs load and playlist playback reaches the archive stream; native expansion still to verify |
| **VLC** | Local playlist/HTTP playback bridge | Two-track playback verified with a test audio output; audible output still to verify |
| **Windows Media Player (classic)** | Local ASX/HTTP playback bridge | Stream requests verified; actual player playback remains unverified |

**Native plugins** can give their player a more integrated experience. **Bridge-based integrations** open a temporary local playlist and need the Archive Stream bridge to remain running during playback. They are not full native archive browser plugins.

## Working functions

The following have passed tests in at least one supported player or in the shared archive backend:

- [x] List audio tracks inside ZIP, 7z and RAR archives.
- [x] Read ordinary and supported solid/nonsolid archives.
- [x] Read supported split and multipart archives.
- [x] Decrypt supported password-protected ZIP, RAR and 7z entries. Encrypted 7z headers and AES+Copy/LZMA2 streams (including Copy+Delta, split archives and solid fallback) pass x86/x64 parity and wrong-password regressions.
- [x] Stream eligible archive entries without extracting an entire album. ZIP, non-solid 7z and non-solid RAR direct HTTP streams pass x86/x64 range tests (`archive_player_bridge/test_stream_priority.py`).
- [x] Use memory before temporary files when archive entries cannot stream, including native in-RAM decryption of encrypted RAR5 entries. The encrypted RAR5 variants pass x86/x64 memory and bridge parity regressions. Injected mid-stream failure recovery passes 8/8 native AIMP, 8/8 native foobar2000 and 4/4 HTTP bridge scenarios, including memory and cache routes with exact source-byte parity.
- [x] Verify decoded audio bytes and seeking.
- [x] Use a shared managed cache and common settings policy in Winamp, foobar2000, AIMP, XMPlay and the HTTP bridge. A synthetic solid 7z test verified background preparation, persistent reuse across Win32/x64 bridge processes, and cache invalidation after a source archive changes.
- [x] Share opt-in debugging and per-host diagnostic log files (including Unicode paths); Win32/x64 HTTP bridge, AIMP Win32/x64, XMPlay Win32, and MusicBee x86 adapter logging plus selective cleanup pass regression tests. Win32/x64 cache-age startup maintenance and preservation of unrelated cache-folder files are also verified. Shared title presets work in Winamp's input/expander metadata (with a title-cache flush), AIMP virtual-file metadata, M3U/ASX bridge playlists, foobar2000's live-refreshing `%archive_stream_title%` field, and XMPlay's reversible archive display aliases. Two-architecture real foobar2000 live field-refresh tests and two-track ZIP/7z XMPlay extraction/alias regressions pass.
- [x] Expand archives into individual playlist tracks in supported hosts.
- [x] Provide portable, self-contained archive readers without external extraction utilities.
- [x] Build the shared backend and newer playback bridge for both 32-bit and 64-bit Windows.

## To do before the project is complete

- [ ] Finish **real, audible playback and seeking tests** for every player and supported music format.
- [ ] Complete MusicBee's interactive queue/playback workflow and MediaMonkey's native playlist-expansion validation.
- [ ] Confirm Windows Media Player's complete playback path on a system with a working audio output.
- [x] Investigate WACUP 64-bit: confirm the current native-plugin limitation and provide tested archive playback through the local playlist bridge.
- [x] Add shared password prompts, settings, managed-cache controls, background solid-archive preparation and title templates to AIMP, XMPlay, MusicBee, Winamp, foobar2000 and playlist-bridge clients.
- [x] Verify the shared Windows Forms settings page interactively: controls, title preview, safe cache/log cleanup buttons, and Cancel preserving registry preferences. Verify foobar2000 live title-field refresh in actual Win32/x64 portable players; verify XMPlay reversible title aliases in the native XMPARC host harness.
- [ ] Finish interactive password-dialog and background-preparation checks inside every player, and validate visible track titles in MusicBee, MediaMonkey and other bridge clients. XMPlay's native archive API does not provide live playlist-row refresh; reopen or reimport archive tracks to apply new display titles. Foobar2000 playlist columns require `$if2(%archive_stream_title%,%title%)` to display the shared field.
- [ ] Expand automatic tests for Unicode names, additional encryption coder graphs, real corruption/truncation, cancellation and cleanup; the current encrypted 7z header/Copy/LZMA2, split, solid, wrong-password and stream-open cancellation matrix is automated and passing.
- [ ] Improve installer/distribution packages and include the required third-party licence notices.
- [x] Publish MIT licensing for the original Archive Stream Player code while retaining separate licences for third-party components.
- [ ] Finish archive-header metadata checks and remaining performance/reliability testing.

## Licence

Original Archive Stream Player code is available under the **[MIT License](LICENSE)**, copyright 2026 arden. The included archive libraries, audio codecs and player SDK headers **keep their own licences**; they are **not relicensed as MIT**. See [licence scope and distribution requirements](LICENSING.md) and the [third-party component notices](third_party/README.md).

## Downloads, development and credits

This project is under active development. The source is available in this repository; prebuilt releases for every player are **not yet published as a completed, fully certified package**.

For setup, development and test instructions, see [Player adapters and installation](PLAYER-ADAPTERS.md). For the libraries and codecs we bundle, their sources, our modifications and redistribution requirements, see [Third-party components and licences](third_party/README.md).

Archive Stream Player is independent of the music-player developers and upstream archive libraries. Thanks to the maintainers of libarchive, the 7-Zip LZMA SDK, RARLAB UnRAR, zlib, dr_libs and stb, and to the developers who provide player plugin interfaces.
