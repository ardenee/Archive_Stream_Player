# media_player_2022/test-mf-bytestream-registration.ps1
# Disposable HKCU diagnostic MIME/extension handler. NEVER writes .zip/.rar/.7z associations.
param([switch]$TestModernPlayer)
$ErrorActionPreference = 'Stop'
$base = $PSScriptRoot
$dll = Join-Path $base 'bin\x64\Release\mf_bytestream_probe.dll'
$harness = Join-Path $base 'bin\x64\Release\mf_bytestream_probe_harness.exe'
$activator = Join-Path $base 'bin\x64\Release\activate_modern_media_file.exe'
$clsid = '{9BCD35B6-0DB6-480B-93A1-3450EA40F016}'
$classKey = 'Software\Classes\CLSID\' + $clsid
$handlerKey = 'Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\.astmfprobe'
$log = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs\mf_bytestream_probe.log'
$source = 'D:\Temp\ArchiveStreamFoobar64Runtime\audio-test\generated-mp3.mp3'
$temporary = Join-Path $env:TEMP ('ast-mf-bytestream-' + [guid]::NewGuid().ToString('N'))
$file = Join-Path $temporary 'diagnostic.astmfprobe'
$registry = [Microsoft.Win32.Registry]::CurrentUser
if (!(Test-Path $dll) -or !(Test-Path $harness) -or !(Test-Path $source) -or ($TestModernPlayer -and !(Test-Path $activator))) {
    throw 'Missing source MP3 or bytestream probe DLL / harness / player activator'
}
if ($registry.OpenSubKey($classKey) -or $registry.OpenSubKey($handlerKey)) { throw 'Refusing to overwrite existing COM or Media Foundation registration' }
New-Item -ItemType Directory -Path $temporary -ErrorAction Stop | Out-Null
Copy-Item -LiteralPath $source -Destination $file -ErrorAction Stop
$before = if (Test-Path $log) { [IO.File]::ReadAllText($log).Length } else { 0 }
try {
    $com = $registry.CreateSubKey($classKey + '\InprocServer32')
    $com.SetValue('', $dll, [Microsoft.Win32.RegistryValueKind]::String)
    $com.SetValue('ThreadingModel', 'Both', [Microsoft.Win32.RegistryValueKind]::String)
    $com.Close()
    $handler = $registry.CreateSubKey($handlerKey)
    $handler.SetValue($clsid, 'Archive Stream diagnostic byte-stream handler only', [Microsoft.Win32.RegistryValueKind]::String)
    $handler.Close()
    'HKCU_BYTE_STREAM_DIAGNOSTIC_REGISTERED=1'
    & $harness $file
    'ELEVATED_DESKTOP_MF_HARNESS_EXIT=' + $LASTEXITCODE
    $all = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
    $events = $all.Substring([Math]::Min($before,$all.Length))
    if ($events -notmatch 'BEGIN_CREATE_OBJECT') {
        $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        $task = 'AST-MF-ByteProbe-' + [guid]::NewGuid().ToString('N')
        $action = New-ScheduledTaskAction -Execute $harness -Argument ('"' + $file + '"') -WorkingDirectory $base
        $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
        try {
            Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force -ErrorAction Stop | Out-Null
            Start-ScheduledTask -TaskName $task
            Start-Sleep -Seconds 3
            $info=Get-ScheduledTaskInfo -TaskName $task
            'NORMAL_USER_HARNESS_EXIT='+$info.LastTaskResult
        } finally {
            Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
        }
        $all = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
        $events = $all.Substring([Math]::Min($before,$all.Length))
    }
    $desktopInvoked = $events -match 'BEGIN_CREATE_OBJECT'
    'DESKTOP_RESOLVER_CALLED_BYTESTREAM_HANDLER='+$desktopInvoked
    $beforePlayer = if (Test-Path $log) { [IO.File]::ReadAllText($log).Length } else { 0 }
    if ($TestModernPlayer) {
        & $activator $file (Join-Path $temporary 'activation.txt')
        'MODERN_MEDIA_PLAYER_ACTIVATE_EXIT='+$LASTEXITCODE
        Start-Sleep -Seconds 4
        $all = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
        $appEvents = $all.Substring([Math]::Min($beforePlayer,$all.Length))
        'MODERN_PLAYER_CALLED_BYTESTREAM_HANDLER='+($appEvents -match 'BEGIN_CREATE_OBJECT')
        'MODERN_PLAYER_HANDLER_EVENTS:'
        $appEvents -split '\r?\n' | Where-Object{$_ -match 'event='} |Select-Object -First 18
    }
    'DESKTOP_HANDLER_EVENTS:'
    $events -split '\r?\n' | Where-Object{$_ -match 'event='} | Select-Object -First 14
} finally {
    if ($registry.OpenSubKey($handlerKey)) { $registry.DeleteSubKeyTree($handlerKey,$false) }
    if ($registry.OpenSubKey($classKey)) { $registry.DeleteSubKeyTree($classKey,$false) }
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
    'DIAGNOSTIC_HANDLER_REGISTRATION_REMOVED=' + (-not [bool]$registry.OpenSubKey($handlerKey) -and -not [bool]$registry.OpenSubKey($classKey))
}
