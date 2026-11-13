# Archive Stream Player

**Play music directly from ZIP, 7z and RAR archives in your favourite Windows music player.**

Archive Stream Player lets supported players browse the audio tracks inside an archive and play them as individual songs. You don't have to manually unpack an entire album first. The source is published here and the project provides a shared archive engine with several player-specific integrations.

## What it can do

- **Open ZIP, 7z and RAR archives** containing music, including supported split/multipart and solid archives.
- **Display and queue individual songs** from archives in players that offer native archive or playlist integration.
- **Play common music formats**, including MP3, FLAC, WAV and Ogg Vorbis. AAC, M4A and WMA depend on the player's available decoders.
- **Read encrypted archives** using saved passwords and, where implemented, player-specific password prompts.
- **Seek within supported tracks**, so playback does not have to start from the beginning.
- **Choose efficient playback automatically:** stream eligible files directly, use memory when appropriate, and fall back to a managed temporary cache when needed.
- **Work without separate archive-extraction programs or runtime archive DLLs.** Archive reading is built into the project's components.
- **Keep archives intact.** Playing a track does not rewrite or modify the original ZIP, 7z or RAR file.

Not every feature above has been verified in every player or with every archive. Availability depends on the player adapter, archive format, codec and encryption method.

## Supported players and current progress

| Player | Integration | Verified so far |
| --- | --- | --- |
| **Winamp** | Native 32-bit input and playlist plugins | Archive playback and expansion implemented; wider live playback regression remains |
| **foobar2000** | Native 32/64-bit component | Playlist integration, audio decoding and seeking verified |
| **WACUP** | Winamp-compatible plugins | **32-bit** expansion and playback progression verified; **64-bit** plugin loading is not supported yet |
| **AIMP** | Native 32/64-bit plugin | Real-player archive expansion verified; audible playback still to verify |
| **XMPlay** | Native 32-bit archive plugin | Plugin loads and archive entry reads verified; audible playback still to verify |
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
- [x] Decrypt supported password-protected ZIP, RAR and 7z entries.
- [x] Stream eligible archive entries without extracting an entire album.
- [x] Use memory or temporary files for archive entries that cannot stream.
- [x] Verify decoded audio bytes and seeking.
- [x] Expand archives into individual playlist tracks in supported hosts.
- [x] Provide portable, self-contained archive readers without external extraction utilities.
- [x] Build the shared backend and newer playback bridge for both 32-bit and 64-bit Windows.

## To do before the project is complete

- [ ] Finish **real, audible playback and seeking tests** for every player and supported music format.
- [ ] Complete MusicBee's interactive queue/playback workflow and MediaMonkey's native playlist-expansion validation.
- [ ] Confirm Windows Media Player's complete playback path on a system with a working audio output.
- [ ] Investigate WACUP 64-bit compatibility or document its permanent host limitations.
- [ ] Bring password prompts, settings, cache controls and track titles to comparable quality across player adapters.
- [ ] Expand the automatic tests for Unicode names, encrypted headers, multipart archives, failures, cancellation and cleanup.
- [ ] Improve installer/distribution packages and include the required third-party licence notices.
- [x] Publish MIT licensing for the original Archive Stream Player code while retaining separate licences for third-party components.
- [ ] Finish archive-header metadata checks and remaining performance/reliability testing.

## Licence

Original Archive Stream Player code is available under the **[MIT License](LICENSE)**, copyright 2026 arden. The included archive libraries, audio codecs and player SDK headers **keep their own licences**; they are **not relicensed as MIT**. See [licence scope and distribution requirements](LICENSING.md) and the [third-party component notices](third_party/README.md).

## Downloads, development and credits

This project is under active development. The source is available in this repository; prebuilt releases for every player are **not yet published as a completed, fully certified package**.

For setup, development and test instructions, see [Player adapters and installation](PLAYER-ADAPTERS.md). For the libraries and codecs we bundle, their sources, our modifications and redistribution requirements, see [Third-party components and licences](third_party/README.md).

Archive Stream Player is independent of the music-player developers and upstream archive libraries. Thanks to the maintainers of libarchive, the 7-Zip LZMA SDK, RARLAB UnRAR, zlib, dr_libs and stb, and to the developers who provide player plugin interfaces.
