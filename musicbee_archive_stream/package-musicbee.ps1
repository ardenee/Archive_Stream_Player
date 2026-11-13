param([string]$OutputFolder = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$OutputFolder) { $OutputFolder = Join-Path $PSScriptRoot 'Release' }
$plugin = Join-Path $PSScriptRoot 'Release\MB_ArchiveStream.dll'
$bridge = Join-Path $root 'archive_player_bridge\Release\archive_player_bridge.exe'
foreach ($path in @($plugin, $bridge)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Build missing: $path" }
}
$stage = Join-Path $env:TEMP ('ArchiveStream-MusicBee-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    Copy-Item $plugin "$stage\MB_ArchiveStream.dll"
    Copy-Item $bridge "$stage\archive_player_bridge.exe"
    @'
Archive Stream Player - MusicBee 3.7 x86 plugin

Copy MB_ArchiveStream.dll and archive_player_bridge.exe into:
C:\Program Files (x86)\MusicBee\Plugins

Restart MusicBee, then use:
Tools > Archive Stream: Add Archive Audio...

The bridge listens only on 127.0.0.1 while MusicBee is running.
Password candidates use %LOCALAPPDATA%\WinampArchiveStream\keeps.txt.
Do not share its temporary tokenized playlist URLs publicly.
'@ | Set-Content -Path "$stage\INSTALL.txt" -Encoding ASCII
    New-Item -ItemType Directory -Force -Path $OutputFolder | Out-Null
    $destination = Join-Path $OutputFolder 'ArchiveStreamPlayer-MusicBee-x86.zip'
    if (Test-Path $destination) { Remove-Item $destination -Force }
    Compress-Archive -Path "$stage\*" -DestinationPath $destination -CompressionLevel Optimal
    Write-Output "MUSICBEE_PACKAGE=$destination"
}
finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
