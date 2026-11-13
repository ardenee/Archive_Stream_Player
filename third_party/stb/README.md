# stb_vorbis

**Upstream:** https://github.com/nothings/stb and https://github.com/nothings/stb/blob/master/stb_vorbis.c

**Vendored version:** **1.22** (`stb_vorbis.c` header).

**Status:** Required for native Ogg Vorbis decoding.

## What it does

The one source file, `stb_vorbis.c`, decodes **Ogg/Vorbis audio** in Winamp and backend metadata routines. It is an audio codec, **not** a ZIP/7z/RAR reader.

## Local modifications and build decisions

No vendor-source modifications are recorded since `027a8aa`. The project includes the decoder with its proper header-only/implementation arrangement in `in_archive_stream/playback.cpp` and `archive_backend/archive_metadata.cpp`. Previous duplicate-definition/link issues were resolved in the application build arrangement, not by modifying `stb_vorbis.c`.

## Licence

The source offers **MIT or public domain (Unlicense)** alternatives, printed in full at the end of `stb_vorbis.c`. If distributing under MIT, include its copyright and licence text in binary-release notices; otherwise retain the original dual-licence notice with the source. The file credits Sean Barrett and other contributors.
