param(
  [ValidateSet('wav','wav-cache','flac','mp3','ogg','all')][string]$Case='all',
  [ValidateSet('zip','7z','rar')][string]$ArchiveKind='zip',
  [switch]$RequireGuiSeek,
  [string]$BassAddonSdkRoot='D:\Temp\ArchiveStreamMusicBeeBassAddonResearch\sdk',
  [string]$BassCoreSdkRoot='D:\Temp\ArchiveStreamMusicBeeBassAddonResearch\bass24\c',
  [string]$SevenZipExe='C:\Program Files\7-Zip\7z.exe',
  [string]$RarExe='C:\Program Files\WinRAR\Rar.exe'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
if(@(Get-CimInstance Win32_Process -Filter "Name='MusicBee.exe'").Count){throw 'Active MusicBee session; refusing to interfere'}
$workspace=Join-Path $env:TEMP ('ArchiveStreamMusicBeeGuiRecovery-'+[guid]::NewGuid().ToString('N'))
$keys=@('AST_MUSICBEE_GUI_ARTIFACT_DIR','AST_NATIVE_ARCHIVE_TEST_PATH','AST_NATIVE_CODEC','AST_GUI_BASS_DLL','AST_GUI_EXPECT_RECOVERY_ROUTE','AST_GUI_EXPECT_ROUTE','AST_GUI_EXPECT_TRACKS','AST_NATIVE_TEST_STREAM_FAIL_AFTER','AST_NATIVE_TEST_MEMORY_MB','AST_GUI_REQUIRE_SEEK','QA_SOURCE_WAV','QA_LONG_WAV')
$original=@{}
foreach($key in $keys){$original[$key]=[Environment]::GetEnvironmentVariable($key)}
New-Item $workspace -ItemType Directory -Force | Out-Null
try {
  $dll=Join-Path $workspace 'bass_archive_stream.dll'
  & (Join-Path $root 'build-native-bass.ps1') -BassAddonSdkRoot $BassAddonSdkRoot -BassCoreSdkRoot $BassCoreSdkRoot -OutputPath $dll -TestFaultInjection | Select-String 'BUILT|error|fatal'
  if($LASTEXITCODE -ne 0 -or !(Test-Path $dll)){throw 'Test-only native BASS build failed'}
  $env:AST_MUSICBEE_GUI_ARTIFACT_DIR=$workspace
  $env:AST_GUI_BASS_DLL=$dll
  $env:AST_GUI_EXPECT_ROUTE='STREAM'
  $env:AST_GUI_EXPECT_TRACKS='1'
  $env:AST_GUI_REQUIRE_SEEK=if($RequireGuiSeek){'1'}else{'0'}
  & python (Join-Path $root 'test_real_native_mf_gui_probe.py') | Select-String 'PASS|error|Error'
  if($LASTEXITCODE -ne 0){throw 'Disposable managed MusicBee probe build failed'}
  $cases=if($Case -eq 'all'){@('wav','flac','mp3','ogg','wav-cache')}else{@($Case)}
  foreach($name in $cases){
    $codec=if($name -eq 'wav-cache'){'wav'}else{$name}
    $source=@{wav='04.WAVFile.wav';flac='03.FlacFile.flac';mp3='01.MP3File.mp3';ogg='02.OGGFile.ogg'}[$codec]
    $member='track.'+$codec
    $target=Join-Path $workspace $member
    if($name -eq 'wav-cache'){
      $env:QA_SOURCE_WAV=Join-Path 'C:\audiotest' $source
      $env:QA_LONG_WAV=$target
      & python -c "import os,wave; a=wave.open(os.environ['QA_SOURCE_WAV'],'rb'); p=a.getparams(); b=a.readframes(a.getnframes()); a.close(); w=wave.open(os.environ['QA_LONG_WAV'],'wb'); w.setparams(p); w.writeframes(b+b); w.close(); print('LONG_WAV_BYTES',len(b+b))"
      if($LASTEXITCODE -ne 0){throw 'Long WAV fixture failed'}
      $settings=Get-ItemProperty 'HKCU:\Software\ArchiveStreamPlayer\Adapters' -ErrorAction SilentlyContinue
      $memory=if($settings -and $settings.MemoryMB){[int]$settings.MemoryMB}else{50}
      if((Get-Item $target).Length -le ([long]$memory*1MB)){throw "WAV cache fixture does not exceed RAM budget $memory MB"}
      $env:AST_GUI_EXPECT_RECOVERY_ROUTE='CACHE'
    }else{
      Copy-Item (Join-Path 'C:\audiotest' $source) $target -Force
      $env:AST_GUI_EXPECT_RECOVERY_ROUTE='BUFFER'
    }
    $archive=Join-Path $workspace ('fault-'+$name+'.'+$ArchiveKind)
    Push-Location $workspace
    try{
      if($ArchiveKind -eq 'zip'){
        $env:AST_QA_ARCHIVE=$archive
        $env:AST_QA_MEMBER=$member
        & python -c "import os,zipfile; a=os.environ['AST_QA_ARCHIVE'];m=os.environ['AST_QA_MEMBER'];z=zipfile.ZipFile(a,'w',compression=zipfile.ZIP_STORED);z.write(m,m);z.close()"
      }
      elseif($ArchiveKind -eq '7z'){& $SevenZipExe a '-t7z' '-mx=1' '-ms=off' $archive $member | Out-Null}
      else{& $RarExe a '-ma5' '-s-' '-m1' $archive $member | Out-Null}
      if($LASTEXITCODE -ne 0){throw "Failed to generate $ArchiveKind $name fault case"}
    } finally {Pop-Location}
    $env:AST_NATIVE_CODEC=$codec.ToUpperInvariant()
    $env:AST_NATIVE_ARCHIVE_TEST_PATH=$archive
    $env:AST_NATIVE_TEST_STREAM_FAIL_AFTER='65536'
    if($name -eq 'ogg'){$env:AST_NATIVE_TEST_MEMORY_MB='1'}else{Remove-Item Env:AST_NATIVE_TEST_MEMORY_MB -ErrorAction SilentlyContinue}
    if($name -eq 'ogg'){$env:AST_GUI_EXPECT_RECOVERY_ROUTE='CACHE'} # OGG tiny compressed file needs forced cache if >1MB
    Write-Output "MUSICBEE_GUI_RECOVERY_CASE_BEGIN codec=$codec archive=$ArchiveKind expected=$env:AST_GUI_EXPECT_RECOVERY_ROUTE"
    $output=Join-Path $workspace ('host-'+$name+'.log')
    & cmd.exe /d /c ('python "'+(Join-Path $root 'test_real_native_mf_gui.py')+'" > "'+$output+'" 2>&1')
    $code=$LASTEXITCODE
    $log=if(Test-Path $output){[IO.File]::ReadAllText($output)}else{''}
    $log -split [char]10 | Where-Object {$_ -match 'MUSICBEE_REAL_HOST_MIDSTREAM_RECOVERY_PASS|MUSICBEE_NATIVE_REAL_HOST_GUI_SEEK_AND_RESUME_PASS|RECOVERY_|SEEK_OK|QA_NATIVE_HOST|QA_NATIVE_EXCEPTION|QA_NATIVE_SESSION|QA_NATIVE_QUEUE|PCM_FIRST|Assertion|Traceback|Error|MUSICBEE_NATIVE_REAL_GUI_SEEK_CLICKED'} | Select-Object -Last 55
    if($code -ne 0 -or !$log.Contains('MUSICBEE_REAL_HOST_MIDSTREAM_RECOVERY_PASS')){throw "Real MusicBee midstream recovery failed case=$name archive=$ArchiveKind code=$code"}
    Write-Output "MUSICBEE_GUI_MIDSTREAM_RECOVERY_CASE_PASS codec=$codec archive=$ArchiveKind expected=$env:AST_GUI_EXPECT_RECOVERY_ROUTE gui_seek=$RequireGuiSeek http=0"
    Remove-Item Env:AST_NATIVE_TEST_STREAM_FAIL_AFTER -ErrorAction SilentlyContinue
  }
  Write-Output "MUSICBEE_GUI_RECOVERY_MATRIX_COMPLETE cases=$($cases.Count) archive=$ArchiveKind"
}finally{
  foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$original[$key])}
  Remove-Item Env:AST_QA_ARCHIVE,Env:AST_QA_MEMBER -ErrorAction SilentlyContinue
  Remove-Item $workspace -Recurse -Force -ErrorAction SilentlyContinue
}