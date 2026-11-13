"""Regression: temporary-cache budget and noninteractive password policy."""
import subprocess
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SOLID = Path(r"D:\Temp\ArchiveStreamNewPlayers\bridge-solid.7z")
ENCRYPTED = Path(r"C:\audiotest\Archives\7Z_Password_NonSolid.7z")

def run_case(binary, archive, args, expected_error):
    process = subprocess.Popen([str(binary), "--archive", str(archive), "--lifetime", "15"] + args,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    try:
        ready = None
        for _ in range(32):
            line = process.stdout.readline()
            if not line:
                break
            if line.startswith("URL_M3U8="):
                ready = line.split("=", 1)[1].strip()
            if line.strip() == "BRIDGE_READY=1":
                break
        assert ready and process.poll() is None
        with urllib.request.urlopen(ready, timeout=5) as response:
            content = response.read().decode("utf-8")
        entry = next(s for s in content.splitlines() if s.startswith("http://"))
        try:
            with urllib.request.urlopen(entry, timeout=15) as response:
                response.read(16)
            raise AssertionError("Expected HTTP failure, but audio was returned")
        except urllib.error.HTTPError as exc:
            assert exc.code == 500, (exc.code, archive)
        shutdown = ready.replace("/playlist.m3u8", "/shutdown")
        with urllib.request.urlopen(urllib.request.Request(shutdown, method="POST"), timeout=4) as response:
            assert response.status == 200
        process.wait(timeout=6)
        stderr = process.stderr.read()
        if expected_error:
            assert expected_error in stderr, stderr
        assert process.returncode == 0
        print("PASS", binary.parent.parent.name, archive.name, expected_error or "password suppressed", flush=True)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()

if __name__ == "__main__":
    assert SOLID.is_file(), "First create bridge-solid.7z under DEV fixtures"
    assert ENCRYPTED.is_file()
    for platform in ("Release", "x64/Release"):
        binary = ROOT / platform / "archive_player_bridge.exe"
        run_case(binary, SOLID, ["--memory-mb", "1", "--cache-limit-mb", "0", "--prompt-password", "0"], "CACHE_LIMIT")
        with tempfile.TemporaryDirectory() as tmp:
            missing = str(Path(tmp) / "empty-passwords.txt")
            run_case(binary, ENCRYPTED, ["--password-list", missing, "--prompt-password", "0"], None)
    print("ALL_CACHE_AND_NONINTERACTIVE_PASSWORD_CONTROLS_PASS", flush=True)
