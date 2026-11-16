param(
    [string]$MusicBeeDll = (Join-Path $PSScriptRoot 'Release\MB_ArchiveStream.dll'),
    [string]$NativeDll = (Join-Path $PSScriptRoot 'Release\bass_archive_stream.dll'),
    [string]$BassRuntime = 'C:\Program Files (x86)\MusicBee\bass.dll',
    [string]$CSharpCompiler = 'C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe'
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'test_native_title_filenames.cs'
foreach ($item in @($source, $MusicBeeDll, $NativeDll, $BassRuntime, $CSharpCompiler)) {
    if (!(Test-Path -LiteralPath $item -PathType Leaf)) { throw "Missing native MusicBee title test prerequisite: $item" }
}
$temp = Join-Path $env:TEMP ('ArchiveStreamNativeTitles-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    Copy-Item -LiteralPath $MusicBeeDll -Destination $temp
    Copy-Item -LiteralPath $NativeDll -Destination $temp
    Copy-Item -LiteralPath $BassRuntime -Destination $temp
    $exe = Join-Path $temp 'test_native_title_filenames.exe'
    & $CSharpCompiler /nologo /target:exe /platform:x86 /optimize+ "/out:$exe" $source
    if ($LASTEXITCODE -ne 0) { throw 'Native MusicBee title regression test compilation failed' }
    & $exe (Join-Path $temp 'MB_ArchiveStream.dll') $temp
    if ($LASTEXITCODE -ne 0) { throw 'Native MusicBee title regression test failed' }
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
