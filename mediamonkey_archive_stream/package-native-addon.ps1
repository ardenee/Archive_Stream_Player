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
$nativeInput = Join-Path $root 'in_archive_stream\Release\in_archive_stream.dll'
foreach ($file in @(
    (Join-Path $source 'info.json'),
    (Join-Path $source 'actions_add.js'),
    (Join-Path $source 'launch-mediamonkey.ps1'),
    (Join-Path $source 'choose-archive.ps1'),
    (Join-Path $source 'launch-hidden.vbs'),
    $bridge,
    $nativeInput,
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
    Copy-Item -LiteralPath (Join-Path $source 'choose-archive.ps1') -Destination (Join-Path $stage 'choose-archive.ps1')
    Copy-Item -LiteralPath (Join-Path $source 'launch-hidden.vbs') -Destination (Join-Path $stage 'launch-hidden.vbs')
    Copy-Item -LiteralPath $bridge -Destination (Join-Path $stage 'archive_player_bridge.exe')
    Copy-Item -LiteralPath $nativeInput -Destination (Join-Path $stage 'in_archive_stream.dll')
    Copy-Item -LiteralPath $settings -Destination (Join-Path $stage 'configure-adapters.ps1')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stage 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSING.md') -Destination (Join-Path $stage 'LICENSING.md')
    Copy-Item -LiteralPath (Join-Path $root 'third_party\README.md') -Destination (Join-Path $stage 'THIRD-PARTY-NOTICES.md')
    @'
Archive Stream Player for MediaMonkey 5 / 2024

Install BOTH MediaMonkey companion packages in:
  MediaMonkey > Tools > Addons > Add

  ArchiveStreamPlayer-MediaMonkey5.mmip
  ArchiveStreamPlayer-MediaMonkey5-Native.mmip

Restart MediaMonkey. Drag ZIP, 7z or RAR archives onto Now
Playing to add their audio tracks. Archive Stream no longer adds
redundant commands to the Tools menu.

Supported audio: MP3, WAV, FLAC, OGG Vorbis, AAC, M4A and WMA.
The native plugin provides archive playback; the optional compatibility
menu item can be used for other configurations.

Set passwords, RAM, managed cache, title presets and logging through
Archive Stream Settings. The original archives and music tags are not
modified. Existing multi-track playlist titles may need to be re-added
after changing a title preset.

Project homepage:
https://www.arden.ie/projects/archive-stream-player/

Original project source uses the MIT License. Third-party licences
remain in force; see THIRD-PARTY-NOTICES.md.
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
