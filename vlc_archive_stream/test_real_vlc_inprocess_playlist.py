"""Test a VLC-native ast://.../list/album.m3u8 in the same process as playback."""
import os,shutil,struct,subprocess,tempfile,wave,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
VLC=Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC\vlc.exe")
DLL=ROOT/"vlc_archive_stream"/"x64"/"Release"/"libarchive_stream_plugin.dll"
def main():
 assert VLC.is_file() and DLL.is_file()
 shutil.copy2(DLL,VLC.parent/"plugins"/"access"/DLL.name)
 with tempfile.TemporaryDirectory(prefix="AST-VLC-InprocessList-",dir=r"D:\Temp") as temp:
  qa=Path(temp)
  for item in ("Roaming","Local","Temp"):(qa/item).mkdir()
  env=os.environ.copy()
  env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
             TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),ARCHIVE_STREAM_ADAPTER_DEBUG="1")
  archive=qa/"album.zip"
  with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as zipout:
   for idx,value in enumerate((1100,2200)):
    data=qa/("track"+str(idx)+".wav")
    with wave.open(str(data),"wb") as w:
     w.setnchannels(1);w.setsampwidth(2);w.setframerate(44100)
     w.writeframes(struct.pack("<h",value)*44100*2)
    zipout.write(data,data.name)
  url="ast://"+str(archive).encode("utf-8").hex().upper()+"/list/album.m3u8"
  output=qa/"out.wav"
  cmd=[str(VLC),"-I","dummy","--no-one-instance","--ignore-config",
       "--no-plugins-cache","--no-video","--play-and-exit",
       "--aout=afile","--audiofile-format=s16",
       "--audiofile-file="+str(output),url]
  p=subprocess.run(cmd,capture_output=True,text=True,errors="replace",
                   cwd=VLC.parent,env=env,timeout=30)
  log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
  text=log.read_text(encoding="utf-8-sig",errors="replace") if log.is_file() else ""
  print("VLC_INPROCESS_PLAYLIST_PROBE",p.returncode,
        "list",text.count("NATIVE_PLAYLIST_OPEN"),
        "entries",text.count("ACCESS_OPEN_OK"),
        "closed",text.count("ACCESS_CLOSE"),
        "streams",text.count("ROUTE_STREAM"),
        "output",output.stat().st_size if output.is_file() else 0,
        "player_error_tail",[x[:220] for x in p.stderr.splitlines() if "error" in x.lower()][-8:],flush=True)
  assert p.returncode==0 and text.count("NATIVE_PLAYLIST_OPEN")==1 and text.count("ACCESS_OPEN_OK")==2 and text.count("ROUTE_STREAM")==2,(p.returncode,text,p.stderr[-1200:])
  print("VLC_INPROCESS_PLAYLIST_TWO_TRACKS_NO_HTTP_PASS",flush=True)
if __name__=="__main__":main()
