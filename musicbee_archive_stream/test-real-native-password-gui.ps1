param(
    [Parameter(Mandatory=$true)][string]$BassAddonSdkRoot,
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [ValidateSet('zip','7z','rar')][string]$ArchiveKind='7z',
    [switch]$EncryptedHeaders,
    [ValidateSet('accept','save','wrong','cancel')][string]$Mode='accept',
    [string]$SevenZipExe='C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe='C:\Program Files\WinRAR\Rar.exe'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$old=@{}
foreach($k in @('AST_MUSICBEE_GUI_ARTIFACT_DIR','AST_NATIVE_ARCHIVE_TEST_PATH','AST_NATIVE_CODEC','AST_GUI_SYNTHETIC_PASSWORD','AST_GUI_PASSWORD_MODE')){
    $old[$k]=[Environment]::GetEnvironmentVariable($k)
}
$workspace=Join-Path $env:TEMP ('ArchiveStreamMusicBeePasswordGui-'+[Guid]::NewGuid().ToString('N'))
New-Item $workspace -ItemType Directory -Force | Out-Null
try {
    if(@(Get-CimInstance Win32_Process -Filter "Name='MusicBee.exe'").Count){
        throw 'User MusicBee session is running, refusing to alter it.'
    }
    $env:AST_MUSICBEE_GUI_ARTIFACT_DIR=$workspace
    $env:AST_NATIVE_CODEC='AAC'
    $env:AST_GUI_PASSWORD_MODE=$Mode
    $env:AST_GUI_SYNTHETIC_PASSWORD='MB-Gui-'+[Guid]::NewGuid().ToString('N')
    $source='C:\audiotest\07.AACFile.aac'
    Copy-Item $source (Join-Path $workspace 'track.aac')
    $archive=Join-Path $workspace ('encrypted-aac.'+$ArchiveKind)
    Push-Location $workspace
    try {
        if($ArchiveKind -eq '7z'){
            $headers=if($EncryptedHeaders){'-mhe=on'}else{'-mhe=off'}
            & $SevenZipExe a '-t7z' '-mx=1' '-ms=off' $headers ('-p'+$env:AST_GUI_SYNTHETIC_PASSWORD) $archive 'track.aac' | Out-Null
        }
        elseif($ArchiveKind -eq 'zip') {
            if($EncryptedHeaders){throw 'ZIP does not support the 7z/RAR encrypted-filename test option'}
            & $SevenZipExe a '-tzip' '-mem=AES256' ('-p'+$env:AST_GUI_SYNTHETIC_PASSWORD) $archive 'track.aac' | Out-Null
        }
        else {
            $pass=if($EncryptedHeaders){'-hp'+$env:AST_GUI_SYNTHETIC_PASSWORD}else{'-p'+$env:AST_GUI_SYNTHETIC_PASSWORD}
            & $RarExe a '-ma5' '-s-' '-m1' $pass $archive 'track.aac' | Out-Null
        }
        if($LASTEXITCODE -ne 0){throw 'Failed to make synthetic password GUI audio'}
    }
    finally{Pop-Location}
    & (Join-Path $root 'build-native-bass.ps1') -BassAddonSdkRoot $BassAddonSdkRoot -BassCoreSdkRoot $BassCoreSdkRoot -OutputPath (Join-Path $workspace 'bass_archive_stream.dll') -TestFaultInjection | Out-Null
    if($LASTEXITCODE -ne 0){throw 'Could not build the isolated test BASS DLL'}
    & python (Join-Path $root 'test_real_native_mf_gui_probe.py') | Select-String 'PASS|error|Error|error CS'
    if($LASTEXITCODE -ne 0){throw 'Could not build managed MusicBee test probe'}
    $env:AST_NATIVE_ARCHIVE_TEST_PATH=$archive
    $output=Join-Path $workspace 'gui-result.txt'
    & cmd.exe /d /c ('python "'+(Join-Path $root 'test_real_native_password_gui.py')+'" > "'+$output+'" 2>&1')
    $exit=$LASTEXITCODE
    $log=if(Test-Path $output){[IO.File]::ReadAllText($output)}else{''}
    $records=@($log -split [char]10 | Where-Object {$_ -match 'PASS|PASSWORD|SEEK_CLICKED|FAIL|Error|Assertion|Traceback'})
    $records | Select-Object -Last 35
    if($exit -ne 0){throw "Real MusicBee password GUI acceptance failed exit=$exit"}
    $needle=if($Mode -in @('wrong','cancel')){'MUSICBEE_REAL_HOST_ENCRYPTED_PASSWORD_DIALOG_FAIL_CLOSED_PASS'}else{'MUSICBEE_REAL_HOST_ENCRYPTED_PASSWORD_DIALOG_PCM_PASS'}
    if(!$log.Contains($needle)){throw 'Real GUI password proof not present'}
    Write-Output "MUSICBEE_REAL_ENCRYPTED_GUI_PASSWORD_TEST_PASS archive=$ArchiveKind headers=$EncryptedHeaders mode=$Mode http=0"
}
finally {
    foreach($k in $old.Keys){[Environment]::SetEnvironmentVariable($k,$old[$k])}
    Remove-Item $workspace -Recurse -Force -ErrorAction SilentlyContinue
}
