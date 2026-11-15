"""VLC 3 external plugin path load without modifying installed VLC."""
import os,shutil,subprocess,tempfile,struct,wave
from pathlib import Path
VLC=Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC\vlc.exe")
NATIVE=Path(r"C:\Users\arden\source\repos\vlc_archive_stream\x64\Release\libarchive_stream_plugin.dll")
def main():
 assert VLC.is_file() and NATIVE.is_file()
 installed=VLC.parent/"plugins"/"access"/NATIVE.name
 with tempfile.TemporaryDirectory(prefix="AST-VLC-PluginPath-",dir=r"D:\Temp") as temp:
  qa=Path(temp)
  folder=qa/"external_plugin";folder.mkdir()
  shutil.copy2(NATIVE,folder/NATIVE.name)
  for name in ("Roaming","Local","Temp"): (qa/name).mkdir()
  source=qa/"sound.wav"
  with wave.open(str(source),"wb") as out:
   out.setnchannels(1);out.setsampwidth(2);out.setframerate(44100)
   out.writeframes(struct.pack("<h",1500)*44100*2)
  import zipfile
  archive=qa/"test.zip"
  with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as z:z.write(source,"sound.wav")
  output=qa/"output.wav"
  env=os.environ.copy()
  env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
             TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),ARCHIVE_STREAM_ADAPTER_DEBUG="1",
             VLC_PLUGIN_PATH=str(folder))
  original=None
  if installed.is_file():
   original=qa/"installed_plugin_saved.dll"
   shutil.move(str(installed),str(original))
  try:
   uri="ast://"+str(archive).encode("utf-8").hex().upper()+"/0/sound.wav"
   cmd=[str(VLC),"-I","dummy","--ignore-config","--no-plugins-cache",
        "--no-one-instance","--no-video","--play-and-exit",
        "--aout=afile","--audiofile-format=s16",
        "--audiofile-file="+str(output),uri]
   r=subprocess.run(cmd,capture_output=True,text=True,timeout=25,cwd=VLC.parent,env=env)
   tracefile=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
   trace=tracefile.read_text(encoding="utf-8-sig",errors="replace") if tracefile.is_file() else ""
   assert r.returncode==0 and "ACCESS_OPEN_OK" in trace and "ROUTE_STREAM" in trace,(r.returncode,trace[-1000:],r.stderr[-800:])
   with wave.open(str(output),"rb") as out:
    data=out.readframes(out.getnframes())
   assert data==struct.pack("<h",1500)*44100*2
   print("VLC_NATIVE_EXTERNAL_PLUGIN_PATH_EXACT_PCM_PASS",len(data),flush=True)
  finally:
   if original and original.is_file():
    shutil.move(str(original),str(installed))
 print("VLC_NATIVE_INSTALLED_TEST_PLAYER_MODULE_RESTORED_PASS",flush=True)
if __name__=="__main__":main()
