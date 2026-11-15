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
This pack includes a versioned v1.2 native access DLL for in-process
album enumeration and playback, the legacy external builder for older
native DLLs, and HTTP fallback for explicitly requested/incompatible hosts.

Tested: isolated VLC 3.0.24 x64 on Windows 11. No external archive DLLs.
Do not install into VLC 4, 32-bit VLC, or an untested build without QA.

Extract this package anywhere writable; no modification to your installed VLC is needed:
- plugins\access\libarchive_stream_plugin.dll is loaded as an external VLC module through VLC_PLUGIN_PATH
- vlc_native_playlist.exe -> older native DLL compatibility (not required by v1.1+)
- launch-archive.ps1, configure-adapters.ps1 -> same folder as builder
- x64\Release\archive_player_bridge.exe -> same folder hierarchy as launcher

Simply keep the files in this extracted folder and pass -PlayerPath to your
VLC 3 x64 executable. The launcher sets VLC_PLUGIN_PATH for its child process
and restores the caller's original environment immediately afterward. It
can alternatively use a DLL already installed in VLC/plugins/access. It is
not necessary to copy DLLs into Program Files or use elevation.

Launch, passing your exact VLC executable:
powershell -File .\launch-archive.ps1 -Player VLC -PlayerPath "D:\Apps\VLC\vlc.exe" -Archive "D:\Audio\album.7z"

When a compatible v1.1+ DLL is available inside VLC or externally, the
launcher opens ast://hexpath/list/album.m3u8 directly in VLC; album and
tracks run in the same process with shared session password state.
Older unversioned native DLLs use the packaged external builder.
Older v1.1 native DLLs retain HTTP fallback for -PasswordList.
With the included v1.2 DLL, native per-launch password lists work:
powershell -File .\launch-archive.ps1 -Player VLC -PlayerPath "D:\Apps\VLC\vlc.exe" -Archive "D:\Audio\album.7z" -PasswordList "D:\Audio\passwords\keeps.txt"
Create the destination parent folder first. The path is passed only to the
VLC child process; the global settings registry is not changed.
Passwords are saved in plaintext only after consent and verification.

Use -UseHttpBridge to force the prior HTTP scheme. When native setup is
not installed or memory/cache/title overrides are supplied, HTTP remains the
working fallback (the bundled bridge EXE is included for this reason).
Settings and passwords follow the project shared adapter preferences.
Generated small playlists contain archive paths and titles, never audio
payloads or passwords. They are under your local ArchiveStreamPlayer data.
Real VLC 3.0.24 passed 7z and RAR5 data/header encryption with credential
entry, Save=No and exact PCM; Cancel and wrong passwords produce no audio.
RAR5 encrypted entries use RAM, not unsupported libarchive encrypted streaming.
Actual launcher tests also passed two-track header-encrypted 7z and RAR5
albums with a single prompt per album, no HTTP or disk, and unchanged
password lists. For v1.2, -PasswordList remains native instead of HTTP;
real two-track encrypted-header 7z/RAR5 launcher tests passed Save=Yes,
wrong+Yes and Cancel with only verified saves to a disposable plaintext
list, no HKCU changes. Physical speaker audio remains unverified.

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
