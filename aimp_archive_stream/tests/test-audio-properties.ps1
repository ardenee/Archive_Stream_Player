# Native, read-only audio header and (optionally) real archive regression.
# By default tests synthetic MP3/Xing, FLAC STREAMINFO and WAV headers.
# For a real directly streamable MP3/FLAC/WAV archive, pass -Archive.
param([string]$Archive = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$aimpSource = Get-Content (Join-Path $repo 'aimp_archive_stream\aimp_archive_stream.cpp') -Raw
if ($aimpSource -notmatch 'SetValueAsFloat\(AIMP_FILEINFO_PROPID_DURATION,\s*audio\.durationSeconds\)')
{
    throw 'AIMP FileInfo duration must receive seconds, not milliseconds'
}
Write-Output 'AIMP_DURATION_SECONDS_PROPERTY_CONTRACT_PASS'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'vswhere not found' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ toolchain not found' }
$dev = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$dir = Join-Path $env:TEMP ('ast-audio-properties-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dir -Force | Out-Null
try
{
    $exe = Join-Path $dir 'verify_audio_properties.exe'
    $source = Join-Path $PSScriptRoot 'verify_audio_properties.cpp'
    $cmd = '"' + $dev + '" -arch=x86 -host_arch=x64 >nul && cd /d "' + $dir + '" && cl /nologo /std:c++20 /EHsc /MD /DUNICODE /D_UNICODE /I"' + $repo + '" /Fe:"' + $exe + '" "' + $source + '" "' +
        (Join-Path $repo 'shared\debug_log.cpp') + '" "' +
        (Join-Path $repo 'archive_backend\Release\archive_backend.lib') + '" "' +
        (Join-Path $repo 'libarchive_static\Release\libarchive_static.lib') + '" "' +
        (Join-Path $repo 'unrar_static\Release\unrar_static.lib') + '" "' +
        (Join-Path $repo 'zlib_static\Release\zlib_static.lib') +
        '" /link /LTCG advapi32.lib bcrypt.lib crypt32.lib version.lib user32.lib shell32.lib'
    & cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw 'Audio-property regression did not compile' }
    if ($Archive)
    {
        & $exe $Archive
    }
    else
    {
        & $exe
    }
    if ($LASTEXITCODE -ne 0) { throw 'Audio-property regression failed' }
}
finally
{
    Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}
