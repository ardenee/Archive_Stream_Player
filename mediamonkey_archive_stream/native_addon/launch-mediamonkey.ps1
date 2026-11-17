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
$bridgeSessionFolder = ''
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

    # MediaMonkey installs user add-on input DLLs in its profile Plugins
    # directory. Check that location before the application Plugins directory;
    # an older DLL may be present in Program Files at the same time.
    $companion = Join-Path $PSScriptRoot 'in_archive_stream.dll'
    $addonScriptsDir = Split-Path $PSScriptRoot -Parent
    $addonProfileRoot = Split-Path $addonScriptsDir -Parent
    $installedCandidates = @(
        (Join-Path $addonProfileRoot 'Plugins\in_archive_stream.dll'),
        (Join-Path $env:APPDATA 'MediaMonkey5\Plugins\in_archive_stream.dll')
    )
    if ($PlayerExe) {
        $installedCandidates += Join-Path (Split-Path $PlayerExe -Parent) 'Plugins\in_archive_stream.dll'
    }
    $useNative = $false
    if (!$UseHttpBridge -and !$NoLaunch -and
        (Test-Path -LiteralPath $companion -PathType Leaf)) {
        try {
            $expectedHash = (Get-FileHash -LiteralPath $companion -Algorithm SHA256).Hash
            foreach ($installed in ($installedCandidates | Select-Object -Unique)) {
                if (!(Test-Path -LiteralPath $installed -PathType Leaf)) { continue }
                $installedHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
                if ($expectedHash -eq $installedHash) {
                    $useNative = $true
                    Trace-Adapter 'NATIVE_INPUT_HASH_MATCH'
                    break
                }
            }
        }
        catch {
            $useNative = $false
            Trace-Adapter 'NATIVE_HASH_CHECK_FAIL using_http_compatibility=1' 'WARN'
        }
    }
    if ($useNative) {
        $mode = 'native'
        # MediaMonkey persists real .ast filenames in its playlists. They are
        # tiny references to archive entries, never extracted audio. Keep them
        # under persistent app data, outside both TEMP and managed audio cache.
        # Hash only the normalized archive pathname, not its contents or CRC.
        $archiveIdentity = [Text.Encoding]::UTF8.GetBytes($Archive.ToUpperInvariant())
        $hasher = [Security.Cryptography.SHA256]::Create()
        try {
            $archiveKey = [BitConverter]::ToString($hasher.ComputeHash($archiveIdentity)).Replace('-', '').ToLowerInvariant()
        }
        finally {
            $hasher.Dispose()
        }
        $nativeBase = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\NativePlaylists\MediaMonkey'
        $nativeRoot = Join-Path $nativeBase $archiveKey
        New-Item -ItemType Directory -Path $nativeRoot -Force | Out-Null
        Trace-Adapter 'MODE_NATIVE_READY http=0 persistent_references=1'
    }
    else {
        Trace-Adapter 'MODE_HTTP_COMPAT reason=missing_or_mismatched_native_input_or_override'
    }

    # Never run the binary in the installed add-on directory: Windows locks a
    # running EXE, preventing MediaMonkey's Addons manager from updating it.
    $bridgeSessionFolder = Join-Path ([System.IO.Path]::GetTempPath()) ('ArchiveStreamMM5Bridge-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $bridgeSessionFolder -Force | Out-Null
    $sessionBridge = Join-Path $bridgeSessionFolder 'archive_player_bridge.exe'
    Copy-Item -LiteralPath $bridge -Destination $sessionBridge -Force
    if ((Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $sessionBridge -Algorithm SHA256).Hash) {
        throw 'Archive Stream bridge session copy failed verification.'
    }
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $sessionBridge
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
    # HTTP sessions stay alive while MediaMonkey is open. Native sessions
    # leave the small persistent .ast references in place for saved playlists;
    # the decoder still streams directly from the original archive at playback.
    # Both launcher/bridge processes exit when MediaMonkey closes.
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
        else {
            $hostMissing++
            if ($hostMissing -ge 2) { break }
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
    # Do not delete $nativeRoot: MediaMonkey saves these .ast references in
    # Now Playing and user playlists. They are metadata, not managed cache.
    # Only the temporary bridge executable session is removed.
    if ($bridgeSessionFolder -and (Split-Path $bridgeSessionFolder -Leaf) -like 'ArchiveStreamMM5Bridge-*' -and
        (Test-Path -LiteralPath $bridgeSessionFolder -PathType Container)) {
        Remove-Item -LiteralPath $bridgeSessionFolder -Recurse -Force -ErrorAction SilentlyContinue
    }
}
