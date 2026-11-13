param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('AIMP','XMPlay')]
    [string]$Player,
    [ValidateSet('Win32','x64')]
    [string]$Platform = 'Win32',
    [string]$OutputFolder = ''
)
$ErrorActionPreference = 'Stop'
if($Player -eq 'XMPlay' -and $Platform -ne 'Win32') {
    throw 'The XMPlay archive API adapter is currently built for Win32 only.'
}
$repo = Split-Path $PSScriptRoot -Parent
if(!$OutputFolder) { $OutputFolder = Join-Path $PSScriptRoot 'Release' }
$settings = Join-Path $PSScriptRoot 'configure-adapters.ps1'
$prefix = if($Platform -eq 'x64') {'x64\Release'} else {'Release'}
$source = if($Player -eq 'AIMP') {
    Join-Path $repo "aimp_archive_stream\$prefix\aimp_archive_stream.dll"
} else {
    Join-Path $repo 'xmp_archive_stream\Release\xmp-archive-stream.dll'
}
if(!(Test-Path -LiteralPath $source) -or !(Test-Path -LiteralPath $settings)) {
    throw "Plugin or configuration script not built: $source"
}
$stage = Join-Path $env:TEMP ('ArchiveAdapter-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage | Out-Null
try {
    $dest = if($Player -eq 'AIMP') { Join-Path $stage 'Plugins\aimp_archive_stream' } else { $stage }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item -LiteralPath $source -Destination (Join-Path $dest (Split-Path $source -Leaf))
    Copy-Item -LiteralPath $settings -Destination (Join-Path $dest 'configure-adapters.ps1')
    if($Player -eq 'AIMP') {
        $install = "Extract the Plugins folder into AIMP's application root, then restart AIMP. Open AIMP Options > Archive Stream Player > Configure archive settings to edit shared preferences."
    } else {
        $install = "Copy xmp-archive-stream.dll and configure-adapters.ps1 to the XMPlay application root, then restart XMPlay. Open XMPlay's Archive Stream plugin Config button to edit shared preferences."
    }
    $install += " Shared settings location: HKCU\Software\ArchiveStreamPlayer\Adapters."
    $install += " Winamp and foobar2000 offer native preference pages as well as this shared settings UI."
    $install | Set-Content -Path (Join-Path $stage 'INSTALL.txt') -Encoding UTF8
    New-Item -ItemType Directory -Path $OutputFolder -Force | Out-Null
    $out = Join-Path $OutputFolder "ArchiveStreamPlayer-$Player-$Platform.zip"
    if(Test-Path $out) { Remove-Item -LiteralPath $out -Force }
    Compress-Archive -Path "$stage\*" -DestinationPath $out -CompressionLevel Optimal
    Write-Output "NATIVE_PACKAGE=$out"
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
