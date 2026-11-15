param(
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$FixtureFlac = 'C:\audiotest\03.FlacFile.flac',
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference = 'Stop'
$folder = $PSScriptRoot
$source = Join-Path $folder 'test_native_wav_parity.cpp'
$addon = Join-Path $folder 'Release\bass_archive_stream.dll'
foreach ($path in @($source,$addon,$FixtureFlac,$VcVars32,(Join-Path $BassCoreSdkRoot 'bass.h'),
                   (Join-Path $BassCoreSdkRoot 'bass.lib'),(Join-Path $MusicBeeRoot 'bass.dll'))) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing native FLAC parity prerequisite: $path" }
}
if (!(Get-Command python -ErrorAction SilentlyContinue)) { throw 'Python is required for independent wave PCM verification' }
$temp = Join-Path $env:TEMP ('ArchiveStreamBassFlacQA-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    Copy-Item -LiteralPath $addon -Destination $temp
    Copy-Item -LiteralPath (Join-Path $MusicBeeRoot 'bass.dll') -Destination $temp
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = Join-Path $temp 'native-flac.zip'
    $zip = [IO.Compression.ZipFile]::Open($archive,[IO.Compression.ZipArchiveMode]::Create)
    try { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$FixtureFlac,'A.FlacFile.flac') | Out-Null }
    finally { $zip.Dispose() }

    $manifest = Join-Path $temp 'entry-0.ast'
    $lf = [char]10
    [IO.File]::WriteAllText($manifest,('ARCHIVE_STREAM_NATIVE_TRACK_V1' + $lf +
        $archive + '.archive_stream|0|A.FlacFile.flac' + $lf),(New-Object Text.UTF8Encoding($false)))

    $exe = Join-Path $temp 'test_native_wav_parity.exe'
    $cmd = 'call "' + $VcVars32 + '" >nul && cl /nologo /EHsc /std:c++17 /W3 /O2 ' +
        '/I "' + $BassCoreSdkRoot + '" /Fe:"' + $exe + '" "' + $source + '" ' +
        '/link /LIBPATH:"' + $BassCoreSdkRoot + '" bass.lib'
    & cmd.exe /d /s /c $cmd
    if ($LASTEXITCODE -ne 0) { throw 'BASS native FLAC parity harness compile failed' }
    $first = Join-Path $temp 'first.pcm'
    $seek = Join-Path $temp 'after-75s.pcm'
    $full = Join-Path $temp 'full-track.pcm'
    Push-Location $temp
    try {
        & $exe (Join-Path $temp 'bass_archive_stream.dll') $manifest $first $seek $full
        $code = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($code -ne 0) { throw "Native BASS FLAC decode/seek failed (exit $code)" }

    $referenceDecoder = 'C:\Program Files (x86)\MusicBee\Codec\flac.exe'
    if (!(Test-Path $referenceDecoder -PathType Leaf)) { throw "Independent MusicBee FLAC reference decoder missing: $referenceDecoder" }
    $reference = Join-Path $temp 'reference-flac.wav'
    & $referenceDecoder --decode --force "--output-name=$reference" $FixtureFlac
    if ($LASTEXITCODE -ne 0 -or !(Test-Path $reference)) { throw 'Independent FLAC reference WAV decode failed' }
    $env:AST_BASS_FLAC_PARITY_SOURCE = $reference
    $env:AST_BASS_FLAC_PARITY_FIRST = $first
    $env:AST_BASS_FLAC_PARITY_SEEK = $seek
    $env:AST_BASS_FLAC_PARITY_FULL = $full
    $script = @'
import os,wave
with wave.open(os.environ["AST_BASS_FLAC_PARITY_SOURCE"], "rb") as wav:
    assert (wav.getnchannels(),wav.getframerate(),wav.getsampwidth()) == (2,44100,2), "expected 16-bit stereo 44100 fixture"
    first=wav.readframes(16384)
    assert first == open(os.environ["AST_BASS_FLAC_PARITY_FIRST"],"rb").read(), "first native BASS PCM bytes differ"
    wav.setpos(75*44100)
    second=wav.readframes(16384)
    assert second == open(os.environ["AST_BASS_FLAC_PARITY_SEEK"],"rb").read(), "native BASS post-seek PCM bytes differ"
import hashlib
fullPath=os.environ["AST_BASS_FLAC_PARITY_FULL"]
with open(fullPath, "rb") as actual, wave.open(os.environ["AST_BASS_FLAC_PARITY_SOURCE"], "rb") as ref:
    digest=hashlib.sha256()
    total=0
    while True:
        expected=ref.readframes(262144)
        if not expected:
            break
        got=actual.read(len(expected))
        assert got==expected, "FLAC full-track PCM mismatch at byte "+str(total)
        digest.update(got)
        total+=len(got)
    assert not actual.read(1), "FLAC decoder output exceeded reference track length"
    assert total==ref.getnframes()*ref.getnchannels()*ref.getsampwidth()
print("MUSICBEE_NATIVE_FLAC_INDEPENDENT_FULL_PCM_PARITY_PASS bytes="+str(total)+" sha256="+digest.hexdigest())
print("MUSICBEE_NATIVE_FLAC_INDEPENDENT_PCM_BYTE_PARITY_PASS bytes=131072 seek_seconds=75")
'@
    $verification = Join-Path $temp 'verify_wav_pcm.py'
    [IO.File]::WriteAllText($verification,$script,(New-Object Text.UTF8Encoding($false)))
    & python $verification
    if ($LASTEXITCODE -ne 0) { throw 'Native FLAC PCM byte parity failed' }
}
finally {
    foreach ($key in @('AST_BASS_FLAC_PARITY_SOURCE','AST_BASS_FLAC_PARITY_FIRST','AST_BASS_FLAC_PARITY_SEEK','AST_BASS_FLAC_PARITY_FULL')) {
        Remove-Item "Env:$key" -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
