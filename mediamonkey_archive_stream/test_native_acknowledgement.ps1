# Verify MediaMonkey's archive bridge remains open only after native queue
# acknowledgement and closes cleanly on shutdown; no player is launched.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$package = Join-Path $PSScriptRoot 'Release\ArchiveStreamPlayer-MediaMonkey5-Native.mmip'
if (!(Test-Path -LiteralPath $package -PathType Leaf)) { throw "Missing packaged native addon: $package" }
$player = 'C:\Program Files (x86)\MediaMonkey 5\MediaMonkey.exe'
if (!(Test-Path -LiteralPath $player -PathType Leaf)) { throw "MediaMonkey test host executable not installed: $player" }
$folder = Join-Path $env:TEMP ('AST-MM5Handshake-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder -Force | Out-Null
$session = $null
try {
    # Test exactly the packaged bridge and launcher, not an incomplete source
    # directory missing its statically linked bridge executable.
    $packageZip = Join-Path $folder 'native-addon.zip'
    Copy-Item -LiteralPath $package -Destination $packageZip
    $extracted = Join-Path $folder 'Extracted'
    Expand-Archive -LiteralPath $packageZip -DestinationPath $extracted
    $runner = Join-Path $extracted 'launch-mediamonkey.ps1'
    if (!(Test-Path (Join-Path $extracted 'archive_player_bridge.exe'))) { throw 'Packaged bridge missing.' }
    $audio = Join-Path $folder 'tiny.mp3'
    [IO.File]::WriteAllBytes($audio, ([byte[]](1..2048 | ForEach-Object { [byte]($_ % 251) })))
    $archive = Join-Path $folder 'tiny.zip'
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $entry = $zip.CreateEntry('tiny.mp3', [IO.Compression.CompressionLevel]::NoCompression)
        $src = [IO.File]::OpenRead($audio)
        $dest = $entry.Open()
        try { $src.CopyTo($dest) } finally { $src.Dispose(); $dest.Dispose() }
    }
    finally { $zip.Dispose() }

    $ready = Join-Path $folder 'ready.json'
    $p = New-Object System.Diagnostics.ProcessStartInfo
    $p.FileName = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $p.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $runner + '" -Archive "' + $archive + '" -PlayerExe "' + $player + '" -ReadyFile "' + $ready + '"'
    $p.UseShellExecute = $false
    $p.CreateNoWindow = $true
    $session = [Diagnostics.Process]::Start($p)
    if (!$session) { throw 'Could not start MediaMonkey bridge launcher test process.' }

    $end = [DateTime]::UtcNow.AddSeconds(22)
    while (!(Test-Path -LiteralPath $ready -PathType Leaf) -and [DateTime]::UtcNow -lt $end) {
        if ($session.HasExited) { throw "Launcher exited before manifest (exit $($session.ExitCode))." }
        Start-Sleep -Milliseconds 200
    }
    if (!(Test-Path -LiteralPath $ready -PathType Leaf)) { throw 'Ready manifest not published.' }
    $manifest = Get-Content -LiteralPath $ready -Raw | ConvertFrom-Json
    if (!$manifest.ok -or $manifest.entries -ne 1 -or !(Test-Path -LiteralPath $manifest.playlist)) {
        throw 'Invalid real bridge-ready manifest.'
    }
    Write-Output 'MM5_NATIVE_BRIDGE_READY_MANIFEST_PASS'

    # The test acknowledges exactly one track, as MediaMonkey does after
    # app.player.addTracksAsync succeeds. No host executable is spawned.
    [IO.File]::WriteAllText($ready + '.ack', 'QUEUED=1')
    Start-Sleep -Seconds 2
    if ($session.HasExited) { throw 'Acknowledged bridge closed before playback could begin.' }

    $media = Get-Content -LiteralPath $manifest.playlist | Where-Object { $_ -match '^http://127\.0\.0\.1:' } | Select-Object -First 1
    if (!$media) { throw 'Media URL missing.' }
    # Windows PowerShell 5.1 reserves the Range header; use HttpWebRequest.
    $request = [Net.HttpWebRequest]::Create($media)
    $request.Proxy = $null
    $request.Timeout = 8000
    $request.AddRange(32, 95)
    $response = [Net.HttpWebResponse]$request.GetResponse()
    try {
        $memory = New-Object System.IO.MemoryStream
        try {
            $stream = $response.GetResponseStream()
            try { $stream.CopyTo($memory) } finally { $stream.Dispose() }
            $received = $memory.ToArray()
        }
        finally { $memory.Dispose() }
        if ([int]$response.StatusCode -ne 206 -or $received.Length -ne 64) {
            throw "Archive HTTP range mismatch: status=$($response.StatusCode) bytes=$($received.Length)"
        }
    }
    finally { $response.Dispose() }
    Write-Output 'MM5_NATIVE_ACKNOWLEDGED_ARCHIVE_STREAM_PASS'

    $shutdown = $media -replace '/entry/.*$', '/shutdown'
    Invoke-WebRequest -Uri $shutdown -Method Post -UseBasicParsing -TimeoutSec 3 | Out-Null
    if (!$session.WaitForExit(10000)) { throw 'Launcher failed to terminate after shutdown.' }
    if ($session.ExitCode -ne 0) { throw "Bridge launcher exited with $($session.ExitCode)" }
    if (Test-Path -LiteralPath $manifest.playlist) { throw 'Bridge failed to remove its temporary playlist.' }
    Write-Output 'MM5_NATIVE_BRIDGE_SHUTDOWN_AND_TEMP_CLEANUP_PASS'
}
finally {
    if ($session) {
        if (!$session.HasExited) { $session.Kill(); $session.WaitForExit(2500) | Out-Null }
        $session.Dispose()
    }
    # PowerShell termination does not automatically stop its child process on
    # a failed assertion. Never leave a test bridge serving decrypted entries.
    if ($extracted) {
        $owned = Join-Path $extracted 'archive_player_bridge.exe'
        Get-CimInstance Win32_Process -Filter "Name='archive_player_bridge.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ExecutablePath -eq $owned } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}
