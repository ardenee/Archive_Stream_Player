param(
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [ValidateSet('aac','m4a','wma')][string]$Codec = 'aac',
    [switch]$OnlyParity,
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$SevenZipExe = 'C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe = 'C:\Program Files\WinRAR\Rar.exe',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$sourceName = @{aac='07.AACFile.aac';m4a='06.M4AFile.m4a';wma='05.WMAFile.wma'}[$Codec]
$source=Join-Path 'C:\audiotest' $sourceName
$addon=Join-Path $root 'Release\bass_archive_stream.dll'
foreach($path in @($source,$addon,$VcVars32,$SevenZipExe,$RarExe,
    (Join-Path $BassCoreSdkRoot 'bass.h'),(Join-Path $BassCoreSdkRoot 'bass.lib'),
    (Join-Path $MusicBeeRoot 'bass.dll'),(Join-Path $root 'test_bass_archive.cpp'),
    (Join-Path $root 'test_native_inprocess_enum.cpp'),
    (Join-Path $root 'test_native_mf_large_fixture.py'))) {
    if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw "Missing native MF cache QA prerequisite: $path"}
}
$temp=Join-Path $env:TEMP ('ArchiveStreamMFNativeCache-'+[Guid]::NewGuid().ToString('N'))
$previous=$env:LOCALAPPDATA
New-Item -Path $temp -ItemType Directory -Force | Out-Null
try {
    $env:LOCALAPPDATA=Join-Path $temp 'isolated-local-app-data'
    New-Item -Path $env:LOCALAPPDATA -ItemType Directory -Force | Out-Null
    $memory=50
    $cacheLimit=2048
    $settings=Get-ItemProperty 'HKCU:\Software\ArchiveStreamPlayer\Adapters' -ErrorAction SilentlyContinue
    if($settings) {
        if($settings.MemoryMB -ge 1 -and $settings.MemoryMB -le 512){$memory=[int]$settings.MemoryMB}
        if($null -ne $settings.CacheLimitMB){$cacheLimit=[int]$settings.CacheLimitMB}
    }
    if($memory -ge 70 -or $cacheLimit -lt 300){
        throw "Test needs configured MemoryMB < 70, CacheLimitMB >= 300; did not alter settings"
    }
    $long=Join-Path $temp ("long."+$Codec)
    & python (Join-Path $root 'test_native_mf_large_fixture.py') $source $long
    if($LASTEXITCODE -ne 0){throw "Unable to generate MF $Codec large fixture"}
    $size=(Get-Item $long).Length
    if($size -le [long]$memory*1MB -or $size -gt [long]$cacheLimit*1MB/3){
        throw "Invalid RAM/cache limits for fixture: $size bytes, RAM=$memory MB, cache=$cacheLimit MB"
    }
    if($Codec -eq 'wma') {
        # A small MP3 goes first, allowing us to test the solid-dependent
        # WMA cache without simultaneously testing synthetic padding on a
        # first-entry RAR stream (which is not the cache route under test).
        Copy-Item 'C:\audiotest\01.MP3File.mp3' (Join-Path $temp 'a.mp3')
        Copy-Item -LiteralPath $long -Destination (Join-Path $temp 'b.wma')
        $firstMember='a.mp3'
    }
    else {
        foreach($name in @('a','b')){Copy-Item -LiteralPath $long -Destination (Join-Path $temp ($name+'.'+$Codec))}
        $firstMember='a.'+$Codec
    }
    Copy-Item -LiteralPath $addon -Destination $temp
    Copy-Item -LiteralPath (Join-Path $MusicBeeRoot 'bass.dll') -Destination $temp
    $dll=Join-Path $temp 'bass_archive_stream.dll'
    $scanner=Join-Path $temp 'disk_enum.exe'
    $decode=Join-Path $temp 'disk_decode.exe'
    foreach($pair in @(@($scanner,'test_native_inprocess_enum.cpp'),@($decode,'test_bass_archive.cpp'))) {
        $cmd='call "'+$VcVars32+'" >nul && cl /nologo /MD /EHsc /std:c++17 /W3 /O2 /I "'+
            $BassCoreSdkRoot+'" /Fe:"'+$pair[0]+'" "'+(Join-Path $root $pair[1])+
            '" /link /LIBPATH:"'+$BassCoreSdkRoot+'" bass.lib'
        & cmd.exe /d /s /c $cmd
        if($LASTEXITCODE -ne 0){throw "Could not compile $($pair[1])"}
    }
    if($Codec -ne 'aac') {
        $parity=Join-Path $temp 'padded_mf_parity.exe'
        $repo=Split-Path $root -Parent
        $sourceCpp=Join-Path $root 'test_native_mf_padded_parity.cpp'
        if(!(Test-Path -LiteralPath $sourceCpp)){throw "Missing MF PCM test: $sourceCpp"}
        $cmd='call "'+$VcVars32+'" >nul && cl /nologo /MD /EHsc /std:c++20 /W3 /O2 '+
            '/I "archive_backend" /I "shared" /I "third_party\dr_libs" '+
            '/Fe:"'+$parity+'" "'+$sourceCpp+'" shared\debug_log.cpp '+
            '/link /LTCG /LIBPATH:"archive_backend\Release" /LIBPATH:"unrar_static\Release" '+
            '/LIBPATH:"libarchive_static\Release" /LIBPATH:"zlib_static\Release" '+
            'archive_backend.lib unrar_static.lib libarchive_static.lib zlib_static.lib '+
            'mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib propsys.lib bcrypt.lib Advapi32.lib Shell32.lib User32.lib'
        Push-Location $repo
        try {
            & cmd.exe /d /s /c $cmd
            if($LASTEXITCODE -ne 0){throw "MF $Codec parity harness build failed"}
        }
        finally { Pop-Location }
        & $parity $source $long
        if($LASTEXITCODE -ne 0){throw "MF $Codec padded source PCM mismatch"}
    }
    if($OnlyParity){return}
    Push-Location $temp
    try {
        & $SevenZipExe a '-t7z' '-mx=1' '-ms=1024m' 'solid.7z' $firstMember ('b.'+$Codec) | Out-Null
        if($LASTEXITCODE -ne 0){throw 'Failed to create solid 7z MF fixture'}
        $meta=(& $SevenZipExe l -slt 'solid.7z' | Out-String)
        if($LASTEXITCODE -ne 0 -or !$meta.Contains('Blocks = 1')){throw '7z fixture must have one solid block'}
        & $RarExe a '-s' '-m1' 'solid.rar' $firstMember ('b.'+$Codec) | Out-Null
        if($LASTEXITCODE -ne 0){throw 'Failed to create solid RAR MF fixture'}
        $hash=(Get-FileHash -LiteralPath $long -Algorithm SHA256).Hash
        foreach($ext in @('7z','rar')){
            $archive=Join-Path $temp ('solid.'+$ext)
            $legacy=Join-Path $temp ('legacy-'+$ext)
            & $scanner $dll $archive $legacy 0 diskogg
            if($LASTEXITCODE -ne 0){throw "Earlier v1.12 scanner unexpectedly accepted solid large $Codec $ext"}
            $selected=Join-Path $temp ('selected-'+$ext)
            & $scanner $dll $archive $selected 2 diskmf
            if($LASTEXITCODE -ne 0){throw "Native disk MF scanner failed $Codec $ext"}
            $cacheDir=Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\ManagedCache'
            $files=@(Get-ChildItem -LiteralPath $cacheDir -Filter ('*.'+$Codec) -File -ErrorAction SilentlyContinue)
            if($files.Count -lt 1){throw "No $Codec encoded-file cache after scan"}
            foreach($file in $files){
                if($file.Length -ne $size -or (Get-FileHash $file.FullName -Algorithm SHA256).Hash -ne $hash){
                    throw "Native $Codec cache differs from encoded fixture"
                }
            }
            $cacheCount=$files.Count
            & $scanner $dll $archive (Join-Path $temp ('repeat-'+$ext)) 2 diskmf
            if($LASTEXITCODE -ne 0){throw "Native $Codec cache reuse failed $ext"}
            $again=@(Get-ChildItem -LiteralPath $cacheDir -Filter ('*.'+$Codec) -File)
            if($again.Count -ne $cacheCount){throw "Duplicate $Codec cache after repeat scan"}
            $cached=0
            $streamed=0
            $buffered=0
            foreach($manifest in @(Get-ChildItem -LiteralPath $selected -Filter '*.ast' -File)){
                $env:AST_BASS_ADDON_LOG=Join-Path $temp ($Codec+'-'+$ext+'-'+$manifest.BaseName+'.log')
                Remove-Item -LiteralPath $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
                if($Codec -eq 'aac') {
                    & $decode $dll $manifest.FullName late | Select-String 'BASS_ARCHIVE_.*PASS'
                }
                else {
                    & $decode $dll $manifest.FullName | Select-String 'BASS_ARCHIVE_.*PASS'
                }
                if($LASTEXITCODE -ne 0){throw "Native $Codec BASS PCM or seek failed $ext $($manifest.Name)"}
                $trace=[IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)
                if($trace.Contains('PCM_FIRST route=CACHE')){
                    if(!$trace.Contains('OPEN_OK codec='+$Codec.ToUpperInvariant()+' route=CACHE')){throw 'Misleading native cached OPEN_OK logging'}
                    ++$cached
                }
                elseif($trace.Contains('PCM_FIRST route=STREAM')){++$streamed}
                elseif($trace.Contains('PCM_FIRST route=BUFFER')){++$buffered}
                else{throw "Unexpected $Codec native decoder route for $ext"}
                if(!$trace.Contains('SEEK_OK')){throw "No $Codec native seek succeeded for $ext"}
            }
            $expectedCache = if($Codec -eq 'wma'){1}else{2}
            if($ext -eq '7z' -and $cached -ne $expectedCache){
                throw "Unexpected solid 7z $Codec cached tracks: expected $expectedCache, got $cached"
            }
            if($ext -eq 'rar' -and $cached -lt 1){throw "Expected at least one RAR cache dependency"}
            Write-Output "MUSICBEE_MF_DISK_LAST_PASS codec=$Codec archive=$ext tracks=2 cache=$cached stream=$streamed encoded_bytes=$size buffer=$buffered sha256=exact seek=pass reuse=pass http=0"
        }
    }
    finally{Pop-Location}
    Write-Output "MUSICBEE_MF_DISK_LAST_ALL_ARCHIVES_PASS codec=$Codec"
}
finally {
    $env:LOCALAPPDATA=$previous
    Remove-Item Env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
