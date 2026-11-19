# Archive Stream Player — features by player

**What this means:** Archive Stream opens the ZIP/7z/RAR archive in every native integration listed here. Audio decoding is provided either by Archive Stream's native decoder or by the player itself, as shown below. These are **different designs**, not different archive settings.

## Choose your player

| Player | Archive/audio decoding | Encryption and solid archives | Playlist titles | Install |
| --- | --- | --- | --- | --- |
| **Winamp 32-bit** | Archive Stream handles archive and all seven audio formats | Passwords; RAM/cache when streaming is impossible | Title presets or optional embedded tags; background refresh | Native plugin |
| **WACUP 32-bit** | Same Winamp-compatible modules | Shared password/cache options | Embedded artist/title/album metadata stable during play/stop in DEV | Native plugin |
| **foobar2000 32/64-bit** | Archive Stream opens archive; host decodes audio | Passwords, RAM and cache | Use File Tags disables synthetic title override so host tag parsing can run | Native component |
| **AIMP 32/64-bit** | Archive Stream exposes archive audio; AIMP decodes | Shared password, RAM, cache and logging | Embedded tags and scoped playlist refresh (Subspecies 4: 9/9). **Native Duration (AIMP seconds)/Bitrate/SampleRate/Channels** from bounded genuine MP3 Info/Xing (LAME gapless delay/padding applied when available), FLAC STREAMINFO or WAV RIFF headers (live MP3: 9/9, 74:04, 44.1 kHz, 320 kbps); unknown headers remain blank. Shared prefix/postfix for preset titles; Use File Tags overrides them | Native plugin (x86/x64 DEV build pass) |
| **XMPlay 32-bit** | Archive Stream opens 7z/RAR, host can open ZIP; XMPlay decodes audio | Native Options and Config | Use File Tags leaves entry names intact for native host tags; title presets available | Native archive plugin v0.2.4 |
| **MusicBee 32-bit** | Native BASS decoder streams full original audio; 9/9 Broken Silence and 5/5 Elementis MP3 tracks validated | Shared ZIP/7z/RAR passwords, RAM and cache | Immediate embedded tags/track sorting. First playback starts low-priority verified PCM duration scan; eligible directly streamable MP3s publish one genuine Info/Xing frame for MusicBee's native Time column (DEV Broken Silence 9/9 Duration values, 37:00 total). Other formats keep safe aliases and optional Verified Durations window. | Archive-only host playlist, no reserved tab/category ID. Optional separate skinned Verified Durations view never overlays tabs; automatic tab selection remains uncertified |

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

**Shared prefix/postfix and AIMP title refresh (November 19, 2026):** AIMP's official playlist API reloads metadata for Archive Stream virtual URI entries after expansion/startup and on settings save, never ordinary music files. DEV Subspecies 4: 9/9 embedded Title/Artist fields, native streaming intact. Shared TitlePrefix/TitlePostfix registry values (up to 20 UTF-8 bytes each) decorate preset titles; Use File Tags bypasses preset and decorations. foobar's legacy local prefix/postfix are retained until shared fields are configured.

**Use File Tags** is shared by all adapters and defaults off. When enabled, the synthetic title template is overridden where embedded tags are readable. Our bounded parser covers ID3v2, FLAC Vorbis comments and Ogg Vorbis/Opus; independently unstreamable solid/password entries fall back to filenames until the player decodes the audio. No metadata probe calculates archive CRCs or extracts an entire solid album. Some hosts need a playlist reload to repaint old rows.

**RAM and cache settings** are the same shared preferences for all playback routes using Archive Stream backend. XMPlay built-in ZIP handling does not follow our ZIP cache/password/title controls; VLC launcher options may override shared preferences.

## What the plugin does not do

It does not automatically add missing **player-specific audio decoders** to foobar2000, AIMP, XMPlay or VLC. It does not play DRM-protected music, fix broken archives, recover missing multipart volumes, save passwords without consent, or guarantee live repainting of titles in every player. It does not unpack an entire album to a folder by default.

## Installer

The **single selectable Inno Setup installer** installs matching native components into chosen, compatible player folders. Portable installations can be selected manually. MediaMonkey is installed directly; VLC uses an external native launcher. WACUP 64-bit and experimental Windows Media Player are not native installer choices.

[Project homepage](https://www.arden.ie/projects/archive-stream-player/) · [README](README.md) · [Setup instructions](PLAYER-ADAPTERS.md)
