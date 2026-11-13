# Shared preferences for AIMP, XMPlay and the HTTP playlist bridge.
# Winamp and foobar2000 retain their independent native preference pages.
param(
    [switch]$ShowSettings,
    [int]$SetMemoryMB = 0,
    [int]$SetCacheLimitMB = -1,
    [string]$SetPasswordList = '',
    [string]$SetTitleTemplate = '',
    [Nullable[bool]]$SetPromptPasswords = $null,
    [Nullable[bool]]$SetAskToSavePassword = $null,
    [switch]$ClearStaleCache
)
$ErrorActionPreference = 'Stop'
$path = 'HKCU:\Software\ArchiveStreamPlayer\Adapters'
$templates = @(
    '<filename>',
    '<archivetype>: <filename>',
    '<#>. <filename>',
    '<filename> [<filetype>]',
    '<archivetype>: <#> - <filename>',
    '[<archive>] <#> - <filename>'
)

function Read-AdapterSettings {
    $stored = Get-ItemProperty -LiteralPath $path -ErrorAction SilentlyContinue
    $get = { param($field,$default)
        if ($null -ne $stored -and $null -ne $stored.$field) { return $stored.$field }
        return $default
    }
    [PSCustomObject]@{
        MemoryMB = [int](& $get 'MemoryMB' 50)
        CacheLimitMB = [int](& $get 'CacheLimitMB' 2048)
        PasswordList = [string](& $get 'PasswordList' '')
        TitleTemplate = [string](& $get 'TitleTemplate' '<filename>')
        PromptPasswords = [bool](& $get 'PromptPasswords' 1)
        AskToSavePassword = [bool](& $get 'AskToSavePassword' 1)
    }
}

function Write-AdapterSettings($settings) {
    if ($settings.MemoryMB -lt 1 -or $settings.MemoryMB -gt 512) { throw 'Memory threshold must be 1–512 MiB.' }
    if ($settings.CacheLimitMB -lt 0 -or $settings.CacheLimitMB -gt 20480) { throw 'Temporary-cache limit must be 0–20480 MiB.' }
    if ($templates -cnotcontains $settings.TitleTemplate) { throw 'Invalid track-title template.' }
    if ($settings.PasswordList.Length -ge 4096) { throw 'Password file path too long.' }
    New-Item -Path $path -Force | Out-Null
    foreach ($number in @('MemoryMB','CacheLimitMB')) {
        New-ItemProperty -LiteralPath $path -Name $number -Value ([int]$settings.$number) -PropertyType DWord -Force | Out-Null
    }
    foreach ($flag in @('PromptPasswords','AskToSavePassword')) {
        New-ItemProperty -LiteralPath $path -Name $flag -Value ([int][bool]$settings.$flag) -PropertyType DWord -Force | Out-Null
    }
    foreach ($field in @('TitleTemplate','PasswordList')) {
        New-ItemProperty -LiteralPath $path -Name $field -Value ([string]$settings.$field) -PropertyType String -Force | Out-Null
    }
}

function Get-StaleSessions {
    $threshold = (Get-Date).AddHours(-24)
    @(
        Get-ChildItem -LiteralPath $env:TEMP -Directory -ErrorAction SilentlyContinue |
        Where-Object {
            ($_.Name -match '^ArchiveBridge-[a-f0-9]{32}$' -or $_.Name -match '^ArchiveStreamMusicBee-[a-f0-9]{32}$') -and
            $_.LastWriteTime -lt $threshold
        }
    )
}

function Get-CacheSummary {
    $folders = @(Get-ChildItem -LiteralPath $env:TEMP -Directory -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -match '^ArchiveBridge-[a-f0-9]{32}$' -or $_.Name -match '^ArchiveStreamMusicBee-[a-f0-9]{32}$'
    })
    $bytes = [long]0
    foreach ($folder in $folders) {
        $files = @(Get-ChildItem -LiteralPath $folder.FullName -File -Recurse -ErrorAction SilentlyContinue)
        foreach ($f in $files) { $bytes += $f.Length }
    }
    return '{0} temporary sessions, {1:N1} MiB on disk, {2} older than 24 hours' -f $folders.Count,($bytes / 1MB),@(Get-StaleSessions).Count
}

function Clear-StaleSessions {
    $live = Get-Process -Name archive_player_bridge -ErrorAction SilentlyContinue
    if ($live) { throw 'A bridge session is running. Stop it before removing stale temporary cache.' }
    if (Get-Process -Name MusicBee -ErrorAction SilentlyContinue) {
        throw 'MusicBee is running. Close it before removing stale temporary session folders.'
    }
    $removed = 0
    foreach ($folder in @(Get-StaleSessions)) {
        Remove-Item -LiteralPath $folder.FullName -Recurse -Force -ErrorAction Stop
        ++$removed
    }
    return $removed
}

