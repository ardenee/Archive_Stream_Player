# Shared playback, cache, passwords, titles and warming for all player adapters.
# Native Winamp and foobar pages keep their host-specific options.
param(
    [switch]$ShowSettings,
    [int]$SetMemoryMB = 0,
    [int]$SetCacheLimitMB = -1,
    [int]$SetCacheAgeDays = 0,
    [Nullable[bool]]$SetCacheAutoCleanup = $null,
    [Nullable[bool]]$SetWarmSolidArchives = $null,
    [string]$SetPasswordList = '',
    [string]$SetTitleTemplate = '',
    [Nullable[bool]]$SetPromptPasswords = $null,
    [Nullable[bool]]$SetAskToSavePassword = $null,
    [Nullable[bool]]$SetDebugLogging = $null,
    [string]$SetLogDirectory = '',
    [switch]$ClearStaleCache,
    [switch]$ClearManagedCache,
    [switch]$ClearLogs
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
        CacheAgeDays = [int](& $get 'CacheAgeDays' 30)
        CacheAutoCleanup = [bool](& $get 'CacheAutoCleanup' 1)
        WarmSolidArchives = [bool](& $get 'WarmSolidArchives' 1)
        PasswordList = [string](& $get 'PasswordList' '')
        TitleTemplate = [string](& $get 'TitleTemplate' '<filename>')
        PromptPasswords = [bool](& $get 'PromptPasswords' 1)
        AskToSavePassword = [bool](& $get 'AskToSavePassword' 1)
        DebugLogging = [bool](& $get 'DebugLogging' 0)
        LogDirectory = [string](& $get 'LogDirectory' '')
    }
}

function Write-AdapterSettings($settings) {
    if ($settings.MemoryMB -lt 1 -or $settings.MemoryMB -gt 512) { throw 'Memory threshold must be 1–512 MiB.' }
    if ($settings.CacheLimitMB -lt 0 -or $settings.CacheLimitMB -gt 20480) { throw 'Managed-cache limit must be 0–20480 MiB.' }
    if ($settings.CacheAgeDays -lt 1 -or $settings.CacheAgeDays -gt 365) { throw 'Cache age must be 1–365 days.' }
    if ($templates -cnotcontains $settings.TitleTemplate) { throw 'Invalid track-title template.' }
    if ($settings.PasswordList.Length -ge 4096) { throw 'Password file path too long.' }
    if ($settings.LogDirectory.Length -ge 4096) { throw 'Log directory path too long.' }
    if ($settings.LogDirectory -and -not [IO.Path]::IsPathRooted($settings.LogDirectory)) { throw 'Log directory must be an absolute path.' }
    New-Item -Path $path -Force | Out-Null
    foreach ($number in @('MemoryMB','CacheLimitMB','CacheAgeDays')) {
        New-ItemProperty -LiteralPath $path -Name $number -Value ([int]$settings.$number) -PropertyType DWord -Force | Out-Null
    }
    foreach ($flag in @('PromptPasswords','AskToSavePassword','CacheAutoCleanup','WarmSolidArchives','DebugLogging')) {
        New-ItemProperty -LiteralPath $path -Name $flag -Value ([int][bool]$settings.$flag) -PropertyType DWord -Force | Out-Null
    }
    foreach ($field in @('TitleTemplate','PasswordList','LogDirectory')) {
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

function Get-ManagedCacheFolder {
    Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\ManagedCache'
}

function Get-ManagedCacheFiles {
    $folder = Get-ManagedCacheFolder
    if (-not (Test-Path -LiteralPath $folder -PathType Container)) { return @() }
    @(Get-ChildItem -LiteralPath $folder -File -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -match '^[0-9A-Fa-f]{16}_[0-9]+\.[A-Za-z0-9]{1,10}$'
    })
}

function Get-ManagedCacheSummary {
    $files = @(Get-ManagedCacheFiles)
    $bytes = [long]0
    foreach ($f in $files) { $bytes += $f.Length }
    '{0} reusable tracks, {1:N1} MiB on disk' -f $files.Count,($bytes / 1MB)
}

function Clear-ManagedCache {
    # Open playback handles reject deletion. Leave those tracks alone and
    # clear other generated entries even when the settings window was opened
    # from a running player.
    $removed = 0
    $locked = 0
    foreach ($f in @(Get-ManagedCacheFiles)) {
        try {
            Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop
            $removed++
        } catch {
            $locked++
        }
    }
    if ($locked) { Write-Warning "$locked cached tracks could not be deleted because they are in use or access was denied." }
    return $removed
}

function Get-LogFolder {
    if ($config.LogDirectory) { return [IO.Path]::GetFullPath($config.LogDirectory) }
    Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs'
}

function Get-LogFiles {
    $folder = Get-LogFolder
    if (-not (Test-Path -LiteralPath $folder -PathType Container)) { return @() }
    @(Get-ChildItem -LiteralPath $folder -File -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -match '^archive_stream_(winamp|winamp-gen|foobar2000|aimp|xmplay|bridge|musicbee)\.log$'
    })
}

