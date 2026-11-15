"""Local native track-reference generator: ZIP/7z/RAR without HTTP.

Verifies the production x64 enumerator creates only small .ast metadata
manifests and a local M3U8, then terminates without serving any URL.
"""
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import wave
import zipfile

ROOT=Path(__file__).resolve().parents[1]
BUILDER=ROOT/"archive_player_bridge"/"x64"/"Release"/"archive_player_bridge.exe"
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")
RAR=Path(r"C:\Program Files\WinRAR\Rar.exe")
MAGIC=b"ARCHIVE_STREAM_NATIVE_TRACK_V1\n"

def main():
    assert all(p.is_file() for p in (BUILDER,SEVENZIP,RAR))
    with tempfile.TemporaryDirectory(prefix="AST-Native-Manifest-QA-",dir=r"D:\Temp") as temp:
        qa=Path(temp)
        for name in ("Local","Roaming","Temp"):
            (qa/name).mkdir()
        files=[]
        for index,value in enumerate((1000,-1000),1):
            filename=qa/f"track{index}.wav"
            with wave.open(str(filename),"wb") as out:
                out.setnchannels(1)
                out.setsampwidth(2)
                out.setframerate(44100)
                out.writeframes(struct.pack("<h",value)*44100)
            files.append(filename)
        env=os.environ.copy()
        env.update(LOCALAPPDATA=str(qa/"Local"),APPDATA=str(qa/"Roaming"),
                   TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        for kind in ("zip","7z","rar"):
            archive=qa/("sample."+kind)
            if kind=="zip":
                with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as out:
                    for f in files:out.write(f,f.name)
            elif kind=="7z":
                command=[str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off",str(archive),*(str(f) for f in files)]
                subprocess.run(command,cwd=qa,capture_output=True,check=True,timeout=55)
            else:
                command=[str(RAR),"a","-ma5","-m0","-s-","-ep",str(archive),*(str(f) for f in files)]
                subprocess.run(command,cwd=qa,capture_output=True,check=True,timeout=55)
            output=qa/("manifests-"+kind)
            args=[str(BUILDER),"--archive",str(archive),"--native-manifest-dir",str(output),
                  "--password-list",str(qa/"Temp"/"keeps.txt"),"--prompt-password","0",
                  "--title-template","<#>. <filename>"]
            run=subprocess.run(args,cwd=qa,env=env,capture_output=True,text=True,
                               errors="replace",timeout=35)
            assert run.returncode==0 and "NATIVE_MANIFEST_READY=1" in run.stdout,(
                kind,run.returncode,run.stdout,run.stderr)
            assert "ARCHIVE_AUDIO_ENTRIES=2" in run.stdout,(kind,run.stdout)
            line=next((x.split("=",1)[1] for x in run.stdout.splitlines() if x.startswith("NATIVE_PLAYLIST_M3U8=")),None)
            assert line and Path(line).is_file(),(kind,run.stdout)
            playlist=Path(line).read_text(encoding="utf-8")
            assert "#EXTM3U" in playlist and "http:" not in playlist.lower()
            assert "#EXTINF:-1,1. track1.wav" in playlist and "#EXTINF:-1,2. track2.wav" in playlist
            generated=sorted(output.iterdir())
            assert len(generated)==3 and all(f.is_file() for f in generated),(kind,generated)
            refs=sorted(output.glob("entry-*.ast"))
            assert len(refs)==2
            for idx,ref in enumerate(refs):
                expected=MAGIC+(str(archive)+f".archive_stream|{idx}|track{idx+1}.wav\n").encode("utf-8")
                assert ref.read_bytes()==expected,(kind,ref,ref.read_bytes(),expected)
                assert ref.stat().st_size<512
                assert str(ref) in playlist
            print("NATIVE_TRACK_MANIFEST_DIRECT_NO_HTTP_PASS",kind,
                  "tracks=2 manifest_only=1 no_disk_audio=1 no_socket=1",flush=True)
        print("NATIVE_TRACK_MANIFEST_THREE_ARCHIVE_MATRIX_PASS",flush=True)
    print("NATIVE_TRACK_MANIFEST_ISOLATED_CLEANUP_PASS",flush=True)

if __name__=="__main__":
    main()
