# Regression for Windows UTF-8 password files with and without BOM.
# Creates only disposable test assets and does not change shared preferences.
param([string]$SampleDirectory = 'D:\Temp\ArchiveStreamFoobar64Runtime\audio-test')
$ErrorActionPreference = 'Stop'
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$wav = Join-Path $SampleDirectory 'Generated WAV.wav'
if (!(Test-Path $sevenZip) -or !(Test-Path $wav)) { throw 'Missing 7-Zip or sample WAV' }
$qa = Join-Path $env:TEMP ('ast-vlc-password-bom-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $qa | Out-Null
$archive = Join-Path $qa 'qa-encrypted-header.7z'
$list = Join-Path $qa 'keeps.txt'
$secret = 'AST-QA-ONLY-2026'
$original = [Environment]::GetEnvironmentVariable('ARCHIVE_STREAM_VLC_PASSWORD_LIST', 'Process')
try
{
    Push-Location $SampleDirectory
    try
    {
        & $sevenZip a '-t7z' '-mhe=on' ('-p' + $secret) '-mx=1' $archive 'Generated WAV.wav' '-y' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Could not create QA encrypted-header 7z' }
    }
    finally { Pop-Location }

    foreach ($withBom in @($true, $false))
    {
        [IO.File]::WriteAllText($list, $secret + "`r`n", (New-Object System.Text.UTF8Encoding($withBom)))
        $env:ARCHIVE_STREAM_VLC_PASSWORD_LIST = $list
        & (Join-Path $PSScriptRoot 'test-native-directory-metadata.ps1') -Archive $archive -ExpectedTracks 1
        if (!$?) { throw ('Encrypted 7z VLC test failed: BOM=' + $withBom) }
        Write-Output ('VLC_ENCRYPTED_PASSWORD_LIST_PASS utf8_bom=' + $withBom)
    }
}
finally
{
    if ($null -eq $original)
    {
        Remove-Item Env:ARCHIVE_STREAM_VLC_PASSWORD_LIST -ErrorAction SilentlyContinue
    }
    else
    {
        $env:ARCHIVE_STREAM_VLC_PASSWORD_LIST = $original
    }
    Remove-Item -LiteralPath $qa -Force -Recurse -ErrorAction SilentlyContinue
}