function Get-LogSummary {
    $files = @(Get-LogFiles)
    $bytes = [long]0
    foreach ($f in $files) { $bytes += $f.Length }
    '{0} player logs, {1:N1} MiB on disk' -f $files.Count,($bytes / 1MB)
}

function Clear-Logs {
    # The logger opens/closes on each write, so remove inactive logs now.
    # If a write currently owns a file handle Windows will reject removal.
    $removed = 0
    $locked = 0
    foreach ($file in @(Get-LogFiles)) {
        try {
            Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
            ++$removed
        } catch {
            ++$locked
        }
    }
    if ($locked) { Write-Warning "$locked log files could not be cleared because they are in use or access was denied." }
    return $removed
}

$config = Read-AdapterSettings
$set = $false
if ($SetMemoryMB -gt 0) { $config.MemoryMB = $SetMemoryMB; $set = $true }
if ($SetCacheLimitMB -ge 0) { $config.CacheLimitMB = $SetCacheLimitMB; $set = $true }
if ($SetCacheAgeDays -gt 0) { $config.CacheAgeDays = $SetCacheAgeDays; $set = $true }
if ($null -ne $SetCacheAutoCleanup) { $config.CacheAutoCleanup = [bool]$SetCacheAutoCleanup; $set = $true }
if ($null -ne $SetWarmSolidArchives) { $config.WarmSolidArchives = [bool]$SetWarmSolidArchives; $set = $true }
if ($SetPasswordList) { $config.PasswordList = $SetPasswordList; $set = $true }
if ($SetTitleTemplate) { $config.TitleTemplate = $SetTitleTemplate; $set = $true }
if ($null -ne $SetPromptPasswords) { $config.PromptPasswords = [bool]$SetPromptPasswords; $set = $true }
if ($null -ne $SetAskToSavePassword) { $config.AskToSavePassword = [bool]$SetAskToSavePassword; $set = $true }
if ($null -ne $SetDebugLogging) { $config.DebugLogging = [bool]$SetDebugLogging; $set = $true }
if ($SetLogDirectory) { $config.LogDirectory = $SetLogDirectory; $set = $true }
if ($set) { Write-AdapterSettings $config }
if ($ClearStaleCache) {
    'STALE_SESSIONS_REMOVED=' + (Clear-StaleSessions)
}
if ($ClearManagedCache) {
    'MANAGED_TRACKS_REMOVED=' + (Clear-ManagedCache)
}
if ($ClearLogs) {
    'LOG_FILES_REMOVED=' + (Clear-Logs)
}
if ($ShowSettings) {
    Write-Output ($config | ConvertTo-Json -Depth 3)
    Write-Output ('CACHE_STATUS=' + (Get-CacheSummary))
    Write-Output ('MANAGED_CACHE_STATUS=' + (Get-ManagedCacheSummary))
    Write-Output ('LOG_STATUS=' + (Get-LogSummary))
    return
}
if ($set -or $ClearStaleCache -or $ClearManagedCache -or $ClearLogs) { return }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = New-Object Windows.Forms.Form
$form.Text = 'Archive Stream Player - Settings (all players)'
$form.ClientSize = New-Object Drawing.Size(650,740)
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
Label 20 17 'Common settings: Winamp, foobar2000, AIMP, XMPlay, MusicBee and bridge players'|Out-Null
Label 20 54 'Memory threshold (MiB)'|Out-Null
$memory = New-Object Windows.Forms.NumericUpDown
$memory.Minimum=1; $memory.Maximum=512; $memory.Value=[Math]::Max(1,[Math]::Min(512,$config.MemoryMB))
$memory.Location = New-Object Drawing.Point(230,50)
$memory.Width=90; $form.Controls.Add($memory)
Label 20 92 'Maximum managed cache (MiB)'|Out-Null
$cache = New-Object Windows.Forms.NumericUpDown
$cache.Minimum=0; $cache.Maximum=20480
$cache.Value=[Math]::Max(0,[Math]::Min(20480,$config.CacheLimitMB))
$cache.Location = New-Object Drawing.Point(230,88)
$cache.Width=90; $form.Controls.Add($cache)
Label 327 92 '0 disables disk fallback'|Out-Null
Label 20 128 'Remove cached tracks older than (days)' | Out-Null
$age = New-Object Windows.Forms.NumericUpDown
$age.Minimum=1; $age.Maximum=365; $age.Value=[Math]::Max(1,[Math]::Min(365,$config.CacheAgeDays))
$age.Location = New-Object Drawing.Point(230,125)
$age.Width=90; $form.Controls.Add($age)
$autoCleanup = New-Object Windows.Forms.CheckBox
$autoCleanup.Text = 'Automatic size and age cleanup'
$autoCleanup.Location = New-Object Drawing.Point(20,160)
$autoCleanup.Width=320; $autoCleanup.Checked=$config.CacheAutoCleanup
$form.Controls.Add($autoCleanup)
$warm = New-Object Windows.Forms.CheckBox
$warm.Text = 'Prepare remaining tracks of solid archives in the background'
$warm.Location = New-Object Drawing.Point(20,189)
$warm.Width=590; $warm.Checked=$config.WarmSolidArchives
$form.Controls.Add($warm)
$prompt = New-Object Windows.Forms.CheckBox
$prompt.Text = 'Prompt when a saved password is missing'
$prompt.Location = New-Object Drawing.Point(20,220)
$prompt.Width=370; $prompt.Checked=$config.PromptPasswords
$form.Controls.Add($prompt)
$savePassword = New-Object Windows.Forms.CheckBox
$savePassword.Text = 'Offer to save passwords after successful verification'
$savePassword.Location = New-Object Drawing.Point(20,248)
$savePassword.Width=500; $savePassword.Checked=$config.AskToSavePassword
$form.Controls.Add($savePassword)
Label 20 284 'Password list (one password per line; blank = default list)'|Out-Null
$password = New-Object Windows.Forms.TextBox
$password.Location = New-Object Drawing.Point(20,308)
$password.Width=490; $password.Text=$config.PasswordList
$form.Controls.Add($password)
$browse = Button 520 305 105 'Browse...'
$browse.Add_Click({
    $picker = New-Object Windows.Forms.OpenFileDialog
    $picker.Filter='Text files (*.txt)|*.txt|All files (*.*)|*.*'
    if($picker.ShowDialog() -eq 'OK') { $password.Text=$picker.FileName }
})
Label 20 351 'Playlist / metadata track-title format'|Out-Null
$title = New-Object Windows.Forms.ComboBox
$title.DropDownStyle = 'DropDownList'
$title.Location = New-Object Drawing.Point(20,375)
$title.Width=420
[void]$title.Items.AddRange([string[]]$templates)
$title.SelectedItem = $config.TitleTemplate
if ($title.SelectedIndex -lt 0) { $title.SelectedIndex=0 }
$form.Controls.Add($title)
$titlePreview = Label 20 396 ''
$title.Add_SelectedIndexChanged({
    $example = [string]$title.SelectedItem
    $example = $example.Replace('<archivetype>', '7Z').Replace('<filename>', 'Sample.mp3')
    $example = $example.Replace('<filetype>', 'mp3').Replace('<archive>', 'Album').Replace('<#>', '1')
    $titlePreview.Text = "Example title: $example"
})
if ($title.SelectedIndex -ge 0) {
    $example = [string]$title.SelectedItem
    $example = $example.Replace('<archivetype>', '7Z').Replace('<filename>', 'Sample.mp3')
    $example = $example.Replace('<filetype>', 'mp3').Replace('<archive>', 'Album').Replace('<#>', '1')
    $titlePreview.Text = "Example title: $example"
}
$logging = New-Object Windows.Forms.CheckBox
$logging.Text = 'Enable diagnostic logging for all players'
$logging.Location = New-Object Drawing.Point(20,417)
$logging.Width=340; $logging.Checked=$config.DebugLogging
$form.Controls.Add($logging)
Label 20 451 'Log folder (blank = local ArchiveStreamPlayer Logs folder)'|Out-Null
$logs = New-Object Windows.Forms.TextBox
$logs.Location = New-Object Drawing.Point(20,475)
$logs.Width=490; $logs.Text=$config.LogDirectory
$form.Controls.Add($logs)
$browseLogs = Button 520 472 105 'Browse...'
$browseLogs.Add_Click({
    $picker = New-Object Windows.Forms.FolderBrowserDialog
    $picker.Description='Select a folder for per-player diagnostic logs'
    if($picker.ShowDialog() -eq 'OK') { $logs.Text=$picker.SelectedPath }
})
Label 20 512 'Fallback cache is shared across players and reused when a track is requested again.'|Out-Null
$cacheStatus=Label 20 540 (Get-CacheSummary)
$managedStatus=Label 20 563 (Get-ManagedCacheSummary)
$logStatus=Label 20 586 (Get-LogSummary)
$open = Button 20 643 135 'Open temp folder'
$open.Add_Click({ Start-Process explorer.exe -ArgumentList @($env:TEMP) })
$clear = Button 166 643 169 'Clear stale temp cache'
$clear.Add_Click({
    try {
        $count=Clear-StaleSessions
        $cacheStatus.Text=Get-CacheSummary
        [Windows.Forms.MessageBox]::Show("Removed $count inactive sessions older than 24 hours.",'Archive Stream')|Out-Null
    } catch {
        [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Cannot clear cache')|Out-Null
    }
})
$managedOpen = Button 20 609 150 'Open managed cache'
$managedOpen.Add_Click({
    $folder=Get-ManagedCacheFolder
    if (-not (Test-Path -LiteralPath $folder -PathType Container)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
    Start-Process explorer.exe -ArgumentList @($folder)
})
$managedClear = Button 181 609 170 'Clear managed cache'
$managedClear.Add_Click({
    try {
        if ([Windows.Forms.MessageBox]::Show('Delete all reusable Archive Stream Player track files?','Clear managed cache', 'YesNo', 'Warning') -ne 'Yes') { return }
        $count=Clear-ManagedCache
        $managedStatus.Text=Get-ManagedCacheSummary
        [Windows.Forms.MessageBox]::Show("Removed $count cached tracks.",'Archive Stream')|Out-Null
    } catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Cannot clear managed cache')|Out-Null }
})
$logsOpen = Button 362 609 126 'Open logs folder'
$logsOpen.Add_Click({
    try {
        $folder = if ($logs.Text) { [IO.Path]::GetFullPath($logs.Text) } else { Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\Logs' }
        New-Item -ItemType Directory -Path $folder -Force | Out-Null
        Start-Process explorer.exe -ArgumentList @($folder)
    } catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Cannot open log folder')|Out-Null }
})
$logsClear = Button 362 643 126 'Clear all logs'
$logsClear.Add_Click({
    try {
        if ([Windows.Forms.MessageBox]::Show('Delete shared diagnostic logs? Files currently in use will be kept.','Clear Archive Stream logs', 'YesNo', 'Warning') -ne 'Yes') { return }
        $config.LogDirectory = $logs.Text
        $count=Clear-Logs
        $logStatus.Text=Get-LogSummary
        [Windows.Forms.MessageBox]::Show("Removed $count log files.",'Archive Stream')|Out-Null
    } catch { [Windows.Forms.MessageBox]::Show($_.Exception.Message,'Cannot clear logs')|Out-Null }
})
$ok = Button 442 693 86 'Save'
$cancel = Button 538 693 86 'Cancel'
$cancel.Add_Click({ $form.Close() })
$ok.Add_Click({
    $requested = [PSCustomObject]@{
        MemoryMB=[int]$memory.Value; CacheLimitMB=[int]$cache.Value
        CacheAgeDays=[int]$age.Value; CacheAutoCleanup=[bool]$autoCleanup.Checked
        WarmSolidArchives=[bool]$warm.Checked
        PasswordList=$password.Text; TitleTemplate=[string]$title.SelectedItem
        PromptPasswords=$prompt.Checked; AskToSavePassword=$savePassword.Checked
        DebugLogging=[bool]$logging.Checked; LogDirectory=[string]$logs.Text
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
