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
# plugins. This MMIP installs the native .ast archive-entry decoder; install
# the matching Tools addon MMIP for archive listing and native-first queueing.
# Older/mismatched input modules retain authenticated HTTP compatibility.
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
    Write-Host "Install the matching ArchiveStreamPlayer-MediaMonkey5-Native.mmip Tools addon for native-first archive selection and queueing. HTTP compatibility remains available."
} finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
