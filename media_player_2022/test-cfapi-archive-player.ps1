# media_player_2022/test-cfapi-archive-player.ps1
# Native Media Player (2022): projected audio bytes backed by ZIP archive stream.
param([string]$Archive = 'M:\Music\VA - Subspecies 4 (2024).zip')
$ErrorActionPreference = 'Stop'
$base = 'D:\Temp\AST-MediaPlayer2022-MFQA'
$root = Join-Path $base 'cf-archive-root'
$ready = Join-Path $base 'cf-archive-ready'
$stop = Join-Path $base 'cf-archive-stop'
$log = Join-Path $base 'cfapi-archive-events.log'
$provider = Join-Path $PSScriptRoot 'bin\x64\Release\cfapi_probe.exe'
$activator = Join-Path $PSScriptRoot 'bin\x64\Release\activate_modern_media_file.exe'
$track = Join-Path $root 'archive-track-0.mp3'
if (!(Test-Path $Archive) -or !(Test-Path $provider) -or !(Test-Path $activator))
{
    throw 'Missing ZIP sample, provider or Media Player activation helper'
}
if (Test-Path $root)
{
    throw "Refusing existing test sync root: $root"
}
foreach ($p in @($ready, $stop, $log))
{
    if (Test-Path $p)
    {
        Remove-Item -LiteralPath $p -Force
    }
}
$proc = $null
try
{
    $arguments = @('run-archive', ('"' + $root + '"'), ('"' + $Archive + '"'), ('"' + $ready + '"'), ('"' + $stop + '"'), ('"' + $log + '"'))
    $proc = Start-Process -FilePath $provider -ArgumentList $arguments -PassThru
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $exists=$false
    for ($i=0; $i -lt 100; $i++)
    {
        if (Test-Path $ready)
        {
            $exists=$true
            break
        }
        $proc.Refresh()
        if ($proc.HasExited)
        {
            break
        }
        Start-Sleep -Milliseconds 150
    }
    if (!$exists)
    {
        throw 'CfAPI archive provider did not make the MP3 placeholder ready'
    }
    Write-Output ('CFAPI_ARCHIVE_PLACEHOLDER_BYTES=' + (Get-Item $track).Length)
    Write-Output ('CFAPI_ARCHIVE_READY_SECONDS=' + [Math]::Round($timer.Elapsed.TotalSeconds,3))
    & $activator $track (Join-Path $base 'cfapi-archive-activation.txt')
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Modern Media Player refused archive-backed file activation'
    }
    Start-Sleep -Seconds 14
    $content=Get-Content $log -Raw
    Write-Output ('CFAPI_ARCHIVE_OPEN=' + ($content -match 'event=ARCHIVE_STREAM_OPEN'))
    Write-Output ('CFAPI_ARCHIVE_FETCH=' + ($content -match 'event=FETCH_DATA'))
    Write-Output ('CFAPI_ARCHIVE_TRANSFER=' + ($content -match 'event=TRANSFER_DATA'))
    Write-Output ('CFAPI_ARCHIVE_READ_ERROR=' + ($content -match 'event=FETCH_ARCHIVE_READ_FAILED'))
    Write-Output ('CFAPI_ARCHIVE_DURATION_TO_INSPECT_IN_PLAYER=1')
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    $bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp=New-Object Drawing.Bitmap($bounds.Width,$bounds.Height)
    $graphics=[Drawing.Graphics]::FromImage($bmp)
    $graphics.CopyFromScreen($bounds.Location,[Drawing.Point]::Empty,$bounds.Size)
    $bmp.Save((Join-Path $base 'cfapi-archive-modern-gui.png'))
    $graphics.Dispose()
    $bmp.Dispose()
    if ($content -notmatch 'event=ARCHIVE_STREAM_OPEN' -or
        $content -notmatch 'event=FETCH_DATA' -or
        $content -notmatch 'event=TRANSFER_DATA' -or
        $content -match 'event=FETCH_ARCHIVE_READ_FAILED')
    {
        throw 'Archive-backed projected MP3 stream did not successfully deliver data'
    }
    Write-Output 'CFAPI_ARCHIVE_TO_MODERN_MEDIA_PLAYER_STREAM_PASS=1'
}
finally
{
    [IO.File]::WriteAllText($stop, 'stop')
    if ($proc)
    {
        if (!$proc.WaitForExit(11000))
        {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
        $proc.Dispose()
    }
    # The app may still hold a handle: do not delete a projected file out
    # from under a playing track. Clean up after that handle is released.
    if (Test-Path $log)
    {
        Get-Content $log | Select-Object -First 22
    }
}
