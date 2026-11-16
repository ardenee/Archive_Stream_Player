[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$VlcExe,
    [string]$Archive
)
$ErrorActionPreference = 'Stop'
if(!(Test-Path -LiteralPath $VlcExe -PathType Leaf)) { throw "VLC not found: $VlcExe" }
if([string]::IsNullOrWhiteSpace($Archive)) {
    Add-Type -AssemblyName System.Windows.Forms
    $pick = New-Object Windows.Forms.OpenFileDialog
    $pick.Title = 'Choose a ZIP, 7z or RAR music archive'
    $pick.Filter = 'Music archives (*.zip;*.7z;*.rar;*.001)|*.zip;*.7z;*.rar;*.001|All files (*.*)|*.*'
    $pick.CheckFileExists = $true
    if($pick.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { return }
    $Archive = $pick.FileName
}
if(!(Test-Path -LiteralPath $Archive -PathType Leaf)) { throw "Archive not found: $Archive" }
& (Join-Path $PSScriptRoot 'launch-archive.ps1') -Player VLC -PlayerPath $VlcExe -Archive $Archive
