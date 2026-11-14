"""Regression: encrypted RAR must decode directly into memory, not via a temp file.
Requires installed WinRAR's Rar.exe for test fixture generation only.
Run after building archive_backend_test in Release x64 and Win32.
"""
from pathlib import Path
import math
import shutil
import struct
import subprocess
import tempfile
import urllib.request
import wave

ROOT = Path(__file__).resolve().parents[1]
RAR = shutil.which("Rar.exe") or r"C:\Program Files\WinRAR\Rar.exe"
PASSWORD = "ArchiveStreamMemoryRegression2026"
EXECUTABLES = [
    ROOT / "archive_backend_test" / "Release" / "archive_backend_test.exe",
    ROOT / "archive_backend_test" / "x64" / "Release" / "archive_backend_test.exe",
]


def run(args, timeout=90):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True, errors="replace", timeout=timeout)
    if result.returncode:
        raise AssertionError(f"exit={result.returncode}: {' '.join(map(str, args[:3]))}\n{result.stdout[-3000:]}\n{result.stderr[-1000:]}")
    return result.stdout


def bridge_check(platform, archive, original, passwords, folder, memory_mb=1, expected_route="memory", entry_index=0):
    binary = ROOT / "archive_player_bridge" / ("x64/Release" if platform == "x64" else "Release") / "archive_player_bridge.exe"
    assert binary.is_file(), f"Build bridge first: {binary}"
    playlist_dir = folder / ("bridge-" + platform + "-" + archive.stem + "-memory-" + str(memory_mb) + "-entry-" + str(entry_index))
    playlist_dir.mkdir()
    process = subprocess.Popen([str(binary), "--archive", str(archive), "--password-list", str(passwords),
                                "--playlist-dir", str(playlist_dir), "--memory-mb", str(memory_mb),
                                "--cache-limit-mb", "64" if expected_route == "cache" else "0",
                                "--prompt-password", "0", "--lifetime", "15"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    try:
        details = {}
        for _ in range(32):
            line = process.stdout.readline().strip()
            if "=" in line:
                key, value = line.split("=", 1)
                details[key] = value
            if line == "BRIDGE_READY=1":
                break
        assert details.get("BRIDGE_READY") == "1", details
        playlist = Path(details["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig")
        tracks = [line for line in playlist.splitlines() if line.startswith("http://127.0.0.1:")]
        assert len(tracks) > entry_index, (entry_index, len(tracks))
        track = tracks[entry_index]
        with urllib.request.urlopen(urllib.request.Request(track, headers={"Range": "bytes=37-4096"}), timeout=20) as response:
            assert response.status == 206
            assert response.read() == original.read_bytes()[37:4097]
        shutdown = details["URL_M3U8"].replace("/playlist.m3u8", "/shutdown")
        with urllib.request.urlopen(urllib.request.Request(shutdown, method="POST"), timeout=5) as response:
            assert response.status == 200
        process.wait(timeout=10)
        log = process.stdout.read()
        assert process.returncode == 0 and "ROUTE_COUNTS" in log, log
        if expected_route == "cache":
            assert "cache=1" in log and "stream=0" in log and "memory=0" in log, log
        else:
            assert "cache=0" in log and ("memory=1" in log or "stream=1" in log), log
        print(f"PASS {archive.stem} bridge {platform}: " + log.strip(), flush=True)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()


def main():
    assert Path(RAR).is_file(), "Rar.exe is required only to create test fixtures"
    for exe in EXECUTABLES:
        assert exe.is_file(), f"Build backend console first: {exe}"

    with tempfile.TemporaryDirectory(prefix="archive-stream-rar-memory-") as folder:
        directory = Path(folder)
        original = directory / "synthetic.wav"
        with wave.open(str(original), "wb") as writer:
            writer.setnchannels(1)
            writer.setsampwidth(2)
            writer.setframerate(22050)
            samples = [round(7000 * math.sin(2 * math.pi * 440 * i / 22050)) for i in range(22050)]
            writer.writeframes(struct.pack("<" + "h" * len(samples), *samples))

        passwords = directory / "regression-passwords.txt"
        passwords.write_text("wrong-candidate\n" + PASSWORD + "\n", encoding="utf-8")
        for mode, args in (
            ("rar5-data", ["-ma5", "-p" + PASSWORD]),
            ("rar5-headers", ["-ma5", "-hp" + PASSWORD]),
            ("rar4-data", ["-ma4", "-p" + PASSWORD]),
        ):
            archive = directory / (mode + ".rar")
            try:
                run([RAR, "a", "-idq", "-y", "-ep", "-m1", *args, archive, original])
            except AssertionError:
                if mode != "rar4-data":
                    raise
                print("SKIP rar4-data: installed WinRAR does not support creating RAR4 archives", flush=True)
                continue
            for exe in EXECUTABLES:
                output = directory / (mode + "-" + ("x64" if "x64" in str(exe) else "x86") + ".wav")
                result = run([exe, "pwfiletest", archive, passwords, "0", output])
                assert output.read_bytes() == original.read_bytes(), "Native file fallback mismatch"
                assert "PASSWORD_MEMORY_PASS:" in result and "PASSWORD_TEST_PASS:" in result, result
                print(f"PASS {mode} {'x64' if 'x64' in str(exe) else 'x86'}: native RAM parity and hard memory bound", flush=True)
            for platform in ("Win32", "x64"):
                bridge_check(platform, archive, original, passwords, directory)
    print("ENCRYPTED_RAR_MEMORY_REGRESSION_PASS", flush=True)


if __name__ == "__main__":
    main()
