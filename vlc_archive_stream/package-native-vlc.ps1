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
Archive Stream Player - VLC 3.0 x64 native archive access module

The built-in VLC archive handling does not support every tested 7z layout.
This pack includes our native access DLL, native archive-to-M3U8 enumerator,
and the existing statically linked HTTP bridge as a compatibility fallback.

Tested: isolated VLC 3.0.24 x64 on Windows 11. No external archive DLLs.
Do not install into VLC 4, 32-bit VLC, or an untested build without QA.

Install into the desired VLC x64 installation (or a portable VLC copy):
- plugins\access\libarchive_stream_plugin.dll -> VLC\plugins\access\
- vlc_native_playlist.exe -> the folder holding launch-archive.ps1
- launch-archive.ps1, configure-adapters.ps1 -> same folder as builder
- x64\Release\archive_player_bridge.exe -> same folder hierarchy as launcher

The easiest arrangement is to extract the archive OVER A PORTABLE VLC 3 x64
folder, retaining its existing binaries and plugin directories. Existing
files are not removed. For a Program Files VLC, a manual elevated copy
may be required. Back up custom player files before changing a real install.

Launch, passing your exact VLC executable:
powershell -File .\launch-archive.ps1 -Player VLC -PlayerPath "D:\Apps\VLC\vlc.exe" -Archive "D:\Audio\album.7z"

The launcher chooses native M3U8 ast:// URLs only if it finds the DLL
beside that VLC executable and the native playlist builder is available.
Use -UseHttpBridge to force the prior HTTP scheme. When native setup is
not installed or per-launch overrides are supplied, HTTP remains the
working fallback (the bundled bridge EXE is included for this reason).
Settings and passwords follow the project shared adapter preferences.
Generated small playlists contain archive paths and titles, never audio
payloads or passwords. They are under your local ArchiveStreamPlayer data.
A speaker output or every encryption layout is not certified by this pack.

The VLC official SDK headers are not redistributed here.
Source + build instructions: repository vlc_archive_stream/README.md
Original project code MIT; bundled third-party libraries retain licenses.
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
