# media_player_2022/verify-media-player-filepicker.ps1
# Read-only comparison of Media Player's live File Open filter and its signed app manifest.
# Use -OpenIfNeeded to open the native picker during the same verification process.
param([switch]$OpenIfNeeded)
$ErrorActionPreference = 'Stop'
$package = Get-AppxPackage -Name Microsoft.ZuneMusic
if (!$package)
{
    throw 'Microsoft.ZuneMusic (Media Player 2022) is not installed'
}
$manifest = [xml](Get-Content -LiteralPath (Join-Path $package.InstallLocation 'AppxManifest.xml') -Raw)
$manifestTypes = @(
    Select-Xml -Xml $manifest -XPath "//*[local-name()='FileTypeAssociation']/*[local-name()='SupportedFileTypes']/*[local-name()='FileType']" |
        ForEach-Object { $_.Node.InnerText.ToLowerInvariant() } |
        Sort-Object -Unique
)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$desktop = [System.Windows.Automation.AutomationElement]::RootElement
$condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, 'Media Player')
$frame = $desktop.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
if (!$frame)
{
    throw 'Media Player (2022) top-level window is not open'
}
$items = $frame.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
$filter = @(
    $items |
        Where-Object { $_.Current.Name -match '^All files \(\*\.' } |
        Select-Object -First 1
)
if ($filter.Count -eq 0 -and $OpenIfNeeded)
{
    $button = $frame.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, 'Open file(s)'))
    if (!$button)
    {
        throw 'Media Player Open file(s) button not found'
    }
    $invoke = $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $invoke.Invoke()
    for ($i = 0; $i -lt 25 -and $filter.Count -eq 0; ++$i)
    {
        Start-Sleep -Milliseconds 250
        $items = $frame.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
        $filter = @($items | Where-Object { $_.Current.Name -match '^All files \(\*\.' } | Select-Object -First 1)
    }
}
if ($filter.Count -eq 0)
{
    throw 'Open file(s) is not visible; run with -OpenIfNeeded to open it automatically'
}
$pickerTypes = @(
    [regex]::Matches($filter[0].Current.Name, '\*\.([A-Za-z0-9]+)') |
        ForEach-Object { $_.Value.TrimStart('*').ToLowerInvariant() } |
        Sort-Object -Unique
)
Write-Output "MEDIA_PLAYER_VERSION=$($package.Version)"
Write-Output "MANIFEST_SUPPORTED_EXTENSIONS=$($manifestTypes.Count)"
Write-Output "OPEN_DIALOG_SUPPORTED_EXTENSIONS=$($pickerTypes.Count)"
Write-Output "FILTER_EXACTLY_MATCHES_MANIFEST=$(($manifestTypes -join ';') -eq ($pickerTypes -join ';'))"
Write-Output "FILTER_EXTRA_TYPES=$((@($pickerTypes | Where-Object { $_ -notin $manifestTypes })) -join ',')"
Write-Output "MANIFEST_EXTRA_TYPES=$((@($manifestTypes | Where-Object { $_ -notin $pickerTypes })) -join ',')"
foreach ($ext in @('.zip', '.7z', '.rar'))
{
    Write-Output "EXT=$ext MANIFEST=$($manifestTypes -contains $ext) FILE_OPEN=$($pickerTypes -contains $ext)"
}
$stateFile = Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\QA\media-player-2022-archive-extension-test.json'
Write-Output "TEST_REGISTRATIONS_ACTIVE=$(Test-Path $stateFile)"
Write-Output 'REGISTRY_CHANGES=0'
