[CmdletBinding()]
param(
    [string]$Iscc = 'C:\Program Files\Inno Setup 7\ISCC.exe',
    [switch]$StageOnly
)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $PSScriptRoot 'payload'
if(!(Test-Path -LiteralPath $Iscc -PathType Leaf) -and !$StageOnly) { throw "Inno Setup compiler not installed: $Iscc" }
if(Test-Path -LiteralPath $stage) { Remove-Item $stage -Recurse -Force }
New-Item $stage -ItemType Directory -Force | Out-Null

function StageFile([string]$RelativeSource, [string]$RelativeDestination) {
    $source = Join-Path $project $RelativeSource
    $target = Join-Path $stage $RelativeDestination
    if(!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required release component is missing: $source" }
    $parent = Split-Path $target -Parent
    if(!(Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    Copy-Item -LiteralPath $source -Destination $target -Force
    if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) { throw "Component changed while staging: $RelativeSource" }
}

# Static archive backend: all archive dependencies are linked into the plugin DLLs.
$map = [ordered]@{
    'archive_player_bridge\configure-adapters.ps1'='Shared\configure-adapters.ps1'
    'in_archive_stream\Release\in_archive_stream.dll'='Winamp\in_archive_stream.dll'
    'gen_archive_stream\Release\gen_archive_stream.dll'='Winamp\gen_archive_stream.dll'
    'foo_archive_stream\Release\foo_archive_stream.dll'='Foobar32\foo_archive_stream.dll'
    'foo_archive_stream\x64\Release\foo_archive_stream.dll'='Foobar64\foo_archive_stream.dll'
    'aimp_archive_stream\Release\aimp_archive_stream.dll'='Aimp32\aimp_archive_stream.dll'
    'aimp_archive_stream\x64\Release\aimp_archive_stream.dll'='Aimp64\aimp_archive_stream.dll'
    'xmp_archive_stream\Release\xmp-archive-stream.dll'='XMPlay\xmp-archive-stream.dll'
    'musicbee_archive_stream\Release\bass_archive_stream.dll'='MusicBee\bass_archive_stream.dll'
    'musicbee_archive_stream\Release\MB_ArchiveStream.dll'='MusicBee\MB_ArchiveStream.dll'
    'archive_player_bridge\Release\archive_player_bridge.exe'='MusicBee\archive_player_bridge.exe'
    'vlc_archive_stream\x64\Release\libarchive_stream_plugin.dll'='VLC\libarchive_stream_plugin.dll'
    'vlc_archive_stream\x64\Release\vlc_native_playlist.exe'='VLC\vlc_native_playlist.exe'
    'archive_player_bridge\x64\Release\archive_player_bridge.exe'='VLC\archive_player_bridge.exe'
    'archive_player_bridge\launch-archive.ps1'='VLC\launch-archive.ps1'
}
foreach($item in $map.GetEnumerator()) { StageFile $item.Key $item.Value }
$helper = Join-Path $PSScriptRoot 'Open-VLC-Archive.ps1'
if(!(Test-Path $helper -PathType Leaf)) { throw "Missing installer helper: $helper" }
Copy-Item $helper (Join-Path $stage 'VLC\Open-VLC-Archive.ps1')

# MediaMonkey requires the host's MMIP installer; generate both packages.
$mm = Join-Path $stage 'MediaMonkey'
New-Item $mm -ItemType Directory -Force | Out-Null
& (Join-Path $project 'mediamonkey_archive_stream\package-mmip.ps1') -OutputFolder $mm
if($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw "MediaMonkey input package failed" }
& (Join-Path $project 'mediamonkey_archive_stream\package-native-addon.ps1') -OutputFolder $mm
if($LASTEXITCODE -and $LASTEXITCODE -ne 0) { throw "MediaMonkey Tools package failed" }
foreach($name in @('ArchiveStreamPlayer-MediaMonkey5.mmip','ArchiveStreamPlayer-MediaMonkey5-Native.mmip')) {
    if(!(Test-Path (Join-Path $mm $name))) { throw "MediaMonkey package missing: $name" }
}

$notices = @('LICENSE','LICENSING.md','third_party\README.md',
    'third_party\libarchive\COPYING','third_party\libarchive\README.md',
    'third_party\lzma-sdk\README.md','third_party\unrar\license.txt','third_party\unrar\README.md',
    'third_party\zlib\LICENSE','third_party\zlib\README.md','third_party\dr_libs\README.md',
    'third_party\stb\README.md','third_party\winamp\README.md')
foreach($notice in $notices) { StageFile $notice ("Notices\" + $notice) }

$entries = @(Get-ChildItem -LiteralPath $stage -File -Recurse)
Write-Output ("STAGED_PLUGIN_FILES={0} STAGED_BYTES={1}" -f $entries.Count,(($entries | Measure-Object Length -Sum).Sum))
if(!$StageOnly) {
    $iss = Join-Path $PSScriptRoot 'ArchiveStreamPlayer.iss'
    & $Iscc $iss
    if($LASTEXITCODE -ne 0) { throw "Inno Setup compile failed (exit $LASTEXITCODE)" }
    $output = Join-Path $PSScriptRoot 'output\ArchiveStreamPlayer-Setup.exe'
    if(!(Test-Path -LiteralPath $output)) { throw "Installer output missing: $output" }
    Write-Output ("INSTALLER_BUILD_OK path={0} bytes={1} sha256={2}" -f $output,(Get-Item $output).Length,(Get-FileHash $output -Algorithm SHA256).Hash)
}
