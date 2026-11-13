"""Test XMPlay title aliases for ZIP and 7z without modifying real user settings.
The C++ verifier uses a process-private RegOverridePredefKey() sandbox.
"""
import struct,subprocess,tempfile,wave,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MODULE=ROOT/"xmp_archive_stream"
HOST=MODULE/"Release"/"xmplay_adapter_test.exe"
PLUGIN=MODULE/"Release"/"xmp-archive-stream.dll"
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")
assert all(p.is_file() for p in (HOST,PLUGIN,SEVENZIP))
with tempfile.TemporaryDirectory(prefix="asp-xmplay-title-",dir=r"D:\Temp") as temp:
    folder=Path(temp)
    wavs=[]
    for index in range(2):
        track=folder/f"Track{index}.wav"
        with wave.open(str(track),"wb") as writer:
            writer.setparams((1,2,44100,0,"NONE","not compressed"))
            writer.writeframes(struct.pack("<h",777+index)*44100)
        wavs.append(track)
    zipname=folder/"Tracks.zip"
    with zipfile.ZipFile(zipname,"w") as z:
        for track in wavs:
            z.write(track,track.name)
    seven=folder/"Tracks.7z"
    subprocess.run([str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=on","-y",
                    str(seven),*(str(t) for t in wavs)],capture_output=True,check=True)
    for archive in (zipname,seven):
        result=subprocess.run([str(HOST),str(PLUGIN),str(archive),"--test-title-parity"],
                              capture_output=True,text=True,errors="replace",timeout=90)
        assert result.returncode==0,(archive.name,result.stdout,result.stderr)
        assert "PASS XMPlay reversible title aliases=2" in result.stdout
        assert "PASS XMPlay default preset legacy archive names" in result.stdout
        assert result.stdout.count("PASS XMPlay decoded file=")==2,result.stdout
        print("PASS_XMPLAY_TITLE_ALIAS_ROUNDTRIP",archive.suffix,"two_tracks old_and_new_names",flush=True)
print("ALL_XMPLAY_TITLE_PARITY_TESTS_PASS",flush=True)
