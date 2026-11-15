# Standalone Winamp output PCM contract regression, no audio device or host.
# Compiles a tiny mock Out_Module on both Win32 and x64 and compares all output bytes.
param([ValidateSet('Win32','x64','Both')][string]$Architecture = 'Both')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere))
{
    throw 'Visual Studio Installer vswhere.exe is required'
}

$vs = (& $vswhere -latest -products '*' -property installationPath | Select-Object -First 1)
if (!$vs)
{
    throw 'No compatible Visual Studio C++ installation found'
}

$temp = Join-Path ([IO.Path]::GetTempPath()) ('ArchiveStreamPcmOutputQA-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp -Force | Out-Null

try
{
    $targets = if ($Architecture -eq 'Both') { @('Win32','x64') } else { @($Architecture) }
    foreach ($target in $targets)
    {
        $vcvars = Join-Path $vs ('VC\Auxiliary\Build\' + $(if ($target -eq 'Win32') { 'vcvars32.bat' } else { 'vcvars64.bat' }))
        if (!(Test-Path -LiteralPath $vcvars))
        {
            throw ('Missing C++ compiler environment: ' + $vcvars)
        }

        $source = Join-Path $PSScriptRoot 'test_pcm_output_writer.cpp'
        $include = Join-Path $root 'third_party\winamp'
        $exe = Join-Path $temp ('pcm-output-' + $target + '.exe')
        $obj = Join-Path $temp ('pcm-output-' + $target + '.obj')
        $compile = 'call "' + $vcvars + '" >nul && cl /nologo /EHsc /std:c++17 /O2 /W4 ' +
                   '/I"' + $include + '" /Fo:"' + $obj + '" /Fe:"' + $exe + '" "' + $source + '"'
        & cmd.exe /d /c $compile
        if ($LASTEXITCODE -ne 0)
        {
            throw ('PCM contract compile failed: ' + $target)
        }

        & $exe
        if ($LASTEXITCODE -ne 0)
        {
            throw ('PCM contract output parity failed: ' + $target)
        }

        Write-Output ('ARCHIVE_STREAM_PCM_OUTPUT_CONTRACT_PASS ' + $target)
    }
}
finally
{
    if ((Test-Path -LiteralPath $temp) -and (Split-Path $temp -Leaf) -like 'ArchiveStreamPcmOutputQA-*')
    {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
