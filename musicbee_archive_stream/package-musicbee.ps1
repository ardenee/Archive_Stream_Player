param([string]$OutputFolder = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$OutputFolder) { $OutputFolder = Join-Path $PSScriptRoot 'Release' }
$plugin = Join-Path $PSScriptRoot 'Release\MB_ArchiveStream.dll'
$bridge = Join-Path $root 'archive_player_bridge\Release\archive_player_bridge.exe'
$settings = Join-Path $root 'archive_player_bridge\configure-adapters.ps1'
foreach ($path in @($plugin, $bridge, $settings)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Build missing: $path" }
}
$stage = Join-Path $env:TEMP ('ArchiveStream-MusicBee-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    Copy-Item $plugin "$stage\MB_ArchiveStream.dll"
    Copy-Item $bridge "$stage\archive_player_bridge.exe"
    Copy-Item $settings "$stage\configure-adapters.ps1"
    @'
Archive Stream Player - MusicBee 3.7 x86 plugin

Copy MB_ArchiveStream.dll, archive_player_bridge.exe and configure-adapters.ps1 into:
C:\Program Files (x86)\MusicBee\Plugins

Restart MusicBee, then use:
Tools > Archive Stream: Add Archive Audio...
Tools > Archive Stream: Settings...

The bridge listens only on 127.0.0.1 while MusicBee is running.
Default saved passwords use %LOCALAPPDATA%\WinampArchiveStream\keeps.txt.
Shared settings are stored under HKCU\Software\ArchiveStreamPlayer\Adapters.
MusicBee 3.7 rejects tested OGG/WAV/WMA/M4A archive HTTP playback;
FLAC streams but fails to resume after host GUI seek.
OGG/FLAC/WAV/WMA/M4A use content-length-checked temporary local files,
bounded per session by CacheLimitMB. MP3/AAC retain HTTP streaming and GUI seeking.
The MusicBee queue API has no memory-stream input for these host-limited codecs.
A zero cache limit blocks local fallback. Unencrypted temporary audio may remain
after a crash; clear stale sessions in shared settings. Normal shutdown removes it.
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
