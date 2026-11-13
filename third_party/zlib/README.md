# zlib

**Upstream:** https://www.zlib.net/ and https://github.com/madler/zlib

**Vendored version:** **1.3.2**, reported in `zlib.h`.

**Project library:** `zlib_static.lib`. **Status: required.**

## What it does

Provides DEFLATE/inflate/gzip and related compression routines required by the static archive build, particularly libarchive's ZIP handling. It is a compression library, **not** an obsolete archive reader.

## Local modifications

The `zlib_static/zlib_static.vcxproj` selects upstream C sources and builds a static library for Windows. No post-import vendor-source modifications are recorded for `third_party/zlib` since `027a8aa`; changes in static-library compilation and linking live in our MSVC project. Upstream `contrib` and example/test sources remain in the checkout but are **not runtime dependencies**; they may have separate file-level notices.

## Licence

The **zlib licence**, full text [LICENSE](LICENSE) and https://www.zlib.net/zlib_license.html. Credit Jean-loup Gailly and Mark Adler. Its terms allow reuse and modification but require not misrepresenting origin, marking altered versions and preserving notices in distributed source. Include applicable original notices in release packaging.

This README describes **the bundled 1.3.2 release**, not the latest upstream version. Updating it must be done deliberately with fresh compatibility and safety tests.
