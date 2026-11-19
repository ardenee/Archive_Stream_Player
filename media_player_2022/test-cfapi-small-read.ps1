# media_player_2022/test-cfapi-small-read.ps1
# Bounded independent read across CfAPI hydration modes; no archive extraction.
param([ValidateSet('Partial','Progressive','ProgressiveDisk')][string]$Policy='ProgressiveDisk')
$ErrorActionPreference = 'Stop'
$base='D:\Temp\AST-MediaPlayer2022-MFQA'
$label=$Policy.ToLowerInvariant()
$root=Join-Path $base ('small-read-root-'+$label)
$ready=Join-Path $base ('small-read-ready-'+$label)
$stop=Join-Path $base ('small-read-stop-'+$label)
$log=Join-Path $base ('small-read-events-'+$label+'.log')
$readLog=Join-Path $base ('small-read-result-'+$label+'.txt')
$source='D:\Temp\ArchiveStreamFoobar64Runtime\audio-test\generated-mp3.mp3'
$exe=Join-Path $PSScriptRoot 'bin\x64\Release\cfapi_probe.exe'
$reader=Join-Path $PSScriptRoot 'bin\x64\Release\cfapi_read_probe.exe'
$track=Join-Path $root 'generated-mp3.mp3'
if (!(Test-Path $source) -or !(Test-Path $exe) -or !(Test-Path $reader))
{
    throw 'Fixture/provider/reader executable missing'
}
if (Test-Path $root) { throw 'Refusing to reuse an existing test root' }
foreach($p in @($ready,$stop,$log,$readLog))
{
    if(Test-Path $p){Remove-Item -LiteralPath $p -Force}
}
$provider=$null
$readerProcess=$null
try
{
    $mode=switch($Policy)
    {
        'Partial' {'run-partial'}
        'Progressive' {'run-progressive'}
        'ProgressiveDisk' {'run-progressive-disk'}
    }
    $args=@($mode,('"'+$root+'"'),('"'+$source+'"'),('"'+$ready+'"'),('"'+$stop+'"'),('"'+$log+'"'))
    $provider=Start-Process $exe -ArgumentList $args -PassThru
    $isReady=$false
    for($i=0;$i -lt 60;$i++)
    {
        if(Test-Path $ready){$isReady=$true;break}
        $provider.Refresh()
        if($provider.HasExited){break}
        Start-Sleep -Milliseconds 200
    }
    if(!$isReady){throw 'Cloud Files provider failed to create placeholder'}
    Write-Output ('SMALL_READ_POLICY='+$Policy)
    $readStart=[Diagnostics.Stopwatch]::StartNew()
    $readerProcess=Start-Process -FilePath $reader -ArgumentList ('"'+$track+'"') -RedirectStandardOutput $readLog -PassThru
    $completed=$readerProcess.WaitForExit(8000)
    if(!$completed)
    {
        Write-Output 'SMALL_READ_COMPLETED=NO timeout_seconds=8'
        Stop-Process -Id $readerProcess.Id -Force -ErrorAction SilentlyContinue
        $readerProcess.WaitForExit(2000)|Out-Null
    }
    else
    {
        $readerProcess.Refresh()
        $resultText = if(Test-Path $readLog){Get-Content $readLog -Raw}else{''}
        Write-Output ('SMALL_READ_COMPLETED=YES elapsed_ms='+$readStart.ElapsedMilliseconds)
        Write-Output $resultText
        if($resultText -notmatch 'READ_RESULT ok=1 bytes=4096'){throw 'Native projected-file read did not return all 4096 bytes'}
    }
}
finally
{
    if($readerProcess)
    {
        $readerProcess.Refresh()
        if(!$readerProcess.HasExited){Stop-Process -Id $readerProcess.Id -Force -ErrorAction SilentlyContinue}
        $readerProcess.Dispose()
    }
    [IO.File]::WriteAllText($stop,'stop')
    if($provider)
    {
        if(!$provider.WaitForExit(11000)){Stop-Process -Id $provider.Id -Force -ErrorAction SilentlyContinue}
        $provider.Dispose()
    }
    if(Test-Path $log)
    {
        Get-Content $log |Where-Object{$_ -match 'event=(POLICY_|MODIFIER_|FETCH_DATA|FETCH_REQUESTER|FETCH_COMPLETED|CF_ONDISK_BYTES|SNAPSHOT_|REMOVE_ROOT|REMOVE_PLACEHOLDER_FAILED)'}|Select-Object -Last 18
    }
    if(Test-Path $root)
    {
        $files=@(Get-ChildItem -LiteralPath $root -Force)
        if($files.Count -eq 1 -and $files[0].Name -eq 'generated-mp3.mp3' -and $files[0].Length -eq (Get-Item $source).Length)
        {
            $files[0].Attributes=[IO.FileAttributes]::Normal
            Remove-Item -LiteralPath $files[0].FullName -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $root -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Output ('SMALL_READ_ROOT_REMAINS='+(Test-Path $root))
}
