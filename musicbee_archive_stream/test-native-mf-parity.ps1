param(
    [string]$FixtureAac = 'C:\audiotest\07.AACFile.aac',
    [string]$FixtureM4a = 'C:\audiotest\06.M4AFile.m4a',
    [string]$FixtureWma = 'C:\audiotest\05.WMAFile.wma',
    [string]$SevenZipExe = 'C:\Program Files\7-Zip\7z.exe',
    [string]$RarExe = 'C:\Program Files\WinRAR\Rar.exe',
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$source = Join-Path $PSScriptRoot 'test_native_mf_parity.cpp'
foreach ($path in @($source, $FixtureAac, $FixtureM4a, $FixtureWma, $VcVars32,
                    (Join-Path $repo 'archive_backend\Release\archive_backend.lib'),
                    (Join-Path $repo 'unrar_static\Release\unrar_static.lib'),
                    (Join-Path $repo 'libarchive_static\Release\libarchive_static.lib'),
                    (Join-Path $repo 'zlib_static\Release\zlib_static.lib'))) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing MF native PCM parity test prerequisite: $path" }
}
$qa = Join-Path $env:TEMP ('ArchiveStreamMFPcmParity-' + [Guid]::NewGuid().ToString('N'))
New-Item -Path $qa -ItemType Directory -Force | Out-Null
try {
    $exe = Join-Path $qa 'test_native_mf_parity.exe'
    $cmd = 'call "' + $VcVars32 + '" >nul && cl /nologo /MD /EHsc /std:c++20 /W3 /O2 ' +
        '/I "archive_backend" /I "shared" /I "third_party\dr_libs" ' +
        '/Fe:"' + $exe + '" "' + $source + '" shared\debug_log.cpp ' +
        '/link /LTCG /LIBPATH:"archive_backend\Release" /LIBPATH:"unrar_static\Release" ' +
        '/LIBPATH:"libarchive_static\Release" /LIBPATH:"zlib_static\Release" ' +
        'archive_backend.lib unrar_static.lib libarchive_static.lib zlib_static.lib ' +
        'mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib propsys.lib bcrypt.lib Advapi32.lib Shell32.lib User32.lib'
    Push-Location $repo
    try {
        & cmd.exe /d /s /c $cmd
        if ($LASTEXITCODE -ne 0) { throw 'MF archive PCM parity test harness failed to compile/link' }
    }
    finally { Pop-Location }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($item in @(@('aac', $FixtureAac), @('m4a', $FixtureM4a), @('wma', $FixtureWma))) {
        $ext = [string]$item[0]
        $file = [string]$item[1]
        $zipPath = Join-Path $qa ("mf-$ext.zip")
        $zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
        try { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file, "sample.$ext") | Out-Null }
        finally { $zip.Dispose() }
        & $exe $file $zipPath $ext
        if ($LASTEXITCODE -ne 0) { throw "Native archive $ext ZIP PCM parity failed" }
        Write-Output "NATIVE_MF_FULL_PCM_ZIP_PARITY_PASS codec=$ext"

        $local = Join-Path $qa ("sample.$ext")
        Copy-Item -LiteralPath $file -Destination $local -Force
        if (Test-Path -LiteralPath $SevenZipExe -PathType Leaf) {
            $seven = Join-Path $qa ("mf-$ext.7z")
            Push-Location $qa
            try {
                & $SevenZipExe a '-t7z' '-mx=0' '-ms=off' $seven ("sample.$ext") | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "Native MF 7z archive creation failed $ext" }
            }
            finally { Pop-Location }
            & $exe $file $seven $ext
            if ($LASTEXITCODE -ne 0) { throw "Native archive $ext 7z PCM parity failed" }
            Write-Output "NATIVE_MF_FULL_PCM_7Z_PARITY_PASS codec=$ext"
        }
        if (Test-Path -LiteralPath $RarExe -PathType Leaf) {
            $rar = Join-Path $qa ("mf-$ext.rar")
            Push-Location $qa
            try {
                & $RarExe a '-m0' '-s-' $rar ("sample.$ext") | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "Native MF RAR archive creation failed $ext" }
            }
            finally { Pop-Location }
            & $exe $file $rar $ext
            if ($LASTEXITCODE -ne 0) { throw "Native archive $ext RAR PCM parity failed" }
            Write-Output "NATIVE_MF_FULL_PCM_RAR_PARITY_PASS codec=$ext"
        }
    }
    Write-Output 'MUSICBEE_NATIVE_MEDIA_FOUNDATION_FULL_TRACK_PCM_ALL_ARCHIVES_PASS'
}
finally {
    Remove-Item -LiteralPath $qa -Recurse -Force -ErrorAction SilentlyContinue
}
