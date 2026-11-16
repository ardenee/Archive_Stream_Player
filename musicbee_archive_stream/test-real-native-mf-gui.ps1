param(
    [ValidateSet('aac','m4a','wma','all')][string]$Codec = 'all',
    [ValidateSet('zip','7z','rar','all')][string]$ArchiveKind = 'all',
    [switch]$AllowSkip,
    [string]$SevenZipExe = 'C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe = 'C:\Program Files\WinRAR\Rar.exe'
)
# Explicit real GUI acceptance, NOT an audible speaker or injected-fault test.
# Must run on an unlocked interactive DEV desktop without an existing MusicBee.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$probeSource = Join-Path $root 'test_real_native_mf_gui_probe.py'
$hostSource = Join-Path $root 'test_real_native_mf_gui.py'
$interface = 'D:\Temp\ArchiveStreamNewPlayers\MusicBeeInterface.cs'
$automation = 'D:\Temp\ArchiveStreamFoobar32Runtime\automation'
$musicBee = 'C:\Program Files (x86)\MusicBee\MusicBee.exe'
foreach ($path in @($probeSource,$hostSource,$interface,$musicBee,
                   (Join-Path $root 'Release\MB_ArchiveStream.dll'),
                   (Join-Path $root 'Release\bass_archive_stream.dll'),
                   $SevenZipExe,$RarExe)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing MusicBee GUI test prerequisite: $path"
    }
}
if (!(Test-Path -LiteralPath $automation -PathType Container)) {
    throw "Missing DEV pywinauto runtime: $automation"
}
if (@(Get-CimInstance Win32_Process -Filter "Name='MusicBee.exe'").Count) {
    throw 'MusicBee is running: refusing to disturb the active user session.'
}
$folder = Join-Path $env:TEMP ('ArchiveStreamRealMusicBeeGui-' + [Guid]::NewGuid().ToString('N'))
$oldRoot = $env:AST_MUSICBEE_GUI_ARTIFACT_DIR
$oldArchive = $env:AST_NATIVE_ARCHIVE_TEST_PATH
$oldCodec = $env:AST_NATIVE_CODEC
New-Item -ItemType Directory -Force -Path $folder | Out-Null
try {
    $env:AST_MUSICBEE_GUI_ARTIFACT_DIR = $folder
    & python $probeSource
    if ($LASTEXITCODE -ne 0) { throw 'Could not compile disposable MusicBee managed probe' }
    $sources = @{aac='07.AACFile.aac';m4a='06.M4AFile.m4a';wma='05.WMAFile.wma'}
    $codecs = if ($Codec -eq 'all') {@('aac','m4a','wma')} else {@($Codec)}
    $formats = if ($ArchiveKind -eq 'all') {@('zip','7z','rar')} else {@($ArchiveKind)}
    $passed=0
    $skipped=0
    foreach ($format in $formats) {
        foreach ($name in $codecs) {
            $source = Join-Path 'C:\audiotest' $sources[$name]
            if (!(Test-Path -LiteralPath $source)) { throw "Missing test audio: $source" }
            $member = 'sample.' + $name
            $file = Join-Path $folder $member
            Copy-Item -LiteralPath $source -Destination $file -Force
            $archive = Join-Path $folder ('musicbee-native-' + $name + '.' + $format)
            if ($format -eq 'zip') {
                Add-Type -AssemblyName System.IO.Compression
                Add-Type -AssemblyName System.IO.Compression.FileSystem
                $zip = [IO.Compression.ZipFile]::Open($archive,[IO.Compression.ZipArchiveMode]::Create)
                try { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$file,$member,[IO.Compression.CompressionLevel]::NoCompression) | Out-Null }
                finally { $zip.Dispose() }
            }
            else {
                Push-Location $folder
                try {
                    if ($format -eq '7z') { & $SevenZipExe a '-t7z' '-mx=1' '-ms=off' $archive $member | Out-Null }
                    else { & $RarExe a '-s-' '-m1' $archive $member | Out-Null }
                    if ($LASTEXITCODE -ne 0) { throw "Cannot make $format test archive" }
                }
                finally { Pop-Location }
            }
            $env:AST_NATIVE_CODEC = $name.ToUpperInvariant()
            $env:AST_NATIVE_ARCHIVE_TEST_PATH = $archive
            $output = Join-Path $folder ('host-' + $name + '-' + $format + '.log')
            Write-Output "MUSICBEE_REAL_GUI_CASE_BEGIN codec=$name archive=$format"
            $command = 'python "'+$hostSource+'" > "'+$output+'" 2>&1'
            & cmd.exe /d /s /c $command
            $exit = $LASTEXITCODE
            $logs = if (Test-Path $output) { [IO.File]::ReadAllText($output) } else { '' }
            if ($exit -eq 77 -and $logs.Contains('MUSICBEE_REAL_GUI_INPUT_DESKTOP_UNAVAILABLE')) {
                Write-Output "MUSICBEE_REAL_GUI_DESKTOP_LOCKED_NOT_TESTED codec=$name archive=$format"
                ++$skipped
                if ($AllowSkip) { continue }
                throw 'Desktop unavailable; GUI acceptance was NOT verified.'
            }
            if ($exit -ne 0) {
                Write-Output ($logs -split [char]10 | Select-Object -Last 38)
                throw "MusicBee GUI test failed codec=$name archive=$format exit=$exit"
            }
            if (!$logs.Contains('MUSICBEE_NATIVE_MF_REAL_HOST_GUI_SEEK_PASS') -or
                !$logs.Contains('MUSICBEE_NATIVE_REAL_HOST_GUI_SEEK_AND_RESUME_PASS') -or
                !$logs.Contains('AST_BASS PCM_FIRST route=STREAM') -or
                $logs.Contains('PCM_FIRST route=CACHE') -or
                $logs.Contains('SESSION_READY route=BRIDGE')) {
                throw "Native playback/GUI seek evidence missing codec=$name archive=$format"
            }
            Write-Output ($logs -split [char]10 | Where-Object {
                $_ -match 'MUSICBEE_NATIVE_REAL_HOST_GUI_SEEK_AND_RESUME_PASS|MUSICBEE_NATIVE_MF_REAL_HOST_GUI_SEEK_PASS'
            })
            ++$passed
            Write-Output "MUSICBEE_REAL_GUI_NATIVE_STREAM_SEEK_PASS codec=$name archive=$format http=0"
        }
    }
    Write-Output "MUSICBEE_REAL_GUI_MATRIX_COMPLETE passed=$passed skipped=$skipped"
}
finally {
    foreach ($item in @(@('AST_MUSICBEE_GUI_ARTIFACT_DIR',$oldRoot),
                       @('AST_NATIVE_ARCHIVE_TEST_PATH',$oldArchive),
                       @('AST_NATIVE_CODEC',$oldCodec))) {
        if ($null -eq $item[1]) { Remove-Item ('Env:'+$item[0]) -ErrorAction SilentlyContinue }
        else { Set-Item ('Env:'+$item[0]) $item[1] }
    }
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}
