"""Actual PowerShell native VLC launcher: encrypted-header album, one credential
dialog across playlist enumeration and both tracks, with no HTTP.
Tests a disposable VLC install, two silent synthetic WAV tracks, and a
temporary user profile. Does not modify the user's VLC install or password list.
"""
import hashlib,os,re,secrets,shutil,struct,subprocess,tempfile,threading,time,wave
from pathlib import Path
import test_real_vlc_password as credentials
import test_real_vlc_launcher_inprocess as launcher

ROOT=Path(__file__).resolve().parents[1]
SCRIPT=launcher.SCRIPT
VLC=launcher.VLC
MODULE=launcher.INSTALLED
DLL=launcher.DLL

def run_case(kind):
 assert kind in ("7z-hidden","rar-headers")
 baseline=launcher.pids()
 assert not baseline,"A disposable VLC process is already active; refusing to interrupt it"
 configured,prior=credentials.user_password_file()
 with tempfile.TemporaryDirectory(prefix="AST-VLC-LauncherAES-",dir=r"D:\Temp") as temp:
  qa=Path(temp)
  for directory in ("Local","Roaming","Temp"):(qa/directory).mkdir()
  env=os.environ.copy()
  env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
             TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),ARCHIVE_STREAM_ADAPTER_DEBUG="1")
  files=[]
  for i in range(2):
   filename=qa/("silent"+str(i)+".wav")
   with wave.open(str(filename),"wb") as output:
    output.setnchannels(1);output.setsampwidth(2);output.setframerate(44100)
    output.writeframes(struct.pack("<h",0)*88200)
   files.append(filename)
  password="AST-VLC-LAUNCH-"+secrets.token_hex(10)
  if kind=="7z-hidden":
   archive=qa/"native-encrypted.7z"
   cmd=[str(credentials.SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off",
        "-mhe=on","-p"+password,str(archive),*(str(x) for x in files)]
  else:
   archive=qa/"native-encrypted.rar"
   cmd=[str(credentials.RAR),"a","-ma5","-m0","-s-","-ep",
        "-hp"+password,str(archive),*(str(x) for x in files)]
  generated=subprocess.run(cmd,cwd=qa,text=True,capture_output=True,timeout=65)
  assert generated.returncode==0 and archive.is_file(),generated.stderr[-1000:]
  saved=qa/"installed-qa-dll-backup"
  had=MODULE.is_file()
  if had:shutil.move(str(MODULE),str(saved))
  pid=None
  done=threading.Event()
  worker=None
  events=[]
  try:
   args=["powershell.exe","-NoProfile","-NonInteractive","-File",str(SCRIPT),
         "-Archive",str(archive),"-Player","VLC","-PlayerPath",str(VLC)]
   r=subprocess.run(args,capture_output=True,text=True,timeout=28,errors="replace",env=env)
   assert r.returncode==0 and "NATIVE_VLC_READY=1 mode=in_process http=0" in r.stdout,(
       r.returncode,r.stdout,r.stderr)
   match=re.search(r"NATIVE_VLC_PLAYER_PID=(\d+)",r.stdout)
   assert match,r.stdout
   pid=int(match.group(1))
   deadline=time.monotonic()+7
   while time.monotonic()<deadline and pid not in launcher.pids():
    time.sleep(.1)
   assert pid in launcher.pids(),("Started VLC PID not observed",pid)
   worker=threading.Thread(target=credentials.worker,args=(pid,password,done,events,"accept"),daemon=True)
   worker.start()
   log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
   trace=""
   deadline=time.monotonic()+31
   while time.monotonic()<deadline:
    if log.is_file():
     trace=log.read_text(encoding="utf-8-sig",errors="replace")
     if trace.count("NATIVE_PLAYLIST_OPEN")==1 and trace.count("ACCESS_CLOSE")>=3:
      break
    time.sleep(.25)
   done.set()
   worker.join(timeout=2)
   counts={name:trace.count(name) for name in ("NATIVE_PLAYLIST_OPEN","ACCESS_OPEN_OK","ACCESS_CLOSE","ROUTE_STREAM","ROUTE_MEMORY","ROUTE_CACHE")}
   print("VLC_ACTUAL_LAUNCHER_ENCRYPTED_ALBUM",kind,"events",events,"counts",counts,flush=True)
   assert events.count("PASSWORD_ENTERED")==1 and events.count("SAVE_DECLINED")==1,events
   assert counts["NATIVE_PLAYLIST_OPEN"]==1 and counts["ACCESS_OPEN_OK"]==2 and counts["ACCESS_CLOSE"]>=3,trace[-1800:]
   assert counts["ROUTE_CACHE"]==0
   if kind=="7z-hidden":
    assert counts["ROUTE_STREAM"]==2 and counts["ROUTE_MEMORY"]==0,counts
   else:
    assert counts["ROUTE_MEMORY"]==2 and counts["ROUTE_STREAM"]==0,counts
   if configured:
    digest=hashlib.sha256(configured.read_bytes()).digest() if configured.is_file() else None
    assert digest==prior,"Original configured password list was mutated"
   print("VLC_ACTUAL_LAUNCHER_ENCRYPTED_TWO_TRACKS_ONE_PROMPT_PASS",kind,flush=True)
  finally:
   done.set()
   if worker:worker.join(timeout=2)
   launcher.stop_owned_processes(baseline)
   if saved.is_file():shutil.move(str(saved),str(MODULE))
   assert MODULE.is_file()==had,"Disposable VLC QA module restore failed"
 print("VLC_ACTUAL_LAUNCHER_ENCRYPTED_PROFILE_CLEANED_PASS",kind,flush=True)

if __name__=="__main__":
 for kind in ("7z-hidden","rar-headers"):
  run_case(kind)
 print("VLC_ACTUAL_LAUNCHER_NATIVE_ENCRYPTED_ALBUM_ALL_PASS",flush=True)
