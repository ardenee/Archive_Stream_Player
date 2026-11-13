param(
    [Parameter(Mandatory = $true)]
    [string]$Archive,
    [ValidateSet('VLC','WMP','MusicBee','MediaMonkey','WACUPx64')]
    [string]$Player = 'VLC',
    [int]$MemoryMB = 50,
    [string]$PasswordList = '',
    [string]$PlayerPath = '',
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $Archive -PathType Leaf)) {
    throw "Archive not found: $Archive"
}
$Archive = (Resolve-Path -LiteralPath $Archive).ProviderPath
$bridge = Join-Path $PSScriptRoot 'x64\Release\archive_player_bridge.exe'
if (!(Test-Path $bridge)) {
    throw "Bridge not built; compile archive_player_bridge.vcxproj Release|x64."
}
if (!$PasswordList) {
    $PasswordList = Join-Path $env:LOCALAPPDATA 'WinampArchiveStream\keeps.txt'
}
$hosts = @{
    VLC = @('C:\Program Files\VideoLAN\VLC\vlc.exe','C:\Program Files (x86)\VideoLAN\VLC\vlc.exe')
    WMP = @('C:\Program Files\Windows Media Player\wmplayer.exe','C:\Program Files (x86)\Windows Media Player\wmplayer.exe')
    MusicBee = @('C:\Program Files (x86)\MusicBee\MusicBee.exe','C:\Program Files\MusicBee\MusicBee.exe')
    MediaMonkey = @('C:\Program Files (x86)\MediaMonkey 5\MediaMonkey.exe','C:\Program Files\MediaMonkey 5\MediaMonkey.exe')
    WACUPx64 = @('C:\Program Files\WACUP\wacup.exe')
}
if ($PlayerPath) {
    if (!(Test-Path -LiteralPath $PlayerPath -PathType Leaf)) {
        throw "Player executable not found: $PlayerPath"
    }
    $playerExe = (Resolve-Path -LiteralPath $PlayerPath).ProviderPath
}
else {
    $playerExe = $hosts[$Player] | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (!$NoLaunch -and !$playerExe) {
    throw "$Player is not installed at a supported path. Install it or update the script."
}
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $bridge
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $false
$psi.CreateNoWindow = $true
$psi.Arguments = '--archive "' + $Archive.Replace('"', '') + '" --memory-mb ' + $MemoryMB +
    ' --lifetime 7200 --password-list "' + $PasswordList.Replace('"', '') + '"'
$process = [Diagnostics.Process]::Start($psi)
if (!$process) { throw 'Could not start Archive Stream bridge.' }
$playlistM3u = ''
$playlistAsx = ''
$shutdown = ''
try {
    $ready = $false
    for ($lineNo = 0; $lineNo -lt 30; $lineNo++) {
        $line = $process.StandardOutput.ReadLine()
        if ($null -eq $line) { break }
        if ($line.StartsWith('PLAYLIST_M3U8=')) { $playlistM3u = $line.Substring(14) }
        if ($line.StartsWith('PLAYLIST_ASX=')) { $playlistAsx = $line.Substring(13) }
        if ($line.StartsWith('URL_M3U8=')) {
            $shutdown = $line.Substring(9).Replace('/playlist.m3u8', '/shutdown')
        }
        if ($line -eq 'BRIDGE_READY=1') {
            $ready = $true
            break
        }
    }
    if (!$ready -or !$playlistM3u -or !$playlistAsx) {
        throw 'Archive Stream could not open the archive or find playable audio.'
    }
    $playlist = if ($Player -eq 'WMP') { $playlistAsx } else { $playlistM3u }
    Write-Host "Archive ready for $Player. Playlist: $playlist"
    if (!$NoLaunch) {
        if ($Player -eq 'VLC') {
            Start-Process -FilePath $playerExe -ArgumentList @('--no-one-instance', ('"' + $playlist + '"'))
        }
        elseif ($Player -eq 'MediaMonkey') {
            # MM5+ officially accepts /Add with M3U playlists.
            Start-Process -FilePath $playerExe -ArgumentList @('/NoSplash', '/Add', ('"' + $playlist + '"'))
        }
        elseif ($Player -eq 'WACUPx64') {
            # The WACUP x64 preview does not load third-party Winamp input/general DLLs.
            # Its own playlist and HTTP playback interfaces accept the archive bridge URLs.
            Start-Process -FilePath $playerExe -WorkingDirectory (Split-Path -Path $playerExe -Parent) -ArgumentList @('/ENQPLAY', ('"' + $playlist + '"'))
        }
        else {
            Start-Process -FilePath $playerExe -ArgumentList @('"' + $playlist + '"')
        }
        Read-Host 'Press Enter to close the bridge and remove temporary cache files'
    }
    else {
        $content = Get-Content -LiteralPath $playlist -Raw
        if (!$content) { throw 'The generated playlist is empty.' }
        Write-Host "DRY_RUN_PASS player=$Player"
    }
}
finally {
    if ($shutdown -and !$process.HasExited) {
        try { Invoke-WebRequest -Uri $shutdown -Method Post -UseBasicParsing -TimeoutSec 3 | Out-Null } catch { }
    }
    if (!$process.WaitForExit(3500)) { $process.Kill() }
    $process.Dispose()
}
