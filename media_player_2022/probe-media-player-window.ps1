# media_player_2022/probe-media-player-window.ps1
# Read-only Win32 inspection of modern Media Player's top-level frame and descendants.
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class ASPWindowProbe {
    public delegate bool EnumCallback(IntPtr hwnd, IntPtr unused);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCallback callback, IntPtr unused);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr root, EnumCallback callback, IntPtr unused);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder buffer, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder buffer, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
}
'@
[ASPWindowProbe]::EnumWindows({
    param($hwnd, $unused)
    $title = [Text.StringBuilder]::new(512)
    $class = [Text.StringBuilder]::new(256)
    [void][ASPWindowProbe]::GetWindowText($hwnd, $title, $title.Capacity)
    [void][ASPWindowProbe]::GetClassName($hwnd, $class, $class.Capacity)
    if ($title.ToString() -eq 'Media Player' -or $class.ToString() -eq 'ApplicationFrameWindow')
    {
        $processId = [uint32]0
        [void][ASPWindowProbe]::GetWindowThreadProcessId($hwnd, [ref]$processId)
        [Console]::WriteLine("FRAME=$hwnd PID=$processId CLASS=$class TITLE=$title VISIBLE=$([ASPWindowProbe]::IsWindowVisible($hwnd))")
        [ASPWindowProbe]::EnumChildWindows($hwnd, {
            param($child, $unused)
            $childClass = [Text.StringBuilder]::new(256)
            [void][ASPWindowProbe]::GetClassName($child, $childClass, $childClass.Capacity)
            $childPid = [uint32]0
            [void][ASPWindowProbe]::GetWindowThreadProcessId($child, [ref]$childPid)
            [Console]::WriteLine("CHILD=$child PID=$childPid CLASS=$childClass")
            return $true
        }, [IntPtr]::Zero) | Out-Null
    }
    return $true
}, [IntPtr]::Zero) | Out-Null
