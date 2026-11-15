# MediaMonkey 5/2024 native JavaScript addon launcher.
# Uses the statically linked archive backend: native .ast input references first,
# with the existing HTTP bridge preserved for missing/mismatched input DLLs.
param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [string]$PlayerExe = '',
    [string]$ReadyFile = '',
    [switch]$NoLaunch,
    [switch]$UseHttpBridge
)
$ErrorActionPreference = 'Stop'
$bridge = Join-Path $PSScriptRoot 'archive_player_bridge.exe'
$bridgeProcess = $null
$shutdownUrl = ''
$ready = $false
$nativeReady = $false
$nativeRoot = ''
$mode = 'http'

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

    # The matching x86 Winamp input DLL must be installed as a MediaMonkey input
    # plugin. An old DLL may load but does not know the .ast manifest format.
    # Compare the actual installed DLL with the companion shipped in this
    # addon; mismatch safely retains the old HTTP bridge.
    $companion = Join-Path $PSScriptRoot 'in_archive_stream.dll'
    $installed = if ($PlayerExe) { Join-Path (Split-Path $PlayerExe -Parent) 'Plugins\in_archive_stream.dll' } else { '' }
    $useNative = $false
    if (!$UseHttpBridge -and !$NoLaunch -and $installed -and
        (Test-Path -LiteralPath $companion -PathType Leaf) -and
        (Test-Path -LiteralPath $installed -PathType Leaf)) {
        try {
            $expectedHash = (Get-FileHash -LiteralPath $companion -Algorithm SHA256).Hash
            $installedHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
            $useNative = $expectedHash -eq $installedHash
        }
        catch {
            $useNative = $false
            Trace-Adapter 'NATIVE_HASH_CHECK_FAIL using_http_compatibility=1' 'WARN'
        }
    }
    if ($useNative) {
        $mode = 'native'
        $nativeRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('ArchiveStreamMM5Native-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $nativeRoot -Force | Out-Null
        Trace-Adapter 'MODE_NATIVE_READY http=0'
    }
    else {
        Trace-Adapter 'MODE_HTTP_COMPAT reason=missing_or_mismatched_native_input_or_override'
    }

    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $bridge
    $start.Arguments = '--archive "' + $Archive + '" --lifetime 7200'
    if ($useNative) {
        $start.Arguments += ' --native-manifest-dir "' + $nativeRoot + '"'
    }
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
        if ($line.StartsWith('NATIVE_PLAYLIST_M3U8=')) { $playlist = $line.Substring(21) }
        if ($line.StartsWith('ARCHIVE_AUDIO_ENTRIES=')) { $entries = [int]$line.Substring(22) }
        if ($line.StartsWith('URL_M3U8=')) { $shutdownUrl = $line.Substring(9).Replace('/playlist.m3u8', '/shutdown') }
        if ($line -eq 'BRIDGE_READY=1' -and !$useNative) { $ready = $true; break }
        if ($line -eq 'NATIVE_MANIFEST_READY=1' -and $useNative) { $ready = $true; $nativeReady = $true; break }
    }
    if (!$ready -or !$playlist -or !(Test-Path -LiteralPath $playlist -PathType Leaf)) {
        throw 'Archive Stream bridge failed to prepare the archive playlist.'
    }
    Trace-Adapter "SESSION_READY mode=$mode entries=$entries"

    if ($ReadyFile) {
        # A private temp manifest allows the JavaScript addon to insert tracks
        # using MediaMonkey's native player API. Never write passwords or URLs
        # into this manifest; only an internal temporary playlist filename.
        $handoff = @{ ok = $true; playlist = $playlist; entries = $entries; mode = $mode; nativeRoot = $nativeRoot } | ConvertTo-Json -Compress
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
        Write-Output "MEDIAMONKEY_NATIVE_ADDON_SESSION_READY mode=$mode entries=$entries"
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
        if ($ackText -notmatch '^QUEUED=[1-9][0-9]*\s*$') {
            throw 'MediaMonkey returned an invalid archive queue acknowledgement.'
        }
        Trace-Adapter "HOST_QUEUE_ACCEPTED mode=$mode entries=$entries"
    }
    else {
        # External bridge clients retain the documented CLI import fallback.
        Start-Process -FilePath $PlayerExe -ArgumentList @('/NoSplash', '/Add', ('"' + $playlist + '"'))
        Trace-Adapter "PLAYLIST_IMPORT_DISPATCHED entries=$entries"
    }
    # HTTP keeps the bridge alive. Native mode keeps only the tiny .ast
    # reference files until MediaMonkey closes; no HTTP process remains alive.
    # Both modes are bounded to two hours and end after host exit.
    $until = [DateTime]::UtcNow.AddSeconds(7190)
    $hostWasSeen = $false
    $hostMissing = 0
    while (($nativeReady -or !$bridgeProcess.HasExited) -and [DateTime]::UtcNow -lt $until) {
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
    if ($nativeRoot -and (Split-Path $nativeRoot -Leaf) -like 'ArchiveStreamMM5Native-*' -and
        (Test-Path -LiteralPath $nativeRoot -PathType Container)) {
        Remove-Item -LiteralPath $nativeRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
