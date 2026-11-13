# Requires: built Release archive_backend_test Win32/x64 and 7-Zip CLI for
# *fixture creation only*. Runtime extraction uses the statically linked 7-Zip AES/SHA-256 source; no extra DLL.
param(
    [string]$SevenZipExe = "C:\Program Files\7-Zip\7z.exe",
    [switch]$KeepArtifacts
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$x86Exe = Join-Path $root "archive_backend_test\Release\archive_backend_test.exe"
$x64Exe = Join-Path $root "archive_backend_test\x64\Release\archive_backend_test.exe"

foreach ($required in @($SevenZipExe, $x86Exe, $x64Exe))
{
    if (-not (Test-Path -LiteralPath $required))
    {
        throw "Required test tool/runtime missing: $required"
    }
}

$work = Join-Path $env:TEMP ("ArchiveStream-7z-AES-test-" + [Guid]::NewGuid().ToString("N"))
$fixtureDir = Join-Path $work "input"
New-Item -ItemType Directory -Path $fixtureDir -Force | Out-Null
$password = "7z-fixture-2026"
$passList = Join-Path $work "passwords.txt"
$wrongList = Join-Path $work "wrong.txt"
[IO.File]::WriteAllText($passList, "incorrect-password" + [Environment]::NewLine + $password + [Environment]::NewLine)
[IO.File]::WriteAllText($wrongList, "incorrect-password" + [Environment]::NewLine)
$unicodeName = [string][char]0x65E5 + [char]0x672C + [char]0x8A9E + ".wav"

function New-TestWav([string]$name, [int]$seed)
{
    $pcm = [byte[]]::new(4096 * 4)
    ([Random]::new($seed)).NextBytes($pcm)
    $wav = [byte[]]::new(44 + $pcm.Length)
    [Text.Encoding]::ASCII.GetBytes("RIFF").CopyTo($wav, 0)
    [BitConverter]::GetBytes([int]($wav.Length - 8)).CopyTo($wav, 4)
    [Text.Encoding]::ASCII.GetBytes("WAVEfmt ").CopyTo($wav, 8)
    [BitConverter]::GetBytes([int]16).CopyTo($wav, 16)
    [BitConverter]::GetBytes([int16]1).CopyTo($wav, 20)
    [BitConverter]::GetBytes([int16]2).CopyTo($wav, 22)
    [BitConverter]::GetBytes([int]44100).CopyTo($wav, 24)
    [BitConverter]::GetBytes([int]176400).CopyTo($wav, 28)
    [BitConverter]::GetBytes([int16]4).CopyTo($wav, 32)
    [BitConverter]::GetBytes([int16]16).CopyTo($wav, 34)
    [Text.Encoding]::ASCII.GetBytes("data").CopyTo($wav, 36)
    [BitConverter]::GetBytes([int]$pcm.Length).CopyTo($wav, 40)
    $pcm.CopyTo($wav, 44)
    [IO.File]::WriteAllBytes((Join-Path $fixtureDir $name), $wav)
}

function Invoke-Test([string]$exe, [string]$archive, [string]$passwordFile, [string]$out0, [string]$out1)
{
    & $exe pwfiletest $archive $passwordFile "0" $out0 "1" $out1
    if ($LASTEXITCODE -ne 0)
    {
        throw "Passworded extraction failed: $archive using $exe"
    }
    $expected = @(Get-ChildItem -LiteralPath $fixtureDir -File | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName).Hash } | Sort-Object)
    # Sort the entire array independently, not just its last item.
    $actual = @(@((Get-FileHash -LiteralPath $out0).Hash, (Get-FileHash -LiteralPath $out1).Hash) | Sort-Object)
    if ((Compare-Object -ReferenceObject $expected -DifferenceObject $actual).Count -ne 0)
    {
        throw "Extracted 7z bytes did not match original fixture bytes: $archive"
    }
}

function Assert-Rejected([string]$exe, [string]$archive, [string]$passwordFile, [string]$output)
{
    Remove-Item -LiteralPath $output -Force -ErrorAction SilentlyContinue
    & $exe pwfiletest $archive $passwordFile "0" $output | Out-Null
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $output))
    {
        throw "Invalid archive/password unexpectedly extracted or left partial output: $archive"
    }
}

