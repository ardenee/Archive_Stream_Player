param(
    [string]$OutputFolder = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputFolder) { $OutputFolder = Join-Path $PSScriptRoot 'Release' }

$native = Join-Path $PSScriptRoot 'Release\bass_archive_stream.dll'
$managed = Join-Path $PSScriptRoot 'Release\MB_ArchiveStream.dll'
$builder = Join-Path $repo 'archive_player_bridge\Release\archive_player_bridge.exe'
$settings = Join-Path $repo 'archive_player_bridge\configure-adapters.ps1'
foreach ($path in @($managed, $native, $builder, $settings)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Build missing: $path" }
}

$stage = Join-Path $env:TEMP ('ArchiveStream-MusicBee-' + [Guid]::NewGuid().ToString('N'))
$plugins = Join-Path $stage 'Plugins'
New-Item -ItemType Directory -Path $plugins -Force | Out-Null
try {
    Copy-Item -LiteralPath $native -Destination (Join-Path $stage 'bass_archive_stream.dll')
    Copy-Item -LiteralPath $managed -Destination (Join-Path $plugins 'MB_ArchiveStream.dll')
    Copy-Item -LiteralPath $builder -Destination (Join-Path $plugins 'archive_player_bridge.exe')
    Copy-Item -LiteralPath $settings -Destination (Join-Path $plugins 'configure-adapters.ps1')
    $noticeRoot = Join-Path $stage 'Notices'
    $thirdPartyNotices = Join-Path $noticeRoot 'third_party'
    New-Item -ItemType Directory -Path $thirdPartyNotices -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $noticeRoot
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSING.md') -Destination $noticeRoot
    Copy-Item -LiteralPath (Join-Path $repo 'third_party\README.md') -Destination $thirdPartyNotices
    foreach ($dependency in @('libarchive', 'lzma-sdk', 'unrar', 'zlib', 'dr_libs', 'stb')) {
        $folder = Join-Path $thirdPartyNotices $dependency
        New-Item -ItemType Directory -Path $folder -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $repo "third_party\$dependency\README.md") -Destination $folder
    }
    Copy-Item -LiteralPath (Join-Path $repo 'third_party\libarchive\COPYING') -Destination (Join-Path $thirdPartyNotices 'libarchive')
    Copy-Item -LiteralPath (Join-Path $repo 'third_party\unrar\license.txt') -Destination (Join-Path $thirdPartyNotices 'unrar')
    Copy-Item -LiteralPath (Join-Path $repo 'third_party\zlib\LICENSE') -Destination (Join-Path $thirdPartyNotices 'zlib')
    @'
Archive Stream Player - MusicBee 3.7 (32-bit only)

INSTALL
Extract the contents of this ZIP into the MusicBee application folder:
C:\Program Files (x86)\MusicBee\

Main binaries have this layout (Notices includes required licence texts):
  bass_archive_stream.dll
  Plugins\MB_ArchiveStream.dll
  Plugins\archive_player_bridge.exe
  Plugins\configure-adapters.ps1

Do not place bass_archive_stream.dll inside Plugins. The native add-on is
loaded by the existing 32-bit BASS runtime inside MusicBee. Do not overwrite
MusicBee's own bass.dll or install/redistribute a separate BASS runtime.

Restart MusicBee, then use Tools > Archive Stream: Add Archive Audio...
or Tools > Archive Stream: Settings...

NATIVE MODE
Unencrypted streamable MP3, PCM16 WAV, FLAC16/24, OGG/Vorbis, AAC,
M4A and WMA play natively inside ZIP, non-solid 7z and RAR archives.
Solid 7z and RAR MP3/OGG entries may also play from bounded RAM.
Oversized unencrypted FLAC, PCM16 WAV, MP3, OGG/Vorbis, AAC, M4A
and WMA may use the shared encoded cache after stream/RAM fail, if
CacheLimitMB permits. All seven passed standalone cache/PCM/seek tests.
M4A/WMA enlarged-container tests do not extend their audio duration.
Chained OGG seek validation uses a valid multi-link, CRC-checked fixture.
Real MusicBee solid-file GUI acceptance remains unverified because
the DEV interactive desktop was unavailable.
Mixed seven-codec playlists are supported by MusicBee's native BASS add-on.
AAC/M4A/WMA reuse Windows Media Foundation and the shared archive reader.
No helper executable, HTTP listener or decoded-audio cache is used for
these native routes. Small .ast path references are cleaned normally.
Real MusicBee GUI seek/resume passed for all seven codecs across all
three tested archive formats and a mixed seven-track playlist.
Full MF PCM parity and chained Vorbis playback/seek tests also passed.
ADTS AAC duration is computed from exact audio-frame counts.
Newly queued native tracks display the shared TitleTemplate presets. The
titles are applied to temporary .ast filenames without editing archives.
Unsafe filename characters are replaced; duplicates remain unique.
Changes to presets apply on the next queue, not existing rows.
Unsupported encoded variants retain the compatibility route.
COMPATIBILITY MODE
Other codecs or unsupported encoded layouts, encrypted archives, other solid entries,
unsupported streaming layouts, or missing/unloadable BASS add-ons retain
the established MusicBee compatibility bridge, which uses direct streaming
first, bounded memory where supported and disk only when required. MP3/AAC bridge playback uses local HTTP range requests; fallback OGG/FLAC/
WAV/WMA/M4A need bounded temporary local files due to MusicBee host
limitations. The bridge only binds to 127.0.0.1. Temporary media may persist
after a crash and should be cleared from shared settings. A zero cache limit
prevents the bridge's local fallback when the host requires it.

Default saved passwords: %LOCALAPPDATA%\WinampArchiveStream\keeps.txt
Shared settings: HKCU\Software\ArchiveStreamPlayer\Adapters
Avoid sharing temporary tokenized HTTP playlist URLs publicly.

No BASS SDK headers/import library or bass.dll are included in this package.
The Archive Stream source code uses MIT; third-party code retains its own
licenses. See Notices/LICENSE, Notices/LICENSING.md and Notices/third_party.
Libarchive, UnRAR and zlib upstream licence texts are included there.

The tested native path verified host Playing state, advancing position, and
streaming PCM delivery; a full audible speaker and native GUI seek certification
for every codec and archive layout is not yet claimed.
'@ | Set-Content -LiteralPath (Join-Path $stage 'INSTALL.txt') -Encoding ASCII

    New-Item -ItemType Directory -Force -Path $OutputFolder | Out-Null
    $destination = Join-Path $OutputFolder 'ArchiveStreamPlayer-MusicBee-x86.zip'
    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $destination -CompressionLevel Optimal
    Write-Output "MUSICBEE_PACKAGE=$destination"
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}