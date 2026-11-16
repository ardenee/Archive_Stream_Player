"""Create a valid large chained Vorbis fixture from a single logical Ogg stream.

Every additional link receives a distinct Ogg serial number and correct Ogg
page CRC-32, rather than illegally concatenating identical serial numbers.
Only the isolated test script uses the output; original audio is unmodified.
"""
import pathlib
import struct
import sys

POLY = 0x04C11DB7
TABLE = []
for i in range(256):
    value = i << 24
    for _ in range(8):
        value = ((value << 1) ^ (POLY if value & 0x80000000 else 0)) & 0xFFFFFFFF
    TABLE.append(value)

def checksum(page):
    crc = 0
    for b in page:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ TABLE[((crc >> 24) ^ b) & 255]
    return crc

def main(source, target, copies):
    assert copies >= 2
    original = pathlib.Path(source).read_bytes()
    pages = []
    cursor = 0
    while cursor < len(original):
        assert original[cursor:cursor + 4] == b"OggS", ("Ogg signature", cursor)
        count = original[cursor + 26]
        length = 27 + count + sum(original[cursor + 27:cursor + 27 + count])
        page = bytearray(original[cursor:cursor + length])
        assert len(page) == length
        expected = struct.unpack_from("<I", page, 22)[0]
        page[22:26] = b"\x00" * 4
        assert checksum(page) == expected, ("source CRC mismatch", cursor)
        pages.append(page)
        cursor += length
    assert cursor == len(original)
    serials = {struct.unpack_from("<I", page, 14)[0] for page in pages}
    assert len(serials) == 1
    assert sum(bool(page[5] & 2) for page in pages) == 1
    assert sum(bool(page[5] & 4) for page in pages) == 1
    first_serial = serials.pop()
    path = pathlib.Path(target)
    with path.open("wb") as output:
        for index in range(copies):
            serial = (first_serial + 131071 * index) & 0xFFFFFFFF
            for source_page in pages:
                page = bytearray(source_page)
                struct.pack_into("<I", page, 14, serial)
                struct.pack_into("<I", page, 22, checksum(page))
                output.write(page)
    assert path.stat().st_size == len(original) * copies
    print("OGG_VALID_CHAINED_LINKS", copies, "PAGES", len(pages) * copies,
          "ENCODED_BYTES", path.stat().st_size, "UNIQUE_SERIALS", copies)

if __name__ == "__main__":
    assert len(sys.argv) == 4
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]))
