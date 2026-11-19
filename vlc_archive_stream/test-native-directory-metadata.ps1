param(
    [string]$Archive = 'M:\Music\VA - Subspecies 4 (2024).zip',
    [int]$ExpectedTracks = 9,
    [switch]$RequireEmbeddedTags
)

$ErrorActionPreference = 'Stop'
$vlc = 'C:\Program Files\VideoLAN\VLC\vlc.exe'
$plugin = 'C:\Program Files\VideoLAN\VLC\plugins\access\libarchive_stream_plugin.dll'
$log = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs\archive_stream_vlc.log'
if (!(Test-Path $vlc) -or !(Test-Path $plugin) -or !(Test-Path $Archive))
{
    throw 'VLC, native plugin, or test archive not found'
}
$before = if (Test-Path $log) { [IO.File]::ReadAllText($log).Length } else { 0 }
$env:ARCHIVE_STREAM_ADAPTER_DEBUG = '1'
$qa = Join-Path $env:TEMP ('ast-vlc-native-meta-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $qa | Out-Null
$stderr = Join-Path $qa 'vlc-stderr.txt'
$proc = Start-Process -FilePath $vlc -ArgumentList @('--no-one-instance','--intf','dummy','--no-video','--aout','dummy','--verbose=2',('"' + $Archive + '"')) -PassThru -RedirectStandardError $stderr
try
{
    Start-Sleep -Seconds 7
}
finally
{
    $live = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
    if ($live) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}
if (!(Test-Path $log)) { throw 'VLC native log was not written' }
$all = [IO.File]::ReadAllText($log)
if ($all.Length -lt $before) { throw 'VLC native log rotated during the test' }
$new = $all.Substring($before)
if ($new -notmatch 'LOCAL_ARCHIVE_REDIRECT')
{
    throw 'VLC did not select the native local-archive access handler'
}
$albumMarker = 'archive=' + $Archive + ' |'
$albums = @([regex]::Matches($new, 'NATIVE_DIRECTORY_OPEN[^\r\n]*') | ForEach-Object { $_.Value })
$matchingAlbums = @($albums | Where-Object { $_.Contains($albumMarker) -and $_ -match ('entries=' + $ExpectedTracks + '\b') })
if (!$matchingAlbums.Count)
{
    throw "VLC did not list $ExpectedTracks native-directory tracks for $Archive"
}
$plays = @([regex]::Matches($new, 'ACCESS_OPEN_OK[^\r\n]*') | ForEach-Object { $_.Value })
if (!@($plays | Where-Object { $_.Contains($albumMarker) }).Count)
{
    throw 'VLC did not start audio playback from the native access module for this archive'
}
$tags = @([regex]::Matches($new,'NATIVE_DIRECTORY_TAGS[^\r\n]*') | ForEach-Object { $_.Value } | Where-Object { $_.Contains($albumMarker) })
$indexes = @($tags | Select-String 'index=(\d+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
if ($indexes.Count -ne $ExpectedTracks)
{
    throw "Pre-play metadata did not populate for every track ($($indexes.Count)/$ExpectedTracks)"
}
if ($RequireEmbeddedTags)
{
    foreach ($line in $tags)
    {
        if ($line -notmatch 'tagged=1' -or $line -notmatch 'artist=\S' -or
            $line -notmatch 'album=\S' -or $line -notmatch 'genre=\S' -or $line -notmatch 'year=\d')
        {
            throw "Missing embedded pre-play tags: $line"
        }
    }
}
$raw = Get-Content $stderr -Raw
if ($raw -notmatch '/list/album\.astdir' -or $raw -notmatch 'using demux module "directory"')
{
    throw 'VLC did not select its native directory demux for the album'
}
Write-Output "VLC_NATIVE_DIRECTORY_METADATA_PASS entries=$ExpectedTracks tagged=$($RequireEmbeddedTags.IsPresent) playback=1 http=0"
