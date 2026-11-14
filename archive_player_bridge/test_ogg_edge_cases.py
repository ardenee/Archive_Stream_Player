"""Verify malformed and chained Ogg/Vorbis bridge behavior without player GUI.

A genuine logical-stream chain is constructed by changing the second stream's
serial numbers and recomputing every Ogg page CRC. The shared bridge must not
mistake the second stream's EOS granule for a duration of the entire chain.
All entries must remain byte-accurate direct archive streams.
"""
import subprocess
import tempfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = Path(r"C:\audiotest\02.OGGFile.ogg")
POLYNOMIAL = 0x04C11DB7


def crc_table():
    table = []
    for byte in range(256):
        crc = byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (POLYNOMIAL if crc & 0x80000000 else 0)) & 0xffffffff
        table.append(crc)
    return table


CRC_TABLE = crc_table()


def ogg_crc(data):
    crc = 0
    for byte in data:
        crc = ((crc << 8) & 0xffffffff) ^ CRC_TABLE[((crc >> 24) ^ byte) & 255]
    return crc


def pages(data):
    pos = 0
    result = []
    while pos < len(data):
        assert data[pos:pos + 4] == b"OggS" and pos + 27 <= len(data), ("bad Ogg page", pos)
        count = data[pos + 26]
        assert pos + 27 + count <= len(data)
        length = 27 + count + sum(data[pos + 27:pos + 27 + count])
        assert pos + length <= len(data)
        page = data[pos:pos + length]
        expected = int.from_bytes(page[22:26], "little")
        check = bytearray(page)
        check[22:26] = bytes(4)
        assert ogg_crc(check) == expected, ("bad source CRC", pos)
        result.append((pos, length, int.from_bytes(page[14:18], "little"),
                       page[5], int.from_bytes(page[6:14], "little")))
        pos += length
    return result


def second_logical_stream(data, table):
    previous_serials = {page[2] for page in table}
    assert len(previous_serials) == 1, "source must be a single logical stream"
    old_serial = table[0][2]
    new_serial = old_serial ^ 0xa5a5a5a5
    assert new_serial not in previous_serials
    output = bytearray(data)
    for pos, length, _, _, _ in table:
        page = bytearray(output[pos:pos + length])
        page[14:18] = new_serial.to_bytes(4, "little")
        page[22:26] = bytes(4)
        page[22:26] = ogg_crc(page).to_bytes(4, "little")
        output[pos:pos + length] = page
    assert len(pages(output)) == len(table), "changed serial invalidated Ogg CRC"
    return bytes(output)


def launch_and_check(arch, zip_path, originals, expected_seconds, folder):
    binary = HERE / ("x64/Release" if arch == "x64" else "Release") / "archive_player_bridge.exe"
    assert binary.is_file(), "Build bridge Release for " + arch
    playlist_folder = folder / ("playlist-" + arch)
    playlist_folder.mkdir()
    process = subprocess.Popen([str(binary), "--archive", str(zip_path),
                                "--playlist-dir", str(playlist_folder),
                                "--lifetime", "30", "--memory-mb", "1",
                                "--title-template", "<filename>"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, bufsize=1)
    try:
        startup = {}
        for _ in range(40):
            line = process.stdout.readline().strip()
            if "=" in line:
                key, value = line.split("=", 1)
                startup[key] = value
            if line == "BRIDGE_READY=1":
                break
        assert startup.get("BRIDGE_READY") == "1", startup
        playlist = Path(startup["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig")
        urls = [line.strip() for line in playlist.splitlines()
                if line.startswith("http://127.0.0.1:")]
        assert len(urls) == len(originals), (len(urls), playlist)
        for (name, raw), url in zip(originals.items(), urls):
            duration = expected_seconds[name]
            assert "#EXTINF:{},".format(duration) + name in playlist, (name, playlist)
            with urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=12) as res:
                assert res.status == 200
                assert int(res.headers["Content-Length"]) == len(raw)
            for offset in (0, len(raw) // 2, max(0, len(raw) - 2048)):
                end = min(len(raw) - 1, offset + 2047)
                req = urllib.request.Request(url, headers={"Range": "bytes={}-{}".format(offset, end)})
                with urllib.request.urlopen(req, timeout=15) as res:
                    assert res.status == 206, (name, res.status)
                    assert res.read() == raw[offset:end + 1], (name, offset)
            print("PASS", arch, name, "duration", duration, "three HTTP byte ranges", flush=True)
        shutdown = startup["URL_M3U8"].replace("/playlist.m3u8", "/shutdown")
        with urllib.request.urlopen(urllib.request.Request(shutdown, method="POST"), timeout=10) as res:
            assert res.status == 200
        process.wait(timeout=12)
        remaining = process.stdout.read()
        assert process.returncode == 0 and "ROUTE_COUNTS" in remaining, remaining
        assert "memory=0 cache=0" in remaining, remaining
        print("PASS", arch, "direct streaming without memory/disk cache", remaining.strip(), flush=True)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=15)


def main():
    original = SOURCE.read_bytes()
    table = pages(original)
    assert table[0][3] & 0x02 and table[-1][3] & 0x04, "source has no BOS/EOS"
    ident = original.find(b"\x01vorbis")
    assert ident >= 0
    rate = int.from_bytes(original[ident + 12:ident + 16], "little")
    assert 8000 <= rate <= 384000
    seconds = max(1, (table[-1][4] * 1000 // rate + 500) // 1000)
    chained = original + second_logical_stream(original, table)
    truncated = original[:table[-1][0]]
    expected = {"single.ogg": seconds, "chained.ogg": -1, "missing-eos.ogg": -1}
    sources = {"single.ogg": original, "chained.ogg": chained, "missing-eos.ogg": truncated}
    with tempfile.TemporaryDirectory(prefix="ast-ogg-edge-") as tmp:
        folder = Path(tmp)
        archive = folder / "ogg-edge.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as out:
            for name, raw in sources.items():
                out.writestr(name, raw)
        for arch in ("Win32", "x64"):
            launch_and_check(arch, archive, sources, expected, folder)
    print("OGG_EDGE_CASE_BRIDGE_REGRESSION_PASS", flush=True)


if __name__ == "__main__":
    main()
