"""Verify stream-first byte serving for ZIP, nonsolid 7z and RAR on both architectures.
Uses production static backends; external 7z is used only to create a test fixture.
"""
import re
import subprocess
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
AUDIO = Path(r"C:\audiotest")
SOURCE = Path(r"D:\Temp\ArchiveStreamFoobar64Runtime\audio-test")
SEVENZIP = Path(r"C:\Program Files\7-Zip\7z.exe")


def run_case(binary, archive):
    process = subprocess.Popen(
        [str(binary), "--archive", str(archive), "--lifetime", "30",
         "--memory-mb", "1", "--cache-limit-mb", "0", "--prompt-password", "0"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        startup = {}
        for _ in range(32):
            line = process.stdout.readline()
            if not line:
                break
            line = line.strip()
            if "=" in line:
                key, value = line.split("=", 1)
                startup[key] = value
            if line == "BRIDGE_READY=1":
                break
        assert startup.get("BRIDGE_READY") == "1", (archive, startup, process.poll())
        with urllib.request.urlopen(startup["URL_M3U8"], timeout=6) as reply:
            playlist = reply.read().decode("utf-8-sig")
        entry = next(line for line in playlist.splitlines() if line.startswith("http://127.0.0.1:"))
        with urllib.request.urlopen(urllib.request.Request(entry, headers={"Range": "bytes=64-1023"}), timeout=20) as reply:
            assert reply.status == 206
            assert len(reply.read()) == 960
        shutdown = startup["URL_M3U8"].replace("/playlist.m3u8", "/shutdown")
        with urllib.request.urlopen(urllib.request.Request(shutdown, method="POST"), timeout=6) as reply:
            assert reply.status == 200
        process.wait(timeout=10)
        output = process.stdout.read()
        assert process.returncode == 0, (archive, process.returncode, output)
        match = re.search(r"ROUTE_COUNTS stream=(\d+) memory=(\d+) cache=(\d+)", output)
        assert match and tuple(map(int, match.groups())) == (1, 0, 0), (archive, output, process.stderr.read())
        print("PASS_STREAM_FIRST", binary.parent, archive.name, match.group(0), flush=True)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()


if __name__ == "__main__":
    mp3 = AUDIO / "01.MP3File.mp3"
    ogg = AUDIO / "02.OGGFile.ogg"
    zip_archive = SOURCE / "generated-audio.zip"
    rar_archive = SOURCE / "generated-audio.rar"
    assert all(p.is_file() for p in (mp3, ogg, zip_archive, rar_archive, SEVENZIP))
    with tempfile.TemporaryDirectory(prefix="archive-stream-priority-") as folder:
        seven_archive = Path(folder) / "nonsolid.7z"
        subprocess.run([str(SEVENZIP), "a", "-t7z", "-mx=1", "-ms=off", str(seven_archive), str(mp3), str(ogg), "-y"],
                       check=True, capture_output=True)
        for platform in ("Release", "x64/Release"):
            exe = ROOT / platform / "archive_player_bridge.exe"
            assert exe.is_file(), exe
            for archive in (zip_archive, seven_archive, rar_archive):
                run_case(exe, archive)
    print("ALL_STREAM_FIRST_REGRESSIONS_PASS", flush=True)
