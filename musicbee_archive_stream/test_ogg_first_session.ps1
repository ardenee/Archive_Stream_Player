# Verify OGG-first archive sessions, original-byte parity and cleanup.
# Run under Win32 Windows PowerShell with the official MusicBee SDK-built plugin.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Split-Path $PSScriptRoot -Parent
$plugin = Join-Path $root 'musicbee_archive_stream\Release\MB_ArchiveStream.dll'
$bridge = Join-Path $root 'archive_player_bridge\Release\archive_player_bridge.exe'
$sourceOgg = 'C:\audiotest\02.OGGFile.ogg'
$sourceMp3 = 'C:\audiotest\01.MP3File.mp3'
foreach ($file in @($plugin, $bridge, $sourceOgg, $sourceMp3)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing test file: $file" }
}

$originalTemp = $env:TEMP
$originalTmp = $env:TMP
$originalLocal = $env:LOCALAPPDATA
$originalDebug = $env:ARCHIVE_STREAM_ADAPTER_DEBUG
$testRoot = Join-Path $env:TEMP ('ASP-MusicBee-OGGFirst-' + [Guid]::NewGuid().ToString('N'))
New-Item -Path $testRoot -ItemType Directory -Force | Out-Null
$instance = $null
try {
    $app = Join-Path $testRoot 'Plugin'
    $tempFolder = Join-Path $testRoot 'Temp'
    $localFolder = Join-Path $testRoot 'Local'
    New-Item -Path $app,$tempFolder,$localFolder -ItemType Directory -Force | Out-Null
    Copy-Item $plugin (Join-Path $app 'MB_ArchiveStream.dll')
    Copy-Item $bridge (Join-Path $app 'archive_player_bridge.exe')
    $archive = Join-Path $testRoot 'OGG-First.zip'
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($track in @(@{path=$sourceOgg; name='First.ogg'}, @{path=$sourceMp3; name='Second.mp3'})) {
            $entry = $zip.CreateEntry($track.name, [IO.Compression.CompressionLevel]::NoCompression)
            $from = [IO.File]::OpenRead($track.path)
            $to = $entry.Open()
            try { $from.CopyTo($to) }
            finally { $from.Dispose(); $to.Dispose() }
        }
    }
    finally { $zip.Dispose() }

    $env:TEMP = $tempFolder
    $env:TMP = $tempFolder
    $env:LOCALAPPDATA = $localFolder
    $env:ARCHIVE_STREAM_ADAPTER_DEBUG = '1'
    $asm = [Reflection.Assembly]::LoadFrom((Join-Path $app 'MB_ArchiveStream.dll'))
    $type = $asm.GetType('MusicBeePlugin.Plugin', $true)
    $instance = [Activator]::CreateInstance($type)
    $method = $type.GetMethod('StartArchiveSession', [Reflection.BindingFlags]'NonPublic,Instance')
    if (!$method) { throw 'MusicBee archive session method not found' }
    [object[]]$arguments = New-Object object[] 1
    $arguments[0] = [string]$archive
    [string[]]$tracks = $method.Invoke($instance, $arguments)
    if ($tracks.Count -ne 2) { throw "Expected two MusicBee queue entries, got $($tracks.Count)" }
    if (!$tracks[0].EndsWith('.ogg') -or !(Test-Path -LiteralPath $tracks[0])) {
        throw "First OGG track not materialized to a local file"
    }
    if (!$tracks[1].StartsWith('http://127.0.0.1:') -or !$tracks[1].EndsWith('.mp3')) {
        throw 'Second MP3 track lost its HTTP streaming path'
    }
    $oggHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceOgg).Hash
    $fallbackHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $tracks[0]).Hash
    if ($oggHash -ne $fallbackHash) { throw 'Materialized OGG differs from original' }
    Write-Output "OGG_FIRST_LOCAL_SHA256_MATCH bytes=$((Get-Item $tracks[0]).Length)"
    Write-Output 'SECOND_MP3_HTTP_STREAM_PRESERVED'
    $localOggPath = $tracks[0]
    $instance.Close([MusicBeePlugin.Plugin+PluginCloseReason]::MusicBeeClosing)
    $instance = $null
    if (Test-Path -LiteralPath $localOggPath) { throw 'Session cleanup failed to remove plaintext OGG' }
    Write-Output 'OGG_FIRST_SESSION_CLEANUP_PASS'
    Write-Output 'ALL_MUSICBEE_OGG_FIRST_SESSION_CHECKS_PASS'
}
finally {
    if ($instance) {
        try { $instance.Close([MusicBeePlugin.Plugin+PluginCloseReason]::MusicBeeClosing) } catch { }
    }
    $env:TEMP = $originalTemp
    $env:TMP = $originalTmp
    $env:LOCALAPPDATA = $originalLocal
    $env:ARCHIVE_STREAM_ADAPTER_DEBUG = $originalDebug
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
