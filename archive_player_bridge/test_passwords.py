r"""Encrypted C:\audiotest 7z verification without printing or committing passwords.

By default tests the existing Winamp keeps.txt; use --audiotest-generator-key
to test against the password specified by C:\audiotest\make_test_archives.cmd
in a temporary file, leaving keeps.txt unchanged.
"""
import argparse
import os
import re
import subprocess
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ARCHIVES = Path(r"C:\audiotest\Archives")
WINAMP_PASSWORDS = Path(os.environ["LOCALAPPDATA"]) / "WinampArchiveStream" / "keeps.txt"

def extract_fixture_key():
    script = Path(r"C:\audiotest\make_test_archives.cmd").read_text(encoding="utf-8-sig", errors="replace")
    match = re.search(r'(?im)^\s*set\s+"?PASSWORD\s*=\s*([^\r\n]+)', script)
    if not match:
        raise RuntimeError("Generator script is missing its PASSWORD assignment.")
    return match.group(1).strip().strip('"')

def test_case(arch, archive, password_list):
    binary = ROOT / ("x64/Release" if arch == "x64" else "Release") / "archive_player_bridge.exe"
    if not binary.is_file():
        raise RuntimeError("Build Release|{} first.".format(arch))
    child = subprocess.Popen([str(binary), "--archive", str(archive), "--password-list",
        str(password_list), "--lifetime", "75"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    try:
        playlist = None
        count = 0
        for _ in range(24):
            line = child.stdout.readline().strip()
            if line.startswith("URL_M3U8="):
                playlist = line.split("=", 1)[1]
            elif line.startswith("ARCHIVE_AUDIO_ENTRIES="):
                count = int(line.split("=", 1)[1])
            elif line == "BRIDGE_READY=1":
                break
        assert playlist and count == 21, "Encrypted archive enumeration did not match fixtures."
        with urllib.request.urlopen(playlist, timeout=10) as response:
            content = response.read().decode("utf-8")
        urls = [line for line in content.splitlines() if line.startswith("http://127.0.0.1:")]
        assert len(urls) == count
        request = urllib.request.Request(urls[0], headers={"Range": "bytes=0-255"})
        with urllib.request.urlopen(request, timeout=55) as response:
            data = response.read()
            assert response.status == 206 and len(data) == 256
        stop = playlist.replace("/playlist.m3u8", "/shutdown")
        with urllib.request.urlopen(urllib.request.Request(stop, method="POST"), timeout=5) as response:
            assert response.status == 200
        child.wait(timeout=9)
        routes = child.stdout.read()
        assert child.returncode == 0
        route = "stream" if "stream=1" in routes else "memory" if "memory=1" in routes else "cache" if "cache=1" in routes else "unknown"
        assert route != "unknown", routes
        print("PASS", arch, archive.name, "entries", count, "decoded_bytes=256", "route="+route, flush=True)
    finally:
        if child.poll() is None:
            child.kill()
        child.wait()
        error = child.stderr.read().strip()
        if error:
            print("EXTRACTION_DIAGNOSTIC", error[:240], flush=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password-list", type=Path, default=WINAMP_PASSWORDS)
    parser.add_argument("--audiotest-generator-key", action="store_true",
        help="Use a temporary password list from the original fixture generator; does not modify keeps.txt.")
    args = parser.parse_args()
    password_file = args.password_list
    temporary = None
    try:
        if args.audiotest_generator_key:
            # Do not expose the key in process command lines, logs, or Git.
            temporary = tempfile.TemporaryDirectory(prefix="archive-password-test-")
            password_file = Path(temporary.name) / "candidate.txt"
            password_file.write_text(extract_fixture_key() + "\r\n", encoding="utf-8")
            print("PASSWORD_SOURCE=audiotest_generator_temporary_only", flush=True)
        else:
            if not password_file.is_file():
                raise RuntimeError("Saved password list not found: {}".format(password_file))
            print("PASSWORD_SOURCE=WinampArchiveStream_keeps", flush=True)
        for arch in ("Win32", "x64"):
            for name in ("7Z_Password_NonSolid.7z", "7Z_Password_Solid.7z"):
                test_case(arch, ARCHIVES / name, password_file)
    finally:
        if temporary:
            temporary.cleanup()
    print("ALL_ENCRYPTED_7Z_REGRESSIONS_PASS", flush=True)

if __name__ == "__main__":
    main()
