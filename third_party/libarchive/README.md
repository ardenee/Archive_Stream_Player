# libarchive

**Upstream:** https://www.libarchive.org/ and https://github.com/libarchive/libarchive/tree/v3.8.7

**Vendored version:** **3.8.7**, reported as `ARCHIVE_VERSION_NUMBER 3008007` in `archive.h`.

**Project library:** `libarchive_static.lib`. **Status: required.**

## What it does

Reads archives, principally ZIP and RAR for this project, and supplies entry listing, extracted-file reading and supported streaming. Its static build uses our bundled zlib. It is **not** the sole archive engine: native 7-Zip handles 7z AES and special coder paths, and UnRAR handles cases that libarchive does not decrypt or extract correctly.

## Local integration and modifications

The vendored source contains the project-specific Windows/MSVC `config.h` (guard `LIBARCHIVE_CUSTOM_CONFIG_H`) and a static MSVC project `libarchive_static/libarchive_static.vcxproj` explicitly selects compiled source files. The local Windows configuration enables portability and links to static dependencies. No **post-import source edits** are recorded under `third_party/libarchive` after the initial Git import (`027a8aa`). The exact provenance of the initial local `config.h` should not be confused with a pristine upstream configuration.

Upstream tests are still present but are not compiled into the shipped player plugins. The project itself restricts the supported audio archive list and does not advertise all formats upstream libarchive can read.

## Licence

**BSD-style permissive**, with file-level exceptions. The complete [`COPYING`](COPYING) is from upstream **v3.8.7**; keep it alongside the source and include required copyright and disclaimer terms with binary distribution. The package has differently licensed components (for example, BLAKE2 code and some compression filters); `COPYING` identifies exceptions and **each file's own notice controls**.

Do not label the entire tree public domain or remove in-file notices. Updating libarchive requires reviewing its new `COPYING` and rerunning ZIP/RAR regression tests.
