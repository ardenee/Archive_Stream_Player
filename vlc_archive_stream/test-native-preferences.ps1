param([string]$Archive = 'D:\Temp\ArchiveStreamFoobar64Runtime\audio-test\generated-audio.zip')
$ErrorActionPreference = 'Stop'
$vlc = 'C:\Program Files\VideoLAN\VLC\vlc.exe'
$log = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs\archive_stream_vlc.log'
if (!(Test-Path $Archive) -or !(Test-Path $vlc)) { throw 'Missing VLC or audio ZIP sample' }
$temp = Join-Path $env:TEMP ('ast-vlc-prefs-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
$cfg = Join-Path $temp 'vlcrc'
$stderr = Join-Path $temp 'stderr.txt'
$env:ARCHIVE_STREAM_ADAPTER_DEBUG = '1'

function Test-Launch([string]$Mode)
{
    $before = if (Test-Path $log) { [IO.File]::ReadAllText($log).Length } else { 0 }
    $args = @('--no-one-instance',('--config=' + $cfg),'--intf','dummy','--no-video','--aout','dummy','--verbose=1',('"' + $Archive + '"'))
    $proc = Start-Process $vlc -ArgumentList $args -PassThru -RedirectStandardError $stderr
    try { Start-Sleep -Seconds 5 }
    finally
    {
        if (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)
        {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if (!(Test-Path $log)) { throw 'No VLC adapter log' }
    $all = [IO.File]::ReadAllText($log)
    if ($all.Length -lt $before) { throw 'Adapter log rotated unexpectedly' }
    $delta = $all.Substring($before)
    $prefs = [regex]::Matches($delta,'NATIVE_PREFS[^\r\n]*') | Select-Object -Last 1
    $tags = [regex]::Matches($delta,'NATIVE_DIRECTORY_TAGS[^\r\n]*') | Select-Object -Last 1
    if (!$prefs -or !$tags) { throw ('Native VLC preferences or directory tags missing in ' + $Mode) }
    $p = $prefs.Value
    $t = $tags.Value
    if ($Mode -eq 'vlc')
    {
        if ($p -notmatch 'source=vlc' -or $p -notmatch 'memoryMb=17' -or $p -notmatch 'cacheLimitMb=321')
        {
            throw ('VLC override not read: ' + $p)
        }
        if ($t -notmatch 'title=QA-') { throw ('VLC title decoration not used: ' + $t) }
    }
    else
    {
        if ($p -notmatch 'source=shared') { throw ('Shared override not restored: ' + $p) }
        if ($t -match 'title=QA-') { throw ('VLC-specific title leaked to shared mode: ' + $t) }
    }
    Write-Output ('VLC_PREFERENCES_PASS mode=' + $Mode)
}

try
{
    # A private VLC config file exercises persisted values without modifying
    # the user's actual VLC settings or the shared Archive Stream registry.
    @(
        '[archive_stream] # Archive Stream native ZIP/7z/RAR seekable access'
        'ast-native-settings=1'
        'ast-memory-mb=17'
        'ast-cache-mb=321'
        'ast-use-file-tags=0'
        'ast-title-template=<#>. <filename>'
        'ast-title-prefix=QA-'
        'ast-title-postfix=-END'
        'ast-debug-logging=1'
    ) | Set-Content -LiteralPath $cfg -Encoding Ascii
    Test-Launch 'vlc'
    Test-Launch 'vlc'  # Second launch proves the same saved values are reloaded.
    (Get-Content -LiteralPath $cfg) -replace '^ast-native-settings=1$', 'ast-native-settings=0' |
        Set-Content -LiteralPath $cfg -Encoding Ascii
    Test-Launch 'shared'
}
finally
{
    Remove-Item -LiteralPath $temp -Force -Recurse -ErrorAction SilentlyContinue
}
