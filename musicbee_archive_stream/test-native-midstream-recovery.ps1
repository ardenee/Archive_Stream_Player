param(
    [Parameter(Mandatory=$true)][string]$BassAddonSdkRoot,
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$MusicBeeRoot='C:\Program Files (x86)\MusicBee',
    [string]$VcVars32='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$temp=Join-Path $env:TEMP ('ArchiveStreamMidstreamRecovery-'+[Guid]::NewGuid().ToString('N'))
$originalLocal=$env:LOCALAPPDATA
$originalFault=$env:AST_NATIVE_TEST_STREAM_FAIL_AFTER
$originalLog=$env:AST_BASS_ADDON_LOG
New-Item -Path $temp -ItemType Directory -Force | Out-Null
try {
    $env:LOCALAPPDATA=Join-Path $temp 'isolated-local-appdata'
    New-Item -Path $env:LOCALAPPDATA -ItemType Directory -Force | Out-Null
    $p=Get-ItemProperty 'HKCU:\Software\ArchiveStreamPlayer\Adapters' -ErrorAction SilentlyContinue
    $memory=if($p -and $p.MemoryMB){[int]$p.MemoryMB}else{50}
    $cache=if($p -and $null -ne $p.CacheLimitMB){[int]$p.CacheLimitMB}else{2048}
    if($memory -lt 46 -or $memory -gt 80 -or $cache -lt 300){
        throw "Test fixture expects 46-80MB memory and >=300MB cache (actual MemoryMB=$memory CacheLimitMB=$cache); left settings unchanged"
    }
    $addon=Join-Path $temp 'bass_archive_stream.dll'
    & (Join-Path $root 'build-native-bass.ps1') -BassAddonSdkRoot $BassAddonSdkRoot -BassCoreSdkRoot $BassCoreSdkRoot -OutputPath $addon -TestFaultInjection
    if($LASTEXITCODE -ne 0 -or !(Test-Path $addon)){throw 'Build of opt-in fault-injected BASS DLL failed'}
    Copy-Item (Join-Path $MusicBeeRoot 'bass.dll') (Join-Path $temp 'bass.dll') -Force
    $exe=Join-Path $temp 'recover_probe.exe'
    $cmd='call "'+$VcVars32+'" >nul && cl /nologo /MD /EHsc /std:c++17 /W3 /O2 '+
        '/I "'+$BassCoreSdkRoot+'" /Fe:"'+$exe+'" "'+
        (Join-Path $root 'test_native_midstream_recovery.cpp')+
        '" /link /LIBPATH:"'+$BassCoreSdkRoot+'" bass.lib'
    Push-Location $temp
    try {
        & cmd.exe /d /s /c $cmd
        if($LASTEXITCODE -ne 0){throw 'Fault injection PCM capture harness build failed'}
    }
    finally {Pop-Location}
    $env:QA_MIDSTREAM_AUDIO_IN='C:\audiotest'
    $env:QA_MIDSTREAM_FOLDER=$temp
    & python -c "import os, pathlib, wave, zipfile;root=pathlib.Path(os.environ['QA_MIDSTREAM_FOLDER']);aud=pathlib.Path(os.environ['QA_MIDSTREAM_AUDIO_IN']);raw=aud/'04.WAVFile.wav';src=wave.open(str(raw),'rb');pars=src.getparams();pcm=src.readframes(src.getnframes());src.close();assert 45*1048576<len(pcm)<65*1048576 and pars.sampwidth==2 and pars.nchannels in (1,2);wav=root/'long.wav';out=wave.open(str(wav),'wb');out.setparams(pars);out.writeframes(pcm+pcm);out.close();entries=[('wav-buffer',raw,'source.wav'),('wav-cache',wav,'source.wav'),('flac-buffer',aud/'03.FlacFile.flac','source.flac'),('mp3-buffer',aud/'01.MP3File.mp3','source.mp3')];[(lambda z,path,name:(z.write(path,name),z.close()))(zipfile.ZipFile(root/(tag+'.zip'),'w',compression=zipfile.ZIP_STORED),path,member) for tag,path,member in entries];[(root/(tag+'.ast')).write_bytes(('ARCHIVE_STREAM_NATIVE_TRACK_V1\n'+str(root/(tag+'.zip'))+'.archive_stream|0|'+member+'\n').encode('utf-8')) for tag,path,member in entries];print('MIDSTREAM_WAV_FIXTURE_PCM',len(pcm),'long_wave_bytes',wav.stat().st_size)"
    if($LASTEXITCODE -ne 0){throw 'Midstream ZIP/WAV/FLAC/MP3 fixture generation failed'}
    foreach($item in @(@('wav-buffer','BUFFER'),@('wav-cache','CACHE'),@('flac-buffer','BUFFER'),@('mp3-buffer','BUFFER'))){
        $tag=$item[0]
        $expectedRoute=$item[1]
        $manifest=Join-Path $temp ($tag+'.ast')
        $baseline=Join-Path $temp ($tag+'-baseline.pcm')
        $faulty=Join-Path $temp ($tag+'-recovered.pcm')
        $env:AST_BASS_ADDON_LOG=Join-Path $temp ($tag+'.log')
        Remove-Item Env:AST_NATIVE_TEST_STREAM_FAIL_AFTER -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
        & $exe $addon $manifest $baseline | Select-String 'RECOVER_NATIVE_FULL_PCM_PASS|FAIL'
        if($LASTEXITCODE -ne 0){if(Test-Path $env:AST_BASS_ADDON_LOG){Get-Content $env:AST_BASS_ADDON_LOG | Select-Object -Last 35};throw "Native baseline PCM failed: $tag"}
        $baselineTrace=[IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)
        if($baselineTrace.Contains('RECOVERY_START')){Write-Output ($baselineTrace -split "`n" | Select-Object -Last 16);throw "Unexpected recovery without test fault: $tag"}
        $env:AST_NATIVE_TEST_STREAM_FAIL_AFTER='1048576'
        Remove-Item -LiteralPath $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
        & $exe $addon $manifest $faulty | Select-String 'RECOVER_NATIVE_FULL_PCM_PASS|FAIL'
        if($LASTEXITCODE -ne 0){throw "Native stream-to-cache PCM failed: $tag"}
        $log=[IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)
        if(!$log.Contains('PCM_FIRST route=STREAM') -or !$log.Contains('RECOVERY_START codec=') -or
            !$log.Contains('RECOVERY_OK route='+$expectedRoute) -or !$log.Contains('RECOVERY_PCM_RESUMED')){
            throw "Expected real injected STREAM -> $expectedRoute PCM recovery trace missing: $tag"
        }
        $one=Get-FileHash -LiteralPath $baseline -Algorithm SHA256
        $two=Get-FileHash -LiteralPath $faulty -Algorithm SHA256
        if($tag -eq 'mp3-buffer') {
            $env:AST_RECOVERY_BASELINE=$baseline
            $env:AST_RECOVERY_RESULT=$faulty
            & python -c "import os,pathlib,hashlib;a=pathlib.Path(os.environ['AST_RECOVERY_BASELINE']).read_bytes();b=pathlib.Path(os.environ['AST_RECOVERY_RESULT']).read_bytes();assert len(a)>1000000 and 0<=len(b)-len(a)<=2304*4,(len(a),len(b));assert b[:len(a)]==a,'PCM prefix changed after MP3 recovery';print('MUSICBEE_MP3_RECOVERY_PCM_PREFIX_EXACT bytes',len(a),'tail_extension_bytes',len(b)-len(a))"
            if($LASTEXITCODE -ne 0){throw 'MP3 injected recovery corrupted shared decoded PCM prefix'}
            Remove-Item Env:AST_RECOVERY_BASELINE,Env:AST_RECOVERY_RESULT -ErrorAction SilentlyContinue
        }
        elseif($one.Hash -ne $two.Hash -or (Get-Item $baseline).Length -ne (Get-Item $faulty).Length){
            throw "PCM content differs across injected failure and recovery: $tag"
        }
        if($tag -eq 'wav-cache'){
            $cached=@(Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\ManagedCache') -Filter '*.wav' -File -ErrorAction SilentlyContinue)
            if($cached.Count -lt 1){throw 'WAV recovery did not populate shared cache'}
            $sourceWav=Join-Path $temp 'long.wav'
            $sha=(Get-FileHash -LiteralPath $sourceWav -Algorithm SHA256).Hash
            if(@($cached | Where-Object {(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -eq $sha}).Count -eq 0){
                throw 'Shared encoded WAV recovery cache does not match original'
            }
        }
        if($tag -eq 'mp3-buffer'){
            Write-Output "MUSICBEE_NATIVE_MIDSTREAM_PCM_PREFIX_PASS track=$tag recovery=$expectedRoute bytes=$((Get-Item $baseline).Length) pcm_prefix_exact=1 tail_padding_bounded=1 http=0"
        }
        else{
            Write-Output "MUSICBEE_NATIVE_MIDSTREAM_PCM_EXACT_PASS track=$tag recovery=$expectedRoute bytes=$((Get-Item $baseline).Length) sha256_equal=1 http=0"
        }
    }
    Write-Output 'MUSICBEE_NATIVE_MIDSTREAM_STREAM_MEMORY_CACHE_PCM_PARITY_ALL_PASS'
}
finally {
    $env:LOCALAPPDATA=$originalLocal
    if($null -ne $originalFault){$env:AST_NATIVE_TEST_STREAM_FAIL_AFTER=$originalFault}
    else{Remove-Item Env:AST_NATIVE_TEST_STREAM_FAIL_AFTER -ErrorAction SilentlyContinue}
    if($null -ne $originalLog){$env:AST_BASS_ADDON_LOG=$originalLog}
    else{Remove-Item Env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue}
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item Env:QA_MIDSTREAM_AUDIO_IN,Env:QA_MIDSTREAM_FOLDER -ErrorAction SilentlyContinue
}
