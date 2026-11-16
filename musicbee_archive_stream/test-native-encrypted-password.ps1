param(
    [Parameter(Mandatory=$true)][string]$BassAddonSdkRoot,
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [ValidateSet('7z','rar','zip','all')][string]$ArchiveKind='all',
    [ValidateSet('aac','mp3','wav','flac','ogg','m4a','wma')][string]$Codec='aac',
    [ValidateSet('accept','save','wrong','cancel','all')][string]$Scenario='all',
    [string]$SevenZipExe='C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe='C:\Program Files\WinRAR\Rar.exe',
    [string]$VcVars32='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$repo=Split-Path $root -Parent
$temp=Join-Path $env:TEMP ('ArchiveStreamMusicBeeEncrypted-'+[Guid]::NewGuid().ToString('N'))
$oldLocal=$env:LOCALAPPDATA
$envKeys=@('AST_NATIVE_TEST_PASSWORD','AST_NATIVE_TEST_PASSWORD_MODE','AST_NATIVE_TEST_PASSWORD_LIST',
    'AST_BASS_ADDON_LOG','ARCHIVE_STREAM_RAR_PASSWORD_LIST')
$oldEnv=@{}
foreach($key in $envKeys){$oldEnv[$key]=[Environment]::GetEnvironmentVariable($key)}
New-Item -Path $temp -ItemType Directory -Force | Out-Null
try {
    $env:LOCALAPPDATA=Join-Path $temp 'localappdata'
    New-Item -Path $env:LOCALAPPDATA -ItemType Directory -Force | Out-Null
    $addon=Join-Path $temp 'bass_archive_stream.dll'
    & (Join-Path $root 'build-native-bass.ps1') -BassAddonSdkRoot $BassAddonSdkRoot -BassCoreSdkRoot $BassCoreSdkRoot -OutputPath $addon -TestFaultInjection | Out-Null
    if($LASTEXITCODE -ne 0){throw 'Encrypted test-only BASS addon build failed'}
    Copy-Item -LiteralPath 'C:\Program Files (x86)\MusicBee\bass.dll' -Destination $temp -Force
    $scanner=Join-Path $temp 'native_encrypted_scan.exe'
    $decoder=Join-Path $temp 'native_encrypted_decode.exe'
    foreach($case in @(@('test_native_inprocess_enum.cpp',$scanner),
                       @('test_bass_archive.cpp',$decoder))) {
        $cmd='call "'+$VcVars32+'" >nul && cl /nologo /MD /EHsc /std:c++17 /W3 /O2 '+
            '/I "'+$BassCoreSdkRoot+'" /Fe:"'+$case[1]+'" "'+
            (Join-Path $root $case[0])+'" /link /LIBPATH:"'+$BassCoreSdkRoot+'" bass.lib'
        Push-Location $temp
        try { & cmd.exe /d /s /c $cmd }
        finally {Pop-Location}
        if($LASTEXITCODE -ne 0){throw "Could not compile encrypted QA harness $($case[0])"}
    }
    $audioFiles=@{aac='07.AACFile.aac';mp3='01.MP3File.mp3';wav='04.WAVFile.wav';
        flac='03.FLACFile.flac';ogg='02.OGGFile.ogg';m4a='06.M4AFile.m4a';wma='05.WMAFile.wma'}
    $source=Join-Path 'C:\audiotest' $audioFiles[$Codec]
    if(!(Test-Path -LiteralPath $source)){throw "Missing actual $Codec test audio"}
    $password='MusicBeeSynthetic-'+[Guid]::NewGuid().ToString('N')
    $wrong=$password+'-INVALID'
    $list=Join-Path $temp 'test-keeps.txt'
    $env:AST_NATIVE_TEST_PASSWORD_LIST=$list
    $env:ARCHIVE_STREAM_RAR_PASSWORD_LIST=$list
    $types=if($ArchiveKind -eq 'all'){@('zip','7z','rar')}else{@($ArchiveKind)}
    foreach($kind in $types) {
        foreach($headers in $(if($kind -eq 'zip'){@($false)}else{@($false,$true)})){
            $mode=if($headers){'headers'}else{'data'}
            $archive=Join-Path $temp ("encrypted-$kind-$mode."+$kind)
            $member='track.'+$Codec
            Copy-Item -LiteralPath $source -Destination (Join-Path $temp $member) -Force
            Push-Location $temp
            try {
                if($kind -eq '7z'){
                    $head=if($headers){'-mhe=on'}else{'-mhe=off'}
                    & $SevenZipExe a '-t7z' '-mx=1' '-ms=off' $head ('-p'+$password) $archive $member | Out-Null
                }
                elseif($kind -eq 'zip') {
                    & $SevenZipExe a '-tzip' '-mem=AES256' ('-p'+$password) $archive $member | Out-Null
                }
                else {
                    $pass=if($headers){'-hp'+$password}else{'-p'+$password}
                    & $RarExe a '-ma5' '-s-' '-m1' $pass $archive $member | Out-Null
                }
                if($LASTEXITCODE -ne 0){throw "Could not create $kind $mode AES fixture"}
            }
            finally{Pop-Location}
            foreach($caseMode in $(if($Scenario -eq 'all'){@('accept','save','wrong','cancel')}else{@($Scenario)})){
                # Each operation is isolated: saved passwords MUST NOT
                # cause a failed credential to be accepted.
                [IO.File]::WriteAllBytes($list,[byte[]]@())
                $env:AST_NATIVE_TEST_PASSWORD=if($caseMode -eq 'wrong'){$wrong}else{$password}
                $env:AST_NATIVE_TEST_PASSWORD_MODE=$caseMode
                $env:AST_BASS_ADDON_LOG=Join-Path $temp ("$kind-$mode-$caseMode.log")
                $output=Join-Path $temp ("scan-$kind-$mode-$caseMode")
                $expected=if($caseMode -eq 'cancel'){-2}elseif($caseMode -eq 'wrong'){-3}else{1}
                Remove-Item -LiteralPath $env:AST_BASS_ADDON_LOG -ErrorAction SilentlyContinue
                Push-Location $temp
                try { & $scanner $addon $archive $output $expected encrypted | Select-String 'QA_NATIVE_AUDIO_SCAN|QA_NATIVE_IN_PROCESS_ENUM_PASS' }
                finally {Pop-Location}
                if($LASTEXITCODE -ne 0){
                    if(Test-Path $env:AST_BASS_ADDON_LOG){Get-Content $env:AST_BASS_ADDON_LOG | Select -Last 36}
                    throw "Native MusicBee password scanner failed kind=$kind headers=$headers scenario=$caseMode"
                }
                $lines=@([IO.File]::ReadAllLines($list))
                if($caseMode -eq 'save') {
                    if($lines.Count -ne 1 -or $lines[0] -ne $password){throw "Password verified-only save failed kind=$kind mode=$mode"}
                    Write-Output "MUSICBEE_NATIVE_PASSWORD_VERIFIED_SAVE_PASS archive=$kind headers=$mode count=1"
                }
                elseif($lines.Count -gt 0 -and @($lines | Where-Object {$_ -ne ''}).Count -gt 0){
                    throw "Unexpected saved password in $kind $mode $caseMode"
                }
                if($caseMode -in @('accept','save')) {
                    $track=@(Get-ChildItem -LiteralPath $output -Filter '*.ast' -File)
                    if($track.Count -ne 1){throw "Wrong native queued manifest count kind=$kind scenario=$caseMode"}
                    # After Save=Yes, a new process must open with only
                    # the isolated saved list. Any unexpected prompt is
                    # forced to reject immediately by the test-only hook.
                    $env:AST_NATIVE_TEST_PASSWORD_MODE=if($caseMode -eq 'save'){'cancel'}else{'accept'}
                    $env:AST_NATIVE_TEST_PASSWORD=if($caseMode -eq 'save'){$wrong}else{$password}
                    Push-Location $temp
                    try { & $decoder $addon $track[0].FullName }
                    finally {Pop-Location}
                    if($LASTEXITCODE -ne 0){if(Test-Path $env:AST_BASS_ADDON_LOG){Get-Content $env:AST_BASS_ADDON_LOG | Select-Object -Last 40};throw "BASS native encrypted AAC decoder/seek failed kind=$kind mode=$mode"}
                    $trace=if(Test-Path $env:AST_BASS_ADDON_LOG){[IO.File]::ReadAllText($env:AST_BASS_ADDON_LOG)}else{''}
                    if(!$trace.Contains('AST_BASS OPEN_OK codec='+$Codec.ToUpperInvariant()) -or !$trace.Contains('AST_BASS PCM_FIRST') -or
                        !$trace.Contains('AST_BASS SEEK_OK') -or
                        $trace.Contains('route=HTTP') -or
                        ($caseMode -eq 'save' -and $trace.Contains('AST_BASS NATIVE_PASSWORD_CANCELLED'))){
                        throw "Actual encrypted native AAC PCM and seek trace missing kind=$kind headers=$mode"
                    }
                    if($caseMode -eq 'save'){
                        Write-Output "MUSICBEE_NATIVE_SAVED_PASSWORD_REUSE_NO_REPROMPT_PASS codec=$Codec archive=$kind headers=$mode"
                    }
                    Write-Output "MUSICBEE_NATIVE_ENCRYPTED_PCM_SEEK_PASS codec=$Codec archive=$kind headers=$mode scenario=$caseMode http=0"
                }
                else {
                    if(Test-Path $output){
                        if(@(Get-ChildItem $output -Filter '*.ast').Count){throw 'Denied password created native queue references'}
                    }
                    Write-Output "MUSICBEE_NATIVE_ENCRYPTED_PASSWORD_FAIL_CLOSED_PASS archive=$kind headers=$mode scenario=$caseMode no_manifest=1 no_saved_password=1"
                }
            }
        }
    }
    Write-Output "MUSICBEE_NATIVE_ENCRYPTED_PASSWORD_TEST_PASS codec=$Codec kind=$ArchiveKind scenario=$Scenario"
}
finally {
    $env:LOCALAPPDATA=$oldLocal
    foreach($key in $envKeys){[Environment]::SetEnvironmentVariable($key,$oldEnv[$key])}
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
