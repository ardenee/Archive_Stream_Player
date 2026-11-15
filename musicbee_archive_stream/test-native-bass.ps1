param(
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$FixtureMp3 = 'C:\audiotest\01.MP3File.mp3',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$source = Join-Path $PSScriptRoot 'test_bass_archive.cpp'
$addon = Join-Path $PSScriptRoot 'Release\bass_archive_stream.dll'
foreach ($path in @($source, $addon, (Join-Path $BassCoreSdkRoot 'bass.h'),
                   (Join-Path $BassCoreSdkRoot 'bass.lib'), (Join-Path $MusicBeeRoot 'bass.dll'),
                   $VcVars32, $FixtureMp3)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing native BASS test prerequisite: $path" }
}
$work = Join-Path $env:TEMP ('ArchiveStreamBassNativeQA-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
try {
    Copy-Item -LiteralPath (Join-Path $MusicBeeRoot 'bass.dll') -Destination $work
    Copy-Item -LiteralPath $addon -Destination $work
    $zipPath = Join-Path $work 'native-mp3.zip'
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureMp3, '01.MP3File.mp3') | Out-Null
    }
    finally { $zip.Dispose() }
    $track = Join-Path $work 'entry-0.ast'
    $lf = [char]10
    [IO.File]::WriteAllText($track, ("ARCHIVE_STREAM_NATIVE_TRACK_V1" + $lf + $zipPath + ".archive_stream|0|01.MP3File.mp3" + $lf), (New-Object System.Text.UTF8Encoding($false)))
    $exe = Join-Path $work 'test_bass_archive.exe'
    $compile = 'call "' + $VcVars32 + '" >nul && cl /nologo /EHsc /std:c++17 /W3 /O2 ' +
        '/I "' + $BassCoreSdkRoot + '" /Fe:"' + $exe + '" "' + $source + '" ' +
        '/link /LIBPATH:"' + $BassCoreSdkRoot + '" bass.lib'
    & cmd.exe /d /s /c $compile
    if ($LASTEXITCODE -ne 0) { throw 'BASS native probe x86 compilation failed' }
    Push-Location $work
    try {
        $results = & $exe (Join-Path $work 'bass_archive_stream.dll') $track 2>&1 | Out-String
        $exit = $LASTEXITCODE
    }
    finally { Pop-Location }
    Write-Output $results.TrimEnd()
    if ($exit -ne 0 -or $results -notmatch 'BASS_ARCHIVE_MP3_STANDALONE_PCM_AND_SEEK_ALL_PASS') {
        throw "BASS native archive MP3 PCM/seek failed: $exit"
    }
    Write-Output 'MUSICBEE_NATIVE_BASS_MP3_PCM_SEEK_PASS'
}
finally {
    Remove-Item -LiteralPath $work -Force -Recurse -ErrorAction SilentlyContinue
}