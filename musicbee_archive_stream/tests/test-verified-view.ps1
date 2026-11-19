# Standalone UI contract: MusicBee-coloured verified duration list must be
# independent of its native tabs, present correct Time and tag columns,
# and never modify a playlist.
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (!$vsRoot) { throw 'Visual Studio C# compiler required' }
$csc = Join-Path $vsRoot 'MSBuild\Current\Bin\Roslyn\csc.exe'
if (!(Test-Path -LiteralPath $csc)) { throw 'Roslyn C# compiler missing' }
$dll = Join-Path $project 'Release\MB_ArchiveStream.dll'
if (!(Test-Path -LiteralPath $dll)) { throw 'Build MusicBee adapter Release before this test' }
$folder = Join-Path $env:TEMP ('ast-verified-view-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder -Force | Out-Null
try
{
    Copy-Item $dll (Join-Path $folder 'MB_ArchiveStream.dll') -Force
    $exe = Join-Path $folder 'verify_archive_verified_view.exe'
    & $csc /nologo /target:exe /platform:x86 "/out:$exe" /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll "/r:$dll" (Join-Path $PSScriptRoot 'verify_archive_verified_view.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Verified-view harness did not compile' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Verified-view harness failed' }
}
finally
{
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}
