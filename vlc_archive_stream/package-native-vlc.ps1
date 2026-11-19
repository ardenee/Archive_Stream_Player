param(
    [string]$OutputFolder = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$OutputFolder) { $OutputFolder = Join-Path $PSScriptRoot 'Release' }
$dll = Join-Path $PSScriptRoot 'x64\Release\libarchive_stream_plugin.dll'
$builder = Join-Path $PSScriptRoot 'x64\Release\vlc_native_playlist.exe'
$bridge = Join-Path $root 'archive_player_bridge\x64\Release\archive_player_bridge.exe'
$launcher = Join-Path $root 'archive_player_bridge\launch-archive.ps1'
$settings = Join-Path $root 'archive_player_bridge\configure-adapters.ps1'
foreach ($file in @($dll, $builder, $bridge, $launcher, $settings, (Join-Path $root 'LICENSE'), (Join-Path $root 'LICENSING.md'), (Join-Path $root 'third_party\README.md'))) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing VLC native adapter dependency: $file" }
}
$stage = Join-Path $env:TEMP ('ArchiveStream-VLCNative-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'plugins\access') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'x64\Release') | Out-Null
try {
    Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'plugins\access\libarchive_stream_plugin.dll')
    Copy-Item -LiteralPath $builder -Destination (Join-Path $stage 'vlc_native_playlist.exe')
    Copy-Item -LiteralPath $bridge -Destination (Join-Path $stage 'x64\Release\archive_player_bridge.exe')
    Copy-Item -LiteralPath $launcher -Destination (Join-Path $stage 'launch-archive.ps1')
    Copy-Item -LiteralPath $settings -Destination (Join-Path $stage 'configure-adapters.ps1')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stage 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSING.md') -Destination (Join-Path $stage 'LICENSING.md')
    Copy-Item -LiteralPath (Join-Path $root 'third_party\README.md') -Destination (Join-Path $stage 'THIRD-PARTY-NOTICES.md')
    @'
Archive Stream Player for VLC 3 (64-bit)

Recommended: use the main Archive Stream Player installer to enable
VLC 3 x64 Open File and archive drag-and-drop automatically.

For portable/manual VLC 3 x64 installs, copy the packaged DLL from
plugins\access\libarchive_stream_plugin.dll into your VLC directory's
plugins\access folder. Then run VLC's vlc-cache-gen.exe against VLC's
plugins folder and restart VLC. After that, open an archive normally
with Media > Open File or drag the archive into VLC.

The older launcher remains available for diagnostics and compatibility:

  powershell -File .\launch-archive.ps1 -Player VLC
    -PlayerPath "C:\Path\To\VLC\vlc.exe" -Archive "C:\Music\album.7z"

VLC plays the audio using its own codecs. Archive Stream supplies the
ZIP/7z/RAR reader, encrypted archive passwords, streaming, RAM and
managed-cache fallbacks, and album playlists. No HTTP bridge or launcher
is needed for ordinary VLC 3 x64 Open File or archive drag/drop.

Use configure-adapters.ps1 to change shared settings. An explicit
compatibility mode is available for older VLC versions where supported.
VLC 4 and VLC 32-bit are not part of this native package.

Project homepage:
https://www.arden.ie/projects/archive-stream-player/

Our original source code is MIT-licensed. Third-party licences remain
in force; see LICENSING.md and THIRD-PARTY-NOTICES.md.
'@ | Set-Content -LiteralPath (Join-Path $stage 'INSTALL.txt') -Encoding UTF8
    New-Item -ItemType Directory -Force -Path $OutputFolder | Out-Null
    $name = Join-Path $OutputFolder 'ArchiveStreamPlayer-VLC3-x64.zip'
    Remove-Item -LiteralPath $name -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $name -CompressionLevel Optimal
    Write-Output "VLC_NATIVE_PACKAGE_READY=$name"
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
