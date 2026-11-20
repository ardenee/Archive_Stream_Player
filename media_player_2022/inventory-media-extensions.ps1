# media_player_2022/inventory-media-extensions.ps1
# Read-only inspection of the modern Media Player package and Windows media handlers.
# The legacy Windows Media Player SDK uses different registry extension keys.
$ErrorActionPreference = 'Stop'
$pkg = Get-AppxPackage -Name Microsoft.ZuneMusic
if (!$pkg)
{
    throw 'Windows Media Player (2022) package Microsoft.ZuneMusic was not found'
}
$xml = [xml](Get-Content -LiteralPath (Join-Path $pkg.InstallLocation 'AppxManifest.xml') -Raw)
$types = @((Select-Xml -Xml $xml -XPath "//*[local-name()='FileTypeAssociation']/*[local-name()='SupportedFileTypes']/*[local-name()='FileType']" |
    ForEach-Object { $_.Node.InnerText }))
Write-Output "MODERN_PLAYER_VERSION=$($pkg.Version)"
Write-Output "PACKAGE_SUPPORTED_EXTENSION_COUNT=$($types.Count)"
$base = 'SOFTWARE\Microsoft\Windows Media Foundation\ByteStreamHandlers'
foreach ($ext in @('.mp3','.wav','.flac','.ogg','.m3u','.m3u8','.zip','.7z','.rar'))
{
    $native = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($base + '\' + $ext)
    $local = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($base + '\' + $ext)
    $handlerNames = @()
    if ($native) { $handlerNames += $native.GetValueNames(); $native.Dispose() }
    if ($local) { $handlerNames += $local.GetValueNames(); $local.Dispose() }
    Write-Output ("EXT={0} PACKAGE_SUPPORTED={1} MF_BYTESTREAM_HANDLERS={2}" -f $ext, ($types -contains $ext), ($handlerNames -join ','))
}
Write-Output 'NO_REGISTRY_CHANGES=1'