from pathlib import Path
import struct,zipfile,sys,hashlib
def crc(page):
    table=[]
    for i in range(256):
        x=i<<24
        for _ in range(8):
            x=((x<<1)^(0x04C11DB7 if x&0x80000000 else 0))&0xffffffff
        table.append(x)
    val=0
    for byte in page:
        val=((val<<8)&0xffffffff)^table[((val>>24)^byte)&255]
    return val
def build(source,out):
    original=source.read_bytes()
    pages=[]
    offset=0
    while offset<len(original):
        assert original[offset:offset+4]==b"OggS",("invalid Ogg page",offset)
        count=original[offset+26]
        length=27+count+sum(original[offset+27:offset+27+count])
        page=bytearray(original[offset:offset+length])
        assert len(page)==length
        checksum=struct.unpack_from("<I",page,22)[0]
        page[22:26]=bytes(4)
        assert crc(page)==checksum,("bad source CRC",offset)
        pages.append(page)
        offset+=length
    assert offset==len(original)
    first={struct.unpack_from("<I",p,14)[0] for p in pages}
    assert len(first)==1 and sum(bool(p[5]&2) for p in pages)==1 and sum(bool(p[5]&4) for p in pages)==1, "single-link Vorbis source required"
    serial=(first.pop()+123456)&0xffffffff
    second=[]
    for page in pages:
        p=bytearray(page)
        struct.pack_into("<I",p,14,serial)
        p[22:26]=bytes(4)
        struct.pack_into("<I",p,22,crc(p))
        second.append(p)
    data=original+b"".join(second)
    out.mkdir(exist_ok=True,parents=True)
    archive=out/"chained.zip"
    with zipfile.ZipFile(archive,"w",compression=zipfile.ZIP_STORED) as z:
        z.writestr("Chained.ogg",data)
    (out/"chained.ast").write_bytes(b"ARCHIVE_STREAM_NATIVE_TRACK_V1\n"+str(archive).encode()+b".archive_stream|0|Chained.ogg\n")
    print("OGG_CHAIN_QA_VALID_PAGE_CRC_FIXTURE_CREATED",len(pages)*2,len(data))
def verify(out):
    combined=(out/"chain-full.pcm").read_bytes()
    reference=(out/"single-full.pcm").read_bytes()
    seek=(out/"chain-seek.pcm").read_bytes()
    assert len(combined)==2*len(reference),("chained PCM length",len(combined),len(reference))
    assert combined==reference+reference,"Chained PCM differs from two independent original links"
    # 44.1k stereo 16-bit fixture and 240s seek, in the second link.
    absolute=240*44100*4
    delta=absolute-len(reference)
    assert 0<delta<len(reference)-len(seek),(delta,len(reference))
    assert seek==reference[delta:delta+len(seek)],"240-second seek PCM doesn't match original second-link samples"
    print("OGG_CHAIN_TWO_LINK_FULL_PCM_AND_CROSS_LINK_SEEK_PASS",
          "bytes=",len(combined),"seek=240s","sha256=",hashlib.sha256(combined).hexdigest())
if __name__=="__main__":
    assert len(sys.argv)==4
    mode,source,location=sys.argv[1:]
    if mode=="build":
        build(Path(source),Path(location))
    elif mode=="verify":
        verify(Path(location))
    else:
        raise ValueError(mode)
