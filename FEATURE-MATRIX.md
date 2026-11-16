# Archive Stream Player — features by player

**What this means:** Archive Stream opens the ZIP/7z/RAR archive in every native integration listed here. Audio decoding is provided either by Archive Stream's native decoder or by the player itself, as shown below. These are **different designs**, not different archive settings.

## Choose your player

| Player | Archive/audio decoding | Encryption and solid archives | Playlist titles | Install |
| --- | --- | --- | --- | --- |
| **Winamp 32-bit** | Archive Stream handles the archive **and all seven listed audio formats** | Passwords; RAM/cache for entries that cannot stream | Shared title presets, with refresh support | Native plugin |
| **WACUP 32-bit** | Same Winamp-compatible archive and audio modules | Same shared password/cache options | Title presets | Native plugin |
| **foobar2000 32/64-bit** | Archive Stream opens archive; **foobar2000 decodes audio** | Archive Stream passwords, RAM and cache | Live title updates | Native component |
| **AIMP 32/64-bit** | Archive Stream opens archive; **AIMP decodes audio** | Archive Stream passwords, RAM and cache | Title presets; already-queued rows may not redraw | Native plugin |
| **XMPlay 32-bit** | Archive Stream handles **7z/RAR**; XMPlay can handle ordinary ZIP itself; **XMPlay decodes audio** | Archive Stream passwords/RAM/cache for plugin-handled 7z/RAR; built-in ZIP bypasses them | Reimport to update existing titles | Native archive plugin |
| **MusicBee 32-bit** | Archive Stream native audio modules play **all seven listed formats** | ZIP/7z/RAR passwords, RAM/cache and fallback | New-track titles; requeue after template changes | Native plugin + Tools integration |
| **MediaMonkey 5 / 2024** | Archive Stream native input module plays **all seven listed formats** | ZIP/7z/RAR passwords, RAM/cache and fallback | Track titles; multi-track in-place refresh limited | Two MMIP add-ons |
| **VLC 3 64-bit** | Archive Stream opens archive; **VLC decodes audio** | Passwords, RAM/cache and fallback | Titles on newly opened albums | Native launcher |
| **WACUP 64-bit** | Compatibility mode, **not a native plugin** | Depends on compatibility route | Playlist reload may be needed | Not a native installer selection |
| **Windows Media Player** | Experimental compatibility only | Not certified | Not certified | Not in installer |

**The seven named audio formats** are MP3, **16-bit PCM WAV**, **16/24-bit FLAC**, **OGG Vorbis**, AAC, M4A and WMA. The Winamp-compatible, MusicBee and MediaMonkey native decoders handle these formats without asking you to install another player codec plug-in. AAC/M4A/WMA decoding uses Windows Media Foundation. For foobar2000/AIMP/XMPlay/VLC, the player must be capable of playing that codec as a normal file because the archive component exposes the compressed song to its own decoder.

## Archives explained

| What you have | What Archive Stream does |
| --- | --- |
| **Normal ZIP / 7z / RAR album** | Lists tracks and streams individual songs when possible |
| **Password-protected archive** | Prompts for the password; a correct password permits playback |
| **7z with encrypted filenames** | Prompts before listing filenames |
| **RAR5 with encrypted filenames** | Prompts before listing filenames |
| **Solid album** (songs compressed together) | Reads from RAM or managed cache when independent streaming is impossible |
| **Multi-part archive** (.001, .part01.rar, etc.) | Reads the set when all required volumes are present |
| **Damaged file, missing volume or wrong password** | Reports a failure rather than pretending a track played |
| **Uncommon unsupported compression or encrypted method** | May be rejected; it will not convert the archive to a different format |

**RAM and cache settings** are the same shared preferences for all playback routes using Archive Stream's backend. XMPlay's own ZIP loader is **not** an Archive Stream route and therefore does not follow our ZIP password, RAM, cache or title controls. VLC's launcher can also override shared settings for that launch.

## What the plugin does not do

It does not automatically add missing **player-specific audio decoders** to foobar2000, AIMP, XMPlay or VLC. It does not play DRM-protected music, fix broken archives, recover missing multipart volumes, save passwords without consent, or guarantee live repainting of titles in every player. It does not unpack an entire album to a folder by default.

## Installer

The **single selectable Inno Setup installer** installs matching native components into chosen, compatible player folders. Portable installations can be selected manually. MediaMonkey needs confirmation through its own Addons interface; VLC uses an external native launcher. WACUP 64-bit and experimental Windows Media Player are not native installer choices.

[Project homepage](https://www.arden.ie/projects/archive-stream-player/) · [README](README.md) · [Setup instructions](PLAYER-ADAPTERS.md)
