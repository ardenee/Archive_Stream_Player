# 7-Zip LZMA SDK

**Upstream:** https://www.7-zip.org/sdk.html

**Vendored version:** **26.01**, dated 27 April 2026 in `7zVersion.h`.

**Project library:** SDK C sources compiled into `archive_backend.lib`. **Status: required.**

## What it does

Provides native 7z parsing, LZMA/LZMA2 decoding, filters, solid/nonsolid extraction and confirmed AES-only encoded-header and multipart 7z support. The project does not require `7z.dll` or any external decompressor process during playback.

## Confirmed changes relative to initial Git import

Four SDK files were added or modified after initial import `027a8aa`. Later fixes also extend `7zDec.c` for encrypted headers:

- **`7z.h` (modified):** declares `Sz7z_SetPassword` and `Sz7z_GetPassword`, supporting scoped UTF-16 password state.
- **`7zDec.c` (modified):** integrates an AES coder path with thread-local password state into the existing C decoder. A further local change handles **single-coder AES-only encoded headers** (as written by `-mhe=on`), decrypting exactly the unpadded logical header bytes and rejecting invalid cipher/output size relationships. Supported AES → Copy/LZMA/LZMA2/filter chains retain their original decoder paths.
- **`7zAesDec.c` and `7zAesDec.h` (added):** local AES-CBC look-stream, 7z key derivation and sensitive-buffer cleanup.

These are **Archive Stream Player additions**, not an untouched upstream 7-Zip SDK. Upstream `Aes.c`, `AesOpt.c`, `Sha256.c` and `Sha256Opt.c` are statically linked alongside them. More unusual coder chains may still need fallback or may be unsupported. Split-volume handling, AES+Copy/Delta stream routing and constant-memory CRC-based password verification live in our `archive_backend` code, outside the SDK. The reproducible archive fixture matrix is `archive_backend_test/test_7z_encrypted_layouts.py`; `7z.exe` is used only to generate test archives, never during playback.

## Licence

The official **LZMA SDK source is public domain**, per https://www.7-zip.org/sdk.html. The included `7zVersion.h` credits Igor Pavlov. Preserve its original notices and this local-change record. The **full 7-Zip desktop application** has different licence terms; this folder is the public-domain LZMA SDK, not the entire application.
