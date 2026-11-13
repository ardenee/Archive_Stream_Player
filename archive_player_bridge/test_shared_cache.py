"""End-to-end shared cache regression: Win32 -> x64 -> same archive, solid warmup,
cross-process reuse and source modification invalidation. Uses isolated LOCALAPPDATA.
"""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import time
import urllib.request
import wave

ROOT = Path(__file__).resolve().parents[1]
SEVENZIP = Path(r"C:\Program Files\7-Zip\7z.exe")
BRIDGES = {k: ROOT / "archive_player_bridge" / p / "archive_player_bridge.exe"
           for k,p in (("Win32","Release"),("x64","x64/Release"))}

def start_bridge(binary, archive, work, profile):
    env = os.environ.copy()
    env["LOCALAPPDATA"] = str(profile)
    process = subprocess.Popen([str(binary), "--archive", str(archive),
        "--playlist-dir", str(work), "--lifetime", "45", "--memory-mb", "1",
        "--cache-limit-mb", "100", "--prompt-password", "0"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    ready = {}
    for _ in range(35):
        line = process.stdout.readline().strip()
        if "=" in line:
            name, value = line.split("=", 1)
            ready[name] = value
        if line == "BRIDGE_READY=1":
            break
    assert ready.get("BRIDGE_READY") == "1", ready
    playlist = Path(ready["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig")
    links = [line for line in playlist.splitlines() if line.startswith("http://127.0.0.1:")]
    assert len(links) == 2, links
    return process, ready, links

def end_bridge(process, info):
    with urllib.request.urlopen(urllib.request.Request(info["URL_M3U8"].replace("/playlist.m3u8", "/shutdown"),
        method="POST"), timeout=8) as response:
        assert response.status == 200
    process.wait(timeout=12)
    output = process.stdout.read()
    errors = process.stderr.read()
    assert process.returncode == 0, (process.returncode, output, errors[-600:])
    return output, errors

def read_track(url, source):
    offset = 321
    end = 64000
    with urllib.request.urlopen(urllib.request.Request(url,
        headers={"Range": f"bytes={offset}-{end}"}), timeout=35) as response:
        assert response.status == 206
        assert response.read() == source[offset:end+1]

def files(folder):
    return {p.name:p.stat().st_size for p in folder.iterdir() if p.is_file() and ".partial-" not in p.name}

def main():
    assert SEVENZIP.is_file()
    assert all(p.is_file() for p in BRIDGES.values())
    with tempfile.TemporaryDirectory(prefix="archive-shared-cache-parity-") as temp:
        root = Path(temp)
        profile = root / "profile"
        profile.mkdir()
        source = []
        for i in range(2):
            path = root / f"Track{i}.wav"
            with wave.open(str(path), "wb") as audio:
                audio.setparams((1,2,44100,0,"NONE","not compressed"))
                sample = struct.pack("<h", 2341+i)
                for _ in range(14):
                    audio.writeframes(sample * 44100)
            assert path.stat().st_size > 1_000_000
            source.append(path)
        archive = root / "solid.7z"
        subprocess.run([str(SEVENZIP), "a", "-t7z", "-ms=on", "-m0=LZMA2", "-y",
            str(archive), *(str(s) for s in source)],capture_output=True,check=True)
        cache = profile / "ArchiveStreamPlayer" / "ManagedCache"
        paths = {}
        for arch in ("Win32", "x64"):
            work = root / f"playlist-{arch}"
            work.mkdir()
            bridge, details, urls = start_bridge(BRIDGES[arch], archive, work, profile)
            try:
                # Select only the second file: the first must appear in cache
                # through background preparation, not because of an HTTP GET.
                read_track(urls[1], source[1].read_bytes())
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline and (not cache.is_dir() or len(files(cache)) < 2):
                    time.sleep(.2)
                if not cache.is_dir() or len(files(cache)) != 2:
                    output,errors=end_bridge(bridge,details)
                    bridge=None
                    raise AssertionError((arch, files(cache) if cache.is_dir() else "no folder", output, errors[-1700:]))
                before = files(cache)
                read_track(urls[0], source[0].read_bytes())
                read_track(urls[1], source[1].read_bytes())
                assert files(cache) == before
                output, errors = end_bridge(bridge,details)
                bridge = None
                assert "ROUTE_COUNTS stream=0 memory=0 cache=3" in output, (arch,output)
                if arch == "x64":
                    assert set(before) == set(paths), (set(before), set(paths))
                paths = before
                print(f"PASS_SHARED_SOLID_CACHE {arch} two-file-warmup and byte-parity and cross-process reuse",flush=True)
            finally:
                if bridge is not None and bridge.poll() is None:
                    bridge.kill()
                    bridge.wait()
        # Changing the on-disk archive invalidates the identity and all cached entries.
        old = set(paths)
        os.utime(archive, ns=(archive.stat().st_atime_ns,archive.stat().st_mtime_ns+2_000_000_000))
        work = root / "playlist-invalidated"
        work.mkdir()
        bridge, details, urls = start_bridge(BRIDGES["x64"],archive,work,profile)
        try:
            read_track(urls[0], source[0].read_bytes())
            deadline=time.monotonic()+20
            while time.monotonic()<deadline and (not cache.is_dir() or len(set(files(cache)) - old)<2):
                time.sleep(.2)
            current=set(files(cache))
            assert len(current) >= 2 and len(current - old) == 2, (old,current)
            output,errors = end_bridge(bridge,details)
            bridge=None
            assert "cache=1" in output,output
            print("PASS_SHARED_CACHE_SOURCE_MODIFICATION_INVALIDATES_ENTRIES",flush=True)
        finally:
            if bridge is not None and bridge.poll() is None:
                bridge.kill()
                bridge.wait()
    print("ALL_SHARED_CACHE_PARITY_REGRESSIONS_PASS",flush=True)

if __name__ == "__main__":
    main()
