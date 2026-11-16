param(
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$FixtureMp3 = 'C:\audiotest\01.MP3File.mp3',
    [string]$FixtureOgg = 'C:\audiotest\02.OGGFile.ogg',
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$SevenZipExe = 'C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe = 'C:\Program Files\WinRAR\Rar.exe',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference = 'Stop'
$folder = $PSScriptRoot
$addon = Join-Path $folder 'Release\bass_archive_stream.dll'
foreach ($path in @($FixtureMp3, $FixtureOgg, $addon, $VcVars32, $SevenZipExe, $RarExe,
    (Join-Path $BassCoreSdkRoot 'bass.h'), (Join-Path $BassCoreSdkRoot 'bass.lib'),
    (Join-Path $MusicBeeRoot 'bass.dll'), (Join-Path $folder 'test_bass_archive.cpp'),
    (Join-Path $folder 'test_native_inprocess_enum.cpp'))) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing solid archive QA prerequisite: $path" }
}
$temp = Join-Path $env:TEMP ('ArchiveStreamNativeSolid-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    Copy-Item -LiteralPath $addon -Destination $temp -Force
    Copy-Item -LiteralPath (Join-Path $MusicBeeRoot 'bass.dll') -Destination $temp -Force
    $bassDll = Join-Path $temp 'bass_archive_stream.dll'
    $scanner = Join-Path $temp 'solid_enum.exe'
    $decoder = Join-Path $temp 'solid_decode.exe'
    foreach ($exe in @(@($scanner, 'test_native_inprocess_enum.cpp'),
                       @($decoder, 'test_bass_archive.cpp'))) {
        $cmd = 'call "' + $VcVars32 + '" >nul && cl /nologo /MD /EHsc /std:c++17 /W3 /O2 ' +
            '/I "' + $BassCoreSdkRoot + '" /Fe:"' + $exe[0] + '" "' +
            (Join-Path $folder $exe[1]) + '" /link /LIBPATH:"' + $BassCoreSdkRoot + '" bass.lib'
        & cmd.exe /d /s /c $cmd
        if ($LASTEXITCODE -ne 0) { throw "Unable to compile solid QA harness: $($exe[1])" }
    }
    Copy-Item $FixtureMp3 (Join-Path $temp 'a.mp3') -Force
    Copy-Item $FixtureOgg (Join-Path $temp 'b.ogg') -Force
    Push-Location $temp
    try {
        & $SevenZipExe a '-t7z' '-m0=lzma2' '-ms=on' 'solid.7z' 'a.mp3' 'b.ogg' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Unable to create solid 7z MP3/Vorbis fixture' }
        & $RarExe a '-s' '-m3' 'solid.rar' 'a.mp3' 'b.ogg' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Unable to create solid RAR MP3/Vorbis fixture' }
        foreach ($ext in @('7z','rar')) {
            $archive = Join-Path $temp "solid.$ext"
            $native = Join-Path $temp "native-$ext"
            $legacy = Join-Path $temp "legacy-$ext"
            & $scanner $bassDll $archive $legacy 0 'seven'
            if ($LASTEXITCODE -ne 0) { throw "Legacy seven-codec ABI unexpectedly selected solid $ext" }
            & $scanner $bassDll $archive $native 2 'buffered'
            if ($LASTEXITCODE -ne 0) { throw "Buffered scanner did not select both solid $ext entries" }
            $manifests = @(Get-ChildItem -LiteralPath $native -Filter '*.ast' -File | Sort-Object Name)
            if ($manifests.Count -ne 2) { throw "Expected two native solid $ext tracks" }
            $buffered = 0
            $streamed = 0
            foreach ($manifest in $manifests) {
                $env:AST_BASS_ADDON_LOG = Join-Path $temp ($ext + '-' + $manifest.BaseName + '.log')
                Remove-Item -LiteralPath $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
                & $decoder $bassDll $manifest.FullName
                if ($LASTEXITCODE -ne 0) { throw "Native PCM/seek failed: $($manifest.Name) in $ext" }
                $log = [IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)
                if ($log.Contains('PCM_FIRST route=BUFFER')) { ++$buffered }
                elseif ($log.Contains('PCM_FIRST route=STREAM')) { ++$streamed }
                else { throw "Unknown solid $ext native PCM route: $($manifest.Name)" }
                if (!$log.Contains('SEEK_OK')) { throw "Native seek missing for solid $ext $($manifest.Name)" }
            }
            if ($ext -eq '7z' -and $buffered -ne 2) { throw "Solid 7z must use bounded RAM, not STREAM; BUFFER=$buffered" }
            if ($ext -eq 'rar' -and $buffered -lt 1) { throw "Solid RAR dependency entry did not use bounded RAM" }
            Write-Output "MUSICBEE_NATIVE_SOLID_MP3_OGG_PCM_SEEK_PASS archive=$ext tracks=2 buffer=$buffered stream=$streamed http=0 cache=0"
        }
    }
    finally { Pop-Location }
    Write-Output 'MUSICBEE_NATIVE_SOLID_BUFFER_STANDALONE_ALL_PASS'
}
finally {
    Remove-Item Env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
