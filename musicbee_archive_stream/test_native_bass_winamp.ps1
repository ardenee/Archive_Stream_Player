param(
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$FixtureMp3 = 'C:\audiotest\01.MP3File.mp3'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $PSScriptRoot 'test_native_bass_winamp.cpp'
$input = Join-Path $root 'in_archive_stream\Release\in_archive_stream.dll'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
$required = @($source, $input, $vcvars, $FixtureMp3,
    (Join-Path $MusicBeeRoot 'bass.dll'),
    (Join-Path $MusicBeeRoot 'bass_winamp.dll'))
foreach ($file in $required) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Missing x86 native decoder QA prerequisite: $file"
    }
}
$tempRoot = if (Test-Path -LiteralPath 'D:\Temp' -PathType Container) { 'D:\Temp' } else { [IO.Path]::GetTempPath() }
$qa = Join-Path $tempRoot ('AST-MusicBee-BassSDK-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $qa | Out-Null
$oldLocal = $env:LOCALAPPDATA
$oldRoaming = $env:APPDATA
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
try {
    foreach ($folder in @('Local','Roaming','Temp')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $qa $folder) | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $MusicBeeRoot 'bass.dll') -Destination $qa
    Copy-Item -LiteralPath (Join-Path $MusicBeeRoot 'bass_winamp.dll') -Destination $qa
    Copy-Item -LiteralPath $input -Destination $qa
    $archive = Join-Path $qa 'native-bass-compat.zip'
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($archive,[IO.Compression.ZipArchiveMode]::Create)
    try {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$FixtureMp3,[IO.Path]::GetFileName($FixtureMp3)) | Out-Null
    }
    finally {
        $zip.Dispose()
    }
    $exe = Join-Path $qa 'native_bass_winamp_probe.exe'
    $command = 'call "' + $vcvars + '" >nul && cl /nologo /EHsc /std:c++17 /W3 /Fe:"' + $exe + '" "' + $source + '"'
    & cmd.exe /d /c $command
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw 'BASS/Winamp QA probe x86 build failed'
    }
    $env:LOCALAPPDATA = Join-Path $qa 'Local'
    $env:APPDATA = Join-Path $qa 'Roaming'
    $env:TEMP = Join-Path $qa 'Temp'
    $env:TMP = Join-Path $qa 'Temp'
    $archiveTrack = $archive + '.archive_stream|0|' + [IO.Path]::GetFileName($FixtureMp3)
    Push-Location $qa
    try {
        $result = & $exe (Join-Path $qa 'in_archive_stream.dll') $archiveTrack 2>&1 | Out-String
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    Write-Output $result.TrimEnd()
    if ($code -ne 0 -or $result -notmatch 'BASS_WINAMP_STREAM=[1-9]\d*' -or $result -notmatch 'BASS_WINAMP_PCM_BYTES=65536') {
        throw "Standalone MusicBee BASS_WINAMP native PCM probe failed (exit $code)"
    }
    Write-Output 'MUSICBEE_STANDALONE_BASS_WINAMP_VIRTUAL_MP3_PCM_PASS'
    Write-Output 'MUSICBEE_HOST_DISPATCH_NOT_TESTED_BY_THIS_HARNESS'
}
finally {
    $env:LOCALAPPDATA = $oldLocal
    $env:APPDATA = $oldRoaming
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
    if (Test-Path -LiteralPath $qa) {
        Remove-Item -LiteralPath $qa -Recurse -Force -ErrorAction SilentlyContinue
    }
}
