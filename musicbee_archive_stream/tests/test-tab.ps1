# Run from Windows PowerShell with the .NET Framework/VS C# compiler installed.
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio vswhere.exe is required' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (!$vsRoot) { throw 'MSBuild with Roslyn C# compiler is required' }
$csc = Join-Path $vsRoot 'MSBuild\Current\Bin\Roslyn\csc.exe'
if (!(Test-Path -LiteralPath $csc)) { throw "Compiler missing: $csc" }

$testExe = Join-Path $env:TEMP 'archive_stream_musicbee_tab_test.exe'
try {
    & $csc /nologo /define:AST_ARCHIVE_TAB_TEST /target:exe "/out:$testExe" /r:System.Windows.Forms.dll /r:System.Core.dll (Join-Path $PSScriptRoot 'verify_archive_player_tab.cs') (Join-Path $project 'ArchivePlayerTab.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Archive Player tab test build failed' }
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw 'Archive Player tab test failed' }
}
finally {
    Remove-Item -LiteralPath $testExe -Force -ErrorAction SilentlyContinue
}
