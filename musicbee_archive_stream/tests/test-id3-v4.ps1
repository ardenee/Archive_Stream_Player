# Standalone ID3v2.3 metadata-alias contract test.
# No player installation, user music, or actual archives are changed.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio installation locator is required' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (!$vsRoot) { throw 'MSBuild is required' }
$csc = Join-Path $vsRoot 'MSBuild\Current\Bin\Roslyn\csc.exe'
if (!(Test-Path -LiteralPath $csc)) { throw "C# compiler missing: $csc" }

$testExe = Join-Path $env:TEMP ('ast-id3-v4-test-' + [guid]::NewGuid().ToString('N') + '.exe')
try
{
    & $csc /nologo /target:exe "/out:$testExe" /r:System.Core.dll (Join-Path $PSScriptRoot 'verify_archive_player_id3_v4.cs') (Join-Path $root 'ArchivePlayerId3Alias.cs')
    if ($LASTEXITCODE -ne 0) { throw 'ID3 v4 contract harness build failed' }
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw 'ID3 v4 contract harness execution failed' }
}
finally
{
    Remove-Item -LiteralPath $testExe -Force -ErrorAction SilentlyContinue
}
