"""Build genuinely frame-continuous ADTS AAC or container-padded M4A/WMA fixtures.

AAC contains actual repeated compressed audio frames. M4A/WMA keep their
original compressed audio and add ignorable metadata padding to test large-file
native cache routing; these are NOT long-audio duration parity fixtures.
"""
from pathlib import Path
import struct
import sys

def validate_aac(data):
    pos=0
    frames=0
    while pos<len(data):
        if len(data)-pos<7:
            raise ValueError("short ADTS frame")
        h=data[pos:pos+7]
        size=((h[3]&3)<<11)|(h[4]<<3)|(h[5]>>5)
        if h[0]!=255 or h[1]&0xF6!=0xF0 or size<7 or pos+size>len(data):
            raise ValueError(f"invalid ADTS frame at {pos}")
        frames+=1
        pos+=size
    return frames

def main(source,target):
    source=Path(source)
    target=Path(target)
    data=source.read_bytes()
    codec=source.suffix.lower()
    minbytes=70*1024*1024
    if codec==".aac":
        frames=validate_aac(data)
        repeats=(minbytes//len(data))+1
        target.write_bytes(data*repeats)
        print("MF_CACHE_VALID_ADTS_AAC",len(data)*repeats,"frames",frames*repeats,"repetitions",repeats)
    elif codec==".m4a":
        assert data[4:8]==b"ftyp"
        pad=max(minbytes-len(data)+1024,0)
        # A valid MP4 top-level free box. No audio sample table changes.
        atom=struct.pack(">I",pad+8)+b"free"
        with target.open("wb") as out:
            out.write(data)
            out.write(atom)
            out.write(b"\x00"*pad)
        print("MF_CACHE_VALID_M4A_FREE_BOX",target.stat().st_size,"free_bytes",pad)
    elif codec==".wma":
        assert data[:16]==bytes.fromhex("3026b2758e66cf11a6d900aa0062ce6c")
        pad=max(minbytes-len(data)+1024,0)
        # Synthetic oversized-container fixture. ASF itself is unchanged;
        # extra bytes follow the complete media object. It is NOT a
        # long-audio/strict-ASF-conformance test; PCM parity checks required.
        guid=bytes.fromhex("d4d40618dfca0945a4ba9aabcb96aae8")
        with target.open("wb") as out:
            out.write(data)
            out.write(guid)
            out.write(struct.pack("<Q",pad+24))
            out.write(b"\x00"*pad)
        print("MF_CACHE_WMA_SYNTHETIC_TRAILING_PADDING",target.stat().st_size,
            "padding_bytes",pad)
    else:
        raise ValueError(f"Unsupported codec {codec}")
    assert target.stat().st_size>64*1024*1024

if __name__=="__main__":
    assert len(sys.argv)==3
    main(*sys.argv[1:])
