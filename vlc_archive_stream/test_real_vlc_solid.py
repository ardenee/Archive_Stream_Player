"""Real VLC 3 native 7z solid/RAM fallback and PCM-parity test."""
import os,shutil,struct,subprocess,tempfile,wave
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
VLC=Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC\vlc.exe")
DLL=ROOT/"vlc_archive_stream"/"x64"/"Release"/"libarchive_stream_plugin.dll"
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")
def main():
 assert DLL.is_file() and VLC.is_file()
 shutil.copy2(DLL,VLC.parent/"plugins"/"access"/DLL.name)
 with tempfile.TemporaryDirectory(prefix="AST-VLC-SolidHost-",dir=r"D:\Temp") as t:
  qa=Path(t); env=os.environ.copy()
  for n in ("Roaming","Local","Temp"):
   (qa/n).mkdir()
  env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),TEMP=str(qa/"Temp"),
             TMP=str(qa/"Temp"),ARCHIVE_STREAM_ADAPTER_DEBUG="1")
  tracks=[]
  for i,val in enumerate((1212,2323)):
   f=qa/("test-"+str(i)+".wav")
   with wave.open(str(f),"wb") as w:
    w.setnchannels(1);w.setsampwidth(2);w.setframerate(44100)
    w.writeframes(struct.pack("<h",val)*44100*4)
   tracks.append(f)
  archive=qa/"solid.7z"
  build=subprocess.run([str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=on",str(archive),
                        *(str(x) for x in tracks)],capture_output=True,text=True,
                       timeout=60,cwd=qa)
  assert build.returncode==0 and archive.is_file(),build.stderr[-1000:]
  for i,expected in enumerate((1212,2323)):
   link="ast://"+str(archive).encode("utf-8").hex().upper()+f"/{i}/test-{i}.wav"
   output=qa/("decoded-"+str(i)+".wav")
   proc=subprocess.run([str(VLC),"-I","dummy","--no-one-instance","--ignore-config",
                        "--no-plugins-cache","--no-video","--play-and-exit",
                        "--aout=afile","--audiofile-format=s16",
                        "--audiofile-file="+str(output),link],capture_output=True,
                       text=True,errors="replace",timeout=27,cwd=VLC.parent,env=env)
   assert proc.returncode==0 and output.is_file(),(i,proc.stderr[-1000:])
   with wave.open(str(output),"rb") as w:
    samples=w.readframes(w.getnframes())
    assert w.getnframes()==44100*4
   assert samples==struct.pack("<h",expected)*44100*4,(i,"PCM mismatch")
   print("VLC_NATIVE_SOLID_7Z_PCM_PARITY_PASS",i,len(samples),flush=True)
  trace=(qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log").read_text(encoding="utf-8-sig",errors="replace")
  memory=trace.count("ROUTE_MEMORY")
  stream=trace.count("ROUTE_STREAM")
  cache=trace.count("ROUTE_CACHE")
  assert memory==2 and stream==0 and cache==0,(memory,stream,cache,trace[-2800:])
  print("VLC_NATIVE_SOLID_7Z_RAM_BEFORE_DISK_PASS",memory,stream,cache,flush=True)
 print("VLC_NATIVE_SOLID_ISOLATION_CLEANED_PASS",flush=True)
if __name__=="__main__":main()
