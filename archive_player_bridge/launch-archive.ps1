param(
    [string]$Archive = '',
    [switch]$Configure,
    [ValidateSet('VLC','WMP','MusicBee','MediaMonkey','WACUPx64','XMPlay')]
    [string]$Player = 'VLC',
    [int]$MemoryMB = 0,
    [int]$CacheLimitMB = -1,
    [string]$TitleTemplate = '',
    [string]$PasswordList = '',
    [string]$PlayerPath = '',
    [switch]$UseHttpBridge,
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
if ($Configure) {
    $settingsScript = Join-Path $PSScriptRoot 'configure-adapters.ps1'
    if ($NoLaunch) { & $settingsScript -ShowSettings }
    else { & $settingsScript }
    return
}
if (!$Archive -or !(Test-Path -LiteralPath $Archive -PathType Leaf)) {
    throw "Archive not found: $Archive"
}
$Archive = (Resolve-Path -LiteralPath $Archive).ProviderPath
$hosts = @{
    VLC = @('C:\Program Files\VideoLAN\VLC\vlc.exe','C:\Program Files (x86)\VideoLAN\VLC\vlc.exe')
    WMP = @('C:\Program Files\Windows Media Player\wmplayer.exe','C:\Program Files (x86)\Windows Media Player\wmplayer.exe')
    MusicBee = @('C:\Program Files (x86)\MusicBee\MusicBee.exe','C:\Program Files\MusicBee\MusicBee.exe')
    MediaMonkey = @('C:\Program Files (x86)\MediaMonkey 5\MediaMonkey.exe','C:\Program Files\MediaMonkey 5\MediaMonkey.exe')
    WACUPx64 = @('C:\Program Files\WACUP\wacup.exe')
    XMPlay = @('C:\Program Files\XMPlay\xmplay.exe','C:\Program Files (x86)\XMPlay\xmplay.exe')
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

# XMPlay's native XMPARC adapter has real-host playback acceptance for
# single-file .7z and .rar archives. Its ZIP SDK contract passes, but XMPlay
# did not dispatch tested ZIP playback to the plugin. Preserve the working
# bridge for ZIP, split-volume and other unverified launch layouts.
# -UseHttpBridge always opts into host-on-demand HTTP streaming.
$nativeXmplayDispatch = ($Archive -match '(?i)\.(?:7z|rar)$') -and ($Archive -notmatch '(?i)\.part[0-9]+\.rar$')
if ($Player -eq 'XMPlay' -and !$UseHttpBridge -and $nativeXmplayDispatch) {
    if (!$playerExe) {
        throw 'Cannot locate XMPlay. Supply -PlayerPath to verify its native XMPARC installation.'
    }
    $playerFolder = Split-Path -Path $playerExe -Parent
    $native = @(
        (Join-Path $playerFolder 'xmp-archive-stream.dll'),
        (Join-Path $playerFolder 'Plugins\xmp-archive-stream.dll')
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (!$native) {
        throw 'XMPlay native Archive Stream XMPARC plugin is not installed beside the player. Install xmp-archive-stream.dll, or use -UseHttpBridge explicitly.'
    }
    Write-Host "NATIVE_ARCHIVE_READY player=XMPlay archive=$Archive plugin=$native"
    if ($NoLaunch) {
        Write-Host 'DRY_RUN_PASS player=XMPlay mode=native_xmparc http=0'
        return
    }
    $nativeProcess = Start-Process -FilePath $playerExe -WorkingDirectory $playerFolder -ArgumentList @('-play', ('"' + $Archive + '"')) -PassThru
    Write-Host "XMPLAY_NATIVE_PLAYER_PID=$($nativeProcess.Id)"
    Write-Host 'XMPlay native archive playback requested; no HTTP bridge was started.'
    return
}
if ($Player -eq 'XMPlay' -and !$UseHttpBridge -and !$nativeXmplayDispatch) {
    Write-Host 'XMPLAY_BRIDGE_FALLBACK reason=real_host_native_dispatch_unverified mode=on_demand_http'
}

# VLC 3 x64 can play an original archive through our own statically linked
# VLC access DLL. Build a native ast:// playlist instead of starting HTTP.
# Preserve the bridge if native binaries are not installed or the caller
# requested one-off bridge options that shared native settings cannot express.
if ($Player -eq 'VLC' -and !$UseHttpBridge) {
    # VLC 3's access-plugin ABI is 3_0_0f; this DLL is presently x64 only.
    # Never load this build into VLC 4, VLC 32-bit, or another PE machine.
    $vlcCompatible = $false
    if ($playerExe) {
        try {
            $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($playerExe)
            $stream = [IO.File]::OpenRead($playerExe)
            $reader = [IO.BinaryReader]::new($stream)
            try {
                $stream.Position = 0x3C
                $headerOffset = $reader.ReadInt32()
                if ($headerOffset -ge 64 -and ($headerOffset + 6) -le $stream.Length) {
                    $stream.Position = $headerOffset
                    $signature = $reader.ReadUInt32()
                    $machine = $reader.ReadUInt16()
                    $vlcCompatible = ($version.FileMajorPart -eq 3 -and $signature -eq 17744 -and $machine -eq 34404)
                }
            }
            finally {
                $reader.Dispose()
                $stream.Dispose()
            }
        }
        catch {
            $vlcCompatible = $false
        }
    }
    $vlcFolder = if ($playerExe) { Split-Path -Path $playerExe -Parent } else { '' }
    $installedVlcModule = if ($vlcFolder) { Join-Path $vlcFolder 'plugins\access\libarchive_stream_plugin.dll' } else { '' }
    $nativeModuleCandidates = @(
        $installedVlcModule,
        (Join-Path $PSScriptRoot 'plugins\access\libarchive_stream_plugin.dll'),
        (Join-Path $PSScriptRoot 'libarchive_stream_plugin.dll'),
        (Join-Path (Split-Path $PSScriptRoot -Parent) 'vlc_archive_stream\x64\Release\libarchive_stream_plugin.dll')
    )
    $vlcModule = $nativeModuleCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
    $nativeBuilderCandidates = @(
        (Join-Path $PSScriptRoot 'vlc_native_playlist.exe'),
        (Join-Path (Split-Path $PSScriptRoot -Parent) 'vlc_archive_stream\x64\Release\vlc_native_playlist.exe')
    )
    $nativeBuilder = $nativeBuilderCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    $oneOffOptions = $MemoryMB -gt 0 -or $CacheLimitMB -ge 0 -or $PasswordList -or $TitleTemplate
    if ($oneOffOptions) {
        Write-Host 'VLC_BRIDGE_FALLBACK reason=per_launch_overrides_require_bridge'
    }
    elseif (!$vlcCompatible) {
        Write-Host 'VLC_BRIDGE_FALLBACK reason=native_module_requires_VLC3_x64'
    }
    elseif (!$vlcModule -or !$nativeBuilder) {
        Write-Host 'VLC_BRIDGE_FALLBACK reason=native_dll_or_playlist_builder_not_available'
    }
    else {
        $nativeRoot = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\NativePlaylists'
        New-Item -ItemType Directory -Force -Path $nativeRoot | Out-Null
        # Remove only our own generated playlists older than a week.
        # This folder contains no audio payload or passwords.
        $expiry = [DateTime]::UtcNow.AddDays(-7)
        Get-ChildItem -LiteralPath $nativeRoot -Filter 'vlc-native-*.m3u8' -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTimeUtc -lt $expiry } |
            Remove-Item -Force -ErrorAction SilentlyContinue
        $nativePlaylist = Join-Path $nativeRoot ('vlc-native-' + [Guid]::NewGuid().ToString('N') + '.m3u8')
        & $nativeBuilder --archive $Archive --playlist $nativePlaylist
        if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $nativePlaylist -PathType Leaf)) {
            throw 'VLC native archive enumeration failed; no HTTP fallback after a password or listing error.'
        }
        $nativeContents = Get-Content -LiteralPath $nativePlaylist -Raw -Encoding UTF8
        if (!$nativeContents -or !$nativeContents.Contains('ast://') -or $nativeContents.Contains('http://127.0.0.1:')) {
            Remove-Item -LiteralPath $nativePlaylist -Force -ErrorAction SilentlyContinue
            throw 'VLC native archive playlist does not contain valid ast:// entries.'
        }
        Write-Host "NATIVE_VLC_READY=1 playlist=$nativePlaylist"
        if ($NoLaunch) {
            Remove-Item -LiteralPath $nativePlaylist -Force -ErrorAction SilentlyContinue
            Write-Host 'DRY_RUN_PASS player=VLC mode=native_sdk http=0'
            return
        }
        # VLC 3 officially loads modules from VLC_PLUGIN_PATH. Use the bundled
        # or locally built DLL without copying anything into Program Files.
        # The temporary change is inherited by only this launched VLC process.
        $oldPluginPath = $env:VLC_PLUGIN_PATH
        try {
            if ($vlcModule -ne $installedVlcModule) {
                $nativeFolder = Split-Path -Path $vlcModule -Parent
                $env:VLC_PLUGIN_PATH = if ($oldPluginPath) { $nativeFolder + ';' + $oldPluginPath } else { $nativeFolder }
            }
            Start-Process -FilePath $playerExe -WorkingDirectory $vlcFolder -ArgumentList @('--no-one-instance', ('"' + $nativePlaylist + '"'))
        }
        finally {
            $env:VLC_PLUGIN_PATH = $oldPluginPath
        }
        Write-Host 'VLC native archive playback requested; no HTTP bridge was started.'
        return
    }
}

$bridge = Join-Path $PSScriptRoot 'x64\Release\archive_player_bridge.exe'
if (!(Test-Path $bridge)) {
    throw 'Bridge not built; compile archive_player_bridge.vcxproj Release|x64.'
}
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $bridge
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $false
$psi.CreateNoWindow = $true
$psi.Arguments = '--archive "' + $Archive.Replace('"', '') + '" --lifetime 7200'
if ($MemoryMB -gt 0) { $psi.Arguments += ' --memory-mb ' + $MemoryMB }
if ($CacheLimitMB -ge 0) { $psi.Arguments += ' --cache-limit-mb ' + $CacheLimitMB }
if ($PasswordList) { $psi.Arguments += ' --password-list "' + $PasswordList.Replace('"', '') + '"' }
if ($TitleTemplate) { $psi.Arguments += ' --title-template "' + $TitleTemplate.Replace('"', '') + '"' }
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
        elseif ($Player -eq 'XMPlay') {
            # Explicit -UseHttpBridge override for large files or compatibility testing.
            Start-Process -FilePath $playerExe -WorkingDirectory (Split-Path -Path $playerExe -Parent) -ArgumentList @('-play', ('"' + $playlist + '"'))
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
