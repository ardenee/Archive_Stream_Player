# Run the Windows archive picker outside MediaMonkey's embedded UI.
# The caller supplies a private temporary response file; no audio is extracted.
param([Parameter(Mandatory = $true)][string]$ResultFile)

$ErrorActionPreference = 'Stop'
$chosen = ''
$failed = ''
try {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    try {
        $dialog.Title = 'Choose audio archive'
        $dialog.Filter = 'Audio archives (*.zip;*.7z;*.rar;*.001)|*.zip;*.7z;*.rar;*.001|All files (*.*)|*.*'
        $dialog.CheckFileExists = $true
        $dialog.Multiselect = $false
        $dialog.AutoUpgradeEnabled = $true
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            $chosen = $dialog.FileName
        }
    }
    finally {
        $dialog.Dispose()
    }
}
catch {
    $failed = $_.Exception.Message
}
finally {
    $answer = @{ ok = ($failed -eq ''); path = $chosen; error = $failed } | ConvertTo-Json -Compress
    $temporary = $ResultFile + '.partial'
    [System.IO.File]::WriteAllText($temporary, $answer, (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temporary -Destination $ResultFile -Force
}
