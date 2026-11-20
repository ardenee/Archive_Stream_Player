# media_player_2022/test-archive-format-registrations.ps1
# DEV diagnostic only. Registers ZIP/7z/RAR via multiple current-user methods.
# Does NOT change existing archive defaults or Microsoft's signed package manifest.
# Enable: changes persist only until explicitly disabled. Disable restores baseline.
param(
    [ValidateSet('Enable','Status','Disable')][string]$Action='Status',
    [ValidateSet('All','Shell','SystemFileAssociations','Mime','OpenWith','MediaFoundation','LegacyWMP')][string[]]$Methods=@('All')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$family='Microsoft.ZuneMusic_8wekyb3d8bbwe'
$appId='Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic'
$appXProgId='AppXqj98qxeaynz6dv4459ayz6bnqxbyaqcs'
$probeClass='{9BCD35B6-0DB6-480B-93A1-3450EA40F016}'
$root=[Microsoft.Win32.Registry]::CurrentUser
$base=Split-Path $MyInvocation.MyCommand.Path -Parent
$dll=Join-Path $base 'bin\x64\Release\mf_bytestream_probe.dll'
$launcher=Join-Path $base 'bin\x64\Release\modern_media_drop.exe'
$stateFolder=Join-Path $env:LOCALAPPDATA 'ArchiveStreamPlayer\QA'
$stateFile=Join-Path $stateFolder 'media-player-2022-archive-extension-test.json'
$extensions=@('.zip','.7z','.rar')
$formatNames=@{'.zip'='zip';'.7z'='7z';'.rar'='rar'}
$selected=@($Methods)
if($selected -contains 'All') {
    $selected=@('Shell','SystemFileAssociations','Mime','OpenWith','MediaFoundation','LegacyWMP')
}
function Includes([string]$method) { return $selected -contains $method }
$roots=New-Object 'System.Collections.Generic.List[string]'
foreach($ext in $extensions) {
    $name=$formatNames[$ext]
    foreach($path in @(
      "Software\Classes\$ext",
      "Software\Classes\SystemFileAssociations\$ext",
      "Software\Classes\MIME\Database\Content Type\audio/x-archive-stream-$name",
      "Software\Classes\ArchiveStream.MediaPlayer2022.QA.$name",
      "Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\$ext",
      "Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\audio/x-archive-stream-$name",
      "Software\Microsoft\MediaPlayer\Player\Extensions\$ext"
    )) { $roots.Add($path) }
}
$roots.Add("Software\Classes\Applications\modern_media_drop.exe")
$roots.Add("Software\Classes\CLSID\$probeClass")

function GetValue([string]$path,[string]$name='') {
    $key=$root.OpenSubKey($path)
    if(!$key) { return $null }
    try { return $key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
    finally { $key.Dispose() }
}
function Put([string]$path,[string]$name,[object]$value,[Microsoft.Win32.RegistryValueKind]$kind) {
    $key=$root.CreateSubKey($path)
    if(!$key) { throw "Cannot create HKCU\$path" }
    try { $key.SetValue($name,$value,$kind) }
    finally { $key.Dispose() }
}
function TestKey([string]$path) {
    $key=$root.OpenSubKey($path)
    if(!$key) { return $false }
    $key.Dispose()
    return $true
}
function HasValue([string]$path,[string]$name) {
    $key=$root.OpenSubKey($path)
    if(!$key) { return $false }
    try { return $key.GetValueNames() -contains $name }
    finally { $key.Dispose() }
}
function GetChoice([string]$ext) {
    return GetValue "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$ext\UserChoice" 'ProgId'
}
function NotifyAssociations {
    if(-not ([System.Management.Automation.PSTypeName]'ArchiveStreamShellNotify').Type) {
        Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class ArchiveStreamShellNotify { [DllImport("shell32.dll")] public static extern void SHChangeNotify(uint eventId,uint flags,IntPtr a,IntPtr b); }'
    }
    [ArchiveStreamShellNotify]::SHChangeNotify(0x08000000,0,[IntPtr]::Zero,[IntPtr]::Zero)
}
function RemoveRoots([bool]$requireExclusive) {
    # The QA roots must have been nonexistent before Enable. Refuse to delete newly
    # added third-party values under .zip/.rar/.7z while a test is pending.
    if($requireExclusive) {
        foreach($ext in $extensions) {
            $path="Software\Classes\$ext"
            $k=$root.OpenSubKey($path)
            if($k) {
                try {
                    $extraValues=@($k.GetValueNames()|Where-Object{$_ -notin @('Content Type','PerceivedType')})
                    $extraKeys=@($k.GetSubKeyNames()|Where-Object{$_ -ne 'OpenWithProgids'})
                    if($extraValues.Count -or $extraKeys.Count) {
                        throw "Unexpected third-party changes in HKCU\$path; refusing unsafe rollback"
                    }
                } finally { $k.Dispose() }
            }
        }
    }
    foreach($path in ($roots.ToArray() | Sort-Object Length -Descending)) {
        if(TestKey $path) { $root.DeleteSubKeyTree($path,$false) }
    }
    NotifyAssociations
}
function PrintStatus {
    'MODE=STATUS'
    'TEST_ENABLED='+(Test-Path $stateFile)
    if(Test-Path $stateFile) {
        $saved=Get-Content $stateFile -Raw |ConvertFrom-Json
        'ACTIVE_METHODS='+($saved.methods -join ',')
    }
    'MICROSOFT_APPX_PROGID='+$appXProgId
    foreach($ext in $extensions) {
        $name=$formatNames[$ext]
        'EXT='+$ext
        '  USER_CHOICE='+(GetChoice $ext)
        '  SHELL_MIME='+(GetValue "Software\Classes\$ext" 'Content Type')
        '  SHELL_PERCEIVED='+(GetValue "Software\Classes\$ext" 'PerceivedType')
        '  SYSTEMFILE_PERCEIVED='+(GetValue "Software\Classes\SystemFileAssociations\$ext" 'PerceivedType')
        '  MF_EXTENSION_REGISTERED='+[bool](GetValue "Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\$ext" $probeClass)
        '  MF_MIME_REGISTERED='+[bool](GetValue "Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\audio/x-archive-stream-$name" $probeClass)
        '  APPX_OPENWITH='+(HasValue "Software\Classes\$ext\OpenWithProgids" $appXProgId)
        '  LEGACY_WMP_PERMISSIONS='+(GetValue "Software\Microsoft\MediaPlayer\Player\Extensions\$ext" 'Permissions')
    }
    'DIAGNOSTIC_DLL_COM_REGISTERED='+(TestKey "Software\Classes\CLSID\$probeClass")
}
if($Action -eq 'Status') { PrintStatus; exit 0 }
if($Action -eq 'Disable') {
    if(!(Test-Path $stateFile)) { throw "No active QA journal found at $stateFile" }
    $journal=Get-Content $stateFile -Raw|ConvertFrom-Json
    if($journal.family -ne $family -or $journal.user -ne [Security.Principal.WindowsIdentity]::GetCurrent().Name) {
        throw 'QA rollback state belongs to a different app or Windows user'
    }
    if(($journal.roots -join '|') -ne ($roots.ToArray() -join '|')) {
        throw 'QA root list differs from journal; refusing rollback'
    }
    RemoveRoots $true
    foreach($ext in $extensions) {
        $prior=($journal.choices|Where-Object{$_.extension -eq $ext}|Select-Object -First 1).userChoice
        if((GetChoice $ext) -ne $prior) { throw "Unexpected default program change for $ext" }
    }
    Remove-Item -LiteralPath $stateFile -Force
    'ARCHIVE_EXTENSION_TEST_DISABLED_AND_ROLLED_BACK=1'
    PrintStatus
    exit 0
}
if(Test-Path $stateFile) { throw "QA is already active; run -Action Status or -Action Disable" }
if(!(Test-Path $dll)) { throw "Build mf_bytestream_probe.dll first: $dll" }
if(!(Test-Path $launcher)) { throw "Build modern_media_drop.exe first: $launcher" }
$package=Get-AppxPackage -Name 'Microsoft.ZuneMusic'
if(!$package -or $package.PackageFamilyName -ne $family) { throw 'Target Microsoft Media Player (2022) app not installed' }
$actualAppId=[string](GetValue "Software\Classes\$appXProgId\Application" 'AppUserModelID')
$merged=[Microsoft.Win32.Registry]::ClassesRoot.OpenSubKey("$appXProgId\Application")
if($merged) {
    try { $actualAppId=[string]$merged.GetValue('AppUserModelID') }
    finally {$merged.Dispose()}
}
if($actualAppId -ne $appId) { throw 'Modern Media Player OpenWith AppX ProgID could not be verified' }
foreach($path in $roots) {
    if(TestKey $path) { throw "Refusing to overwrite preexisting HKCU\$path" }
}
$choices=@(foreach($ext in $extensions) {
    [pscustomobject]@{extension=$ext;userChoice=(GetChoice $ext);defaultProgId=([Microsoft.Win32.Registry]::ClassesRoot.OpenSubKey($ext).GetValue(''))}
})
# Ensure the recovery journal is on disk BEFORE the first registry modification.
New-Item -ItemType Directory -Path $stateFolder -Force|Out-Null
$journal=[pscustomobject]@{
    family=$family;user=[Security.Principal.WindowsIdentity]::GetCurrent().Name
    createdOn=(Get-Date).ToString('o')
    roots=@($roots.ToArray());choices=$choices;comClass=$probeClass;methods=@($selected)
    reason='DEV only: extension recognition diagnostics; never a real decoder'
}
$journal|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $stateFile -Encoding UTF8
try {
    if(Includes 'MediaFoundation') {
        Put "Software\Classes\CLSID\$probeClass\InprocServer32" '' $dll ([Microsoft.Win32.RegistryValueKind]::String)
        Put "Software\Classes\CLSID\$probeClass\InprocServer32" 'ThreadingModel' 'Both' ([Microsoft.Win32.RegistryValueKind]::String)
    }
    $command=('"'+$launcher+'" "%1"')
    if(Includes 'OpenWith') {
        Put 'Software\Classes\Applications\modern_media_drop.exe\shell\open\command' '' $command ([Microsoft.Win32.RegistryValueKind]::String)
    }
    foreach($ext in $extensions) {
        $name=$formatNames[$ext]
        $mime="audio/x-archive-stream-$name"
        $progId="ArchiveStream.MediaPlayer2022.QA.$name"
        # 1. Shell MIME/perceived type: intentionally mark as audio for this QA only.
        if(Includes 'Shell') {
            Put "Software\Classes\$ext" 'Content Type' $mime ([Microsoft.Win32.RegistryValueKind]::String)
            Put "Software\Classes\$ext" 'PerceivedType' 'audio' ([Microsoft.Win32.RegistryValueKind]::String)
        }
        # 2. SystemFileAssociations Shell category.
        if(Includes 'SystemFileAssociations') {
            Put "Software\Classes\SystemFileAssociations\$ext" 'PerceivedType' 'audio' ([Microsoft.Win32.RegistryValueKind]::String)
            Put "Software\Classes\SystemFileAssociations\$ext" 'Content Type' $mime ([Microsoft.Win32.RegistryValueKind]::String)
        }
        # 3. MIME database mapping.
        if(Includes 'Mime') {
            Put "Software\Classes\MIME\Database\Content Type\$mime" 'Extension' $ext ([Microsoft.Win32.RegistryValueKind]::String)
        }
        # 4. Modern packaged Media Player + our companion as *alternate* Open With providers.
        # Never change (Default) or Explorer UserChoice owned by WinRAR.
        if(Includes 'OpenWith') {
            Put "Software\Classes\$ext\OpenWithProgids" $appXProgId ([byte[]]@()) ([Microsoft.Win32.RegistryValueKind]::None)
            Put "Software\Classes\$ext\OpenWithProgids" $progId ([byte[]]@()) ([Microsoft.Win32.RegistryValueKind]::None)
            Put "Software\Classes\$progId" '' "Archive Stream Media Player (2022) QA - $name" ([Microsoft.Win32.RegistryValueKind]::String)
            Put "Software\Classes\$progId\shell\open\command" '' $command ([Microsoft.Win32.RegistryValueKind]::String)
        }
        # 5. Native Media Foundation byte-stream handler by extension and MIME.
        # Diagnostic COM handler logs calls and intentionally rejects, never decodes.
        if(Includes 'MediaFoundation') {
            Put "Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\$ext" $probeClass 'AST QA diagnostic only' ([Microsoft.Win32.RegistryValueKind]::String)
            Put "Software\Microsoft\Windows Media Foundation\ByteStreamHandlers\$mime" $probeClass 'AST QA diagnostic only' ([Microsoft.Win32.RegistryValueKind]::String)
        }
        # 6. Legacy Windows Media Player SDK *comparison only* (wmplayer.exe, NOT modern app).
        if(Includes 'LegacyWMP') {
            Put "Software\Microsoft\MediaPlayer\Player\Extensions\$ext" 'Permissions' 11 ([Microsoft.Win32.RegistryValueKind]::DWord)
            Put "Software\Microsoft\MediaPlayer\Player\Extensions\$ext" 'Runtime' 7 ([Microsoft.Win32.RegistryValueKind]::DWord)
        }
    }
    NotifyAssociations
    foreach($choice in $choices) {
        if((GetChoice $choice.extension) -ne $choice.userChoice) {
            throw "Default archive association changed for $($choice.extension); aborting QA enable"
        }
    }
    'ARCHIVE_EXTENSION_TEST_ENABLED=1'
    'REGISTRY_QA_JOURNAL='+$stateFile
    'NOTE=Restart only modern Media Player before UI test; do not restart WinRAR'
    'NOTE=Diagnostic Media Foundation handler logs but cannot play archive files'
    'ROLLBACK_COMMAND=& "'+$MyInvocation.MyCommand.Path+'" -Action Disable'
    PrintStatus
} catch {
    try {
        RemoveRoots $false
        Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
        'QA_FAILED_AND_ROLLED_BACK=1'
    } catch { 'QA_ROLLBACK_ERROR='+$_.Exception.Message }
    throw
}
