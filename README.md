# Archive Stream Player

**Play music stored inside ZIP, 7z and RAR archives—without manually extracting your albums.**

Archive Stream Player adds archive browsing and playback to supported Windows music players. Choose an archive, add its songs to your playlist, and play or seek as you would with ordinary files.

**[Project homepage](https://www.arden.ie/projects/archive-stream-player/)**

## What you can do

- Open music from **ZIP, 7z and RAR/RAR5**, including common multi-part, solid and password-protected archives.
- Add **individual songs** to a playlist rather than unpack an entire album.
- Play **MP3, WAV, FLAC, OGG Vorbis, AAC, M4A and WMA** within the supported decoder configurations described below.
- **Seek within tracks**, play albums in order and display track names using title presets.
- Enter passwords for encrypted archives; optionally save them with your permission.
- Control how much **RAM** Archive Stream can use, the maximum **disk cache** size, cleanup, title templates and logging.
- Leave the original archive and embedded music tags untouched.

### How audio format support works

Archive Stream supplies the archive reader, and its native Winamp, MusicBee and MediaMonkey playback modules also supply audio decoding (using Windows Media Foundation where needed for AAC/M4A/WMA). **Those integrations do not require separate player codec plug-ins for the seven listed formats.**

**foobar2000, AIMP, XMPlay and VLC use their own audio decoders** after Archive Stream exposes the file inside the archive. If one of those players cannot play an ordinary standalone file of a particular codec, installing Archive Stream does not add that missing codec. This is a genuine limitation of those integrations; see the [feature matrix](FEATURE-MATRIX.md).

For WAV, the directly handled format is **16-bit PCM**; FLAC supports **16/24-bit** files; OGG means **Vorbis**, not every OGG-based codec. DRM-protected audio is not supported.

### Which archives work?

| Archive | What is included |
| --- | --- |
| **ZIP** | Ordinary stored/Deflate ZIP files, supported AES-encrypted entries, and supported split archives |
| **7z** | Common 7z compression, AES passwords including hidden filenames, non-solid and solid albums, and supported split volumes |
| **RAR / RAR5** | Ordinary and encrypted RAR entries, hidden filenames, solid archives, and supported multi-volume sets |

Archive Stream tries **play directly from the archive → use a limited RAM buffer → save the individual encoded song in the managed cache only if necessary**. Solid and encrypted RAR members may need RAM or cache instead of direct streaming. An entry larger than both permitted RAM and cache cannot play. Missing volumes, invalid passwords, damaged archives and uncommon unsupported compression methods may be rejected. The plugin does **not** extract entire albums by default.

Saved archive passwords are stored in a **local plaintext list** only when you choose to save them.

## Players

| Player | Integration | Availability |
| --- | --- | --- |
| **Winamp 32-bit** | Native archive and audio plugins | Working |
| **WACUP 32-bit** | Native Winamp-compatible plugins | Working |
| **foobar2000 32/64-bit** | Native component | Working |
| **AIMP 32/64-bit** | Native archive plugin | Working |
| **XMPlay 32-bit** | Archive plugin for 7z/RAR; built-in ZIP handling | Working; ZIP behaves differently |
| **MusicBee 32-bit** | Native playback plugin and Tools integration | Working |
| **MediaMonkey 5 / 2024** | Native-first input plugin and Tools add-on | Working; requires two host add-ons |
| **VLC 3 64-bit** | Native archive access and launcher | Working |
| **WACUP 64-bit** | Compatibility playlist only | No native 64-bit plugin |
| **Windows Media Player** | Experimental compatibility route | Not included in the main installer |

See the [feature matrix](FEATURE-MATRIX.md) for the differences that affect playback, encryption, settings and track titles.

## Install

**[Download Archive Stream Player for Windows (v0.1.0)](https://github.com/ardenee/Archive_Stream_Player/releases/download/v0.1.0/ArchiveStreamPlayer-Setup.exe)** · [Release notes and checksum](https://github.com/ardenee/Archive_Stream_Player/releases/tag/v0.1.0)

**One Windows installer covers the supported native players.** It detects installed players, preselects compatible versions, lets you choose only the players you want, and allows manual paths for portable installations.

Build the installer on Windows with Inno Setup 7:

\`\`\`powershell
.\installer\build-installer.ps1
\`\`\`

The installer is created at \`installer\output\ArchiveStreamPlayer-Setup.exe\`. You need the locally built Release plugins to create the installer; build instructions and source are in the repository.

**Important installation details:**
- Close the selected music players before installing. The installer checks each player's 32/64-bit executable before placing files.
- **MediaMonkey:** the installer places both MMIP packages together; complete their installation in **MediaMonkey → Tools → Addons → Add**. Its add-on manager must approve the installation.
- **VLC:** use the new **Open an archive in VLC** Start Menu shortcut. It loads the native plugin from Archive Stream's own folder, without altering VLC's installation.
- **Portable players:** choose their actual application folder when prompted.
- WACUP 64-bit and Windows Media Player are not offered as native plugin selections.

Shared settings are available from the **Configure Archive Stream Player** Start Menu shortcut. Uninstalling the program does not deliberately delete your personal passwords, settings or cached audio.

## Current status and limitations

Core playback, playlist handling, passwords and cache management are implemented for the configurations above. Updates to titles already queued depend on the player's playlist API: foobar2000 can refresh them live, while MusicBee and XMPlay may require readding the tracks. XMPlay's built-in ZIP handling does not use Archive Stream password/cache settings.

The Windows installer is available as an official **v0.1.0 release**. It is not digitally signed, so Windows may display a SmartScreen warning. Review the release notes before installing.

## Licence

Original Archive Stream Player code is [MIT licensed](LICENSE). Bundled third-party code keeps its original notices and terms. See [licensing](LICENSING.md) and [third-party components](third_party/README.md).
