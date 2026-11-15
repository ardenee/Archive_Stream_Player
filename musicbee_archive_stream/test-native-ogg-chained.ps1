param(
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$FixtureOgg = 'C:\audiotest\02.OGGFile.ogg',
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
foreach ($file in @((Join-Path $root 'test_native_ogg_chain_fixture.py'),(Join-Path $root 'test_native_wav_parity.cpp'),
    (Join-Path $root 'Release\bass_archive_stream.dll'),(Join-Path $MusicBeeRoot 'bass.dll'),
    (Join-Path $BassCoreSdkRoot 'bass.h'),(Join-Path $BassCoreSdkRoot 'bass.lib'),$VcVars32,$FixtureOgg)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing chained OGG test dependency: $file" }
}
$temp=Join-Path $env:TEMP ('ArchiveStreamChainedOgg-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    Copy-Item (Join-Path $root 'Release\bass_archive_stream.dll') $temp
    Copy-Item (Join-Path $MusicBeeRoot 'bass.dll') $temp
    & python (Join-Path $root 'test_native_ogg_chain_fixture.py') build $FixtureOgg $temp
    if ($LASTEXITCODE -ne 0) { throw 'Chained OGG fixture creation failed' }
    $exe=Join-Path $temp 'test_chained_ogg_bass.exe'
    $cmd='call "' + $VcVars32 + '" >nul && cl /nologo /MD /EHsc /std:c++17 /W3 /O2 ' +
        '/I "' + $BassCoreSdkRoot + '" /Fe:"' + $exe + '" "' + (Join-Path $root 'test_native_wav_parity.cpp') + '" ' +
        '/link /LIBPATH:"' + $BassCoreSdkRoot + '" bass.lib'
    & cmd.exe /d /s /c $cmd
    if ($LASTEXITCODE -ne 0) { throw 'Chained OGG BASS test compilation failed' }
    Push-Location $temp
    try {
        & $exe (Join-Path $temp 'bass_archive_stream.dll') $FixtureOgg (Join-Path $temp 'single-first.pcm') (Join-Path $temp 'single-seek.pcm') (Join-Path $temp 'single-full.pcm')
        if ($LASTEXITCODE -ne 0) { throw 'Original OGG PCM capture failed' }
        & $exe (Join-Path $temp 'bass_archive_stream.dll') (Join-Path $temp 'chained.ast') (Join-Path $temp 'chain-first.pcm') (Join-Path $temp 'chain-seek.pcm') (Join-Path $temp 'chain-full.pcm') '240'
        if ($LASTEXITCODE -ne 0) { throw 'Chained OGG PCM and second-link native seek failed' }
    }
    finally { Pop-Location }
    & python (Join-Path $root 'test_native_ogg_chain_fixture.py') verify $FixtureOgg $temp
    if ($LASTEXITCODE -ne 0) { throw 'Chained OGG full PCM or second-link seek mismatch' }
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
