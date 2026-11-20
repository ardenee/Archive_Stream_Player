# media_player_2022/test-modern-media-overlay.ps1
# QA: confirm the external in-window drop-zone lifecycle without changing player settings.
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'bin\x64\Release\modern_media_drop.exe'
$log = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs\modern_media_drop.log'
if (!(Test-Path -LiteralPath $exe -PathType Leaf))
{
    throw 'Modern Media Player overlay binary not built'
}
if (Get-Process modern_media_drop, archive_player_bridge -ErrorAction SilentlyContinue)
{
    throw 'Cannot run QA while a drop target or archive bridge already runs'
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AstOverlayQa {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rectangle rectangle);
    [StructLayout(LayoutKind.Sequential)] public struct Rectangle {
        public int left, top, right, bottom;
    }
}
'@
$before = CheckNetIsolation.exe LoopbackExempt -s | Out-String
$started = Start-Process -FilePath $exe -ArgumentList '--overlay' -PassThru
try
{
    $found = [IntPtr]::Zero
    for ($i = 0; $i -lt 30; ++$i)
    {
        Start-Sleep -Milliseconds 200
        $started.Refresh()
        if ($started.HasExited)
        {
            throw 'Overlay exited unexpectedly'
        }
        $found = [AstOverlayQa]::FindWindow('ArchiveStreamModernMediaDrop', 'Archive Stream - Media Player (2022)')
        if ($found -ne [IntPtr]::Zero)
        {
            break
        }
    }
    if ($found -eq [IntPtr]::Zero)
    {
        throw 'Overlay did not create its own independent Win32 drop target'
    }
    $lines = @(Get-Content -LiteralPath $log -Tail 50 -ErrorAction SilentlyContinue)
    if (!($lines -match "event=OVERLAY_STARTED \| pid=$($started.Id) \|"))
    {
        throw 'Overlay start event was not logged'
    }
    $player = [AstOverlayQa]::FindWindow('ApplicationFrameWindow', 'Media Player')
    $foreground = [AstOverlayQa]::GetAncestor([AstOverlayQa]::GetForegroundWindow(), 2)
    $visible = [AstOverlayQa]::IsWindowVisible($found)
    if ($player -ne [IntPtr]::Zero -and $foreground -eq $player)
    {
        if (!$visible)
        {
            throw 'Overlay is not visible while the Media Player window is foreground'
        }
        $bounds = [AstOverlayQa+Rectangle]::new()
        $position = [AstOverlayQa+Rectangle]::new()
        [void][AstOverlayQa]::GetWindowRect($player, [ref]$bounds)
        [void][AstOverlayQa]::GetWindowRect($found, [ref]$position)
        if ($position.left -lt $bounds.left -or $position.right -gt $bounds.right -or $position.top -lt $bounds.top -or $position.bottom -gt $bounds.bottom)
        {
            throw 'Archive overlay is not contained within the Media Player window'
        }
        Write-Output 'MODERN_OVERLAY_FOREGROUND_POSITION_PASS'
    }
    else
    {
        if ($visible)
        {
            throw 'Overlay is covering another application while Media Player is not foreground'
        }
        Write-Output 'MODERN_OVERLAY_NONFOREGROUND_HIDE_PASS'
    }
    if (Get-Process archive_player_bridge -ErrorAction SilentlyContinue)
    {
        throw 'Idle overlay unexpectedly started an archive bridge'
    }
    Write-Output "MODERN_OVERLAY_START_PASS pid=$($started.Id)"
}
finally
{
    $window = [AstOverlayQa]::FindWindow('ArchiveStreamModernMediaDrop', 'Archive Stream - Media Player (2022)')
    if ($window -ne [IntPtr]::Zero)
    {
        [void][AstOverlayQa]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    if (!$started.WaitForExit(12000))
    {
        $started.Kill()
        throw 'Overlay did not exit via WM_CLOSE'
    }
    $started.Dispose()
}
$after = CheckNetIsolation.exe LoopbackExempt -s | Out-String
if ($before -ne $after)
{
    throw 'Overlay changed Media Player loopback isolation without opening an archive'
}
Write-Output 'MODERN_OVERLAY_CLEANUP_PASS'
