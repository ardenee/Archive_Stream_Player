# MediaMonkey 5/2024 native JavaScript addon launcher.
# All archive decoding and settings remain in the shared static Archive Stream bridge.
param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [string]$PlayerExe = '',
    [string]$ReadyFile = '',
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$bridge = Join-Path $PSScriptRoot 'archive_player_bridge.exe'
$bridgeProcess = $null
$shutdownUrl = ''
$ready = $false

function Trace-Adapter([string]$message, [string]$level = 'INFO') {
    try {
        $prefs = Get-ItemProperty -Path 'HKCU:\Software\ArchiveStreamPlayer\Adapters' -ErrorAction SilentlyContinue
        if (!$prefs -or [int]$prefs.DebugLogging -ne 1) { return }
        $folder = [string]$prefs.LogDirectory
        if (!$folder) { $folder = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs' }
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        $safe = $message.Replace([char]13, ' ').Replace([char]10, ' ')
        Add-Content -LiteralPath (Join-Path $folder 'archive_stream_mediamonkey_native.log') -Value ((Get-Date).ToUniversalTime().ToString('o') + ' | ' + $level + ' | MEDIAMONKEY | ' + $safe) -Encoding UTF8
    }
    catch { }
}

try {
    if (!(Test-Path -LiteralPath $bridge -PathType Leaf)) { throw 'Archive Stream bridge executable is missing from the addon.' }
    if (!(Test-Path -LiteralPath $Archive -PathType Leaf)) { throw 'Selected archive does not exist.' }
    $Archive = (Resolve-Path -LiteralPath $Archive).ProviderPath
    if ($Archive.IndexOf('"') -ge 0) { throw 'Invalid archive path.' }

    if (!$NoLaunch) {
        if ($PlayerExe -and (Test-Path -LiteralPath $PlayerExe -PathType Container)) {
            $PlayerExe = Join-Path $PlayerExe 'MediaMonkey.exe'
        }
        if (!$PlayerExe) {
            $PlayerExe = @(
                'C:\Program Files (x86)\MediaMonkey 5\MediaMonkey.exe',
                'C:\Program Files\MediaMonkey 5\MediaMonkey.exe'
            ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        }
        if (!$PlayerExe -or !(Test-Path -LiteralPath $PlayerExe -PathType Leaf)) {
            throw 'MediaMonkey 5 executable was not found.'
        }
    }

    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $bridge
    $start.Arguments = '--archive "' + $Archive + '" --lifetime 7200'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $false
    $bridgeProcess = [System.Diagnostics.Process]::Start($start)
    if (!$bridgeProcess) { throw 'Archive Stream bridge failed to start.' }

    $playlist = ''
    $entries = 0
    for ($i = 0; $i -lt 35; $i++) {
        $line = $bridgeProcess.StandardOutput.ReadLine()
        if ($null -eq $line) { break }
        if ($line.StartsWith('PLAYLIST_M3U8=')) { $playlist = $line.Substring(14) }
        if ($line.StartsWith('ARCHIVE_AUDIO_ENTRIES=')) { $entries = [int]$line.Substring(22) }
        if ($line.StartsWith('URL_M3U8=')) { $shutdownUrl = $line.Substring(9).Replace('/playlist.m3u8', '/shutdown') }
        if ($line -eq 'BRIDGE_READY=1') { $ready = $true; break }
    }
    if (!$ready -or !$playlist -or !(Test-Path -LiteralPath $playlist -PathType Leaf)) {
        throw 'Archive Stream bridge failed to prepare the archive playlist.'
    }
    Trace-Adapter "SESSION_READY entries=$entries"

    if ($ReadyFile) {
        # A private temp manifest allows the JavaScript addon to insert tracks
        # using MediaMonkey's native player API. Never write passwords or URLs
        # into this manifest; only an internal temporary playlist filename.
        $handoff = @{ ok = $true; playlist = $playlist; entries = $entries } | ConvertTo-Json -Compress
        $partial = $ReadyFile + '.partial'
        [System.IO.File]::WriteAllText($partial, $handoff, (New-Object System.Text.UTF8Encoding($false)))
        Move-Item -LiteralPath $partial -Destination $ReadyFile -Force
        Trace-Adapter "HOST_HANDOFF_READY entries=$entries"
    }

    if ($NoLaunch) {
        $lines = @(Get-Content -LiteralPath $playlist)
        if (@($lines | Where-Object { $_ -like '#EXTINF:*' }).Count -ne $entries -or $entries -lt 1) {
            throw 'Prepared playlist does not contain the expected track metadata.'
        }
        Write-Output "MEDIAMONKEY_NATIVE_ADDON_BRIDGE_READY entries=$entries"
        Write-Output 'MEDIAMONKEY_NATIVE_ADDON_DRY_RUN_PASS'
        return
    }

    if ($ReadyFile) {
        $ack = $ReadyFile + '.ack'
        for ($attempt = 0; $attempt -lt 120; $attempt++) {
            if (Test-Path -LiteralPath $ack -PathType Leaf) { break }
            Start-Sleep -Milliseconds 250
        }
        if (!(Test-Path -LiteralPath $ack -PathType Leaf)) {
            throw 'MediaMonkey did not acknowledge insertion of archive tracks.'
        }
        $ackText = Get-Content -LiteralPath $ack -Raw
        Remove-Item -LiteralPath $ack -Force -ErrorAction SilentlyContinue
        if ($ackText -notmatch '^QUEUED=[1-9][0-9]*s*$') {
            throw 'MediaMonkey returned an invalid archive queue acknowledgement.'
        }
        Trace-Adapter "HOST_QUEUE_ACCEPTED entries=$entries"
    }
    else {
        # External bridge clients retain the documented CLI import fallback.
        Start-Process -FilePath $PlayerExe -ArgumentList @('/NoSplash', '/Add', ('"' + $playlist + '"'))
        Trace-Adapter "PLAYLIST_IMPORT_DISPATCHED entries=$entries"
    }
    # Keep the bridge and its stdout handle alive while MediaMonkey plays the URLs.
    # Its 2-hour upper lifetime is enforced by the bridge itself; close sooner
    # when the host exits. The bridge owns and deletes its temporary playlist.
    $until = [DateTime]::UtcNow.AddSeconds(7190)
    $hostWasSeen = $false
    $hostMissing = 0
    while (!$bridgeProcess.HasExited -and [DateTime]::UtcNow -lt $until) {
        Start-Sleep -Seconds 5
        $active = @(Get-Process -Name MediaMonkey -ErrorAction SilentlyContinue).Count -gt 0
        if ($active) {
            $hostWasSeen = $true
            $hostMissing = 0
        }
        elseif ($hostWasSeen) {
            $hostMissing++
            if ($hostMissing -ge 6) { break }
        }
    }
}
catch {
    Trace-Adapter ('LAUNCH_FAIL type=' + $_.Exception.GetType().Name) 'ERROR'
    # Report bridge startup errors immediately to the in-process addon
    # instead of making its menu command wait for the full ready timeout.
    if ($ReadyFile -and !(Test-Path -LiteralPath $ReadyFile -PathType Leaf)) {
        try {
            $failure = @{ ok = $false; playlist = ''; entries = 0 } | ConvertTo-Json -Compress
            $partial = $ReadyFile + '.partial'
            [System.IO.File]::WriteAllText($partial, $failure, (New-Object System.Text.UTF8Encoding($false)))
            Move-Item -LiteralPath $partial -Destination $ReadyFile -Force
        }
        catch { }
    }
    throw
}
finally {
    if ($bridgeProcess) {
        if ($shutdownUrl -and !$bridgeProcess.HasExited) {
            try { Invoke-WebRequest -Uri $shutdownUrl -Method Post -UseBasicParsing -TimeoutSec 3 | Out-Null } catch { }
        }
        try {
            if (!$bridgeProcess.WaitForExit(5000)) { $bridgeProcess.Kill() }
        }
        catch {
            try { if (!$bridgeProcess.HasExited) { $bridgeProcess.Kill() } } catch { }
        }
        $bridgeProcess.Dispose()
    }
}
