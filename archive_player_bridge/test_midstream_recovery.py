"""Fault-injection regression for mid-response stream recovery (test binary only).

Build the special bridge executable with ARCHIVE_STREAM_BRIDGE_FAULT_TEST.
Normal release builds have no fault-injection interface.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import urllib.request
import zipfile

AUDIO = Path(r"C:\audiotest\01.MP3File.mp3")
TEST_BINARIES = Path(os.environ.get("ARCHIVE_BRIDGE_FAULT_TEST_BIN_DIR", r"D:\Temp\ArchiveStreamBridgeFaultTest"))


def exercise(arch, archive, source, workspace, route, memory_mb):
    executable = TEST_BINARIES / arch / "archive_player_bridge.exe"
    assert executable.is_file(), f"Missing test-only executable: {executable}"
    playlist_dir = workspace / f"playlist-{arch}-{route}"
    playlist_dir.mkdir()
    process = subprocess.Popen(
        [str(executable), "--archive", str(archive), "--playlist-dir", str(playlist_dir),
         "--lifetime", "30", "--memory-mb", str(memory_mb), "--cache-limit-mb", "100",
         "--prompt-password", "0", "--test-stream-failure-after", "32768"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        startup = {}
        for _ in range(35):
            line = process.stdout.readline().strip()
            if "=" in line:
                key, value = line.split("=", 1)
                startup[key] = value
            if line == "BRIDGE_READY=1":
                break
        assert startup.get("BRIDGE_READY") == "1", (arch, startup)

        playlist = Path(startup["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig")
        url = next(line for line in playlist.splitlines() if line.startswith("http://127.0.0.1:"))
        original = source.read_bytes()
        begin, end = 1234, 201233
        request = urllib.request.Request(url, headers={"Range": f"bytes={begin}-{end}"})
        with urllib.request.urlopen(request, timeout=25) as response:
            assert response.status == 206
            assert response.headers["Content-Length"] == str(end - begin + 1)
            data = response.read()
            assert data == original[begin:end + 1], f"{arch} {route} content mismatch"

        shutdown = startup["URL_M3U8"].replace("/playlist.m3u8", "/shutdown")
        with urllib.request.urlopen(urllib.request.Request(shutdown, method="POST"), timeout=5) as reply:
            assert reply.status == 200
        process.wait(timeout=8)
        output, diagnostics = process.communicate(timeout=4)
        assert process.returncode == 0 and "ROUTE_COUNTS" in output, output
        expected = f"STREAM_RECOVERY route={route} entry=0 offset=66770 remaining=134464"
        assert expected in diagnostics, diagnostics
        counts = re.search(r"ROUTE_COUNTS stream=(\d+) memory=(\d+) cache=(\d+)", output)
        assert counts and counts.groups() == (
            ("1", "1", "0") if route == "memory" else ("1", "0", "1")
        ), (output, diagnostics)
        print(f"MIDSTREAM_RECOVERY_PASS {arch} {route}: {counts.group(0)} byte-parity=ok", flush=True)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()


if __name__ == "__main__":
    assert AUDIO.is_file(), AUDIO
    with tempfile.TemporaryDirectory(prefix="archive-bridge-midstream-test-") as folder:
        workspace = Path(folder)
        archive = workspace / "fault-test.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as handle:
            handle.write(AUDIO, AUDIO.name)
        for arch in ("Win32", "x64"):
            exercise(arch, archive, AUDIO, workspace, "memory", 50)
            exercise(arch, archive, AUDIO, workspace, "cache", 1)
    print("ALL_MIDSTREAM_RECOVERY_REGRESSIONS_PASS", flush=True)
