# media_player_2022/test-modern-media-drop.ps1
# DEV-only: test Windows Shell CF_HDROP -> native drop target -> bridge -> Media Player.
# When loopback is missing, Windows may request one UAC approval for the helper.
param(
    [string]$Archive = 'M:\Music\VA - Subspecies 4 (2024).zip',
    [switch]$ExpectReject
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'bin\x64\Release\modern_media_drop.exe'
$log = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs\modern_media_drop.log'
$familyPattern = '(?i)microsoft.zunemusic_8wekyb3d8bbwe'
if (!(Test-Path -LiteralPath $Archive -PathType Leaf) -or !(Test-Path $exe))
{
    throw 'Test archive/file or the native drop-target build is missing'
}
if (Get-Process modern_media_drop,archive_player_bridge -ErrorAction SilentlyContinue)
{
    throw 'Refusing to test while an Archive Stream drop target or bridge is already running'
}
$before = (CheckNetIsolation.exe LoopbackExempt -s | Out-String) -match $familyPattern
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class AstDropQa { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint msg,IntPtr w,IntPtr l); }'
$process = $null
try
{
    $process = Start-Process -FilePath $exe -ArgumentList ('--qa-ole-drop "' + $Archive + '"') -PassThru
    $matched = $false
    for ($i = 0; $i -lt 160; ++$i)
    {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited)
        {
            throw 'Drop target terminated before its test result was recorded'
        }
        if (!(Test-Path $log))
        {
            continue
        }
        $lines = @(Get-Content $log -Tail 50)
        $dropPassed = [bool]($lines -match "event=OLE_DROP_QA_PASS \| pid=$($process.Id) \|")
        $dropRejected = [bool]($lines -match "event=OLE_DROP_QA_FAILED \| pid=$($process.Id) \|")
        $activated = [bool]($lines -match "event=MODERN_PLAYER_ACTIVATE \| pid=$($process.Id) \|")
        $bridgeReady = [bool]($lines -match "event=BRIDGE_READY \| pid=$($process.Id) \|")
        if ($ExpectReject -and $dropRejected)
        {
            if (Get-Process archive_player_bridge -ErrorAction SilentlyContinue)
            {
                throw 'Rejected audio/file drop unexpectedly started the backend'
            }
            $matched = $true
            Write-Output "MODERN_DROP_REJECTION_PASS pid=$($process.Id)"
            break
        }
        if (!$ExpectReject -and $dropPassed -and $bridgeReady -and $activated)
        {
            $matched = $true
            Write-Output "MODERN_OLE_DROP_ACTIVATION_PASS pid=$($process.Id)"
            break
        }
    }
    if (!$matched)
    {
        throw 'OLE drop or Media Player playlist activation was not confirmed within 40 seconds'
    }
}
finally
{
    if ($process)
    {
        $process.Refresh()
        if (!$process.HasExited -and $process.MainWindowHandle)
        {
            [AstDropQa]::PostMessage($process.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        }
        if (!$process.WaitForExit(12000))
        {
            throw 'Drop target did not exit, so cleanup could not be certified'
        }
        $process.Dispose()
    }
    for ($i = 0; $i -lt 40; ++$i)
    {
        $after = (CheckNetIsolation.exe LoopbackExempt -s | Out-String) -match $familyPattern
        if ($before -eq $after)
        {
            break
        }
        Start-Sleep -Milliseconds 200
    }
    $after = (CheckNetIsolation.exe LoopbackExempt -s | Out-String) -match $familyPattern
    if ($before -ne $after)
    {
        throw "Loopback rollback failed: before=$before after=$after"
    }
    if (Get-Process archive_player_bridge -ErrorAction SilentlyContinue)
    {
        throw 'Archive HTTP bridge remains running after drop target exit'
    }
    Write-Output "MODERN_DROP_CLEANUP_PASS preexisting_loopback=$before after=$after"
}