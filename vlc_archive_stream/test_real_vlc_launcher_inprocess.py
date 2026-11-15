"""Test real PowerShell launcher -> VLC 3.0 native in-process album handoff.
Only stages a disposable silent WAV album and a temporary VLC profile.
Temporarily moves a DLL in the disposable QA VLC installation (not Program Files)
to certify loading from the project's external native DLL.
"""
import os,shutil,struct,subprocess,tempfile,time,wave,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SCRIPT=ROOT/"archive_player_bridge"/"launch-archive.ps1"
VLC=Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC\vlc.exe")
INSTALLED=VLC.parent/"plugins"/"access"/"libarchive_stream_plugin.dll"
DLL=ROOT/"vlc_archive_stream"/"x64"/"Release"/"libarchive_stream_plugin.dll"

def pids():
 c=f'Get-CimInstance Win32_Process -Filter "Name=\'vlc.exe\'" | Where-Object {{ $_.ExecutablePath -eq \'{VLC}\' }} | Select-Object -ExpandProperty ProcessId'
 p=subprocess.run(["powershell.exe","-NoProfile","-NonInteractive","-Command",c],
                  capture_output=True,text=True,errors="replace",timeout=10)
 assert p.returncode==0,p.stderr[-500:]
 return {int(x) for x in p.stdout.splitlines() if x.strip().isdigit()}

def stop_owned_processes(before):
 for pid in pids()-before:
  subprocess.run(["taskkill","/PID",str(pid),"/T","/F"],capture_output=True,timeout=9)

def main():
 assert all(p.is_file() for p in (SCRIPT,VLC,DLL))
 before=pids()
 assert not before,"Disposable VLC QA installation was already in use; refusing to modify"
 with tempfile.TemporaryDirectory(prefix="AST-VLC-RealLauncher-",dir=r"D:\Temp") as scratch:
  qa=Path(scratch)
  for folder in ("Local","Roaming","Temp"):(qa/folder).mkdir()
  env=os.environ.copy()
  env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
             TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),ARCHIVE_STREAM_ADAPTER_DEBUG="1")
  archive=qa/"silent.zip"
  with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as z:
   for i in range(2):
    file=qa/("silent"+str(i)+".wav")
    with wave.open(str(file),"wb") as sound:
     sound.setnchannels(1);sound.setsampwidth(2);sound.setframerate(44100)
     sound.writeframes(struct.pack("<h",0)*44100*2)
    z.write(file,file.name)
  saved=qa/"original-installed.dll"
  had=INSTALLED.is_file()
  if had:
   shutil.move(str(INSTALLED),str(saved))
  try:
   cmd=["powershell.exe","-NoProfile","-NonInteractive","-File",str(SCRIPT),
        "-Player","VLC","-PlayerPath",str(VLC),"-Archive",str(archive)]
   launcher=subprocess.run(cmd,env=env,capture_output=True,text=True,errors="replace",timeout=25)
   assert launcher.returncode==0 and "NATIVE_VLC_READY=1 mode=in_process http=0" in launcher.stdout,(
       launcher.returncode,launcher.stdout,launcher.stderr)
   assert "VLC native archive playback requested" in launcher.stdout,launcher.stdout
   log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
   trace=""
   deadline=time.monotonic()+23
   while time.monotonic()<deadline:
    if log.is_file():
     trace=log.read_text(encoding="utf-8-sig",errors="replace")
     if trace.count("NATIVE_PLAYLIST_OPEN")>=1 and trace.count("ACCESS_CLOSE")>=3:
      break
    time.sleep(.4)
   counts={x:trace.count(x) for x in ("NATIVE_PLAYLIST_OPEN","ACCESS_OPEN_OK","ACCESS_CLOSE","ROUTE_STREAM","ROUTE_CACHE")}
   print("VLC_REAL_LAUNCHER_INPROC_EXTERNAL_DLL",counts,
         "http=",("http://127.0.0.1" in trace),flush=True)
   assert counts["NATIVE_PLAYLIST_OPEN"]==1 and counts["ACCESS_OPEN_OK"]==2 and counts["ACCESS_CLOSE"]==3 and counts["ROUTE_STREAM"]==2 and counts["ROUTE_CACHE"]==0,(launcher.stdout,trace[-2500:])
   assert "http://127.0.0.1" not in trace
   print("VLC_REAL_INPROC_LAUNCHER_EXTERNAL_DLL_TWO_SILENT_TRACKS_PASS",flush=True)
  finally:
   stop_owned_processes(before)
   if had and saved.is_file():
    shutil.move(str(saved),str(INSTALLED))
  assert INSTALLED.is_file()==had,"Disposable VLC module restoration mismatch"
 print("VLC_REAL_LAUNCHER_DISPOSABLE_PROFILE_CLEANED_PASS",flush=True)

if __name__=="__main__":
 main()
