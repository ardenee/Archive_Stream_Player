# Packaged MediaMonkey 5 addon, shared-backend dry run, and session cleanup.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = Split-Path $PSScriptRoot -Parent
$package = Join-Path $PSScriptRoot 'Release\ArchiveStreamPlayer-MediaMonkey5-Native.mmip'
if (!(Test-Path -LiteralPath $package -PathType Leaf)) { throw "Missing package: $package" }
$folder = Join-Path $env:TEMP ('ArchiveStream-MM5AddonTest-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $folder | Out-Null
try {
    $zipPath = Join-Path $folder 'addon.zip'
    Copy-Item -LiteralPath $package -Destination $zipPath
    $installed = Join-Path $folder 'InstalledAddon'
    Expand-Archive -LiteralPath $zipPath -DestinationPath $installed
    $manifest = Get-Content -LiteralPath (Join-Path $installed 'info.json') -Raw | ConvertFrom-Json
    if ($manifest.type -ne 'general' -or $manifest.id -ne 'archiveStreamPlayerNative') {
        throw 'MediaMonkey native addon package manifest is invalid.'
    }
    foreach ($file in @('actions_add.js', 'archive_player_bridge.exe',
                        'launch-mediamonkey.ps1', 'configure-adapters.ps1',
                        'LICENSE', 'LICENSING.md', 'THIRD-PARTY-NOTICES.md')) {
        if (!(Test-Path -LiteralPath (Join-Path $installed $file) -PathType Leaf)) {
            throw "Missing native addon entry: $file"
        }
    }
    Write-Output 'MEDIAMONKEY_NATIVE_ADDON_MANIFEST_AND_PACKAGE_PASS'

    $audio = Join-Path $folder 'synthetic.mp3'
    $bytes = New-Object byte[] 4096
    for ($n = 0; $n -lt $bytes.Length; $n++) { $bytes[$n] = [byte]($n % 251) }
    [System.IO.File]::WriteAllBytes($audio, $bytes)
    $archive = Join-Path $folder 'synthetic-archive.zip'
    $zip = [System.IO.Compression.ZipFile]::Open($archive, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $entry = $zip.CreateEntry('synthetic.mp3', [System.IO.Compression.CompressionLevel]::NoCompression)
        $source = [System.IO.File]::OpenRead($audio)
        $destination = $entry.Open()
        try { $source.CopyTo($destination) }
        finally { $source.Dispose(); $destination.Dispose() }
    }
    finally { $zip.Dispose() }

    $runner = Join-Path $installed 'launch-mediamonkey.ps1'
    $ready = Join-Path $folder 'addon.ready.json'
    $result = & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $runner -Archive $archive -ReadyFile $ready -NoLaunch 2>&1
    $exit = $LASTEXITCODE
    $result | ForEach-Object { Write-Output $_ }
    if ($exit -ne 0 -or @($result | Where-Object { $_ -match 'MEDIAMONKEY_NATIVE_ADDON_DRY_RUN_PASS' }).Count -ne 1) {
        throw 'MediaMonkey native addon bridge session failed its dry-run contract.'
    }
    if (!(Test-Path -LiteralPath $ready -PathType Leaf)) { throw 'Native queue handoff manifest was not published.' }
    $manifest = Get-Content -LiteralPath $ready -Raw | ConvertFrom-Json
    if (!$manifest.ok -or $manifest.entries -ne 1 -or !$manifest.playlist) {
        throw 'Native queue handoff manifest failed its contract.'
    }
    Write-Output 'MEDIAMONKEY_NATIVE_ADDON_PACKAGED_BRIDGE_AND_HANDOFF_PASS'
}
finally {
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}
