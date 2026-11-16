param(
    [ValidateSet('7z','rar')][string]$ArchiveKind='7z',
    [switch]$LargeFlac,
    [string]$SevenZipExe='C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe='C:\Program Files\WinRAR\Rar.exe'
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$workspace=Join-Path $env:TEMP ('ArchiveStreamMusicBeeSolidGui-'+[Guid]::NewGuid().ToString('N'))
$keys=@('AST_MUSICBEE_GUI_ARTIFACT_DIR','AST_NATIVE_ARCHIVE_TEST_PATH','AST_NATIVE_CODEC','AST_GUI_TRACK_INDEX','AST_GUI_EXPECT_ROUTE','AST_GUI_EXPECT_TRACKS')
$previous=@{}
foreach($key in $keys){$previous[$key]=[Environment]::GetEnvironmentVariable($key)}
if(@(Get-CimInstance Win32_Process -Filter "Name='MusicBee.exe'").Count){throw 'A MusicBee process is already active; refusing to alter a user session.'}
New-Item -ItemType Directory -Path $workspace -Force | Out-Null
try {
    $codec='OGG'
    $route='BUFFER'
    if($LargeFlac){
        $codec='FLAC'
        $route='CACHE'
        $flacExe='C:\Program Files (x86)\MusicBee\Codec\flac.exe'
        $decoded=Join-Path $workspace 'source.wav'
        $longWav=Join-Path $workspace 'long.wav'
        $longFlac=Join-Path $workspace 'long.flac'
        & $flacExe '-d' '-f' '-s' '-o' $decoded 'C:\audiotest\03.FlacFile.flac'
        if($LASTEXITCODE -ne 0){throw 'FLAC decode failed'}
        $env:QA_SOURCE_WAV=$decoded
        $env:QA_LONG_WAV=$longWav
        python -c "import wave,os;src=wave.open(os.environ['QA_SOURCE_WAV'],'rb');params=src.getparams();pcm=src.readframes(src.getnframes());src.close();out=wave.open(os.environ['QA_LONG_WAV'],'wb');out.setparams(params);out.writeframes(pcm*2);out.close()"
        if($LASTEXITCODE -ne 0){throw 'Long WAV generation failed'}
        & $flacExe '-f' '-s' '-o' $longFlac $longWav
        if($LASTEXITCODE -ne 0){throw 'Long FLAC encoding failed'}
        $settings=Get-ItemProperty 'HKCU:\Software\ArchiveStreamPlayer\Adapters' -ErrorAction SilentlyContinue
        $memory=if($settings -and $settings.MemoryMB){[int]$settings.MemoryMB}else{50}
        $cacheLimit=if($settings -and $null -ne $settings.CacheLimitMB){[int]$settings.CacheLimitMB}else{2048}
        $encodedSize=(Get-Item $longFlac).Length
        if($encodedSize -le ([long]$memory*1MB) -or $encodedSize -ge ([long]$cacheLimit*1MB/2)){throw "Fixture cannot isolate CACHE: encoded=$encodedSize memory=$memory MB cache=$cacheLimit MB"}
        Copy-Item $longFlac (Join-Path $workspace 'a.flac')
        Copy-Item $longFlac (Join-Path $workspace 'b.flac')
        Remove-Item $decoded,$longWav,$longFlac -Force
        $memberNames=@('a.flac','b.flac')
    }
    else {
        Copy-Item 'C:\audiotest\02.OGGFile.ogg' (Join-Path $workspace 'a.ogg')
        Copy-Item 'C:\audiotest\02.OGGFile.ogg' (Join-Path $workspace 'b.ogg')
        $memberNames=@('a.ogg','b.ogg')
    }
    $archive=Join-Path $workspace ('solid.'+$ArchiveKind)
    Push-Location $workspace
    try {
        if($ArchiveKind -eq '7z'){
            & $SevenZipExe a '-t7z' '-mx=1' '-ms=1024m' $archive $memberNames[0] $memberNames[1] | Out-Null
            if($LASTEXITCODE -ne 0){throw '7z solid archive fixture creation failed'}
            $listing=& $SevenZipExe l '-slt' $archive | Out-String
            if(!$listing.Contains('Blocks = 1')){throw '7z test archive is not a single solid block'}
        }
        else {
            & $RarExe a '-ma5' '-s' '-m1' $archive $memberNames[0] $memberNames[1] | Out-Null
            if($LASTEXITCODE -ne 0){throw 'RAR solid archive fixture creation failed'}
        }
    }
    finally {Pop-Location}
    $env:AST_MUSICBEE_GUI_ARTIFACT_DIR=$workspace
    $env:AST_NATIVE_ARCHIVE_TEST_PATH=$archive
    $env:AST_NATIVE_CODEC=$codec
    $env:AST_GUI_TRACK_INDEX='1'
    $env:AST_GUI_EXPECT_TRACKS='2'
    $env:AST_GUI_EXPECT_ROUTE=$route
    & python (Join-Path $root 'test_real_native_mf_gui_probe.py')
    if($LASTEXITCODE -ne 0){throw 'Failed to compile isolated MusicBee production managed probe'}
    $output=Join-Path $workspace 'gui-solid-result.txt'
    & cmd.exe /d /c ('python "'+(Join-Path $root 'test_real_native_mf_gui.py')+'" > "'+$output+'" 2>&1')
    $exit=$LASTEXITCODE
    $log=if(Test-Path $output){[IO.File]::ReadAllText($output)}else{''}
    $log -split [char]10 | Where-Object {$_ -match 'PASS|BASS_RUNTIME_LOG|SESSION_READY|NATIVE_MANIFEST|Traceback|AssertionError|Error'}
    if($exit -ne 0){throw "Real MusicBee solid-buffer GUI test failed exit=$exit"}
    if(!$log.Contains('MUSICBEE_NATIVE_REAL_HOST_GUI_SEEK_AND_RESUME_PASS') -or !$log.Contains('AST_BASS PCM_FIRST route='+$route)){throw 'Required real-host solid fallback and seek proof absent'}
    Write-Output "MUSICBEE_REAL_SOLID_GUI_PASS archive=$ArchiveKind codec=$codec route=$route track=2 gui_seek=1 http=0"
}
finally {
    foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$previous[$key])}
    Remove-Item -LiteralPath $workspace -Recurse -Force -ErrorAction SilentlyContinue
}