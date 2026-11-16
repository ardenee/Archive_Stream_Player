param(
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$FixtureMp3 = 'C:\audiotest\01.MP3File.mp3',
    [string]$FixtureWav = 'C:\audiotest\04.WAVFile.wav',
    [string]$FixtureFlac = 'C:\audiotest\03.FlacFile.flac',
    [string]$FixtureOgg = 'C:\audiotest\02.OGGFile.ogg',
    [string]$FixtureAac = 'C:\audiotest\07.AACFile.aac',
    [string]$FixtureM4a = 'C:\audiotest\06.M4AFile.m4a',
    [string]$FixtureWma = 'C:\audiotest\05.WMAFile.wma',
    [string]$MusicBeeRoot = 'C:\Program Files (x86)\MusicBee',
    [string]$SevenZipExe = 'C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe = 'C:\Program Files\WinRAR\Rar.exe',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference = 'Stop'
$folder = $PSScriptRoot
$source = Join-Path $folder 'test_native_inprocess_enum.cpp'
$addon = Join-Path $folder 'Release\bass_archive_stream.dll'
foreach ($item in @($source, $addon, $FixtureMp3, $FixtureWav, $FixtureFlac, $FixtureOgg, $FixtureAac, $FixtureM4a, $FixtureWma, (Join-Path $MusicBeeRoot 'bass.dll'),
                    (Join-Path $BassCoreSdkRoot 'bass.h'), (Join-Path $BassCoreSdkRoot 'bass.lib'), $VcVars32)) {
    if (!(Test-Path -LiteralPath $item -PathType Leaf)) { throw "Missing native BASS enum prerequisite: $item" }
}
$temp = Join-Path $env:TEMP ('ArchiveStreamNativeEnumQA-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    Copy-Item -LiteralPath $addon -Destination $temp
    Copy-Item -LiteralPath (Join-Path $MusicBeeRoot 'bass.dll') -Destination $temp
    $exe = Join-Path $temp 'test_native_inprocess_enum.exe'
    $command = 'call "' + $VcVars32 + '" >nul && cl /nologo /EHsc /std:c++17 /W3 /O2 ' +
        '/I "' + $BassCoreSdkRoot + '" /Fe:"' + $exe + '" "' + $source + '" ' +
        '/link /LIBPATH:"' + $BassCoreSdkRoot + '" bass.lib'
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw 'Native in-process scanner harness compilation failed' }

    function Verify([string]$archive, [int]$count, [string]$mode = 'mp3') {
        $folder = Join-Path $temp ([IO.Path]::GetFileName($archive) + "-$mode-manifests")
        if ($mode -in @('pcm','audio','four','seven','buffered','disk','diskpcm','diskmp3')) {
            & $exe (Join-Path $temp 'bass_archive_stream.dll') $archive $folder $count $mode
        }
        else {
            & $exe (Join-Path $temp 'bass_archive_stream.dll') $archive $folder $count
        }
        if ($LASTEXITCODE -ne 0) { throw "Native in-process scanner rejected expected count $count for $archive mode=$mode" }
        Write-Output "MUSICBEE_NATIVE_ENUM_PASS archive=$([IO.Path]::GetFileName($archive)) mode=$mode expected=$count"
    }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipSingle = Join-Path $temp 'single.zip'
    $zipTwo = Join-Path $temp 'two.zip'
    $zipMixed = Join-Path $temp 'mixed.zip'
    foreach ($layout in @(@($zipSingle, 1, $false), @($zipTwo, 2, $false), @($zipMixed, 1, $true))) {
        $zip = [IO.Compression.ZipFile]::Open([string]$layout[0], [IO.Compression.ZipArchiveMode]::Create)
        try {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureMp3, 'A.mp3') | Out-Null
            if ($layout[1] -eq 2) {
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureMp3, 'B.mp3') | Out-Null
            }
            if ($layout[2]) {
                $entry = $zip.CreateEntry('Other.wav')
                $writer = $entry.Open()
                try {
                    $bytes = [Text.Encoding]::ASCII.GetBytes('RIFF')
                    $writer.Write($bytes, 0, $bytes.Length)
                }
                finally { $writer.Dispose() }
            }
        }
        finally { $zip.Dispose() }
    }
    Verify $zipSingle 1
    Verify $zipTwo 2
    Verify $zipMixed 0
    Verify $zipMixed 0 'pcm'  # A filename ending .wav is not sufficient: require a valid WAV header

    $zipWav = Join-Path $temp 'wav.zip'
    $zipMp3Wav = Join-Path $temp 'mixed-pcm.zip'
    foreach ($layout in @(@($zipWav, $false), @($zipMp3Wav, $true))) {
        $zip = [IO.Compression.ZipFile]::Open([string]$layout[0], [IO.Compression.ZipArchiveMode]::Create)
        try {
            if ([bool]$layout[1]) {
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureMp3, 'A.mp3') | Out-Null
            }
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureWav, 'B.wav') | Out-Null
        }
        finally { $zip.Dispose() }
    }
    Verify $zipWav 1 'pcm'
    Verify $zipMp3Wav 2 'pcm'
    Verify $zipMp3Wav 0 'mp3'  # v1.2 ABI stays MP3-only
    Verify $zipMp3Wav 2 'audio' # new ABI preserves PCM16 WAV

    $zipFlac = Join-Path $temp 'flac.zip'
    $zipThree = Join-Path $temp 'mixed-three-codecs.zip'
    $zipFakeFlac = Join-Path $temp 'fake-flac.zip'
    foreach ($layout in @(@($zipFlac, 1), @($zipThree, 3), @($zipFakeFlac, 0))) {
        $zip = [IO.Compression.ZipFile]::Open([string]$layout[0], [IO.Compression.ZipArchiveMode]::Create)
        try {
            if ($layout[1] -eq 3) {
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureMp3, 'A.mp3') | Out-Null
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureWav, 'B.wav') | Out-Null
            }
            if ($layout[1] -eq 0) {
                $entry = $zip.CreateEntry('invalid.flac')
                $writer = $entry.Open()
                try {
                    $bytes = [Text.Encoding]::ASCII.GetBytes('fLaCnot-a-valid-stream')
                    $writer.Write($bytes, 0, $bytes.Length)
                }
                finally { $writer.Dispose() }
            }
            else {
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureFlac, 'C.flac') | Out-Null
            }
        }
        finally { $zip.Dispose() }
    }
    Verify $zipFlac 1 'audio'
    Verify $zipFlac 0 'pcm'  # old scanner remains MP3/WAV-only
    Verify $zipThree 3 'audio'
    Verify $zipThree 0 'pcm'
    Verify $zipFakeFlac 0 'audio' # reject extension-only masquerading
    $zipOgg = Join-Path $temp 'ogg.zip'
    $zipFour = Join-Path $temp 'mixed-four-codecs.zip'
    $zipFakeOgg = Join-Path $temp 'fake-ogg.zip'
    foreach ($layout in @(@($zipOgg, 1), @($zipFour, 4), @($zipFakeOgg, 0))) {
        $zip = [IO.Compression.ZipFile]::Open([string]$layout[0], [IO.Compression.ZipArchiveMode]::Create)
        try {
            if ($layout[1] -eq 4) {
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureMp3, 'A.mp3') | Out-Null
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureWav, 'B.wav') | Out-Null
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureFlac, 'C.flac') | Out-Null
            }
            if ($layout[1] -eq 0) {
                $entry = $zip.CreateEntry('invalid.ogg')
                $writer = $entry.Open()
                try {
                    $bytes = [Text.Encoding]::ASCII.GetBytes('OggS-untrusted-not-vorbis')
                    $writer.Write($bytes, 0, $bytes.Length)
                }
                finally { $writer.Dispose() }
            }
            else {
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $FixtureOgg, 'D.ogg') | Out-Null
            }
        }
        finally { $zip.Dispose() }
    }
    Verify $zipOgg 1 'four'
    Verify $zipOgg 0 'audio' # old ABI cannot select OGG
    Verify $zipFour 4 'four'
    Verify $zipFour 0 'audio'
    Verify $zipFakeOgg 0 'four'
    Verify $zipFour 4 'seven'   # v1.6 preserves all four native codecs
    $zipSeven = Join-Path $temp 'mixed-seven-codecs.zip'
    $zip = [IO.Compression.ZipFile]::Open($zipSeven, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($item in @(@($FixtureMp3, 'A.mp3'), @($FixtureWav, 'B.wav'),
                           @($FixtureFlac, 'C.flac'), @($FixtureOgg, 'D.ogg'),
                           @($FixtureAac, 'E.aac'), @($FixtureM4a, 'F.m4a'),
                           @($FixtureWma, 'G.wma'))) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, [string]$item[0], [string]$item[1]) | Out-Null
        }
    }
    finally { $zip.Dispose() }
    Verify $zipSeven 7 'seven'
    Verify $zipSeven 7 'buffered' # direct-streamable seven-codec path remains unchanged
    Verify $zipSeven 7 'disk' # direct streaming remains first priority
    Verify $zipSeven 7 'diskpcm' # v1.10 must preserve all direct-streaming codecs
    Verify $zipSeven 7 'diskmp3' # v1.11 must preserve all seven native streaming codecs
    Verify $zipSeven 0 'four' # existing four-codec ABI remains unchanged

    foreach ($item in @(@('aac', $FixtureAac), @('m4a', $FixtureM4a), @('wma', $FixtureWma))) {
        $ext = [string]$item[0]
        $one = Join-Path $temp ("mf-$ext.zip")
        $zip = [IO.Compression.ZipFile]::Open($one, [IO.Compression.ZipArchiveMode]::Create)
        try { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, [string]$item[1], "Sample.$ext") | Out-Null }
        finally { $zip.Dispose() }
        Verify $one 1 'seven'
        Verify $one 0 'four'
        $invalid = Join-Path $temp ("invalid-$ext.zip")
        $zip = [IO.Compression.ZipFile]::Open($invalid, [IO.Compression.ZipArchiveMode]::Create)
        try {
            $entry = $zip.CreateEntry("Invalid.$ext")
            $writer = $entry.Open()
            try {
                $bytes = [Text.Encoding]::ASCII.GetBytes("Header-mismatch-invalid-$ext")
                $writer.Write($bytes, 0, $bytes.Length)
            }
            finally { $writer.Dispose() }
        }
        finally { $zip.Dispose() }
        Verify $invalid 0 'seven'
    }




    $input = Join-Path $temp 'a.mp3'
    Copy-Item -LiteralPath $FixtureMp3 -Destination $input
    Copy-Item -LiteralPath $FixtureWav -Destination (Join-Path $temp 'B.wav')
    Copy-Item -LiteralPath $FixtureFlac -Destination (Join-Path $temp 'C.flac')
    Copy-Item -LiteralPath $FixtureOgg -Destination (Join-Path $temp 'D.ogg')
    Copy-Item -LiteralPath $FixtureAac -Destination (Join-Path $temp 'E.aac')
    Copy-Item -LiteralPath $FixtureM4a -Destination (Join-Path $temp 'F.m4a')
    Copy-Item -LiteralPath $FixtureWma -Destination (Join-Path $temp 'G.wma')
    if (Test-Path -LiteralPath $SevenZipExe -PathType Leaf) {
        Push-Location $temp
        try {
            & $SevenZipExe a '-t7z' '-mx=1' '-ms=off' (Join-Path $temp 'non-solid.7z') 'a.mp3' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw '7z non-solid fixture creation failed' }
            Verify (Join-Path $temp 'non-solid.7z') 1
            & $SevenZipExe a '-t7z' '-mx=0' '-ms=off' (Join-Path $temp 'wav.7z') 'B.wav' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw '7z WAV fixture creation failed' }
            Verify (Join-Path $temp 'wav.7z') 1 'pcm'
            & $SevenZipExe a '-t7z' '-mx=0' '-ms=off' (Join-Path $temp 'flac.7z') 'C.flac' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw '7z FLAC fixture creation failed' }
            Verify (Join-Path $temp 'flac.7z') 1 'audio'
            & $SevenZipExe a '-t7z' '-mx=0' '-ms=off' (Join-Path $temp 'ogg.7z') 'D.ogg' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw '7z OGG fixture creation failed' }
            Verify (Join-Path $temp 'ogg.7z') 1 'four'
            foreach ($ext in @('aac','m4a','wma')) {
                $archive = Join-Path $temp ("mf-$ext.7z")
                & $SevenZipExe a '-t7z' '-mx=0' '-ms=off' $archive ("E.aac","F.m4a","G.wma" | Where-Object { $_ -like "*.$ext" }) | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "7z MF $ext fixture creation failed" }
                Verify $archive 1 'seven'
            }


            Copy-Item $input (Join-Path $temp 'b.mp3')
            & $SevenZipExe a '-t7z' '-mx=1' '-ms=on' (Join-Path $temp 'solid.7z') 'a.mp3' 'b.mp3' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw '7z solid fixture creation failed' }
            Verify (Join-Path $temp 'solid.7z') 0
            Verify (Join-Path $temp 'solid.7z') 0 'pcm'
            Verify (Join-Path $temp 'solid.7z') 0 'seven'
            Verify (Join-Path $temp 'solid.7z') 2 'buffered' # two verified RAM-buffered MP3 entries
            & $SevenZipExe a '-t7z' '-mx=1' '-ms=on' (Join-Path $temp 'solid-ogg.7z') 'a.mp3' 'D.ogg' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'Solid 7z MP3/Vorbis fixture creation failed' }
            Verify (Join-Path $temp 'solid-ogg.7z') 2 'buffered'
            Verify (Join-Path $temp 'solid-ogg.7z') 2 'disk'
            Verify (Join-Path $temp 'solid-ogg.7z') 0 'seven'
            & $SevenZipExe a '-tzip' '-mem=AES256' '-pAST-QA-TEST-ONLY' (Join-Path $temp 'encrypted.zip') 'a.mp3' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'encrypted ZIP fixture creation failed' }
            Verify (Join-Path $temp 'encrypted.zip') 0
            Verify (Join-Path $temp 'encrypted.zip') 0 'pcm'
            Verify (Join-Path $temp 'encrypted.zip') 0 'seven'
            Verify (Join-Path $temp 'encrypted.zip') 0 'buffered' # encrypted entries still use password-compatible bridge
            Verify (Join-Path $temp 'encrypted.zip') 0 'disk'
            Verify (Join-Path $temp 'encrypted.zip') 0 'diskpcm' # encrypted route remains on compatibility
            Verify (Join-Path $temp 'encrypted.zip') 0 'diskmp3' # no native password bypass
        }
        finally { Pop-Location }
    }
    else { Write-Output 'MUSICBEE_NATIVE_ENUM_7Z_SKIPPED external 7z.exe unavailable' }

    if (Test-Path -LiteralPath $RarExe -PathType Leaf) {
        Push-Location $temp
        try {
            & $RarExe a '-s-' '-m1' (Join-Path $temp 'non-solid.rar') 'a.mp3' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'RAR non-solid fixture creation failed' }
            Verify (Join-Path $temp 'non-solid.rar') 1
            & $RarExe a '-m0' '-s-' (Join-Path $temp 'wav.rar') 'B.wav' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'RAR WAV fixture creation failed' }
            Verify (Join-Path $temp 'wav.rar') 1 'pcm'
            & $RarExe a '-m0' '-s-' (Join-Path $temp 'flac.rar') 'C.flac' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'RAR FLAC fixture creation failed' }
            Verify (Join-Path $temp 'flac.rar') 1 'audio'
            & $RarExe a '-m0' '-s-' (Join-Path $temp 'ogg.rar') 'D.ogg' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'RAR OGG fixture creation failed' }
            Verify (Join-Path $temp 'ogg.rar') 1 'four'
            foreach ($ext in @('aac','m4a','wma')) {
                $archive = Join-Path $temp ("mf-$ext.rar")
                $inputFile = @('E.aac','F.m4a','G.wma') | Where-Object { $_ -like "*.$ext" }
                & $RarExe a '-m0' '-s-' $archive $inputFile | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "RAR MF $ext fixture creation failed" }
                Verify $archive 1 'seven'
            }


            if (!(Test-Path -LiteralPath (Join-Path $temp 'b.mp3'))) { Copy-Item $input (Join-Path $temp 'b.mp3') }
            & $RarExe a '-s' '-m1' (Join-Path $temp 'solid.rar') 'a.mp3' 'b.mp3' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'RAR solid fixture creation failed' }
            Verify (Join-Path $temp 'solid.rar') 0
            Verify (Join-Path $temp 'solid.rar') 0 'pcm'
            Verify (Join-Path $temp 'solid.rar') 0 'seven'
            Verify (Join-Path $temp 'solid.rar') 2 'buffered' # first entry may stream, second uses RAM
            & $RarExe a '-s' '-m1' (Join-Path $temp 'solid-ogg.rar') 'a.mp3' 'D.ogg' | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'Solid RAR MP3/Vorbis fixture creation failed' }
            Verify (Join-Path $temp 'solid-ogg.rar') 2 'buffered'
            Verify (Join-Path $temp 'solid-ogg.rar') 2 'disk'
            Verify (Join-Path $temp 'solid-ogg.rar') 0 'seven'
        }
        finally { Pop-Location }
    }
    else { Write-Output 'MUSICBEE_NATIVE_ENUM_RAR_SKIPPED external Rar.exe unavailable' }
    Write-Output 'MUSICBEE_NATIVE_INPROCESS_ENUM_SEVEN_CODEC_SOLID_AND_DISK_MP3_REGRESSIONS_PASS'
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}