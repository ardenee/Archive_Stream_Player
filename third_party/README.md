# Third-party components

The seven directories below are **all required** by Archive Stream Player's existing Windows builds. None is an obsolete archive engine. In particular, libarchive does not replace the static 7-Zip SDK, UnRAR or zlib in the project's current implementation.

| Folder | Source | Use | Licence |
| --- | --- | --- | --- |
| [libarchive](libarchive/README.md) | [libarchive](https://github.com/libarchive/libarchive) | General archive and ZIP/RAR access | BSD-style; [COPYING](libarchive/COPYING), per-file exceptions |
| [lzma-sdk](lzma-sdk/README.md) | [7-Zip SDK](https://www.7-zip.org/sdk.html) | Native 7z, encrypted 7z, multipart and filters | Public domain |
| [unrar](unrar/README.md) | [RARLAB UnRAR](https://www.rarlab.com/rar_add.htm) | RAR/RAR5, encrypted, solid, split fallbacks | [UnRAR restrictive source licence](unrar/license.txt) |
| [zlib](zlib/README.md) | [zlib](https://www.zlib.net/) | Static DEFLATE/inflate support | [zlib licence](zlib/LICENSE) |
| [dr_libs](dr_libs/README.md) | [mackron/dr_libs](https://github.com/mackron/dr_libs) | MP3, WAV, FLAC audio decoders | Public domain or MIT-0 (in each header) |
| [stb](stb/README.md) | [nothings/stb](https://github.com/nothings/stb) | Ogg Vorbis decoder | Public domain or MIT (in source) |
| [winamp](winamp/README.md) | [Winamp SDK header mirror (WACUP)](https://getwacup.com/sdk/Winamp/) | Winamp/WACUP/MediaMonkey plugin interfaces | Retain Nullsoft header notices; SDK terms need review |

## Dependency and provenance audit

Audited on 13 November 2026 using source includes and Visual Studio source/link references in `archive_backend`, `libarchive_static`, `unrar_static`, `zlib_static`, `in_archive_stream` and `gen_archive_stream`. All seven directories are used. Git history since the initial vendor import (`027a8aa`, 22 May 2026) shows post-import vendor source edits only in **`lzma-sdk`**, for AES integration. Some local build configurations were already present at first import, so that history alone does **not** prove byte-for-byte upstream identity of any initial snapshot.

The official AIMP, foobar2000, XMPlay and MusicBee SDKs are downloaded separately and are **not** vendored here.

## Redistribution

The repository's root [MIT License](../LICENSE) covers original Archive Stream Player code **only**. All imported code in `third_party` retains the licensing established by its original authors; the project's MIT file **does not** relicense those sources. See [the precise licence scope](../LICENSING.md).

These READMEs are summaries, **not replacement licences**. Preserve upstream copyright and licences in every file and supply applicable notices with binary releases, notably libarchive's `COPYING` and UnRAR's `license.txt`. UnRAR's conditions explicitly prohibit using its source to recreate a RAR-compatible compressor. Libarchive includes differently licensed portions; its individual file notices prevail. The Winamp SDK's precise original distribution version is not recorded; review terms before distributing SDK headers. See each linked folder README for detailed requirements and local changes.
