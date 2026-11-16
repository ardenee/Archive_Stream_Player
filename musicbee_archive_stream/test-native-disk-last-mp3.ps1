param(
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$FixtureWav = 'C:\audiotest\04.WAVFile.wav',
    [string]$LameExe = 'C:\Program Files (x86)\MusicBee\Codec\lame.exe',
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$SevenZipExe = 'C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe = 'C:\Program Files\WinRAR\Rar.exe',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$addon = Join-Path $root 'Release\bass_archive_stream.dll'
foreach ($path in @($FixtureWav, $LameExe, $addon, $VcVars32, $SevenZipExe, $RarExe,
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
    $longWav = Join-Path $temp 'long.wav'
    $longMp3 = Join-Path $temp 'long.mp3'
    $env:QA_SOURCE_WAV = $FixtureWav
    $env:QA_LONG_WAV = $longWav
    # Seven copies of the original PCM source are re-encoded as one
    # continuous 320 kbps CBR MP3. Xing/VBR metadata is therefore accurate
    # for the entire long track, unlike concatenating pre-encoded MP3 files.
    python -c "import wave,os;src=wave.open(os.environ['QA_SOURCE_WAV'],'rb');p=src.getparams();assert p.sampwidth==2 and p.nchannels in (1,2) and p.comptype=='NONE';pcm=src.readframes(src.getnframes());src.close();out=wave.open(os.environ['QA_LONG_WAV'],'wb');out.setparams(p);out.writeframes(pcm*7);out.close();print('MP3_LATE_SEEK_SOURCE_PCM_BYTES',len(pcm)*7,'frames',p.nframes*7)"
    if ($LASTEXITCODE -ne 0) { throw 'Unable to generate continuous PCM source for long MP3 test' }
    & $LameExe '--silent' '-b' '320' '--cbr' $longWav $longMp3
    if ($LASTEXITCODE -ne 0) { throw 'Unable to encode one continuous 320-kbps MP3 long-track fixture' }
    Remove-Item -LiteralPath $longWav -Force
    Write-Output ('MP3_DISK_ENCODED_SOURCE_BYTES ' + (Get-Item $longMp3).Length)
    $mp3Bytes = (Get-Item $longMp3).Length
    if ($mp3Bytes -le ([long]$memory * 1MB) -or $mp3Bytes -gt ([long]$cacheLimit * 1MB / 2)) {
        throw "Large MP3 fixture outside RAM/cache test budget: bytes=$mp3Bytes RAM_MB=$memory cache_MB=$cacheLimit"
    }
    Copy-Item -LiteralPath $longMp3 -Destination (Join-Path $temp 'a.mp3') -Force
    Copy-Item -LiteralPath $longMp3 -Destination (Join-Path $temp 'b.mp3') -Force
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
        & $SevenZipExe a '-t7z' '-mx=1' '-ms=1024m' 'solid.7z' 'a.mp3' 'b.mp3' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Solid 7z two-MP3 fixture creation failed' }
        $archiveMeta = (& $SevenZipExe l -slt 'solid.7z' | Out-String)
        if ($LASTEXITCODE -ne 0 -or -not $archiveMeta.Contains('Blocks = 1')) { throw 'QA requires exactly one 7z solid block' }
        & $RarExe a '-s' '-m1' 'solid.rar' 'a.mp3' 'b.mp3' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Solid RAR two-MP3 fixture creation failed' }
        $sourceHash = (Get-FileHash -LiteralPath $longMp3 -Algorithm SHA256).Hash
        foreach ($type in @('7z','rar')) {
            $archive = Join-Path $temp "solid.$type"
            $legacy = Join-Path $temp "legacy-$type"
            $selected = Join-Path $temp "selected-$type"
            & $scanner $testDll $archive $legacy 0 buffered
            $legacyExit = $LASTEXITCODE
            if ($legacyExit -ne 0) { throw "Legacy buffered scanner unexpectedly accepted oversized solid $type MP3" }
            & $scanner $testDll $archive (Join-Path $temp "legacy-disk-$type") 0 disk
            if ($LASTEXITCODE -ne 0) { throw "v1.9 FLAC-only scanner unexpectedly accepted oversized solid $type MP3" }
            & $scanner $testDll $archive (Join-Path $temp "legacy-diskpcm-$type") 0 diskpcm
            if ($LASTEXITCODE -ne 0) { throw "v1.10 FLAC/WAV-only scanner unexpectedly accepted oversized solid $type MP3" }
            & $scanner $testDll $archive $selected 2 diskmp3
            if ($LASTEXITCODE -ne 0) { throw "New disk-last ABI failed to accept large solid $type MP3" }
            $cacheFolder = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\ManagedCache'
            $cached = @(Get-ChildItem -LiteralPath $cacheFolder -Filter '*.mp3' -File -ErrorAction SilentlyContinue)
            if ($cached.Count -lt 1) { throw "No cached MP3 file published after solid $type scan" }
            foreach ($file in $cached) {
                if ($file.Length -ne $mp3Bytes -or (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ne $sourceHash) {
                    throw "Native solid $type cache bytes differ from source MP3"
                }
            }
            $firstCacheCount = $cached.Count
            $repeatFolder = Join-Path $temp "repeat-$type"
            & $scanner $testDll $archive $repeatFolder 2 diskmp3
            if ($LASTEXITCODE -ne 0) { throw "Managed cache reuse scanner failed for solid $type" }
            $cachedAgain = @(Get-ChildItem -LiteralPath $cacheFolder -Filter '*.mp3' -File)
            if ($cachedAgain.Count -ne $firstCacheCount) { throw "Managed cache unexpectedly duplicated data on repeat scan" }
            $cacheRoutes = 0
            $streamRoutes = 0
            foreach ($manifest in @(Get-ChildItem -LiteralPath $selected -Filter '*.ast' -File)) {
                $env:AST_BASS_ADDON_LOG = Join-Path $temp ($type + '-' + $manifest.BaseName + '.log')
                Remove-Item -LiteralPath $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
                & $decoder $testDll $manifest.FullName late | Select-String 'BASS_ARCHIVE'
                if ($LASTEXITCODE -ne 0) { throw "Solid $type native cache MP3 BASS PCM/seek failed: $($manifest.Name)" }
                $trace = [IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)
                if ($trace.Contains('PCM_FIRST route=CACHE')) { ++$cacheRoutes }
                elseif ($trace.Contains('PCM_FIRST route=STREAM')) { ++$streamRoutes }
                else { throw "Unexpected native route for large solid $type MP3: $($manifest.Name)" }
                if (!$trace.Contains('SEEK_OK')) { throw "Native MP3 seek missing in solid $type" }
            }
            if ($type -eq '7z' -and $cacheRoutes -ne 2) { throw "Solid 7z must cache both large MP3 tracks, got $cacheRoutes" }
            if ($type -eq 'rar' -and $cacheRoutes -lt 1) { throw 'Solid RAR never reached native disk-last recovery' }
            Write-Output "MUSICBEE_NATIVE_DISK_LAST_MP3_PASS archive=$type tracks=2 cache=$cacheRoutes stream=$streamRoutes cached_files=$firstCacheCount encoded_bytes=$mp3Bytes bytes_exact=1 seek=1 reused=1 http=0"
        }
    }
    finally { Pop-Location }
    Write-Output 'MUSICBEE_NATIVE_SHARED_DISK_LAST_MP3_7Z_RAR_PASS'
}
finally {
    $env:LOCALAPPDATA = $oldLocalAppData
    Remove-Item Env:AST_BASS_ADDON_LOG,Env:QA_SOURCE_WAV,Env:QA_LONG_WAV -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
