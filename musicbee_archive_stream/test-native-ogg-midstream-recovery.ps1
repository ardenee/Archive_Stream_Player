param(
    [Parameter(Mandatory=$true)][string]$BassAddonSdkRoot,
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat',
    [ValidateSet('zip','7z','rar')][string]$ArchiveKind = 'zip',
    [string]$SevenZipExe = 'C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe = 'C:\Program Files\WinRAR\Rar.exe'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$temp = Join-Path $env:TEMP ('ArchiveStreamOggMidstream-' + [Guid]::NewGuid().ToString('N'))
$previousLocal = $env:LOCALAPPDATA
$previousFault = $env:AST_NATIVE_TEST_STREAM_FAIL_AFTER
$previousLimit = $env:AST_NATIVE_TEST_MEMORY_MB
$previousLog = $env:AST_BASS_ADDON_LOG
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    $env:LOCALAPPDATA = Join-Path $temp 'isolated-local-appdata'
    New-Item -ItemType Directory -Path $env:LOCALAPPDATA -Force | Out-Null
    $addon = Join-Path $temp 'bass_archive_stream.dll'
    & (Join-Path $root 'build-native-bass.ps1') -BassAddonSdkRoot $BassAddonSdkRoot -BassCoreSdkRoot $BassCoreSdkRoot -OutputPath $addon -TestFaultInjection
    if ($LASTEXITCODE -ne 0 -or !(Test-Path $addon)) { throw 'OGG test-only BASS build failed' }
    Copy-Item -LiteralPath (Join-Path $MusicBeeRoot 'bass.dll') -Destination $temp -Force
    $probe = Join-Path $temp 'recover_probe.exe'
    $cmd = 'call "' + $VcVars32 + '" >nul && cl /nologo /MD /EHsc /std:c++17 /W3 /O2 ' +
        '/I "' + $BassCoreSdkRoot + '" /Fe:"' + $probe + '" "' +
        (Join-Path $root 'test_native_midstream_recovery.cpp') +
        '" /link /LIBPATH:"' + $BassCoreSdkRoot + '" bass.lib'
    Push-Location $temp
    try {
        & cmd.exe /d /s /c $cmd
        if ($LASTEXITCODE -ne 0) { throw 'OGG PCM capture harness build failed' }
    }
    finally { Pop-Location }
    $chain = Join-Path $temp 'chained.ogg'
    & python (Join-Path $root 'test_native_ogg_long_chain.py') 'C:\audiotest\02.OGGFile.ogg' $chain 2
    if ($LASTEXITCODE -ne 0) { throw 'OGG chain generator failed' }
    $env:OGG_TEST_FOLDER = $temp
    & python -c "import os,pathlib,zipfile;root=pathlib.Path(os.environ['OGG_TEST_FOLDER']);src=root/'chained.ogg';assert src.stat().st_size>2000000;[(lambda tag:(lambda z:(z.write(src,'source.ogg'),z.close()))(zipfile.ZipFile(root/(tag+'.zip'),'w',compression=zipfile.ZIP_STORED)))(tag) for tag in ('ogg-buffer','ogg-cache')];[(root/(tag+'.ast')).write_bytes(('ARCHIVE_STREAM_NATIVE_TRACK_V1\n'+str(root/(tag+'.zip'))+'.archive_stream|0|source.ogg\n').encode('utf-8')) for tag in ('ogg-buffer','ogg-cache')];print('OGG_MIDSTREAM_TEST_ARCHIVES_CREATED bytes',src.stat().st_size)"
    if ($LASTEXITCODE -ne 0) { throw 'Failed to create OGG chain ZIP manifests' }
    if ($ArchiveKind -ne 'zip') {
        Copy-Item -LiteralPath $chain -Destination (Join-Path $temp 'source.ogg')
        Push-Location $temp
        try {
            foreach ($tag in @('ogg-buffer','ogg-cache')) {
                $file = $tag + '.' + $ArchiveKind
                if ($ArchiveKind -eq '7z') {
                    & $SevenZipExe a '-t7z' '-mx=1' '-ms=off' $file 'source.ogg' | Out-Null
                }
                else {
                    & $RarExe a '-s-' '-m1' $file 'source.ogg' | Out-Null
                }
                if ($LASTEXITCODE -ne 0) { throw "Could not create non-solid archive: $file" }
                $manifest = Join-Path $temp ($tag + '.ast')
                [IO.File]::WriteAllText($manifest, 'ARCHIVE_STREAM_NATIVE_TRACK_V1' + [char]10 +
                    (Join-Path $temp $file) + '.archive_stream|0|source.ogg' + [char]10,
                    (New-Object Text.UTF8Encoding($false)))
            }
        }
        finally { Pop-Location }
        Write-Output "OGG_MIDSTREAM_NONSOLID_ARCHIVES_CREATED kind=$ArchiveKind"
    }
    foreach ($case in @(@('ogg-buffer','BUFFER'),@('ogg-cache','CACHE'))) {
        $tag = $case[0]
        $route = $case[1]
        $manifest = Join-Path $temp ($tag + '.ast')
        $baseline = Join-Path $temp ($tag + '-baseline.pcm')
        $recovered = Join-Path $temp ($tag + '-recovered.pcm')
        $env:AST_BASS_ADDON_LOG = Join-Path $temp ($tag + '-trace.log')
        Remove-Item Env:AST_NATIVE_TEST_STREAM_FAIL_AFTER, Env:AST_NATIVE_TEST_MEMORY_MB -ErrorAction SilentlyContinue
        Remove-Item $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
        Push-Location $temp
        try { & $probe $addon $manifest $baseline | Select-String 'RECOVER_NATIVE_FULL_PCM_PASS|FAIL' }
        finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "OGG uninterrupted baseline failed $tag" }
        $trace = [IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)
        if ($trace.Contains('RECOVERY_START')) { throw "Unexpected OGG recovery without failure: $tag" }
        $env:AST_NATIVE_TEST_STREAM_FAIL_AFTER = '1048576'
        if ($route -eq 'CACHE') { $env:AST_NATIVE_TEST_MEMORY_MB = '1' }
        Remove-Item $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
        Push-Location $temp
        try { & $probe $addon $manifest $recovered | Select-String 'RECOVER_NATIVE_FULL_PCM_PASS|FAIL' }
        finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) {
            if (Test-Path $env:AST_BASS_ADDON_LOG) { Get-Content $env:AST_BASS_ADDON_LOG | Select-Object -Last 35 }
            throw "Native OGG compressed stream fault recovery failed $tag"
        }
        $trace = [IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)
        if (!$trace.Contains('PCM_FIRST route=STREAM') -or
            !$trace.Contains('RECOVERY_TEST_FAULT codec=OGG') -or
            !$trace.Contains('RECOVERY_START codec=OGG') -or
            !$trace.Contains('RECOVERY_OK codec=OGG route=' + $route)) {
            Write-Output ($trace -split [char]10 | Select-Object -Last 35)
            throw "Expected OGG STREAM -> $route recovery trace missing: $tag"
        }
        $a = Get-Item -LiteralPath $baseline
        $b = Get-Item -LiteralPath $recovered
        if ($a.Length -ne $b.Length -or $a.Length -lt 1000000 -or
            (Get-FileHash -LiteralPath $baseline -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $recovered -Algorithm SHA256).Hash) {
            throw "Chained OGG decoded PCM differs across recovery: $tag"
        }
        if ($route -eq 'CACHE') {
            $cached = @(Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\ManagedCache') -Filter '*.ogg' -File -ErrorAction SilentlyContinue)
            if (!$cached.Count) { throw 'OGG cache route did not publish encoded file' }
            $sha = (Get-FileHash -LiteralPath $chain -Algorithm SHA256).Hash
            if (@($cached | Where-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -eq $sha }).Count -eq 0) {
                throw 'OGG cached encoded bytes differ from source'
            }
        }
        Write-Output "MUSICBEE_OGG_MIDSTREAM_RECOVERY_PCM_EXACT_PASS route=$route bytes=$($a.Length) sha256_equal=1 chain_links=2 http=0"
        # Independently compare recovery after a real BASS seek, starting
        # inside the first link and seeking to 75% (inside the second link).
        $seekBase = Join-Path $temp ($tag + '-seek-base.pcm')
        $seekFault = Join-Path $temp ($tag + '-seek-fault.pcm')
        Remove-Item Env:AST_NATIVE_TEST_STREAM_FAIL_AFTER, Env:AST_NATIVE_TEST_MEMORY_MB -ErrorAction SilentlyContinue
        Push-Location $temp
        try { & $probe $addon $manifest $seekBase midseek | Select-String 'RECOVER_NATIVE_MIDSEEK_PASS|FAIL' }
        finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "BASS 75% baseline seek failed $tag" }
        $env:AST_NATIVE_TEST_STREAM_FAIL_AFTER = '1048576'
        if ($route -eq 'CACHE') { $env:AST_NATIVE_TEST_MEMORY_MB = '1' }
        Remove-Item $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
        Push-Location $temp
        try { & $probe $addon $manifest $seekFault midseek | Select-String 'RECOVER_NATIVE_MIDSEEK_PASS|FAIL' }
        finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "BASS 75% recovered seek failed $tag" }
        $seekTrace = [IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)
        if (!$seekTrace.Contains('RECOVERY_OK codec=OGG route=' + $route) -or
            !$seekTrace.Contains('SEEK_OK')) {
            Write-Output ($seekTrace -split [char]10 | Select-Object -Last 25)
            throw "OGG post-recovery BASS cross-link seek was not verified: $tag"
        }
        if ((Get-Item $seekBase).Length -ne (Get-Item $seekFault).Length -or
            (Get-FileHash $seekBase -Algorithm SHA256).Hash -ne
            (Get-FileHash $seekFault -Algorithm SHA256).Hash) {
            throw "OGG post-recovery cross-link seek PCM mismatch: $tag"
        }
        Write-Output "MUSICBEE_OGG_MIDSTREAM_CROSSLINK_SEEK_PCM_EXACT_PASS route=$route sha256_equal=1"
    }
    Write-Output "MUSICBEE_OGG_MIDSTREAM_BUFFER_AND_CACHE_ALL_PASS archive=$ArchiveKind"
}
finally {
    $env:LOCALAPPDATA = $previousLocal
    foreach ($pair in @(@('AST_NATIVE_TEST_STREAM_FAIL_AFTER',$previousFault),
                       @('AST_NATIVE_TEST_MEMORY_MB',$previousLimit),
                       @('AST_BASS_ADDON_LOG',$previousLog))) {
        if ($null -eq $pair[1]) { Remove-Item ('Env:' + $pair[0]) -ErrorAction SilentlyContinue }
        else { Set-Item ('Env:' + $pair[0]) $pair[1] }
    }
    Remove-Item Env:OGG_TEST_FOLDER -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
