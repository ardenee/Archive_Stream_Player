param(
    [ValidateSet('Win32','x64')]
    [string]$Platform = 'x64',
    [string]$OutputFolder = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $OutputFolder) { $OutputFolder = Join-Path $PSScriptRoot 'Release' }
$source = if ($Platform -eq 'x64') {
    Join-Path $PSScriptRoot 'x64\Release\foo_archive_stream.dll'
} else {
    Join-Path $PSScriptRoot 'Release\foo_archive_stream.dll'
}
$settings = Join-Path $root 'archive_player_bridge\configure-adapters.ps1'
foreach ($item in @($source,$settings)) {
    if (!(Test-Path -LiteralPath $item -PathType Leaf)) { throw "Required component missing: $item" }
}

$stage = Join-Path $env:TEMP ('ArchiveStream-Foobar-' + [Guid]::NewGuid().ToString('N'))
$components = Join-Path $stage 'components'
New-Item -ItemType Directory -Path $components -Force | Out-Null
try {
    Copy-Item -LiteralPath $source -Destination (Join-Path $components 'foo_archive_stream.dll')
    Copy-Item -LiteralPath $settings -Destination (Join-Path $components 'configure-adapters.ps1')
    foreach ($notice in @('LICENSE', 'LICENSING.md','third_party\README.md')) {
        $item = Join-Path $root $notice
        if (Test-Path -LiteralPath $item -PathType Leaf) {
            $destination = Join-Path $stage ('notices\' + ($notice -replace '[\\/]', '-'))
            New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
            Copy-Item -LiteralPath $item -Destination $destination
        }
    }
    @'
Archive Stream Player for foobar2000

Choose the package matching your foobar2000 process architecture.
Close foobar2000. Copy BOTH files inside components\ to your foobar2000
installation's components\ folder. For portable installations, use the
portable application's components\ folder. Restart foobar2000.

The native preference page is under Preferences > Tools > Archive Stream.
It also offers "Shared settings (all players)...", which opens the included
configure-adapters.ps1 for cache, password, track title and warmer options.

Archive Stream reads the shared registry settings under
HKCU\Software\ArchiveStreamPlayer\Adapters.
Third-party code keeps its separate licences. See notices\.
'@ | Set-Content -Path (Join-Path $stage 'INSTALL.txt') -Encoding UTF8
    New-Item -ItemType Directory -Path $OutputFolder -Force | Out-Null
    $file = Join-Path $OutputFolder "ArchiveStreamPlayer-foobar2000-$Platform.zip"
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $file -CompressionLevel Optimal
    Write-Output "FOOBAR_PACKAGE=$file"
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
