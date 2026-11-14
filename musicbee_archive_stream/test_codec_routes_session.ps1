# MusicBee codec route, byte parity, and normal-shutdown cleanup regression.
# Reflection invokes the real compiled plugin without modifying a MusicBee profile.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Split-Path $PSScriptRoot -Parent
$plugin = Join-Path $root 'musicbee_archive_stream\Release\MB_ArchiveStream.dll'
$bridge = Join-Path $root 'archive_player_bridge\Release\archive_player_bridge.exe'
$fixtures = @(
    @{ Path = 'C:\audiotest\01.MP3File.mp3'; Name = 'MusicBee.MP3.mp3'; Local = $false },
    @{ Path = 'C:\audiotest\02.OGGFile.ogg'; Name = 'MusicBee.OGG.ogg'; Local = $true },
    @{ Path = 'C:\audiotest\03.FlacFile.flac'; Name = 'MusicBee.FLAC.flac'; Local = $true },
    @{ Path = 'C:\audiotest\04.WAVFile.wav'; Name = 'MusicBee.WAV.wav'; Local = $true },
    @{ Path = 'C:\audiotest\05.WMAFile.wma'; Name = 'MusicBee.WMA.wma'; Local = $true },
    @{ Path = 'C:\audiotest\06.M4AFile.m4a'; Name = 'MusicBee.M4A.m4a'; Local = $true },
    @{ Path = 'C:\audiotest\07.AACFile.aac'; Name = 'MusicBee.AAC.aac'; Local = $false }
)
foreach ($path in @($plugin, $bridge) + @($fixtures | ForEach-Object { $_.Path })) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing test prerequisite: $path" }
}
$testRoot = Join-Path $env:TEMP ('ASP-MusicBee-CodecRoutes-' + [Guid]::NewGuid().ToString('N'))
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
$previousLocal = $env:LOCALAPPDATA
$previousDebug = $env:ARCHIVE_STREAM_ADAPTER_DEBUG
$instance = $null
New-Item -Path $testRoot -ItemType Directory -Force | Out-Null
try {
    $app = Join-Path $testRoot 'Plugin'
    $tempFolder = Join-Path $testRoot 'Temp'
    $localFolder = Join-Path $testRoot 'Local'
    New-Item -Path $app,$tempFolder,$localFolder -ItemType Directory -Force | Out-Null
    Copy-Item -LiteralPath $plugin -Destination (Join-Path $app 'MB_ArchiveStream.dll')
    Copy-Item -LiteralPath $bridge -Destination (Join-Path $app 'archive_player_bridge.exe')
    $archive = Join-Path $testRoot 'MusicBee-Codecs.zip'
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in $fixtures) {
            $entry = $zip.CreateEntry($f.Name, [IO.Compression.CompressionLevel]::NoCompression)
            $sourceStream = [IO.File]::OpenRead($f.Path)
            $destinationStream = $entry.Open()
            try { $sourceStream.CopyTo($destinationStream) }
            finally { $sourceStream.Dispose(); $destinationStream.Dispose() }
        }
    }
    finally { $zip.Dispose() }

    $env:TEMP = $tempFolder
    $env:TMP = $tempFolder
    $env:LOCALAPPDATA = $localFolder
    $env:ARCHIVE_STREAM_ADAPTER_DEBUG = '1'
    $assembly = [Reflection.Assembly]::LoadFrom((Join-Path $app 'MB_ArchiveStream.dll'))
    $type = $assembly.GetType('MusicBeePlugin.Plugin', $true)
    $instance = [Activator]::CreateInstance($type)
    $method = $type.GetMethod('StartArchiveSession', [Reflection.BindingFlags]'NonPublic,Instance')
    if (!$method) { throw 'MusicBee StartArchiveSession method missing' }
    [string[]]$tracks = $method.Invoke($instance, [object[]]@([string]$archive))
    if ($tracks.Count -ne $fixtures.Count) { throw "Expected $($fixtures.Count) tracks, got $($tracks.Count)" }

    $localTracks = @()
    for ($i = 0; $i -lt $fixtures.Count; $i++) {
        $f = $fixtures[$i]
        if ($f.Local) {
            if (!(Test-Path -LiteralPath $tracks[$i] -PathType Leaf)) { throw "Missing local fallback for $($f.Name)" }
            $originalHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.Path).Hash
            $localHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $tracks[$i]).Hash
            if ($originalHash -ne $localHash) { throw "Materialized data differs for $($f.Name)" }
            if (!$tracks[$i].EndsWith([IO.Path]::GetExtension($f.Name))) { throw "Wrong local codec extension for $($f.Name)" }
            $localTracks += $tracks[$i]
            Write-Output "LOCAL_CODEC_BYTE_PARITY_PASS index=$i codec=$([IO.Path]::GetExtension($f.Name))"
        }
        else {
            if (!$tracks[$i].StartsWith('http://127.0.0.1:')) { throw "Live stream lost for $($f.Name)" }
            Write-Output "HTTP_STREAM_PRESERVED index=$i codec=$([IO.Path]::GetExtension($f.Name))"
        }
    }

    $instance.Close([MusicBeePlugin.Plugin+PluginCloseReason]::MusicBeeClosing)
    $instance = $null
    foreach ($localTrack in $localTracks) {
        if (Test-Path -LiteralPath $localTrack) { throw "Normal shutdown left plaintext track: $localTrack" }
    }
    Write-Output 'MUSICBEE_ALL_LOCAL_CODEC_SESSION_CLEANUP_PASS'
    Write-Output 'ALL_MUSICBEE_CODEC_ROUTE_CHECKS_PASS'
}
finally {
    if ($instance) {
        try { $instance.Close([MusicBeePlugin.Plugin+PluginCloseReason]::MusicBeeClosing) } catch { }
    }
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    $env:LOCALAPPDATA = $previousLocal
    $env:ARCHIVE_STREAM_ADAPTER_DEBUG = $previousDebug
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
