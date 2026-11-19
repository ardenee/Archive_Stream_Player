# Media Player (2022) integration — feasibility and implementation plan

**Target:** The modern Windows 11 Microsoft Store **Media Player** application (`Microsoft.ZuneMusic`, executable `Microsoft.Media.Player.exe`). This is NOT Windows Media Player Legacy (`wmplayer.exe`). The old WMP plug-in SDK (`wmp.h`, `wmpplug.h`) is not an integration contract for this app.

**Status:** Investigated, native playback **not yet implemented or tested**. Do not advertise Media Player (2022) as a supported adapter in the installer or feature matrix until the gates below pass.

## DEV evidence — November 19, 2026

- Installed: `Microsoft.ZuneMusic` v`11.2609.11.0`, x64; package family `Microsoft.ZuneMusic_8wekyb3d8bbwe`.
- Manifest executable: `Microsoft.Media.Player.exe`. The supported-file-types manifest includes `.m3u` and `.m3u8`, but does NOT include `.zip`, `.7z`, or `.rar`.
- Windows feature `Client-ProjFS` (Windows Projected File System) is currently **Disabled**. Do **not** enable this system-wide optional feature without explicit approval, and do not presume that it can be used on all target machines.
- The repository's current Windows Media Player path (`archive_player_bridge/launch-archive.ps1 -Player WMP`) launches **Legacy WMP** and a local HTTP/ASX bridge. It is NOT native Media Player (2022) support.
- The modern app does not expose a publicly documented host-specific equivalent to the legacy WMP user-interface plug-in API. `Windows.Media.MediaExtensionManager` registrations are **local to the calling application**, not global plug-in installation.
- Some packaged in-process media extensions use the restricted `inProcessMediaExtension` capability, unavailable to ordinary third-party developers. Global Media Foundation COM byte-stream/scheme registrations are documented for desktop source resolution, but whether this packaged Media Player loads them is **unverified**. Do not assume installing a DirectShow filter, MFT, or Media Foundation handler automatically integrates with this app.

## Preferred feasibility path: native projected audio files

Prototype a **Windows Projected File System (ProjFS) provider** that exposes ZIP/7z/RAR audio entries as ordinary read-only audio files under a virtual NTFS directory. Media Player can then consume them through its existing media format decoders, with no in-process plug-in, PowerShell command, or HTTP listener.

- Reuse the existing static `archive_backend` and shared settings, logging, passwords and metadata probes. The original archives remain unchanged.
- Use `PrjStartVirtualizing`, enumeration and placeholder metadata callbacks, then `PRJ_GET_FILE_DATA_CB` to serve requested ranges. Track uncompressed lengths from the archive headers, **not computed content CRCs**. Never create or unpack an entire album in advance.
- Archive entry reads follow the policy **direct streaming → memory (configured buffer) → managed disk cache (last resort)**. ProjFS itself may cache hydrated bytes on disk: benchmark and document this operating-system behavior; do NOT falsely claim zero disk writes.
- For solid/encrypted archives, implement bounded background work and coherent fallback (avoid repeated whole-solid-archive scans on track metadata).
- Expose a read-only album directory and an optional generated `.m3u8` file pointing to its projected audio files. User-facing entry point should be a proper Windows Explorer **Play in Media Player** context-menu/shell integration or an Archive Stream UI, NOT a visible PowerShell launcher.
- Media Player (2022) has no supported documented host-specific settings page for us. Use Archive Stream's shared settings utility instead. Native player controls, tag parsing and duration computation belong to Media Player once it sees ordinary audio files.

**Important limitation:** This approach is a **native filesystem integration**, not a Media Player app plug-in. Direct dragging of raw archives into Media Player itself will not automatically expand them unless Microsoft implements an appropriate extensibility API; use the Archive Stream shell command to mount/open the album.

## Acceptance gates before claiming integration

1. Separate DEV feasibility proof: enable ProjFS only after explicit approval, or use a disposable Windows Sandbox/VM with the optional feature enabled. Avoid changing system-wide Windows features as a side effect of a normal build.
2. Validate enumeration of 9/9 `VA - Subspecies 4 (2024).zip` tracks and 10/10 `VA - Natural Order 2 (2025).rar` tracks as ordinary files with correct stable lengths.
3. Verify that **the actual `Microsoft.Media.Player.exe` app** opens a projected `.m3u8` and plays/pauses/seeks multiple tracks, including backward seeks, without HTTP or an external extraction folder. Record the real process and playback/routing logs.
4. Verify native playlist titles, artist, album, genre and duration. Preserve safe metadata fallbacks if the app eagerly reads projected contents for tags.
5. Test non-solid ZIP/7z/RAR, solid and encrypted archives, interrupted/cancelled reads, large offsets, provider restarts, archive disappearance, and cache cleanup; make sure no ordinary file associations are hijacked.
6. Re-run under standard user permissions with a fresh Media Player library, confirm shell entry usability, security boundaries, and that the ProjFS system prerequisite is handled cleanly.
7. Only after these pass: create `media_player_2022` implementation, installer choice, real-player tests and feature-matrix supported status.

## Other feasibility path (secondary)

A registered Media Foundation `IMFByteStreamHandler` or `IMFSchemeHandler` might expose `ast://` to desktop MF source resolution. It is only a candidate for a **controlled actual Media Player (2022) load test**. A desktop MF test succeeding would NOT prove the packaged player accepts it. Do not globally claim `.zip`, `.rar` or `.7z` extensions during probing.

## Primary references

- Modern media app extension manager (per-app registration): https://learn.microsoft.com/en-us/uwp/api/windows.media.mediaextensionmanager
- Media Foundation scheme/byte-stream handlers: https://learn.microsoft.com/en-us/windows/win32/medfound/scheme-handlers-and-byte-stream-handlers
- Windows Projected File System API: https://learn.microsoft.com/en-us/windows/win32/projfs/projected-file-system
- Enabling the Windows optional ProjFS component: https://learn.microsoft.com/en-us/windows/win32/projfs/enabling-windows-projected-file-system
- Microsoft moderator discussion of restricted in-process media capability: https://learn.microsoft.com/en-us/answers/questions/5769519/how-to-develop-custom-decoder-media-foundation-tra
