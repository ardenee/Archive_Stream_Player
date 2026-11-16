param(
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat',
    [ValidateSet('aac','m4a','wma','all')][string]$Codec = 'all',
    [ValidateSet('zip','7z','rar')][string]$ArchiveKind = 'zip',
    [string]$SevenZipExe = 'C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe = 'C:\Program Files\WinRAR\Rar.exe',
    [switch]$TestBlocked
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$temp=Join-Path $env:TEMP ('ArchiveStreamMFMidstream-'+[Guid]::NewGuid().ToString('N'))
$oldLocal=$env:LOCALAPPDATA
$oldFail=$env:AST_MF_TEST_STREAM_FAIL_AFTER
$oldMem=$env:AST_MF_TEST_MEMORY_MB
New-Item -Path $temp -ItemType Directory -Force | Out-Null
try {
    $env:LOCALAPPDATA=Join-Path $temp 'isolated-local'
    New-Item $env:LOCALAPPDATA -ItemType Directory -Force | Out-Null
    $obj=Join-Path $temp 'mf_archive_bytestream_fault.obj'
    $exe=Join-Path $temp 'mf_midstream.exe'
    $buildObj='call "'+$VcVars32+'" >nul && cl /nologo /c /MD /EHsc /std:c++20 /O2 /DNDEBUG '+
        '/DAST_MF_TEST_FAULT_INJECTION /I "archive_backend" /I "shared" '+
        '/Fo:"'+$obj+'" archive_backend\mf_archive_bytestream.cpp'
    $buildExe='call "'+$VcVars32+'" >nul && cl /nologo /MD /EHsc /std:c++20 /W3 /O2 '+
        '/I "archive_backend" /I "shared" /I "third_party\dr_libs" '+
        '/Fe:"'+$exe+'" musicbee_archive_stream\test_native_mf_midstream.cpp shared\debug_log.cpp "'+$obj+'" '+
        '/link /LTCG /LIBPATH:"archive_backend\Release" /LIBPATH:"unrar_static\Release" '+
        '/LIBPATH:"libarchive_static\Release" /LIBPATH:"zlib_static\Release" '+
        'archive_backend.lib unrar_static.lib libarchive_static.lib zlib_static.lib '+
        'mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib propsys.lib bcrypt.lib Advapi32.lib Shell32.lib User32.lib'
    Push-Location $root
    try {
        & cmd.exe /d /s /c $buildObj
        if($LASTEXITCODE -ne 0){throw 'Test-only MF byte-stream object build failed'}
        & cmd.exe /d /s /c $buildExe
        if($LASTEXITCODE -ne 0){throw 'Standalone MF compressed-source recovery PCM test harness build failed'}
    }
    finally {Pop-Location}
    $mapping=@{aac='07.AACFile.aac';m4a='06.M4AFile.m4a';wma='05.WMAFile.wma'}
    $codecs=if($Codec -eq 'all'){@('aac','m4a','wma')}else{@($Codec)}
    foreach($name in $codecs) {
        $source=Join-Path 'C:\audiotest' $mapping[$name]
        if(!(Test-Path $source)){throw "Fixture missing $source"}
        $archive=Join-Path $temp ($name+'.'+$ArchiveKind)
        if ($ArchiveKind -eq 'zip') {
            Add-Type -AssemblyName System.IO.Compression
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $zip=[IO.Compression.ZipFile]::Open($archive,[IO.Compression.ZipArchiveMode]::Create)
            try {[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$source,('file.'+$name),[IO.Compression.CompressionLevel]::NoCompression)|Out-Null}
            finally {$zip.Dispose()}
        }
        else {
            $member='file.'+$name
            Copy-Item -LiteralPath $source -Destination (Join-Path $temp $member)
            Push-Location $temp
            try {
                if ($ArchiveKind -eq '7z') {
                    & $SevenZipExe a '-t7z' '-mx=1' '-ms=off' $archive $member | Out-Null
                }
                else {
                    & $RarExe a '-s-' '-m1' $archive $member | Out-Null
                }
                if ($LASTEXITCODE -ne 0) { throw "Creating $ArchiveKind streaming MF archive failed $name" }
            }
            finally { Pop-Location }
        }
        foreach($route in $(if($TestBlocked){@('blocked')}else{@('buffer','cache')})) {
            $log=Join-Path $temp ("$name-$route.log")
            & $exe $source $name $archive $route $log
            if($LASTEXITCODE -ne 0){if(Test-Path $log){Get-Content $log | Select-Object -Last 24};throw "MF PCM recovery failed codec=$name route=$route"}
            $t=[IO.File]::ReadAllText($log)
            if($route -eq 'blocked') {
                if(!$t.Contains('TEST_FAULT') -or !$t.Contains('RECOVERY_BEGIN') -or
                    !$t.Contains('RECOVERY_FAILED') -or $t.Contains('RECOVERY_OK')) {
                    Write-Output ($t -split [char]10 | Select-Object -Last 35)
                    throw "Expected one-shot fail-closed MF recovery did not occur codec=$name"
                }
                Write-Output "MUSICBEE_MF_MIDSTREAM_NO_DISK_ALLOWED_FAIL_CLOSED_PASS codec=$name"
                continue
            }
            if(!$t.Contains('TEST_FAULT') -or !$t.Contains('RECOVERY_BEGIN') -or !$t.Contains('RECOVERY_OK') -or !$t.Contains(('route='+$route.ToUpperInvariant()))) {
                Write-Output ($t -split [char]10 | Select-Object -Last 35)
                throw "MF fault-injection did not recover during PCM decoding codec=$name route=$route"
            }
            if($route -eq 'cache'){
                $cacheDir=Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\ManagedCache'
                $expected=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
                $match=@(Get-ChildItem -LiteralPath $cacheDir -Filter ('*.'+$name) -File -ErrorAction SilentlyContinue |
                    Where-Object { $_.Length -eq (Get-Item $source).Length -and
                        (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -eq $expected })
                if(!$match.Count){throw "MF encoded cache integrity failed codec=$name"}
            }
            Write-Output "MUSICBEE_MF_MIDSTREAM_PCM_EXACT_PASS codec=$name route=$route archive=$ArchiveKind sha256_encoded_cache=verified http=0"
        }
    }
    if($TestBlocked) {
        Write-Output 'MUSICBEE_NATIVE_MF_MIDSTREAM_RECOVERY_FAIL_CLOSED_PASS'
    }
    else {
        Write-Output 'MUSICBEE_NATIVE_MF_MIDSTREAM_ALL_CODECS_BUFFER_CACHE_PCM_PASS'
    }
}
finally {
    $env:LOCALAPPDATA=$oldLocal
    if($null -eq $oldFail){Remove-Item Env:AST_MF_TEST_STREAM_FAIL_AFTER -ErrorAction SilentlyContinue}else{$env:AST_MF_TEST_STREAM_FAIL_AFTER=$oldFail}
    if($null -eq $oldMem){Remove-Item Env:AST_MF_TEST_MEMORY_MB -ErrorAction SilentlyContinue}else{$env:AST_MF_TEST_MEMORY_MB=$oldMem}
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
