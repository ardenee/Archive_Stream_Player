param(
    [Parameter(Mandatory=$true)][string]$BassAddonSdkRoot,
    [Parameter(Mandatory=$true)][string]$BassCoreSdkRoot,
    [string]$VcVars32 = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$addon = Join-Path $BassAddonSdkRoot 'bass-addon.h'
$core = Join-Path $BassCoreSdkRoot 'bass.h'
$import = Join-Path $BassCoreSdkRoot 'bass.lib'
foreach ($p in @($addon, $core, $import, $VcVars32)) {
    if (!(Test-Path -LiteralPath $p -PathType Leaf)) { throw "Missing external tool/SDK file: $p" }
}
$out = Join-Path $PSScriptRoot 'Release\bass_archive_stream.dll'
New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
Push-Location $repo
try {
    $command = 'call "' + $VcVars32 + '" >nul && cl /nologo /LD /MD /EHsc /std:c++20 /W3 /O2 /DNDEBUG ' +
        '/I "' + $BassAddonSdkRoot + '" /I "' + $BassCoreSdkRoot + '" ' +
        '/I "third_party\dr_libs" /I "archive_backend" /I "shared" /I "in_archive_stream" ' +
        'musicbee_archive_stream\bass_archive_stream.cpp shared\debug_log.cpp ' +
        '/link /NOLOGO /LTCG /OUT:"' + $out + '" /DEF:musicbee_archive_stream\bass_archive_stream.def ' +
        '/LIBPATH:"' + $BassCoreSdkRoot + '" /LIBPATH:"archive_backend\Release" ' +
        '/LIBPATH:"unrar_static\Release" /LIBPATH:"libarchive_static\Release" /LIBPATH:"zlib_static\Release" ' +
        'bass.lib archive_backend.lib unrar_static.lib libarchive_static.lib zlib_static.lib ' +
        'bcrypt.lib Advapi32.lib Shell32.lib User32.lib'
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $out -PathType Leaf)) {
        throw 'Native BASS archive add-on build failed (x86). Build the static backend Release libraries first.'
    }
    Write-Output "MUSICBEE_NATIVE_BASS_ADDON_BUILT=$out"
}
finally {
    Pop-Location
}