"""Isolated real VLC PCM verification for both bridge architectures.

VLC writes its decoder output to a disposable WAV audio-output file rather than
to speakers. Tests exact source WAV PCM content against
a three-segment known-value waveform; archives remain streamed, no cache.
Nonzero VLC GUI/CLI seek behavior is deliberately not certified by this test.
"""
import hashlib
import struct
import subprocess
import tempfile
import urllib.request
import wave
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VLC = Path(r"C:\Program Files\VideoLAN\VLC\vlc.exe")
RATE = 44100
VALUES = (1000, 2000, 3000)
SECONDS = 2


def create_fixture(folder):
    source = folder / "three-segments.wav"
    with wave.open(str(source), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        for sample in VALUES:
            out.writeframes(struct.pack("<h", sample) * RATE * SECONDS)
    archive = folder / "segments.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as out:
        out.write(source, source.name)
    with wave.open(str(source), "rb") as reader:
        pcm = reader.readframes(reader.getnframes())
    return archive, pcm


def pcm_from_vlc(url, output, start_seconds=None):
    command = [str(VLC), "-I", "dummy", "--ignore-config", "--no-video",
               "--no-one-instance", "--play-and-exit", "--aout=afile",
               "--audiofile-format=s16", "--audiofile-file=" + str(output)]
    if start_seconds is not None:
        command.append("--start-time=" + str(start_seconds))
    command.append(url)
    result = subprocess.run(command, capture_output=True, text=True,
                            errors="replace", timeout=35)
    assert result.returncode == 0, (result.returncode, result.stderr[-1200:])
    assert output.is_file() and output.stat().st_size > 1000, (
        "VLC failed to produce PCM", result.stderr[-1200:])
    with wave.open(str(output), "rb") as audio:
        assert audio.getnchannels() == 1 and audio.getframerate() == RATE
        assert audio.getsampwidth() == 2
        count = audio.getnframes()
        pcm = audio.readframes(count)
    return count, pcm


def check_architecture(arch, archive, reference, folder):
    exe = ROOT / ("Release" if arch == "Win32" else "x64/Release") / "archive_player_bridge.exe"
    assert exe.is_file(), "Missing built bridge " + str(exe)
    target = folder / ("playlist-" + arch)
    target.mkdir()
    bridge = subprocess.Popen(
        [str(exe), "--archive", str(archive), "--playlist-dir", str(target),
         "--lifetime", "70", "--memory-mb", "1", "--cache-limit-mb", "0"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    try:
        startup = {}
        for _ in range(35):
            line = bridge.stdout.readline().strip()
            if "=" in line:
                k, v = line.split("=", 1)
                startup[k] = v
            if line == "BRIDGE_READY=1":
                break
        assert startup.get("BRIDGE_READY") == "1", startup
        playlist = Path(startup["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig")
        urls = [x for x in playlist.splitlines() if x.startswith("http://127.0.0.1:")]
        assert len(urls) == 1, playlist
        count, pcm = pcm_from_vlc(urls[0], folder / (arch + "-full.wav"))
        assert count == RATE * SECONDS * len(VALUES), (arch, count)
        assert hashlib.sha256(pcm).digest() == hashlib.sha256(reference).digest(), (
            arch, "complete source-vs-host PCM parity mismatch")
        print("VLC_REAL_HOST_ARCHIVE_PCM_EXACT_PARITY_PASS", arch,
              "frames", count, "pcm_bytes", len(pcm), flush=True)

        stop = startup["URL_M3U8"].replace("/playlist.m3u8", "/shutdown")
        with urllib.request.urlopen(urllib.request.Request(stop, method="POST"), timeout=7) as res:
            assert res.status == 200
        bridge.wait(timeout=12)
        remainder = bridge.stdout.read()
        assert bridge.returncode == 0 and "ROUTE_COUNTS" in remainder, remainder
        assert "memory=0 cache=0" in remainder, remainder
        print("VLC_REAL_HOST_ARCHIVE_DIRECT_STREAM_NO_CACHE_PASS", arch,
              remainder.strip(), flush=True)
    finally:
        if bridge.poll() is None:
            bridge.kill()
        bridge.wait(timeout=8)


def main():
    assert VLC.is_file()
    with tempfile.TemporaryDirectory(prefix="ast-vlc-pcm-", dir=r"D:\Temp") as scratch:
        folder = Path(scratch)
        archive, reference = create_fixture(folder)
        for arch in ("Win32", "x64"):
            check_architecture(arch, archive, reference, folder)
    print("VLC_REAL_HOST_PCM_X86_X64_ALL_PASS", flush=True)


if __name__ == "__main__":
    main()