try
{
    New-TestWav "Track.wav" 17
    New-TestWav $unicodeName 29

    Push-Location $fixtureDir
    try
    {
        $plain = Join-Path $work "plain-nonsolid.7z"
        $visible = Join-Path $work "aes-visible.7z"
        $hidden = Join-Path $work "aes-hidden-solid.7z"
        $split = Join-Path $work "aes-multipart.7z"
        $splitNonSolid = Join-Path $work "aes-multipart-nonsolid.7z"
        foreach ($archiveArguments in @(
            @("a", "-t7z", "-mx=1", "-ms=off", $plain, "*.wav"),
            @("a", "-t7z", "-mx=1", "-ms=off", "-mhe=off", "-p$password", $visible, "*.wav"),
            @("a", "-t7z", "-mx=1", "-ms=on", "-mhe=on", "-p$password", $hidden, "*.wav"),
            @("a", "-t7z", "-mx=1", "-ms=on", "-mhe=on", "-v8k", "-p$password", $split, "*.wav"),
            @("a", "-t7z", "-mx=1", "-ms=off", "-mhe=on", "-v8k", "-p$password", $splitNonSolid, "*.wav")
        ))
        {
            & $SevenZipExe @archiveArguments | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "7-Zip failed to create an AES fixture" }
        }
    }
    finally
    {
        Pop-Location
    }
    $splitFirst = $split + ".001"
    $third = $split + ".003"
    if (-not (Test-Path -LiteralPath $third)) { throw "Fixture did not create at least three 7z volumes" }

    foreach ($arch in @(@("x86", $x86Exe), @("x64", $x64Exe)))
    {
        $label = $arch[0]
        $exe = $arch[1]
        foreach ($fixture in @(@("plain", $plain), @("visible", $visible), @("hidden", $hidden), @("multipart", $splitFirst), @("multipart-nonsolid", ($splitNonSolid + ".001"))))
        {
            $prefix = Join-Path $work ($label + "-" + $fixture[0])
            Invoke-Test $exe $fixture[1] $passList ($prefix + "-0.wav") ($prefix + "-1.wav")
            Write-Host "PASS $label 7z $($fixture[0]): 2 entries and source SHA-256 hashes match"
            if ($fixture[0] -eq "plain" -or $fixture[0] -eq "visible" -or $fixture[0] -eq "multipart-nonsolid")
            {
                foreach ($index in @(0, 1))
                {
                    & $exe pwstreamtest $fixture[1] $passList "$index"
                    if ($LASTEXITCODE -ne 0) { throw "AES stream decode/seek/parity failed: $label $($fixture[0]) entry $index" }
                }
                Write-Host "PASS $label non-solid 7z $($fixture[0]): 2 direct streams with byte parity and seeking"
            }
        }
        & $exe pwstreamtest $visible $wrongList "0" | Out-Null
        if ($LASTEXITCODE -eq 0) { throw "Incorrect AES stream password unexpectedly accepted: $label" }
        & $exe pwstreamtest $hidden $passList "0" | Out-Null
        if ($LASTEXITCODE -eq 0) { throw "Solid AES 7z unexpectedly opened as a direct stream: $label" }
        Write-Host "PASS $label incorrect AES password stream rejected and solid 7z stream refused"
        Assert-Rejected $exe $hidden $wrongList (Join-Path $work "$label-wrong.wav")
        Write-Host "PASS $label incorrect password rejected without partial output"
    }

    Move-Item -LiteralPath $third -Destination ($third + ".missing")
    try
    {
        foreach ($exe in @($x86Exe, $x64Exe))
        {
            Assert-Rejected $exe $splitFirst $passList (Join-Path $work "missing-volume.wav")
        }
        Write-Host "PASS x86 and x64 missing third volume rejected without partial output"
    }
    finally
    {
        Move-Item -LiteralPath ($third + ".missing") -Destination $third
    }

    Write-Host "PASS encrypted 7z regression matrix completed"
}
finally
{
    if ($KeepArtifacts)
    {
        Write-Host "Test artifacts retained: $work"
    }
    else
    {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
