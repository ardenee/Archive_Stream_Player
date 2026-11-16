# Third-party components

Archive Stream Player includes third-party libraries and SDK interfaces needed by the supported players. **The project's MIT licence applies only to our original code.** All third-party copyright notices and licences remain in force.

| Component | Original source | Purpose | Licence |
| --- | --- | --- | --- |
| [libarchive](libarchive/README.md) | [libarchive](https://github.com/libarchive/libarchive) | Archive access | BSD-style and per-file notices; [COPYING](libarchive/COPYING) |
| [7-Zip SDK](lzma-sdk/README.md) | [7-Zip](https://www.7-zip.org/sdk.html) | 7z archives and encryption | Public domain |
| [UnRAR](unrar/README.md) | [RARLAB](https://www.rarlab.com/rar_add.htm) | RAR archives | [UnRAR source licence](unrar/license.txt) |
| [zlib](zlib/README.md) | [zlib](https://www.zlib.net/) | ZIP compression support | [zlib licence](zlib/LICENSE) |
| [dr_libs](dr_libs/README.md) | [mackron/dr_libs](https://github.com/mackron/dr_libs) | MP3, WAV and FLAC decoding | Public domain / MIT-0 |
| [stb](stb/README.md) | [nothings/stb](https://github.com/nothings/stb) | Ogg Vorbis decoding | Public domain / MIT |
| [Winamp SDK headers](winamp/README.md) | [WACUP SDK mirror](https://getwacup.com/sdk/Winamp/) | Player plugin interfaces | Original SDK notices and terms |

The 7-Zip SDK contains project-specific AES integration changes; see its [component README](lzma-sdk/README.md). Player SDKs for AIMP, foobar2000, XMPlay and MusicBee are obtained separately.

Redistribution must preserve the applicable notices, particularly libarchive's [COPYING](libarchive/COPYING), UnRAR's [licence](unrar/license.txt) and individual source-file notices. UnRAR's licence limits use of its source to create RAR-compatible compressors. Review Winamp SDK terms before redistributing its headers.

See [full licensing details](../LICENSING.md).
