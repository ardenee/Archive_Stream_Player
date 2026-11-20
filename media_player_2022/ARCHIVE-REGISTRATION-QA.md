# Media Player (2022) — ZIP / 7z / RAR registration experiment

**DEV only — 20 November 2026.** The experiment attempts multiple **per-user** Windows extension-registration mechanisms. It deliberately **does not modify Media Player's signed AppX manifest**, replace WinRAR/7-Zip file defaults, or install a production archive decoder.

## Reversible test configuration (currently disabled)

Use `media_player_2022/test-archive-format-registrations.ps1`. The script exposes `-Action Enable`, `-Action Status` and `-Action Disable`. All changes are scoped to the current Windows user and recorded in:

`%LOCALAPPDATA%\ArchiveStreamPlayer\QA\media-player-2022-archive-extension-test.json`

The journal is created **before** changing the registry. Enable refuses to overwrite pre-existing per-user test locations; Disable removes only those originally absent registry subtrees and checks that Explorer's default archive handler has not changed.

To **isolate which registration method might matter**, first Disable the combined setup, then Enable exactly one or several methods using `-Methods`. Accepted names: `Shell`, `SystemFileAssociations`, `Mime`, `OpenWith`, `MediaFoundation`, `LegacyWMP` or the default `All`. Example:

```powershell
& 'C:\Users\arden\source\repos\media_player_2022\test-archive-format-registrations.ps1' -Action Disable
& 'C:\Users\arden\source\repos\media_player_2022\test-archive-format-registrations.ps1' -Action Enable -Methods OpenWith
```

Repeat the actual Media Player drop, disable that method, and enable the next. **Only one registration state is active at a time**. The `Status` action reports `ACTIVE_METHODS`. This isolated-method enable/disable cycle was also verified on DEV using `MediaFoundation` alone, followed by re-enabling `All`.

| Registration surface (per extension) | Registered in DEV | Intended test |
| --- | --- | --- |
| `HKCU\Software\Classes\.<ext>` | `Content Type=audio/x-archive-stream-<format>`, `PerceivedType=audio` | Shell classification |
| `HKCU\Software\Classes\SystemFileAssociations\.<ext>` | Audio perceived/content type | Shell's file-type grouping |
| `HKCU\Software\Classes\MIME\Database\Content Type\audio/x-archive-stream-<format>` | `Extension=.<ext>` | MIME extension lookup |
| `HKCU\Software\Classes\.<ext>\OpenWithProgids` | Real Microsoft.ZuneMusic AppX ProgID **and** a separate `ArchiveStream.MediaPlayer2022.QA.<format>` ProgID | Offer Media Player and the standalone Archive Stream companion as non-default handlers |
| `HKCU\Software\Classes\ArchiveStream.MediaPlayer2022.QA.<format>\shell\open\command` | Points to `modern_media_drop.exe "%1"` | Companion launch via *Open with* |
| `HKCU\Software\Classes\Applications\modern_media_drop.exe` | Shell open command | Alternate desktop application registration |
| `HKCU\Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\.<ext>` and `...\audio/x-archive-stream-<format>` | Diagnostic COM class `{9BCD35B6-0DB6-480B-93A1-3450EA40F016}` | Test Windows Media Foundation decoder dispatch |
| `HKCU\Software\Microsoft\MediaPlayer\Player\Extensions\.<ext>` | `Permissions=11`, `Runtime=7` | Comparison with **legacy** `wmplayer.exe` registration only |

The diagnostic `mf_bytestream_probe.dll` **always returns an unsupported media error**; it does **not** actually decode ZIP, 7z or RAR. It is useful only to log `BEGIN_CREATE_OBJECT`. The normal Archive Stream HTTP bridge and the standalone drop target remain separate and unchanged.

## DEV tests already performed

- On DEV prior to Enable, ZIP and RAR used WinRAR and 7z used 7-Zip in Explorer UserChoice. No target HKCU test registrations were present.
- All three extensions were registered successfully. Windows' merged HKCR view now reports the experimental audio MIME/perceived type; it still reports the original underlying default ProgID.
- The Microsoft.ZuneMusic AppX ProgID was verified from the installed app's **working MP3 association**, rather than guessed. It appears in the temporary ZIP/7z/RAR `OpenWithProgids` keys.
- `-Action Disable` successfully removed the created keys and restored the original view. `-Action Enable` was used for in-player testing, **then Disable was run again** to leave DEV clean.
- A native `IMFSourceResolver` test on an actual Subspecies 4 ZIP and on control files with ZIP/7z/RAR extensions **did not invoke** the diagnostic handler, despite the registration. An explicit `MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE` flag also did not change that result. This **does not** establish how modern Media Player's own OLE drop handler interprets the extension list.
- Previously, a diagnostic `.astmfprobe` extension **did** invoke the same COM byte-stream handler under the normal-user desktop Media Foundation resolver, proving that the COM code and registry dispatch work for that invented extension.
- **Actual Media Player drag-and-drop remained unsupported** for archives with all six groups registered. Explorer's Open with successfully submitted an archive but playback reported **0xC00D36C4** (unsupported byte-stream type).
- Live UIAutomation inspection of the File Open filter while all external registrations were enabled yielded exactly **58 supported formats**, identical to the **58** listed in Microsoft.ZuneMusic 11.2609.11.0's AppxManifest.xml. **No ZIP, 7z or RAR** in either. After disabling the registrations, the repeatable read-only `verify-media-player-filepicker.ps1 -OpenIfNeeded` test again returned exactly 58 matching formats and zero archive formats.
- **Result:** None of these external registrations changed the File Open format filter or made direct archive drop work in this version. The diagnostic Media Foundation handler is not a usable archive decoder. The full test configuration was removed. The original WinRAR/7-Zip archive defaults are unchanged.

## Repeating or isolating the test

1. Close and reopen the **new Media Player (2022)**, not `wmplayer.exe`.
2. Drag **one ZIP archive** directly into the actual Media Player window. Note whether it displays **Unsupported file type(s)** or accepts the drop; repeat with **7z** and **RAR**.
3. If accepted, check whether the result is a single unplayable archive, a playable entry, or a list of tracks. A changed drag effect alone is **not** working archive integration.
4. Record the outcome (or screenshot). The test DLL records any Media Foundation handler calls in `%LOCALAPPDATA%\ArchiveStreamPlayer\Logs\mf_bytestream_probe.log`. The app may never call that handler.
5. For *Open with*, compare whether the modern **Media Player** and **Archive Stream companion** appear for each archive; do not set either as the default.

**The test is currently disabled on DEV.** If deliberately re-enabled, it persists until Disabled. Other media applications that use Shell/MIME/Media Foundation classification may see ZIP/7z/RAR as audio, and the diagnostic handler will reject any requests reaching it. This is not suitable for release.

## Rollback

From PowerShell on DEV:

```powershell
& 'C:\Users\arden\source\repos\media_player_2022\test-archive-format-registrations.ps1' -Action Disable
```

Confirm with `-Action Status` that `TEST_ENABLED=False` and the default archive associations are unchanged.

**Do not add these unsuccessful experimental overrides to the main installer**, modify Microsoft's package manifest, or overwrite WinRAR/7-Zip defaults. A practical next direction is testing an external drop surface integrated visually with Media Player, forwarding archive paths to our already verified localhost HTTP/M3U8 backend while preserving normal native file drops. No in-app registration API is established by these tests.
