# RARLAB UnRAR

**Upstream:** https://www.rarlab.com/rar_add.htm

**Vendored version:** **7.21**, dated 27 April 2026 in `version.hpp`.

**Project library:** `unrar_static.lib`. **Status: required.**

## What it does

Reads and extracts RAR/RAR5 data when libarchive cannot handle a particular archive, including encrypted headers, encrypted/solid entries and multipart files. This native UnRAR reader is **not redundant** with libarchive.

## Local changes and build choices

`unrar_static/unrar_static.vcxproj` compiles selected upstream files into a **static** library with `RARDLL`, `UNRAR` and `SILENT` definitions. The project-specific password handling, entry validation, volume callbacks and fallbacks are in `archive_backend` and player adapters. No source edits under `third_party/unrar` appear in Git after the initial vendor import `027a8aa`. Some upstream files are not in the static project's compile list; that is a **build selection**, not evidence that all upstream source was modified.

## Licence — mandatory restrictions

Read and keep the **full original [license.txt](license.txt)** and [acknowledgments](acknow.txt). The UnRAR source licence is **not MIT, BSD or public domain**. It permits use in software to *handle RAR archives*, but **prohibits developing a RAR-compatible archiver or recreating the proprietary RAR compression algorithm**. When distributing *modified UnRAR source*, alone or within another product, clause 2 requires the complete stipulated paragraph (beginning “UnRAR source code”) in the licence, documentation or source comments. The original `license.txt` contains that full paragraph: do not omit it from redistributions. Keep UnRAR as a read/extract dependency only. Check binary-release attribution and third-party notices during packaging.
