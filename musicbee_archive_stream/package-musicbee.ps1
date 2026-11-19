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
Archive Stream Player for MusicBee 3.7 (32-bit)

Install this package into the MusicBee application folder:
  bass_archive_stream.dll            (MusicBee application root)
  Plugins\MB_ArchiveStream.dll
  Plugins\archive_player_bridge.exe
  Plugins\configure-adapters.ps1

Restart MusicBee. Drag ZIP, 7z or RAR albums onto a supported
MusicBee pane or use Tools > Archive Stream: Add Archive Audio.
The Archive Player playlist is created and retained. Select its tab
manually: automatic tab switching is disabled to protect the Music tab.
Use Tools > Archive Stream: Open Archive Player to find the playlist.
Use Tools > Archive Stream: Settings for cache and password options.

Play MP3, WAV, FLAC, OGG Vorbis, AAC, M4A and WMA. Supported solid
or encrypted archives may use memory or the managed cache when direct
streaming is not available. Passwords are stored only with permission.
Use Shared Settings > Use File Tags to show embedded Title / Artist /
Album / Track # / Genre / Year instead of synthetic filename titles.
MusicBee stores small ID3 aliases rather than extracted audio; original
archives and .ast descriptors remain unchanged. Old playlist references
stay playable while existing Archive Player rows upgrade safely.
Tags load immediately; the first track playback starts a background
scan of the album for verified lengths and caches TLEN metadata.
Directly streamable MP3 tracks with a real verified Info/Xing header
populate MusicBee's native Time column after background verification.
Other codecs and MP3s without trustworthy Info/Xing may still lack
native Time values. The separate optional MusicBee-skinned window at
Tools > Archive Stream: Verified Durations can show these cached times.
It never overlays or changes the native MusicBee tabs.

Do not replace the MusicBee bass.dll or install another BASS runtime.
The package does not include third-party BASS runtime binaries.

Project homepage:
https://www.arden.ie/projects/archive-stream-player/

The original project code is MIT-licensed. See Notices for the
licence of the included third-party sources.
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