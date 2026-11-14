param(
    [string]$Configuration = 'Release',
    [string]$OutputFolder = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$OutputFolder) { $OutputFolder = Join-Path $PSScriptRoot 'Release' }
$source = Join-Path $PSScriptRoot 'native_addon'
$bridge = Join-Path $root ("archive_player_bridge\x64\" + $Configuration + "\archive_player_bridge.exe")
$settings = Join-Path $root 'archive_player_bridge\configure-adapters.ps1'
foreach ($file in @(
    (Join-Path $source 'info.json'),
    (Join-Path $source 'actions_add.js'),
    (Join-Path $source 'launch-mediamonkey.ps1'),
    $bridge,
    $settings,
    (Join-Path $root 'LICENSE'),
    (Join-Path $root 'LICENSING.md'),
    (Join-Path $root 'third_party\README.md')
)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing MediaMonkey addon dependency: $file" }
}
$stage = Join-Path $env:TEMP ('ArchiveStream-MM5Native-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage | Out-Null
try {
    Copy-Item -LiteralPath (Join-Path $source 'info.json') -Destination (Join-Path $stage 'info.json')
    Copy-Item -LiteralPath (Join-Path $source 'actions_add.js') -Destination (Join-Path $stage 'actions_add.js')
    Copy-Item -LiteralPath (Join-Path $source 'launch-mediamonkey.ps1') -Destination (Join-Path $stage 'launch-mediamonkey.ps1')
    Copy-Item -LiteralPath $bridge -Destination (Join-Path $stage 'archive_player_bridge.exe')
    Copy-Item -LiteralPath $settings -Destination (Join-Path $stage 'configure-adapters.ps1')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stage 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSING.md') -Destination (Join-Path $stage 'LICENSING.md')
    Copy-Item -LiteralPath (Join-Path $root 'third_party\README.md') -Destination (Join-Path $stage 'THIRD-PARTY-NOTICES.md')
    @'
Archive Stream Player - MediaMonkey 5/2024 JavaScript addon

Install through MediaMonkey: Tools > Addons > Add, select the .mmip, then
restart MediaMonkey (or reload the window when asked).

Two new Tools menu items:
  Archive Stream: Add Archive Audio...
  Archive Stream: Settings...

The addon uses the x64 statically linked Archive Stream bridge. It does NOT
install the obsolete Winamp general-purpose expander. MediaMonkey imports a
tokenized, temporary local M3U8 playlist for ZIP/7z/RAR archive entries.
MP3 and other host-supported codecs stream as the player requests bytes.
Other archive routes use the same shared memory/cache policy.

Bridge sessions end when MediaMonkey closes (after a short grace period),
or automatically after at most two hours. Host playback is not yet certified
for every codec/archive/encryption format. Archive files are never modified.

Shared preferences live at HKCU\Software\ArchiveStreamPlayer\Adapters.
Debug tracing is opt-in; it does not log passwords or bridge session tokens.
'@ | Set-Content -LiteralPath (Join-Path $stage 'INSTALL.txt') -Encoding UTF8
    New-Item -ItemType Directory -Force -Path $OutputFolder | Out-Null
    $target = Join-Path $OutputFolder 'ArchiveStreamPlayer-MediaMonkey5-Native.mmip'
    $zip = Join-Path $OutputFolder 'ArchiveStreamPlayer-MediaMonkey5-Native.zip'
    Remove-Item -LiteralPath $target,$zip -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
    Move-Item -LiteralPath $zip -Destination $target
    Write-Output "MEDIAMONKEY_NATIVE_ADDON_PACKAGE=$target"
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
