param(
    [string]$Configuration = 'Release',
    [string]$OutputFolder = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrEmpty($OutputFolder)) {
    $OutputFolder = Join-Path $PSScriptRoot 'Release'
}
# MediaMonkey 5 supports Winamp input plugins, but not Winamp general-purpose
# plugins. Never ship gen_archive_stream.dll in the MM5 MMIP: it cannot expand
# archive playlists there. Use the bridge for real supported archive playback.
$inputPlugin = Join-Path $root "in_archive_stream\$Configuration\in_archive_stream.dll"
$settings = Join-Path $root 'archive_player_bridge\configure-adapters.ps1'
foreach ($file in @($inputPlugin, $settings)) {
    if (!(Test-Path $file)) {
        throw "Missing Winamp-compatible plugin: $file"
    }
}
$stage = Join-Path $env:TEMP ('ArchiveStream-MMIP-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    Copy-Item "$PSScriptRoot\info.json" "$stage\info.json"
    Copy-Item $inputPlugin "$stage\in_archive_stream.dll"
    Copy-Item $settings "$stage\configure-adapters.ps1"
    New-Item -ItemType Directory -Path $OutputFolder -Force | Out-Null
    $file = Join-Path $OutputFolder 'ArchiveStreamPlayer-MediaMonkey5.mmip'
    $zip = Join-Path $OutputFolder 'ArchiveStreamPlayer-MediaMonkey5.zip'
    if (Test-Path $zip) { Remove-Item $zip }
    if (Test-Path $file) { Remove-Item $file }
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    Move-Item $zip $file
    Write-Host "MediaMonkey 5+ Winamp input-plugin-only MMIP: $file"
    Write-Host "Native archive playlist expansion is not available through Winamp general plugins in MediaMonkey 5. Use the playback bridge."
} finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
