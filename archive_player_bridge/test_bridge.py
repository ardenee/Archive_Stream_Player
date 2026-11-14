"""DEV regression for the ZIP/solid-7z playback bridge on both architectures.

Requires C:\audiotest\01.MP3File.mp3, C:\audiotest\02.OGGFile.ogg,
and 7-Zip for creating fixtures (never used by the production bridge).
"""
import os
import subprocess
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ORIGINALS = [Path(r"C:\audiotest\01.MP3File.mp3"), Path(r"C:\audiotest\02.OGGFile.ogg")]
SEVENZIP = Path(r"C:\Program Files\7-Zip\7z.exe")

def vorbis_seconds(source):
    data = source.read_bytes()
    ident = data.find(b"\x01vorbis")
    assert ident >= 0, "DEV Ogg fixture lacks Vorbis identification"
    rate = int.from_bytes(data[ident + 12:ident + 16], "little")
    last_page = data.rfind(b"OggS")
    assert rate > 0 and last_page >= 0
    granule = int.from_bytes(data[last_page + 6:last_page + 14], "little")
    return max(1, (granule * 1000 // rate + 500) // 1000)

def make_fixtures(folder):
    assert all(item.is_file() for item in ORIGINALS), "Missing C:\\audiotest fixtures"
    zip_path = folder / "bridge-test.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_STORED) as out:
        for item in ORIGINALS:
            out.write(item, arcname=item.name)
    assert SEVENZIP.is_file(), "7z.exe is needed for creating DEV fixtures"
    solid_path = folder / "bridge-solid.7z"
    subprocess.run([str(SEVENZIP), "a", "-t7z", "-mx=1", "-ms=on",
        str(solid_path)] + [str(item) for item in ORIGINALS] + ["-y"],
        check=True, capture_output=True)
    return [zip_path, solid_path]

def check(platform, archive, folder, memory_mb=50, title_template="<filename>", sample_id=0):
    location = ROOT / ("x64/Release" if platform == "x64" else "Release")
    binary = location / "archive_player_bridge.exe"
    assert binary.is_file(), "Build Release|{} first: {}".format(platform, binary)
    playlist_folder = folder / ("playlist-" + platform + "-" + archive.stem + "-" + str(memory_mb) + "-" + str(sample_id))
    playlist_folder.mkdir()
    process = subprocess.Popen([str(binary), "--archive", str(archive),
        "--playlist-dir", str(playlist_folder), "--lifetime", "20", "--memory-mb", str(memory_mb),
        "--title-template", title_template],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    try:
        startup = {}
        for _ in range(32):
            line = process.stdout.readline().strip()
            if not line:
                continue
            if "=" in line:
                key, value = line.split("=", 1)
                startup[key] = value
            if line == "BRIDGE_READY=1":
                break
        assert startup.get("BRIDGE_READY") == "1", startup
        assert int(startup["ARCHIVE_AUDIO_ENTRIES"]) == len(ORIGINALS)
        playlist = Path(startup["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig")
        tracks = [line.strip() for line in playlist.splitlines() if line.startswith("http://127.0.0.1:")]
        assert len(tracks) == len(ORIGINALS)
        asx_text = Path(startup["PLAYLIST_ASX"]).read_text(encoding="utf-8-sig")
        assert "<asx version=\"3.0\">" in asx_text
        for index, original in enumerate(ORIGINALS):
            title = title_template.replace("<filename>", original.name)
            title = title.replace("<archivetype>", archive.suffix.lstrip(".").upper())
            title = title.replace("<filetype>", original.suffix.lstrip("."))
            title = title.replace("<archive>", archive.stem)
            title = title.replace("<#>", str(index + 1))
            # Ogg/Vorbis duration is probed from bounded archive stream
            # windows for non-solid, unencrypted entries. Solid archive
            # layouts intentionally retain an unknown duration at startup.
            seconds = vorbis_seconds(original) if original.suffix.lower() == ".ogg" and archive.suffix == ".zip" else -1
            assert "#EXTINF:{},".format(seconds) + title in playlist, (title, seconds, playlist)
            assert "<title>" + title + "</title>" in asx_text, (title, asx_text)
        for index, (track, original) in enumerate(zip(tracks, ORIGINALS)):
            start = 1111
            size = 2000
            with original.open("rb") as expected:
                expected.seek(start)
                correct = expected.read(size)
            request = urllib.request.Request(track, headers={"Range": "bytes=1111-3110"})
            with urllib.request.urlopen(request, timeout=18) as response:
                assert response.status == 206
                assert response.headers["Content-Range"] == "bytes 1111-3110/{}".format(original.stat().st_size)
                assert response.read() == correct
            with urllib.request.urlopen(urllib.request.Request(track, method="HEAD"), timeout=5) as response:
                assert response.status == 200
                assert int(response.headers["Content-Length"]) == original.stat().st_size
            print("PASS", platform, archive.name, "entry", index, "byte-parity and HEAD", flush=True)
        try:
            urllib.request.urlopen(tracks[0].replace("/entry/", "/invalid/"), timeout=5)
            raise AssertionError("unrecognized URL unexpectedly accepted")
        except urllib.error.HTTPError as error:
            assert error.code == 404
        # Only the process-scoped unguessable token permits shutdown.
        url = startup["URL_M3U8"].replace("/playlist.m3u8", "/shutdown")
        with urllib.request.urlopen(urllib.request.Request(url, method="POST"), timeout=4) as response:
            assert response.status == 200
        process.wait(timeout=9)
        output = process.stdout.read()
        assert process.returncode == 0 and "ROUTE_COUNTS" in output, output
        if archive.suffix == ".zip":
            assert "stream=2" in output, output
        elif memory_mb < 2:
            assert "cache=2" in output, output
        else:
            assert "memory=2" in output, output
        print("PASS", platform, archive.name, output.strip(), flush=True)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="archive-bridge-test-") as location:
        workspace = Path(location)
        fixtures = make_fixtures(workspace)
        for arch in ("Win32", "x64"):
            for archive in fixtures:
                check(arch, archive, workspace)
            check(arch, fixtures[1], workspace, memory_mb=1)
        for number, format_text in enumerate((
            "<archivetype>: <filename>", "<#>. <filename>",
            "<filename> [<filetype>]", "<archivetype>: <#> - <filename>",
            "[<archive>] <#> - <filename>"), start=1):
            check("x64", fixtures[0], workspace, title_template=format_text, sample_id=number)
    print("ALL_ARCHIVE_BRIDGE_REGRESSIONS_PASS", flush=True)
