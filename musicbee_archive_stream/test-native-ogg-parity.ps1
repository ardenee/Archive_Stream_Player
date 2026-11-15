param(
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$FixtureOgg = 'C:\audiotest\02.OGGFile.ogg',
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$source=Join-Path $root 'test_native_wav_parity.cpp'
$addon=Join-Path $root 'Release\bass_archive_stream.dll'
foreach ($path in @($source,$addon,$FixtureOgg,$VcVars32,(Join-Path $BassCoreSdkRoot 'bass.h'),
                   (Join-Path $BassCoreSdkRoot 'bass.lib'),(Join-Path $MusicBeeRoot 'bass.dll'))) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing OGG PCM test dependency: $path" }
}
$temp=Join-Path $env:TEMP ('ArchiveStreamOggParity-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    Copy-Item $addon $temp
    Copy-Item (Join-Path $MusicBeeRoot 'bass.dll') $temp
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive=Join-Path $temp 'ogg.zip'
    $zip=[IO.Compression.ZipFile]::Open($archive,[IO.Compression.ZipArchiveMode]::Create)
    try { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$FixtureOgg,'Sample.ogg') | Out-Null }
    finally { $zip.Dispose() }
    $manifest=Join-Path $temp 'ogg.ast'
    [IO.File]::WriteAllText($manifest,('ARCHIVE_STREAM_NATIVE_TRACK_V1'+[char]10+$archive+'.archive_stream|0|Sample.ogg'+[char]10),(New-Object Text.UTF8Encoding($false)))
    $exe=Join-Path $temp 'ogg_pcm_parity.exe'
    $cmd='call "'+$VcVars32+'" >nul && cl /nologo /MD /EHsc /std:c++17 /W3 /O2 /I "'+$BassCoreSdkRoot+'" '+
        '/Fe:"'+$exe+'" "'+$source+'" /link /LIBPATH:"'+$BassCoreSdkRoot+'" bass.lib'
    & cmd.exe /d /s /c $cmd
    if ($LASTEXITCODE -ne 0) { throw 'BASS OGG PCM capture compilation failed' }
    Push-Location $temp
    try {
        & $exe (Join-Path $temp 'bass_archive_stream.dll') $manifest (Join-Path $temp 'archive-first.pcm') (Join-Path $temp 'archive-seek.pcm') (Join-Path $temp 'archive-full.pcm')
        if ($LASTEXITCODE -ne 0) { throw "Archive OGG BASS PCM capture failed: $LASTEXITCODE" }
        & $exe (Join-Path $temp 'bass_archive_stream.dll') $FixtureOgg (Join-Path $temp 'source-first.pcm') (Join-Path $temp 'source-seek.pcm') (Join-Path $temp 'source-full.pcm')
        if ($LASTEXITCODE -ne 0) { throw "Source OGG BASS PCM capture failed: $LASTEXITCODE" }
    }
    finally { Pop-Location }
    $env:AST_OGG_PARITY_FOLDER=$temp
    $verify=@'
from pathlib import Path
import os,hashlib
r=Path(os.environ["AST_OGG_PARITY_FOLDER"])
for segment in ("first","seek"):
    a=(r/("archive-"+segment+".pcm")).read_bytes()
    b=(r/("source-"+segment+".pcm")).read_bytes()
    assert a==b, "Vorbis compressed archive PCM mismatch in "+segment
digest=hashlib.sha256()
total=0
with (r/"archive-full.pcm").open("rb") as a, (r/"source-full.pcm").open("rb") as b:
    while True:
        ref=b.read(1048576)
        if not ref:
            break
        got=a.read(len(ref))
        assert got==ref, "Vorbis archive PCM corruption at byte "+str(total)
        digest.update(got)
        total+=len(got)
    assert not a.read(1),"Vorbis archived decode longer than source"
print("MUSICBEE_NATIVE_OGG_FULL_PCM_PARITY_PASS bytes="+str(total)+" sha256="+digest.hexdigest())
'@
    $test=Join-Path $temp 'verify_ogg.py'
    [IO.File]::WriteAllText($test,$verify,(New-Object Text.UTF8Encoding($false)))
    & python $test
    if ($LASTEXITCODE -ne 0) { throw 'OGG native full PCM mismatch' }
}
finally {
    Remove-Item Env:AST_OGG_PARITY_FOLDER -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
