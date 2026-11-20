# DEV forced-termination test: owner job must kill its bridge and elevated helper
# must restore its own temporary Windows Media Player loopback exemption.
$ErrorActionPreference = 'Stop'
$family = 'Microsoft.ZuneMusic_8wekyb3d8bbwe'
$exe = Join-Path $PSScriptRoot 'bin\x64\Release\modern_media_drop.exe'
$archive = 'M:\Music\VA - Subspecies 4 (2024).zip'
$log = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs\modern_media_drop.log'
if (Get-Process modern_media_drop,archive_player_bridge -ErrorAction SilentlyContinue)
{
    throw 'Refusing to run crash test with existing drop/bridge processes'
}
$prior = (CheckNetIsolation.exe LoopbackExempt -s | Out-String) -match '(?i)microsoft.zunemusic_8wekyb3d8bbwe'
if ($prior)
{
    throw 'Refusing test because package already has a loopback exemption'
}
$p = $null
$bridgePid = 0
$confirmed = $false
try
{
    $p = Start-Process -FilePath $exe -ArgumentList ('--archive "' + $archive + '"') -PassThru
    for ($i = 0; $i -lt 120; ++$i)
    {
        Start-Sleep -Milliseconds 200
        $p.Refresh()
        if ($p.HasExited)
        {
            throw 'Drop launcher exited before backend was available'
        }
        $lines = if (Test-Path $log) { @(Get-Content $log -Tail 35) } else { @() }
        $ready = [bool]($lines -match "event=BRIDGE_READY \| pid=$($p.Id) \|")
        $opened = [bool]($lines -match "event=MODERN_PLAYER_ACTIVATE \| pid=$($p.Id) \|")
        $bridge = @(Get-CimInstance Win32_Process -Filter "Name='archive_player_bridge.exe'")
        if ($ready -and $opened -and $bridge.Count -eq 1)
        {
            $bridgePid = $bridge[0].ProcessId
            $confirmed = $true
            break
        }
    }
    if (!$confirmed)
    {
        throw 'Could not confirm the archive bridge and Media Player activation'
    }
    'CRASH_TEST_OWNER_PID=' + $p.Id
    'CRASH_TEST_BRIDGE_PID=' + $bridgePid
    'CRASH_TEST_READY=1'
    Stop-Process -Id $p.Id -Force -ErrorAction Stop
    $p.WaitForExit(5000) | Out-Null
    $bridgeGone = $false
    $loopbackGone = $false
    $helperGone = $false
    for ($i = 0; $i -lt 80; ++$i)
    {
        Start-Sleep -Milliseconds 180
        $bridgeGone = -not [bool](Get-Process -Id $bridgePid -ErrorAction SilentlyContinue)
        $loopbackGone = -not ((CheckNetIsolation.exe LoopbackExempt -s | Out-String) -match '(?i)microsoft.zunemusic_8wekyb3d8bbwe')
        $helpers = @(Get-CimInstance Win32_Process -Filter "Name='modern_media_drop.exe'" | Where-Object { $_.CommandLine -match 'loopback-helper' })
        $helperGone = ($helpers.Count -eq 0)
        if ($bridgeGone -and $loopbackGone -and $helperGone)
        {
            break
        }
    }
    'CRASH_BRIDGE_TERMINATED=' + $bridgeGone
    'CRASH_LOOPBACK_RESTORED=' + $loopbackGone
    'CRASH_HELPER_EXITED=' + $helperGone
    if (-not ($bridgeGone -and $loopbackGone -and $helperGone))
    {
        throw 'Forced termination did not fully clean bridge/helper/loopback'
    }
    'CRASH_OWNERSHIP_CLEANUP_PASS=1'
}
finally
{
    if ($p)
    {
        $p.Refresh()
        if (-not $p.HasExited)
        {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
        $p.Dispose()
    }
    $state = (CheckNetIsolation.exe LoopbackExempt -s | Out-String) -match '(?i)microsoft.zunemusic_8wekyb3d8bbwe'
    'FINAL_LOOPBACK_EXEMPTION=' + $state
    'FINAL_BRIDGE_COUNT=' + @((Get-Process archive_player_bridge -ErrorAction SilentlyContinue)).Count
    'FINAL_DROP_HELPER_COUNT=' + @((Get-CimInstance Win32_Process -Filter "Name='modern_media_drop.exe'")).Count
}
