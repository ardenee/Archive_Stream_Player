# media_player_2022/test-http-range-bridge.ps1
# Headless regression for HTTP transport (not a Media Player UI test).
param(
    [string]$Archive = 'M:\Music\VA - Subspecies 4 (2024).zip',
    [int]$ExpectedTracks = 9
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$binary = Join-Path $repository 'archive_player_bridge\x64\Release\archive_player_bridge.exe'
$log = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs\archive_stream_bridge.log'
$qa = Join-Path $env:TEMP ('ast-http-range-' + [Guid]::NewGuid().ToString('N'))
$stdout = Join-Path $qa 'stdout.txt'
$stderr = Join-Path $qa 'stderr.txt'
$process = $null
if (!(Test-Path $binary) -or !(Test-Path $Archive))
{
    throw 'HTTP bridge binary or archive test fixture not found'
}
if (Get-Process archive_player_bridge -ErrorAction SilentlyContinue)
{
    throw 'Another HTTP bridge session is active; test aborted to avoid interference'
}
New-Item -ItemType Directory -Path $qa | Out-Null
$oldDebug = [Environment]::GetEnvironmentVariable('ARCHIVE_STREAM_ADAPTER_DEBUG','Process')
$env:ARCHIVE_STREAM_ADAPTER_DEBUG = '1'
$before = if (Test-Path $log) { [IO.File]::ReadAllText($log).Length } else { 0 }
try
{
    $args = @('--archive', ('"' + $Archive + '"'), '--lifetime', '45', '--playlist-dir', ('"' + $qa + '"'), '--prompt-password', '0')
    $process = Start-Process -FilePath $binary -ArgumentList $args -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $ready = $false
    for ($i = 0; $i -lt 70; $i++)
    {
        Start-Sleep -Milliseconds 150
        if ((Test-Path $stdout) -and ((Get-Content $stdout -Raw) -match 'BRIDGE_READY=1'))
        {
            $ready = $true
            break
        }
        $process.Refresh()
        if ($process.HasExited)
        {
            break
        }
    }
    if (!$ready)
    {
        throw ('Bridge startup failed: ' + (Get-Content $stderr -Raw -ErrorAction SilentlyContinue))
    }
    $lines = Get-Content $stdout
    $playlist = ($lines | Where-Object { $_ -like 'PLAYLIST_M3U8=*' } | Select-Object -Last 1).Substring(14)
    $audio = @([regex]::Matches([IO.File]::ReadAllText($playlist), '(?m)^http://127\.0\.0\.1:\d+/[0-9a-f]+/entry/\d+\.[a-z0-9]+\r?$') | ForEach-Object { $_.Value.TrimEnd([char]13) })
    if ($audio.Count -ne $ExpectedTracks)
    {
        throw "Unexpected playlist track count: $($audio.Count) instead of $ExpectedTracks"
    }
    $head = [System.Net.HttpWebRequest][System.Net.WebRequest]::Create($audio[0])
    $head.Method = 'HEAD'
    $head.Timeout = 5000
    $headResponse = $head.GetResponse()
    try
    {
        if ([int]$headResponse.StatusCode -ne 200)
        {
            throw 'HEAD did not return HTTP 200'
        }
        $total = [long]$headResponse.ContentLength
    }
    finally
    {
        $headResponse.Close()
    }
    $request = [System.Net.HttpWebRequest][System.Net.WebRequest]::Create($audio[0])
    $request.AddRange(0,4095)
    $request.Timeout = 7000
    $response = $request.GetResponse()
    try
    {
        $range = $response.Headers['Content-Range']
        if ([int]$response.StatusCode -ne 206 -or $range -ne "bytes 0-4095/$total")
        {
            throw "Unexpected range response: $range"
        }
        $stream = $response.GetResponseStream()
        $received = 0
        $buffer = New-Object byte[] 4096
        while ($received -lt $buffer.Length)
        {
            $n = $stream.Read($buffer, $received, $buffer.Length - $received)
            if ($n -le 0)
            {
                throw "Range body ended after $received bytes"
            }
            $received += $n
        }
        $stream.Dispose()
    }
    finally
    {
        $response.Close()
    }
    Start-Sleep -Milliseconds 300
    $all = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
    if ($all.Length -lt $before)
    {
        throw 'Bridge log rotated during test'
    }
    $new = $all.Substring($before)
    if ($new -notmatch 'HTTP_AUDIO_TRANSFER[^\r\n]*method=GET[^\r\n]*payload_sent=4096[^\r\n]*completed=1')
    {
        throw 'Bridge did not account for the exact 4096 body bytes'
    }
    Write-Output "HTTP_RANGE_BRIDGE_PASS tracks=$ExpectedTracks head=200 range=206 payload=4096 total=$total"
}
finally
{
    if ($process)
    {
        $lines = Get-Content $stdout -ErrorAction SilentlyContinue
        $url = $lines | Where-Object { $_ -like 'URL_M3U8=*' } | Select-Object -Last 1
        if ($url)
        {
            $shutdown = $url.Substring(9).Replace('/playlist.m3u8','/shutdown')
            try { Invoke-WebRequest -Uri $shutdown -Method POST -UseBasicParsing -TimeoutSec 3 | Out-Null } catch {}
        }
        if (!$process.WaitForExit(4500))
        {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        $process.Dispose()
    }
    [Environment]::SetEnvironmentVariable('ARCHIVE_STREAM_ADAPTER_DEBUG', $oldDebug, 'Process')
    Remove-Item -LiteralPath $qa -Recurse -Force -ErrorAction SilentlyContinue
}