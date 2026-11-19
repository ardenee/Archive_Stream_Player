# media_player_2022/test-mf-scheme-dispatch.ps1
# Nonpersistent per-user COM registration for a diagnostic-only scheme handler.
# No changes to real audio file associations, player configuration or shared settings.
param([switch]$TestModernPlayer)
$ErrorActionPreference = 'Stop'
$probeRoot = $PSScriptRoot
$dll = Join-Path $probeRoot 'bin\x64\Release\mf_scheme_probe.dll'
$harness = Join-Path $probeRoot 'bin\x64\Release\mf_scheme_probe_harness.exe'
$clsid = '{8E55B829-7771-4AD1-A0F2-3E6C294FB7D2}'
$schemeKey = 'Software\Microsoft\Windows Media Foundation\SchemeHandlers\astprobe:'
$classKey = 'Software\Classes\CLSID\' + $clsid
$log = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs\mf_scheme_probe.log'
$url = 'astprobe://archive-stream.test/track.mp3'
$testDir = Join-Path $env:TEMP ('ast-mf-dispatch-' + [Guid]::NewGuid().ToString('N'))
if (!(Test-Path $dll) -or !(Test-Path $harness)) { throw 'Build the probe DLL and harness first' }
$base = [Microsoft.Win32.Registry]::CurrentUser
if ($base.OpenSubKey($schemeKey) -or $base.OpenSubKey($classKey)) { throw 'Probe COM CLSID or scheme already registered: refusing to overwrite' }
New-Item -ItemType Directory -Path $testDir -Force | Out-Null
try
{
    $inproc = $base.CreateSubKey($classKey + '\InprocServer32')
    try
    {
        $inproc.SetValue('', $dll, [Microsoft.Win32.RegistryValueKind]::String)
        $inproc.SetValue('ThreadingModel', 'Both', [Microsoft.Win32.RegistryValueKind]::String)
    }
    finally { $inproc.Close() }
    $scheme = $base.CreateSubKey($schemeKey)
    try { $scheme.SetValue($clsid, 'Archive Stream diagnostic scheme handler', [Microsoft.Win32.RegistryValueKind]::String) }
    finally { $scheme.Close() }
    Write-Output 'MF_DIAGNOSTIC_REGISTRATION_USER_SCOPE=1'
    Write-Output ('REG_HKCU_CLSID=' + (Test-Path ('Registry::HKEY_CURRENT_USER\\' + $classKey + '\\InprocServer32')))
    Write-Output ('REG_HKCR_CLSID=' + (Test-Path ('Registry::HKEY_CLASSES_ROOT\\CLSID\\' + $clsid + '\\InprocServer32')))
    Write-Output ('REG_DIRECT_CLASSPATH=' + $classKey)
    Write-Output ('REG_CLASS_PATH_SLASH_COUNT=' + @($classKey.ToCharArray() | Where-Object { $_ -eq [char]92 }).Count)
    Write-Output ('REG_HKCU_STANDARD_PATH=' + (Test-Path ('HKCU:\Software\Classes\CLSID\' + $clsid + '\InprocServer32')))
    Write-Output ('REG_DIRECT_STANDARD_COM_CLASS=' + [bool]($base.OpenSubKey('Software\Classes\CLSID\' + $clsid + '\InprocServer32')))
    Write-Output ('REG_DIRECT_COM_CLASS=' + [bool]($base.OpenSubKey($classKey + '\InprocServer32')))
    Write-Output ('REG_DIRECT_MF_SCHEME=' + [bool]($base.OpenSubKey($schemeKey)))
    Write-Output ('PROCESS_IS_ADMIN=' + ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
    $logBefore = if (Test-Path $log) { [IO.File]::ReadAllText($log).Length } else { 0 }
    & $harness $url
    if ($LASTEXITCODE -ne 0) { throw 'Source resolver harness could not start' }
    $all = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
    $events = $all.Substring([Math]::Min($logBefore, $all.Length))
    if ($events -notmatch 'CLASS_FACTORY_REQUESTED' -or $events -notmatch 'BEGIN_CREATE_OBJECT')
    {
        # COM ignores per-user class registration in some elevated contexts.
        # Shell.Application delegates to Explorer's normal desktop token.
        Write-Output 'MF_ELEVATED_HARNESS_SCHEME_DISPATCH=NOT_OBSERVED'
        $desktop = New-Object -ComObject Shell.Application
        $desktop.ShellExecute($harness, ('"' + $url + '"'), $probeRoot, 'open', 0)
        Start-Sleep -Seconds 4
        $all = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
        $events = $all.Substring([Math]::Min($logBefore, $all.Length))
    }
    if ($events -notmatch 'CLASS_FACTORY_REQUESTED' -or $events -notmatch 'BEGIN_CREATE_OBJECT')
    {
        $taskName = 'ArchiveStream-MF-Probe-' + [Guid]::NewGuid().ToString('N')
        $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        $action = New-ScheduledTaskAction -Execute $harness -Argument ('"' + $url + '"') -WorkingDirectory $probeRoot
        $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
        try
        {
            Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Force -ErrorAction Stop | Out-Null
            Start-ScheduledTask -TaskName $taskName -ErrorAction Stop
            Start-Sleep -Seconds 5
            $info = Get-ScheduledTaskInfo -TaskName $taskName -ErrorAction Stop
            Write-Output ('MF_LIMITED_TASK_EXIT_CODE=' + $info.LastTaskResult)
        }
        finally
        {
            Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
        }
        $all = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
        $events = $all.Substring([Math]::Min($logBefore, $all.Length))
    }
    if ($events -notmatch 'CLASS_FACTORY_REQUESTED' -or $events -notmatch 'BEGIN_CREATE_OBJECT')
    {
        Write-Output 'MF_NORMAL_DESKTOP_SCHEME_DISPATCH=NOT_OBSERVED'
        throw 'Neither elevated nor desktop test invoked the MF handler; inspect COM registry view and process integrity'
    }
    Write-Output 'MF_DESKTOP_SCHEME_DISPATCH_PASS=1'
    if ($TestModernPlayer)
    {
        $control = Join-Path $testDir 'known-file.m3u8'
        $playlist = Join-Path $testDir 'custom-scheme.m3u8'
        $audio = 'D:\Temp\ArchiveStreamFoobar64Runtime\audio-test\generated-mp3.mp3'
        if (!(Test-Path $audio)) { throw 'Known-good MP3 control sample missing' }
        [IO.File]::WriteAllText($control, "#EXTM3U`r`n#EXTINF:2,Archive Stream Control`r`n$audio`r`n", [Text.Encoding]::UTF8)
        [IO.File]::WriteAllText($playlist, "#EXTM3U`r`n#EXTINF:2,Archive Stream Diagnostic`r`n$url`r`n", [Text.Encoding]::UTF8)
        $alias = Get-Command 'MediaPlayer.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
        if (!$alias) { throw 'Modern Media Player application execution alias not found' }
        $modern = Get-AppxPackage -Name Microsoft.ZuneMusic
        if (!$modern) { throw 'Modern Media Player package not installed' }
        Write-Output ('MEDIA_PLAYER_PACKAGE_VERSION=' + $modern.Version)
        Write-Output ('MEDIA_PLAYER_ALIAS=' + $alias)
        $existing = @(Get-Process 'Microsoft.Media.Player' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
        Write-Output ('MEDIA_PLAYER_PREEXISTING_PROCESSES=' + ($existing -join ','))
        $logBefore = [IO.File]::ReadAllText($log).Length
        $activator = Join-Path $probeRoot 'bin\x64\Release\activate_modern_media_file.exe'
        if (!(Test-Path $activator)) { throw 'Build activate_modern_media_file first' }
        & $activator $playlist (Join-Path $testDir 'modern-activation-result.txt')
        if ($LASTEXITCODE -ne 0) { throw 'Modern Media Player file activation rejected the diagnostic playlist' }
        Start-Sleep -Seconds 9
        $modernProcesses = @(Get-Process 'Microsoft.Media.Player' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
        Write-Output ('MEDIA_PLAYER_AFTER_LAUNCH_PIDS=' + ($modernProcesses -join ','))
        $all = [IO.File]::ReadAllText($log)
        $newEvents = $all.Substring([Math]::Min($logBefore, $all.Length))
        if ($newEvents -match 'BEGIN_CREATE_OBJECT')
        {
            Write-Output 'MODERN_MEDIA_PLAYER_MF_SCHEME_DISPATCH=YES'
            Write-Output ($newEvents | Select-String 'BEGIN_CREATE_OBJECT')
        }
        else
        {
            Write-Output 'MODERN_MEDIA_PLAYER_MF_SCHEME_DISPATCH=NOT_OBSERVED'
            Write-Output 'This does not prove all Media Foundation handlers are unsupported; first verify the modern app actually opened the supplied playlist.'
        }
        # Do not terminate a preexisting player or change a user's queued media.
        Write-Output ('MF_CONTROL_PLAYLIST=' + $control)
    }
}
finally
{
    if ($base.OpenSubKey($schemeKey)) { $base.DeleteSubKeyTree($schemeKey, $false) }
    if ($base.OpenSubKey($classKey)) { $base.DeleteSubKeyTree($classKey, $false) }
    Write-Output 'MF_DIAGNOSTIC_COM_REGISTRATION_REMOVED=1'
    # Both test playlists are disposable; preserve the diagnostic invocation
    # in the log, never retain temporary media files after the test.
    Remove-Item -LiteralPath $testDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Output 'MF_DIAGNOSTIC_TEMP_PLAYLISTS_REMOVED=1'
}
