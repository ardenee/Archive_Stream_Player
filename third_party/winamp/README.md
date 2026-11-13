# Nullsoft Winamp plugin SDK headers

**Origin:** [Winamp](https://www.winamp.com/) historical plugin SDK. A public [WACUP Winamp SDK header mirror](https://getwacup.com/sdk/Winamp/) supplies comparable `gen.h`, `in2.h`, `out.h` and `wa_ipc.h` headers; the exact complete SDK archive/version of **this** imported snapshot is not recorded.

**Vendored files:** `GEN.H`, `IN2.H`, `OUT.H` and `wa_ipc.h`.

**Status:** Required for Winamp's native input/general-purpose plugins and Winamp-compatible WACUP/MediaMonkey paths.

## What they do

The headers define the **Winamp plugin ABI**, output callbacks and IPC messages. They are interface declarations only and are **not** archive decoders. They are referenced by `in_archive_stream` and `gen_archive_stream`.

## Local changes

The vendor headers have no Git-recorded changes since their initial import (`027a8aa`). Winamp IPC message handlers, archive virtual-track routing and WACUP-specific compatibility fixes live in the project's own source files (`in_archive_stream`, `gen_archive_stream` and `shared`), not here. The source archive these headers originally came from has not been identified with enough confidence to declare byte-identical upstream provenance.

## Licensing and notices

`wa_ipc.h` includes a **Copyright (C) 1997–2008 Nullsoft, Inc.** and permissive, zlib-like conditions (do not misrepresent origin; mark modifications; retain the original notice). Keep that entire notice in distributed source. The other three headers do **not** carry the same complete explicit grant in this snapshot, so **do not assume `wa_ipc.h` licenses the whole set**. Confirm original Winamp SDK distribution rights before republishing the headers. Archive Stream Player is not an official Winamp product.
