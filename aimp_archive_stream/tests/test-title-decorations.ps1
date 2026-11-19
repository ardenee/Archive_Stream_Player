# Read-only shared title precedence contract for native host adapters.
# Requires Visual Studio 2026 C++ tools. Does not touch live registry settings.
$ErrorActionPreference = 'Stop'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'Visual Studio Installer vswhere not found.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ toolchain not installed.' }
$dev = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
if (!(Test-Path $dev)) { throw 'VsDevCmd.bat not found.' }
$source = Join-Path $PSScriptRoot 'verify_title_decorations.cpp'
$out = Join-Path $env:TEMP ('ast-title-decorations-' + [guid]::NewGuid().ToString('N') + '.exe')
$cmd = '"' + $dev + '" -arch=x86 -host_arch=x64 >nul && cl /nologo /std:c++20 /EHsc /MD /DUNICODE /D_UNICODE /Fe:"' + $out + '" "' + $source + '" /link advapi32.lib shell32.lib user32.lib'
try
{
    & cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw 'Native prefix/postfix test did not compile.' }
    & $out
    if ($LASTEXITCODE -ne 0) { throw 'Native prefix/postfix test failed.' }
}
finally
{
    Remove-Item $out -Force -ErrorAction SilentlyContinue
    Remove-Item ([IO.Path]::ChangeExtension($out,'.obj')) -Force -ErrorAction SilentlyContinue
}
