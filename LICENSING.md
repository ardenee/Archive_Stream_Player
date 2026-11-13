# Licensing and third-party code

Archive Stream Player's **original code and documentation** are licensed under the [MIT License](LICENSE), copyright 2026 arden.

This licence applies to project-authored material, including the player adapters, shared archive backend, build/test scripts and project documentation, **except where a file contains its own third-party licence or attribution terms**.

## Third-party code is not relicensed

Everything originally obtained from third-party authors in [third_party/](third_party/README.md) remains under its **own upstream licence**, not the project's MIT grant. See the corresponding folders for the authoritative licence text, notices and source links:

| Component | Applicable upstream terms |
| --- | --- |
| [libarchive](third_party/libarchive/README.md) | BSD-style terms and per-file exceptions; [COPYING](third_party/libarchive/COPYING) |
| [7-Zip LZMA SDK](third_party/lzma-sdk/README.md) | Public-domain SDK source; local modifications described separately |
| [UnRAR](third_party/unrar/README.md) | Restrictive UnRAR source licence, including RAR compressor prohibition; [license.txt](third_party/unrar/license.txt) |
| [zlib](third_party/zlib/README.md) | [zlib licence](third_party/zlib/LICENSE) |
| [dr_libs](third_party/dr_libs/README.md) | Public domain or MIT-0 as specified in each source header |
| [stb_vorbis](third_party/stb/README.md) | MIT or public domain, as specified in source |
| [Winamp SDK headers](third_party/winamp/README.md) | Original Nullsoft notices; verify terms for individual headers |

**Project-authored additions to the imported 7-Zip SDK** (notably `7zAesDec.c`, `7zAesDec.h` and the AES integrations in `7z.h` and `7zDec.c`) are licensed under MIT **to the extent of the author's own contributions**. The underlying Igor Pavlov/7-Zip SDK code retains its original public-domain status. See the [modification record](third_party/lzma-sdk/README.md). No copyright to other authors' source code is claimed.

Original SDKs for foobar2000, AIMP, XMPlay and MusicBee are obtained separately and are not relicensed by this repository's MIT file.

## Distributing a build

MIT permission for Archive Stream Player's own source **does not remove** third-party obligations. Keep applicable upstream notices, attribution and full licence texts with source and packaged binaries. In particular, comply with the UnRAR source restrictions and all libarchive individual-file exceptions. The README summaries are not a substitute for those licences.

If any project file incorporates separately licensed code from another author, its own licence notice takes precedence for that code; **this repository's MIT licence only covers material we own or are entitled to license**.

Licence identifier for the project's original code: `SPDX-License-Identifier: MIT`. The canonical MIT text is [published by SPDX](https://spdx.org/licenses/MIT).