$config = Read-AdapterSettings
$set = $false
if ($SetMemoryMB -gt 0) { $config.MemoryMB = $SetMemoryMB; $set = $true }
if ($SetCacheLimitMB -ge 0) { $config.CacheLimitMB = $SetCacheLimitMB; $set = $true }
if ($SetPasswordList) { $config.PasswordList = $SetPasswordList; $set = $true }
if ($SetTitleTemplate) { $config.TitleTemplate = $SetTitleTemplate; $set = $true }
if ($null -ne $SetPromptPasswords) { $config.PromptPasswords = [bool]$SetPromptPasswords; $set = $true }
if ($null -ne $SetAskToSavePassword) { $config.AskToSavePassword = [bool]$SetAskToSavePassword; $set = $true }
if ($set) { Write-AdapterSettings $config }
if ($ClearStaleCache) {
    'STALE_SESSIONS_REMOVED=' + (Clear-StaleSessions)
}
if ($ShowSettings) {
    Write-Output ($config | ConvertTo-Json -Depth 3)
    Write-Output ('CACHE_STATUS=' + (Get-CacheSummary))
    return
}
if ($set -or $ClearStaleCache) { return }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = New-Object Windows.Forms.Form
$form.Text = 'Archive Stream Player - Shared Adapter Settings'
$form.ClientSize = New-Object Drawing.Size(650,460)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false

function Label($x,$y,$label) {
    $c = New-Object Windows.Forms.Label
    $c.Location = New-Object Drawing.Point($x,$y)
    $c.AutoSize = $true
    $c.Text = $label
    $form.Controls.Add($c)
    return $c
}
function Button($x,$y,$width,$caption) {
    $c = New-Object Windows.Forms.Button
    $c.Location = New-Object Drawing.Point($x,$y)
    $c.Size = New-Object Drawing.Size($width,28)
    $c.Text = $caption
    $form.Controls.Add($c)
    return $c
}
Label 20 17 'Shared settings: AIMP, XMPlay, MusicBee and bridge-based players'|Out-Null
Label 20 54 'Memory threshold (MiB)'|Out-Null
$memory = New-Object Windows.Forms.NumericUpDown
$memory.Minimum=1; $memory.Maximum=512; $memory.Value=[Math]::Max(1,[Math]::Min(512,$config.MemoryMB))
$memory.Location = New-Object Drawing.Point(230,50)
$memory.Width=90; $form.Controls.Add($memory)
Label 20 92 'Maximum temporary cache (MiB)'|Out-Null
$cache = New-Object Windows.Forms.NumericUpDown
$cache.Minimum=0; $cache.Maximum=20480
$cache.Value=[Math]::Max(0,[Math]::Min(20480,$config.CacheLimitMB))
$cache.Location = New-Object Drawing.Point(230,88)
$cache.Width=90; $form.Controls.Add($cache)
Label 327 92 '0 disables temporary-file fallback'|Out-Null
$prompt = New-Object Windows.Forms.CheckBox
$prompt.Text = 'Prompt when a saved password is missing'
$prompt.Location = New-Object Drawing.Point(20,122)
$prompt.Width=370; $prompt.Checked=$config.PromptPasswords
$form.Controls.Add($prompt)
$savePassword = New-Object Windows.Forms.CheckBox
$savePassword.Text = 'Offer to save passwords after successful verification'
$savePassword.Location = New-Object Drawing.Point(20,150)
$savePassword.Width=500; $savePassword.Checked=$config.AskToSavePassword
$form.Controls.Add($savePassword)
Label 20 190 'Password list (one password per line; blank = adapter default)'|Out-Null
$password = New-Object Windows.Forms.TextBox
$password.Location = New-Object Drawing.Point(20,215)
$password.Width=490; $password.Text=$config.PasswordList
$form.Controls.Add($password)
$browse = Button 520 212 105 'Browse...'
$browse.Add_Click({
    $picker = New-Object Windows.Forms.OpenFileDialog
    $picker.Filter='Text files (*.txt)|*.txt|All files (*.*)|*.*'
    if($picker.ShowDialog() -eq 'OK') { $password.Text=$picker.FileName }
})
Label 20 262 'Playlist / metadata track-title format'|Out-Null
$title = New-Object Windows.Forms.ComboBox
$title.DropDownStyle = 'DropDownList'
$title.Location = New-Object Drawing.Point(20,286)
$title.Width=420
[void]$title.Items.AddRange([string[]]$templates)
$title.SelectedItem = $config.TitleTemplate
if ($title.SelectedIndex -lt 0) { $title.SelectedIndex=0 }
$form.Controls.Add($title)
Label 20 327 'Temporary extraction files are removed when their bridge or native track ends.'|Out-Null
$cacheStatus=Label 20 350 (Get-CacheSummary)
$open = Button 20 379 110 'Open temp folder'
$open.Add_Click({ Start-Process explorer.exe -ArgumentList @($env:TEMP) })
$clear = Button 142 379 169 'Clear stale temp cache'
$clear.Add_Click({
    try {
        $count=Clear-StaleSessions
        $cacheStatus.Text=Get-CacheSummary
        [Windows.Forms.MessageBox]::Show("Removed $count inactive sessions older than 24 hours.",'Archive Stream')|Out-Null
    } catch {
        [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Cannot clear cache')|Out-Null
    }
})
$ok = Button 442 417 86 'Save'
$cancel = Button 538 417 86 'Cancel'
$cancel.Add_Click({ $form.Close() })
$ok.Add_Click({
    $requested = [PSCustomObject]@{
        MemoryMB=[int]$memory.Value; CacheLimitMB=[int]$cache.Value
        PasswordList=$password.Text; TitleTemplate=[string]$title.SelectedItem
        PromptPasswords=$prompt.Checked; AskToSavePassword=$savePassword.Checked
    }
    try {
        Write-AdapterSettings $requested
        $form.Close()
    } catch {
        [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Invalid settings')|Out-Null
    }
})
[void]$form.ShowDialog()
$form.Dispose()
