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
    Copy-Item -LiteralPath $bridge -Destination (Join-Path $stage 'archive_player_bridge.exe')
    Copy-Item -LiteralPath $nativeInput -Destination (Join-Path $stage 'in_archive_stream.dll')
    Copy-Item -LiteralPath $settings -Destination (Join-Path $stage 'configure-adapters.ps1')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stage 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSING.md') -Destination (Join-Path $stage 'LICENSING.md')
    Copy-Item -LiteralPath (Join-Path $root 'third_party\README.md') -Destination (Join-Path $stage 'THIRD-PARTY-NOTICES.md')
    @'
Archive Stream Player - MediaMonkey 5/2024 JavaScript addon

Install through MediaMonkey: Tools > Addons > Add, select the .mmip, then
restart MediaMonkey (or reload the window when asked).

Four Tools menu items:
  Archive Stream: Add Archive Audio... (native-first)
  Archive Stream: Add Archive Audio (HTTP compatibility)...
  Archive Stream: Settings...
  Archive Stream: Refresh Queued Titles

Native mode requires the MATCHING x86 in_archive_stream.dll installed in
MediaMonkey's Plugins folder using the companion MediaMonkey5 input MMIP.
The launcher checks its SHA-256 against the matching DLL bundled here.
Missing/different DLLs retain the existing authenticated loopback HTTP mode.
Native ZIP/7z/RAR listing uses the static backend to generate small .ast
track references; MediaMonkey's native input decoder streams compressed
archive entries directly, falling back to bounded RAM or managed cache only
when required. No HTTP listener or extracted audio file in native mode.
The addon never installs the obsolete Winamp general-purpose expander.
Use the explicit HTTP compatibility menu action if needed.
The original audio files and their embedded tags are not modified.
Existing titles are refreshed from authenticated bridge playlists only for
single-entry archive sessions. The third Tools action can refresh immediately.
Multi-entry sessions retain their existing row titles to protect playback;
new tracks still use the configured shared title template. No tags are changed.
Single-entry ZIP MP3 title refresh passed real portable MediaMonkey 2024 QA.
MP3/OGG/FLAC/WAV/WMA/M4A/AAC ZIP streams pass real-host playback and
GUI seek/resume acceptance. Vorbis duration is obtained via bounded archive
stream header/tail reads when safe, allowing direct OGG seeking without
extracting. Synthetic AES non-solid 7z plays/seeks through direct stream.
Synthetic encrypted RAR5 plays/seeks using bounded RAM fallback if direct
streaming is unsupported. The real host passed password entry, cancellation
for both formats, and wrong-password rejection for RAR5 without saving keys.
First-use encrypted RAR5 filenames also pass host password entry, queue,
playback/seek, cancellation and wrong-password rejection without saved keys.
Other archive layouts use the shared stream -> memory -> cache policy.

Native manifest files and HTTP bridge sessions end when MediaMonkey closes
(after a short grace period), or automatically after at most two hours.
The native manifest helper exits before starting any listening socket.
Host playback is not yet certified
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
