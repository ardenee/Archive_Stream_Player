"""Reproducible encrypted 7z layout regressions, without runtime 7z.exe.

7z.exe is used only to generate synthetic archives. Runtime decode, file
extraction, memory parity and seeking use statically linked Archive Stream.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import urllib.request
import wave
import struct

ROOT = Path(__file__).resolve().parents[1]
SEVENZIP = Path(r"C:\Program Files\7-Zip\7z.exe")
PASSWORD = "ArchiveStream7zCoderLayoutTest2026"


def run(args, timeout=80):
    result = subprocess.run([str(x) for x in args], capture_output=True, text=True,
                            errors="replace", timeout=timeout)
    return result


def check(result, label, pattern):
    if result.returncode or pattern not in result.stdout:
        raise AssertionError(f"{label}: exit={result.returncode} stdout={result.stdout[-1200:]} stderr={result.stderr[-500:]}")


def check_archive(name, archive, original, password_file, solid=False):
    expected = original.read_bytes()
    for architecture, path in (("Win32", "Release"), ("x64", "x64/Release")):
        binary = ROOT / "archive_backend_test" / path / "archive_backend_test.exe"
        assert binary.is_file(), binary
        output = archive.parent / f"extract-{name}-{architecture}{original.suffix}"
        result = run([binary, "pwfiletest", archive, password_file, "0", output])
        check(result, f"{name} {architecture} file", "PASSWORD_TEST_PASS:")
        assert output.read_bytes() == expected, f"{name} {architecture}: file bytes differ"

        if solid:
            # Solid decoding must use the full-folder memory/cache route,
            # not falsely advertise independently streamable tracks.
            result = run([binary, "pwstreamtest", archive, password_file, "0"])
            assert result.returncode != 0 and "PASSWORD_STREAM_OPEN_FAIL" in result.stdout, (
                name, architecture, result.stdout
            )
            print(f"PASS_7Z_SOLID_FALLBACK {architecture} {name} exact-file-parity", flush=True)
        else:
            result = run([binary, "pwstreamtest", archive, password_file, "0"])
            check(result, f"{name} {architecture} stream", "PASSWORD_STREAM_PASS:")
            assert "seek=PASS memory_parity=PASS encrypted=1" in result.stdout, result.stdout
            print(f"PASS_7Z_ENCRYPTED {architecture} {name} file-memory-stream-seek-parity", flush=True)


def check_bridge(name, archive, original, password_file, solid=False):
    source = original.read_bytes()
    for architecture, path in (("Win32", "Release"), ("x64", "x64/Release")):
        bridge = ROOT / "archive_player_bridge" / path / "archive_player_bridge.exe"
        assert bridge.is_file(), bridge
        work = archive.parent / f"bridge-{name}-{architecture}"
        work.mkdir(exist_ok=True)

        process = subprocess.Popen([
            str(bridge), "--archive", str(archive),
            "--password-list", str(password_file), "--prompt-password", "0",
            "--memory-mb", "1", "--cache-limit-mb", "0",
            "--playlist-dir", str(work), "--lifetime", "30"
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        try:
            details = {}
            for _ in range(32):
                line = process.stdout.readline().strip()
                if "=" in line:
                    key, value = line.split("=", 1)
                    details[key] = value
                if line == "BRIDGE_READY=1":
                    break
            assert details.get("BRIDGE_READY") == "1", (name, architecture, details)

            playlist = Path(details["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig")
            url = next(line for line in playlist.splitlines() if line.startswith("http://127.0.0.1:"))
            begin, end = 31, min(131072, len(source) - 1)
            request = urllib.request.Request(url, headers={"Range": f"bytes={begin}-{end}"})
            with urllib.request.urlopen(request, timeout=25) as response:
                assert response.status == 206
                assert response.headers["Content-Length"] == str(end - begin + 1)
                assert response.read() == source[begin:end + 1], (name, architecture, "byte mismatch")

            shutdown = details["URL_M3U8"].replace("/playlist.m3u8", "/shutdown")
            with urllib.request.urlopen(urllib.request.Request(shutdown, method="POST"), timeout=7) as response:
                assert response.status == 200
            process.wait(timeout=10)
            output = process.stdout.read()
            expected = "ROUTE_COUNTS stream=0 memory=1 cache=0" if solid else "ROUTE_COUNTS stream=1 memory=0 cache=0"
            assert process.returncode == 0 and expected in output, (
                name, architecture, output, process.stderr.read()[-600:]
            )
            print(f"PASS_7Z_ENCRYPTED_BRIDGE {architecture} {name} {expected} HTTP-range-parity", flush=True)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()


def main():
    assert SEVENZIP.is_file(), SEVENZIP
    with tempfile.TemporaryDirectory(prefix="archive-7z-encrypted-layouts-") as temp:
        folder = Path(temp)
        wav = folder / "Synthetic.wav"
        with wave.open(str(wav), "wb") as audio:
            audio.setparams((1, 2, 44100, 0, "NONE", "not compressed"))
            audio.writeframes(struct.pack("<h", 5734) * 200000)

        second_wav = folder / "Synthetic2.wav"
        with wave.open(str(second_wav), "wb") as audio:
            audio.setparams((1, 2, 44100, 0, "NONE", "not compressed"))
            audio.writeframes(struct.pack("<h", 9531) * 65000)

        # MP3 suffix avoids the WAV-specific Delta filter while still
        # exercising AES -> Copy. The bytes need not be decoded as audio.
        mp3 = folder / "Synthetic.mp3"
        mp3.write_bytes(bytes(range(256)) * 1600)
        password_file = folder / "passwords.txt"
        password_file.write_text("incorrect-candidate\n" + PASSWORD + "\n", encoding="utf-8")

        cases = (
            ("copy-delta", [wav], ["-m0=Copy", "-mhe=off", "-ms=off"], False),
            ("copy-no-delta", [mp3], ["-m0=Copy", "-mhe=off", "-ms=off"], False),
            ("lzma2", [wav], ["-m0=LZMA2", "-mhe=off", "-ms=off"], False),
            ("headers-copy-delta", [wav], ["-m0=Copy", "-mhe=on", "-ms=off"], False),
            ("headers-copy-no-delta", [mp3], ["-m0=Copy", "-mhe=on", "-ms=off"], False),
            ("headers-lzma2", [wav], ["-m0=LZMA2", "-mhe=on", "-ms=off"], False),
            ("headers-copy-split", [wav], ["-m0=Copy", "-mhe=on", "-ms=off", "-v64k"], False),
            ("headers-lzma2-solid", [wav, second_wav], ["-m0=LZMA2", "-mhe=on", "-ms=on"], True),
        )

        for name, sources, options, solid in cases:
            archive = folder / (name + ".7z")
            create = run([SEVENZIP, "a", "-t7z", "-y", "-p" + PASSWORD, *options,
                          archive, *sources], timeout=110)
            check(create, f"create {name}", "Everything is Ok")
            selected = archive.with_suffix(".7z.001") if "-v64k" in options else archive
            assert selected.is_file(), f"missing archive: {selected}"
            check_archive(name, selected, sources[0], password_file, solid=solid)
            check_bridge(name, selected, sources[0], password_file, solid=solid)
            if solid:
                # Also certify extracting the second member from the same
                # encrypted solid folder without claiming per-entry streaming.
                for arch, subdir in (("Win32", "Release"), ("x64", "x64/Release")):
                    binary = ROOT / "archive_backend_test" / subdir / "archive_backend_test.exe"
                    output = folder / f"solid-second-{arch}.wav"
                    result = run([binary, "pwfiletest", selected, password_file, "1", output])
                    check(result, f"solid second member {arch}", "PASSWORD_TEST_PASS:")
                    assert output.read_bytes() == second_wav.read_bytes()
                    print(f"PASS_7Z_SOLID_SECOND_MEMBER {arch} exact-file-parity", flush=True)

        # A large AES+Copy entry requires a full CRC check before streaming.
        # Interrupt that preflight from another thread and confirm the stream
        # reports Cancelled without exposing a partially opened decoder.
        large = folder / "Synthetic-Large.mp3"
        block = bytes(range(256)) * 256
        with large.open("wb") as destination:
            for _ in range(512):
                destination.write(block)
        cancel_archive = folder / "cancel-aes-copy.7z"
        create = run([SEVENZIP, "a", "-t7z", "-y", "-p" + PASSWORD,
                      "-m0=Copy", "-mhe=off", "-ms=off", cancel_archive, large], timeout=110)
        check(create, "create cancellation fixture", "Everything is Ok")
        for arch, subdir in (("Win32", "Release"), ("x64", "x64/Release")):
            binary = ROOT / "archive_backend_test" / subdir / "archive_backend_test.exe"
            result = run([binary, "pwcancelopentest", cancel_archive, password_file], timeout=30)
            check(result, f"cancel AES+Copy preflight {arch}", "PASSWORD_STREAM_CANCEL_OPEN_PASS:")
            print(f"PASS_7Z_AES_COPY_PRECHECK_CANCELLATION {arch}", flush=True)

        # Header ciphertext must not be accepted with a wrong password.
        incorrect = folder / "wrong-password.txt"
        incorrect.write_text("definitely-not-the-password\n", encoding="utf-8")
        for case in ("copy-delta", "headers-copy-delta"):
            test_archive = folder / (case + ".7z")
            for arch, path in (("Win32", "Release"), ("x64", "x64/Release")):
                binary = ROOT / "archive_backend_test" / path / "archive_backend_test.exe"
                result = run([binary, "pwfiletest", test_archive, incorrect, "0",
                              folder / f"wrong-{case}-{arch}.wav"], timeout=20)
                expected = "PASSWORD_TEST_LIST_FAIL" if case.startswith("headers") else "PASSWORD_TEST_EXTRACT_FAIL"
                assert result.returncode != 0 and expected in result.stdout, (
                    case, arch, result.stdout, result.stderr
                )
                stream = run([binary, "pwstreamtest", test_archive, incorrect, "0"], timeout=20)
                expected = "PASSWORD_STREAM_LIST_FAIL" if case.startswith("headers") else "PASSWORD_STREAM_OPEN_FAIL"
                assert stream.returncode != 0 and expected in stream.stdout, (
                    case, arch, stream.stdout, stream.stderr
                )
                print(f"PASS_7Z_WRONG_PASSWORD_REJECTED {arch} {case} file-and-stream", flush=True)
    print("ALL_7Z_ENCRYPTED_LAYOUTS_PASS", flush=True)


if __name__ == "__main__":
    main()
