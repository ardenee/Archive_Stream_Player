"""Real VLC 3.0.24 native access-module PCM test; no HTTP bridge.

Runs a disposable copy of VLC with our statically linked native DLL.
An ast:// URL identifies archive UTF-8 path via reversible hex and entry index.
No user VLC configuration, sound device, HTTP service, or persistent cache.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import wave
import zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
VLC=Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC\vlc.exe")
DLL=ROOT/"vlc_archive_stream"/"x64"/"Release"/"libarchive_stream_plugin.dll"
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")
RAR=Path(r"C:\Program Files\WinRAR\Rar.exe")
RATE=44100
TEST_TYPES=("zip","7z","rar")

def archive_source(kind,qa,original):
    archive=qa/("native-"+kind+"."+kind)
    if kind=="zip":
        with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as out:
            out.write(original,"ast_pcm.wav")
    elif kind=="7z":
        result=subprocess.run([str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off",
                               str(archive),str(original)],cwd=qa,capture_output=True,
                              text=True,timeout=60)
        assert result.returncode==0,result.stderr[-1000:]
    else:
        result=subprocess.run([str(RAR),"a","-ma5","-m0","-s-","-ep",
                               str(archive),str(original)],cwd=qa,capture_output=True,
                              text=True,timeout=60)
        assert result.returncode==0,result.stderr[-1000:]
    assert archive.is_file()
    return archive

def main():
    assert VLC.is_file() and DLL.is_file()
    plugin=VLC.parent/"plugins"/"access"/"libarchive_stream_plugin.dll"
    shutil.copy2(DLL,plugin)
    with tempfile.TemporaryDirectory(prefix="AST-VLC-NativeSDK-",dir=r"D:\Temp") as scratch:
        qa=Path(scratch)
        for name in ("Local","Roaming","Temp"):
            (qa/name).mkdir()
        env=os.environ.copy()
        env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
                   TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        original=qa/"ast_pcm.wav"
        with wave.open(str(original),"wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(RATE)
            for sample in (1000,2000,3000):
                output.writeframes(struct.pack("<h",sample)*RATE*2)
        with wave.open(str(original),"rb") as originalwav:
            source_pcm=originalwav.readframes(originalwav.getnframes())
        print("AST_VLC_NATIVE_ORIGINAL_PCM_SHA256",hashlib.sha256(source_pcm).hexdigest(),flush=True)
        for number,kind in enumerate(TEST_TYPES,1):
            archive=archive_source(kind,qa,original)
            # Last URI segment gives VLC demuxers a matching extension, while
            # the actual index, not the untrusted URI display name, selects bytes.
            uri="ast://"+str(archive).encode("utf-8").hex().upper()+"/0/ast_pcm.wav"
            outfile=qa/(kind+"-out.wav")
            cmd=[str(VLC),"-I","dummy","--no-one-instance","--ignore-config",
                 "--no-plugins-cache","--no-video","--play-and-exit",
                 "--aout=afile","--audiofile-format=s16",
                 "--audiofile-file="+str(outfile),uri]
            try:
                result=subprocess.run(cmd,cwd=VLC.parent,env=env,text=True,
                                      capture_output=True,timeout=30,errors="replace")
            except subprocess.TimeoutExpired:
                raise AssertionError("VLC native host did not finish within timeout for "+kind)
            frames=-1
            match=False
            if outfile.is_file():
                try:
                    with wave.open(str(outfile),"rb") as decoded:
                        frames=decoded.getnframes()
                        match=decoded.readframes(frames)==source_pcm
                except Exception:pass
            log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
            route=log.read_text(encoding="utf-8-sig",errors="replace") if log.is_file() else ""
            events=[l.strip()[:220] for l in route.splitlines() if "VLC" in l or "ROUTE" in l]
            assert result.returncode==0 and match and frames==RATE*6,(kind,result.returncode,match,frames,result.stderr[-800:])
            assert route.count("ROUTE_STREAM")==number and "ROUTE_MEMORY" not in route and "ROUTE_CACHE" not in route,(kind,route[-2400:])
            print("AST_VLC_NATIVE_HOST_PCM",kind,"rc",result.returncode,
                  "frames",frames,"pcm_equal",match,"log",events[-12:],
                  "player_errors",[l[:230] for l in result.stderr.splitlines()
                                   if "error" in l.lower()][-8:],flush=True)
        print("AST_VLC_NATIVE_HOST_PROFILE_REMOVED_ON_EXIT",flush=True)

if __name__=="__main__":
    main()
