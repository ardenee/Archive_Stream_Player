# media_player_2022/test-cfapi-placeholder.ps1
# Isolated, read-only CfAPI provider test (not yet tied to archive backend).
$ErrorActionPreference = 'Stop'
$rootBase = 'D:\Temp\AST-MediaPlayer2022-MFQA'
$root = Join-Path $rootBase 'cf-root'
$ready = Join-Path $rootBase 'cf-provider-ready'
$stop = Join-Path $rootBase 'cf-provider-stop'
$log = Join-Path $rootBase 'cfapi-test-events.log'
$source = 'D:\Temp\ArchiveStreamFoobar64Runtime\audio-test\generated-mp3.mp3'
$provider = Join-Path $PSScriptRoot 'bin\x64\Release\cfapi_probe.exe'
$projected = Join-Path $root (Split-Path $source -Leaf)
if (!(Test-Path $provider) -or !(Test-Path $source)) { throw 'Missing provider or MP3 test input' }
if (Test-Path $root) { throw 'Refusing to re-use a preexisting directory: ' + $root }
foreach($p in @($ready,$stop,$log)) { if(Test-Path $p){Remove-Item -LiteralPath $p -Force} }
$process = $null
try
{
    $args = @('run',('"'+$root+'"'),('"'+$source+'"'),('"'+$ready+'"'),('"'+$stop+'"'),('"'+$log+'"'))
    $process = Start-Process -FilePath $provider -ArgumentList $args -PassThru
    $started = $false
    for($i=0;$i -lt 80;$i++)
    {
        Start-Sleep -Milliseconds 200
        if(Test-Path $ready) { $started=$true; break }
        $process.Refresh()
        if($process.HasExited) { break }
    }
    if(!$started)
    {
        if(Test-Path $log){Get-Content $log|Select-Object -Last 20|ForEach-Object{Write-Output $_}}
        throw 'CfAPI sync root or placeholder creation did not complete'
    }
    Write-Output 'CFAPI_PROVIDER_READY=1'
    $holder = Get-Item -LiteralPath $projected
    Write-Output ('CFAPI_PLACEHOLDER_BYTES=' + $holder.Length)
    if($holder.Length -ne (Get-Item $source).Length){throw 'Placeholder length differs from original audio'}
    $originalBytes = [IO.File]::ReadAllBytes($source)
    $projectedBytes = New-Object byte[] 4096
    $handle = [IO.File]::OpenRead($projected)
    try { $count=$handle.Read($projectedBytes,0,$projectedBytes.Length) }
    finally {$handle.Close()}
    if($count -le 0){throw 'Projected audio file returned zero bytes'}
    for($i=0;$i -lt $count;$i++)
    {
        if($projectedBytes[$i] -ne $originalBytes[$i]){throw ('Projected stream mismatch at byte '+$i)}
    }
    Write-Output ('CFAPI_PLACEHOLDER_READ_MATCH_BYTES=' + $count)
    $events = Get-Content $log -Raw
    if($events -notmatch 'event=FETCH_DATA' -or $events -notmatch 'event=TRANSFER_DATA'){
        throw 'No native CfAPI fetch and transfer callbacks observed'
    }
    Write-Output 'CFAPI_FETCH_DATA_AND_TRANSFER_PASS=1'
}
finally
{
    [IO.File]::WriteAllText($stop, 'stop')
    if($process)
    {
        if(!$process.WaitForExit(11000)){Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue}
        $process.Dispose()
    }
    & $provider cleanup $root (Split-Path $source -Leaf) $log | Out-Null
    Write-Output 'CFAPI_PROVIDER_STOP_AND_CLEANUP_EXECUTED=1'
    if(Test-Path $log){Get-Content $log | Select-Object -Last 24}
}
