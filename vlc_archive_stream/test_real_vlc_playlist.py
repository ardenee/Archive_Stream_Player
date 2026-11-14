"""Actual VLC SDK-native multi-track archive queue regression, no HTTP.

A dedicated statically linked playlist-builder lists archive entries and emits
per-track ast:// MRLs. Real VLC opens them with the native access plugin.
No personal VLC profile or audio device is modified.
"""
import os, shutil, struct, subprocess, tempfile, wave, zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
VLC=Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC\vlc.exe")
PLUGIN=ROOT/"vlc_archive_stream"/"x64"/"Release"/"libarchive_stream_plugin.dll"
BUILDER=ROOT/"vlc_archive_stream"/"x64"/"Release"/"vlc_native_playlist.exe"
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")
RAR=Path(r"C:\Program Files\WinRAR\Rar.exe")

def archive_for(kind,qa,files):
    archive=qa/("native-album."+kind)
    if kind=="zip":
        with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as z:
            for file in files:z.write(file,file.name)
    elif kind=="7z":
        cmd=[str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off",str(archive),*(str(p) for p in files)]
        p=subprocess.run(cmd,cwd=qa,capture_output=True,text=True,timeout=75)
        assert p.returncode==0,p.stderr[-800:]
    else:
        cmd=[str(RAR),"a","-ma5","-m0","-s-","-ep",str(archive),*(str(p) for p in files)]
        p=subprocess.run(cmd,cwd=qa,capture_output=True,text=True,timeout=75)
        assert p.returncode==0,p.stderr[-800:]
    assert archive.is_file()
    return archive

def main():
    assert VLC.is_file() and PLUGIN.is_file() and BUILDER.is_file()
    shutil.copy2(PLUGIN,VLC.parent/"plugins"/"access"/PLUGIN.name)
    with tempfile.TemporaryDirectory(prefix="AST-VLC-NativePlaylist-",dir=r"D:\Temp") as temp:
        qa=Path(temp)
        for name in ("Local","Roaming","Temp"): (qa/name).mkdir()
        env=os.environ.copy()
        env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
                   TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        files=[]
        for num,sample in enumerate((1111,2222)):
            p=qa/("ast-track-"+str(num)+".wav")
            with wave.open(str(p),"wb") as wav:
                wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(44100)
                wav.writeframes(struct.pack("<h",sample)*44100*2)
            files.append(p)
        for kind in ("zip","7z","rar"):
            archive=archive_for(kind,qa,files)
            playlist=qa/("ast-playlist-"+kind+".m3u8")
            result=subprocess.run([str(BUILDER),"--archive",str(archive),
                                   "--playlist",str(playlist)],capture_output=True,
                                  text=True,errors="replace",env=env,cwd=qa,timeout=35)
            assert result.returncode==0 and "NATIVE_VLC_PLAYLIST_ENTRIES=2" in result.stdout,(
                kind,result.returncode,result.stdout,result.stderr)
            content=playlist.read_text(encoding="utf-8")
            assert content.count("ast://")==2 and "http://" not in content,(kind,content[:800])
            for label in ("ast-track-0.wav","ast-track-1.wav"):
                assert label in content,(kind,label,content[:800])
            log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
            if log.is_file(): log.unlink()
            output=qa/(kind+"-last-output.wav")
            args=[str(VLC),"-I","dummy","--ignore-config","--no-plugins-cache",
                  "--no-one-instance","--no-video","--play-and-exit",
                  "--aout=afile","--audiofile-format=s16",
                  "--audiofile-file="+str(output),str(playlist)]
            r=subprocess.run(args,capture_output=True,text=True,errors="replace",
                             env=env,cwd=VLC.parent,timeout=35)
            trace=log.read_text(encoding="utf-8-sig",errors="replace") if log.is_file() else ""
            opened=trace.count("ACCESS_OPEN_OK")
            closed=trace.count("ACCESS_CLOSE")
            streamed=trace.count("ROUTE_STREAM")
            print("VLC_REAL_NATIVE_MULTI_TRACK",kind,"rc",r.returncode,
                  "opened",opened,"closed",closed,"streamed",streamed,
                  "output",output.stat().st_size if output.is_file() else 0,
                  "error_tail",[line[-150:] for line in r.stderr.splitlines() if "error" in line.lower()][-4:],flush=True)
            assert r.returncode==0 and opened==2 and closed==2 and streamed==2,(
                kind,r.returncode,trace[-2800:],r.stderr[-1800:])
    print("VLC_NATIVE_PLAYLIST_ALL_ARCHIVES_NO_HTTP_PASS",flush=True)

if __name__=="__main__":
    main()
