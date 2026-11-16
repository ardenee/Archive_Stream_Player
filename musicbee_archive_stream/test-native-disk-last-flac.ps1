param(
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$FixtureFlac = 'C:\audiotest\03.FlacFile.flac',
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$SevenZipExe = 'C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe = 'C:\Program Files\WinRAR\Rar.exe',
    [string]$FlacExe = 'C:\Program Files (x86)\MusicBee\Codec\flac.exe',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$addon = Join-Path $root 'Release\bass_archive_stream.dll'
foreach ($path in @($FixtureFlac, $addon, $VcVars32, $SevenZipExe, $RarExe, $FlacExe,
    (Join-Path $BassCoreSdkRoot 'bass.h'), (Join-Path $BassCoreSdkRoot 'bass.lib'),
    (Join-Path $MusicBeeRoot 'bass.dll'), (Join-Path $root 'test_bass_archive.cpp'),
    (Join-Path $root 'test_native_inprocess_enum.cpp'))) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Native MusicBee disk-last test needs: $path" }
}
$temp = Join-Path $env:TEMP ('ArchiveStreamNativeDiskLast-' + [Guid]::NewGuid().ToString('N'))
$oldLocalAppData = $env:LOCALAPPDATA
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    $env:LOCALAPPDATA = Join-Path $temp 'isolated-local-app-data'
    New-Item -ItemType Directory -Path $env:LOCALAPPDATA -Force | Out-Null
    $memory = 50
    $cacheLimit = 2048
    $settings = Get-ItemProperty 'HKCU:\Software\ArchiveStreamPlayer\Adapters' -ErrorAction SilentlyContinue
    if ($settings) {
        if ($settings.MemoryMB -ge 1 -and $settings.MemoryMB -le 512) { $memory = [int]$settings.MemoryMB }
        if ($null -ne $settings.CacheLimitMB) { $cacheLimit = [int]$settings.CacheLimitMB }
    }
    if ($memory -ge 256 -or $cacheLimit -lt 256) {
        throw "QA requires configured MemoryMB < 256 and CacheLimitMB >= 256 (MemoryMB=$memory CacheLimitMB=$cacheLimit); did not change registry"
    }
    $decodedWav = Join-Path $temp 'source.wav'
    $longWav = Join-Path $temp 'long.wav'
    $longFlac = Join-Path $temp 'long.flac'
    & $FlacExe '-d' '-f' '-s' '-o' $decodedWav $FixtureFlac
    if ($LASTEXITCODE -ne 0) { throw 'Unable to decode FLAC fixture for long disk-cache case' }
    $env:QA_SOURCE_WAV = $decodedWav
    $env:QA_LONG_WAV = $longWav
    python -c "import wave,os;src=wave.open(os.environ['QA_SOURCE_WAV'],'rb');params=src.getparams();pcm=src.readframes(src.getnframes());src.close();out=wave.open(os.environ['QA_LONG_WAV'],'wb');out.setparams(params);out.writeframes(pcm*2);out.close();print('SOLID_DISK_PCM_SOURCE_BYTES',len(pcm)*2)"
    if ($LASTEXITCODE -ne 0) { throw 'Unable to generate doubled FLAC PCM fixture' }
    & $FlacExe '-f' '-s' '-o' $longFlac $longWav
    if ($LASTEXITCODE -ne 0) { throw 'Unable to encode large valid FLAC fixture' }
    $flacBytes = (Get-Item $longFlac).Length
    if ($flacBytes -le ([long]$memory * 1MB) -or $flacBytes -gt ([long]$cacheLimit * 1MB / 2)) {
        throw "Large FLAC fixture is not above configured RAM and inside cache budget: bytes=$flacBytes RAM_MB=$memory cache_MB=$cacheLimit"
    }
    Remove-Item -LiteralPath $decodedWav,$longWav -Force
    Copy-Item -LiteralPath $longFlac -Destination (Join-Path $temp 'a.flac') -Force
    Copy-Item -LiteralPath $longFlac -Destination (Join-Path $temp 'b.flac') -Force
    Copy-Item -LiteralPath $addon -Destination $temp
    Copy-Item -LiteralPath (Join-Path $MusicBeeRoot 'bass.dll') -Destination $temp
    $testDll = Join-Path $temp 'bass_archive_stream.dll'
    $scanner = Join-Path $temp 'disk_enum.exe'
    $decoder = Join-Path $temp 'disk_decode.exe'
    foreach ($pair in @(@($scanner, 'test_native_inprocess_enum.cpp'), @($decoder, 'test_bass_archive.cpp'))) {
        $cmd = 'call "' + $VcVars32 + '" >nul && cl /nologo /MD /EHsc /std:c++17 /W3 /O2 ' +
            '/I "' + $BassCoreSdkRoot + '" /Fe:"' + $pair[0] + '" "' +
            (Join-Path $root $pair[1]) + '" /link /LIBPATH:"' + $BassCoreSdkRoot + '" bass.lib'
        & cmd.exe /d /s /c $cmd
        if ($LASTEXITCODE -ne 0) { throw "Disk-last harness compile failed: $($pair[1])" }
    }
    Push-Location $temp
    try {
        & $SevenZipExe a '-t7z' '-mx=1' '-ms=1024m' 'solid.7z' 'a.flac' 'b.flac' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Solid 7z two-FLAC fixture creation failed' }
        $archiveMeta = (& $SevenZipExe l -slt 'solid.7z' | Out-String)
        if ($LASTEXITCODE -ne 0 -or -not $archiveMeta.Contains('Blocks = 1')) { throw 'QA requires exactly one 7z solid block' }
        & $RarExe a '-s' '-m1' 'solid.rar' 'a.flac' 'b.flac' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Solid RAR two-FLAC fixture creation failed' }
        $sourceHash = (Get-FileHash -LiteralPath $longFlac -Algorithm SHA256).Hash
        foreach ($type in @('7z','rar')) {
            $archive = Join-Path $temp "solid.$type"
            $legacy = Join-Path $temp "legacy-$type"
            $selected = Join-Path $temp "selected-$type"
            & $scanner $testDll $archive $legacy 0 buffered
            $legacyExit = $LASTEXITCODE
            if ($legacyExit -ne 0) { throw "Legacy buffered scanner unexpectedly accepted oversized solid $type FLAC" }
            & $scanner $testDll $archive $selected 2 disk
            if ($LASTEXITCODE -ne 0) { throw "New disk-last ABI failed to accept large solid $type FLAC" }
            $cacheFolder = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\ManagedCache'
            $cached = @(Get-ChildItem -LiteralPath $cacheFolder -Filter '*.flac' -File -ErrorAction SilentlyContinue)
            if ($cached.Count -lt 1) { throw "No cached FLAC file published after solid $type scan" }
            foreach ($file in $cached) {
                if ($file.Length -ne $flacBytes -or (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ne $sourceHash) {
                    throw "Native solid $type cache bytes differ from source FLAC"
                }
            }
            $firstCacheCount = $cached.Count
            $repeatFolder = Join-Path $temp "repeat-$type"
            & $scanner $testDll $archive $repeatFolder 2 disk
            if ($LASTEXITCODE -ne 0) { throw "Managed cache reuse scanner failed for solid $type" }
            $cachedAgain = @(Get-ChildItem -LiteralPath $cacheFolder -Filter '*.flac' -File)
            if ($cachedAgain.Count -ne $firstCacheCount) { throw "Managed cache unexpectedly duplicated data on repeat scan" }
            $cacheRoutes = 0
            $streamRoutes = 0
            foreach ($manifest in @(Get-ChildItem -LiteralPath $selected -Filter '*.ast' -File)) {
                $env:AST_BASS_ADDON_LOG = Join-Path $temp ($type + '-' + $manifest.BaseName + '.log')
                Remove-Item -LiteralPath $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
                & $decoder $testDll $manifest.FullName | Select-String 'BASS_ARCHIVE_.*PASS'
                if ($LASTEXITCODE -ne 0) { throw "Solid $type native cache FLAC BASS PCM/seek failed: $($manifest.Name)" }
                $trace = [IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)
                if ($trace.Contains('PCM_FIRST route=CACHE')) { ++$cacheRoutes }
                elseif ($trace.Contains('PCM_FIRST route=STREAM')) { ++$streamRoutes }
                else { throw "Unexpected native route for large solid $type FLAC: $($manifest.Name)" }
                if (!$trace.Contains('SEEK_OK')) { throw "Native FLAC seek missing in solid $type" }
            }
            if ($type -eq '7z' -and $cacheRoutes -ne 2) { throw "Solid 7z must cache both large FLAC tracks, got $cacheRoutes" }
            if ($type -eq 'rar' -and $cacheRoutes -lt 1) { throw 'Solid RAR never reached native disk-last recovery' }
            Write-Output "MUSICBEE_NATIVE_DISK_LAST_FLAC_PASS archive=$type tracks=2 cache=$cacheRoutes stream=$streamRoutes cached_files=$firstCacheCount encoded_bytes=$flacBytes bytes_exact=1 seek=1 reused=1 http=0"
        }
    }
    finally { Pop-Location }
    Write-Output 'MUSICBEE_NATIVE_SHARED_DISK_LAST_FLAC_7Z_RAR_PASS'
}
finally {
    $env:LOCALAPPDATA = $oldLocalAppData
    Remove-Item Env:AST_BASS_ADDON_LOG,Env:QA_SOURCE_WAV,Env:QA_LONG_WAV -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
