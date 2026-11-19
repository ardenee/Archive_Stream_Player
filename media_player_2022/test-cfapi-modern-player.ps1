# media_player_2022/test-cfapi-modern-player.ps1
# Actual Media Player (2022) file activation against a temporary CfAPI placeholder.
$ErrorActionPreference = 'Stop'
$rootBase = 'D:\Temp\AST-MediaPlayer2022-MFQA'
$root = Join-Path $rootBase 'cf-root'
$ready = Join-Path $rootBase 'cf-player-ready'
$stop = Join-Path $rootBase 'cf-player-stop'
$log = Join-Path $rootBase 'cfapi-player-events.log'
$source = 'D:\Temp\ArchiveStreamFoobar64Runtime\audio-test\generated-mp3.mp3'
$provider = Join-Path $PSScriptRoot 'bin\x64\Release\cfapi_probe.exe'
$activator = Join-Path $PSScriptRoot 'bin\x64\Release\activate_modern_media_file.exe'
$target = Join-Path $root (Split-Path $source -Leaf)
if (Test-Path $root) { throw 'Refusing to reuse an existing CfAPI root' }
if (!(Test-Path $source) -or !(Test-Path $provider) -or !(Test-Path $activator)) { throw 'Missing test executable or source MP3' }
foreach($p in @($ready,$stop,$log)){if(Test-Path $p){Remove-Item -LiteralPath $p -Force}}
$process = $null
try
{
    $args = @('run',('"'+$root+'"'),('"'+$source+'"'),('"'+$ready+'"'),('"'+$stop+'"'),('"'+$log+'"'))
    $process = Start-Process $provider -ArgumentList $args -PassThru
    for($i=0;$i -lt 60;$i++)
    {
        if(Test-Path $ready){break}
        $process.Refresh()
        if($process.HasExited){break}
        Start-Sleep -Milliseconds 200
    }
    if(!(Test-Path $ready)){throw 'CfAPI provider did not create placeholder'}
    Write-Output ('CFAPI_MODERN_PLACEHOLDER_READY=' + (Get-Item $target).Length)
    & $activator $target (Join-Path $rootBase 'cfapi-player-activation.txt')
    if($LASTEXITCODE -ne 0){throw 'Modern Media Player rejected CfAPI projected file activation'}
    Write-Output 'MODERN_MEDIA_PLAYER_CFAPI_FILE_ACTIVATION=PASS'
    Start-Sleep -Seconds 12
    $events = Get-Content $log -Raw
    Write-Output ('MODERN_CFAPI_FETCH_OBSERVED=' + ($events -match 'event=FETCH_DATA'))
    Write-Output ('MODERN_CFAPI_TRANSFER_OBSERVED=' + ($events -match 'event=TRANSFER_DATA'))
    Write-Output ('MODERN_PLAYER_PIDS=' + (@(Get-Process 'Microsoft.Media.Player' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id) -join ','))
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    $bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp=New-Object Drawing.Bitmap($bounds.Width,$bounds.Height)
    $graphics=[Drawing.Graphics]::FromImage($bmp)
    $graphics.CopyFromScreen($bounds.Location,[Drawing.Point]::Empty,$bounds.Size)
    $bmp.Save((Join-Path $rootBase 'cfapi-modern-gui.png'))
    $graphics.Dispose()
    $bmp.Dispose()
    Write-Output ('MODERN_CFAPI_SCREENSHOT=' + (Join-Path $rootBase 'cfapi-modern-gui.png'))
}
finally
{
    [IO.File]::WriteAllText($stop,'stop')
    if($process)
    {
        if(!$process.WaitForExit(11000)){Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue}
        $process.Dispose()
    }
    & $provider cleanup $root (Split-Path $source -Leaf) $log | Out-Null
    Write-Output 'MODERN_CFAPI_PROVIDER_STOPPED_AND_CLEANUP_CALLED=1'
    if(Test-Path $log){Get-Content $log |Select-Object -Last 24}
}
