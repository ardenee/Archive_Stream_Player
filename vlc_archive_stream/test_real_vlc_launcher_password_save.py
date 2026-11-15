"""VLC 3 native v1.2 actual PowerShell launcher: Save=Yes, wrong, Cancel.

The production launcher takes an explicit *disposable* -PasswordList path.
Only its VLC child sees ARCHIVE_STREAM_VLC_PASSWORD_LIST; the shared HKCU
configuration and original configured list are never modified. Tests both
archive enumeration and native audio entry Open for encrypted filename albums.
Set AST_VLC_QA_UNICODE_PASSWORD_LIST=1 to also verify Unicode filename paths.
"""
import hashlib,os,re,sys,secrets,shutil,struct,subprocess,tempfile,threading,time,wave
from pathlib import Path
import test_real_vlc_password as credentials
import test_real_vlc_launcher_inprocess as launcher

ROOT=Path(__file__).resolve().parents[1]
SCRIPT=launcher.SCRIPT
VLC=launcher.VLC
MODULE=launcher.INSTALLED
DLL=launcher.DLL

def run_case(kind,mode):
 assert mode in ("save","wrong-save","cancel")
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
  password="AST-VLC-LAUNCH-SAVE-"+secrets.token_hex(10)
  list_folder="Café_日本語_Пример" if os.environ.get("AST_VLC_QA_UNICODE_PASSWORD_LIST")=="1" else "passwords"
  qa_list=qa/list_folder/"keeps.txt"
  qa_list.parent.mkdir()
  qa_list.write_bytes(b"")
  decoy_list=qa/"decoy"/"keeps.txt"
  decoy_list.parent.mkdir()
  decoy_list.write_bytes(b"")
  env["ARCHIVE_STREAM_VLC_PASSWORD_LIST"]=str(decoy_list)
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
         "-Archive",str(archive),"-Player","VLC","-PlayerPath",str(VLC),
         "-PasswordList",str(qa_list)]
   r=subprocess.run(args,capture_output=True,text=True,timeout=28,errors="replace",env=env)
   assert r.returncode==0 and "NATIVE_VLC_READY=1 mode=in_process http=0" in r.stdout and \
          "NATIVE_VLC_PASSWORD_LIST_OVERRIDE=1" in r.stdout,(
       r.returncode,r.stdout,r.stderr)
   match=re.search(r"NATIVE_VLC_PLAYER_PID=(\d+)",r.stdout)
   assert match,r.stdout
   pid=int(match.group(1))
   deadline=time.monotonic()+7
   while time.monotonic()<deadline and pid not in launcher.pids():
    time.sleep(.1)
   assert pid in launcher.pids(),("Started VLC PID not observed",pid)
   worker=threading.Thread(target=credentials.worker,args=(pid,password,done,events,mode),daemon=True)
   worker.start()
   log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
   trace=""
   deadline=time.monotonic()+31
   while time.monotonic()<deadline:
    if log.is_file():
     trace=log.read_text(encoding="utf-8-sig",errors="replace")
     if mode=="save":
      if trace.count("NATIVE_PLAYLIST_OPEN")==1 and trace.count("ACCESS_CLOSE")>=3:
       break
     elif "PASSWORD_CANCELLED" in events:
      time.sleep(.4)
      break
     elif mode=="wrong-save" and "SAVE_AGREED" in events:
      # Header authentication can fail immediately without a second
      # prompt. Give VLC time to fail closed, then inspect route logs.
      time.sleep(2.0)
      break
    time.sleep(.25)
   done.set()
   worker.join(timeout=2)
   counts={name:trace.count(name) for name in ("NATIVE_PLAYLIST_OPEN","ACCESS_OPEN_OK","ACCESS_CLOSE","ROUTE_STREAM","ROUTE_MEMORY","ROUTE_CACHE")}
   print("VLC_ACTUAL_LAUNCHER_SAVE_RESULT",kind,mode,"events",events,"counts",counts,flush=True)
   assert counts["ROUTE_CACHE"]==0,trace[-1800:]
   if mode=="save":
    assert events.count("PASSWORD_ENTERED")==1 and events.count("SAVE_AGREED")==1,events
    assert counts["NATIVE_PLAYLIST_OPEN"]==1 and counts["ACCESS_OPEN_OK"]==2 and counts["ACCESS_CLOSE"]>=3,trace[-1800:]
    if kind=="7z-hidden":
     assert counts["ROUTE_STREAM"]==2 and counts["ROUTE_MEMORY"]==0,counts
    else:
     assert counts["ROUTE_MEMORY"]==2 and counts["ROUTE_STREAM"]==0,counts
    assert qa_list.read_text(encoding="utf-8-sig").splitlines()==[password],"Verified key not saved exactly once"
    print("VLC_ACTUAL_LAUNCHER_VERIFIED_SAVE_ONE_ENTRY_TWO_TRACKS_PASS",kind,flush=True)
   else:
    if mode=="wrong-save":
     assert events.count("PASSWORD_WRONG_ENTERED")==1 and events.count("SAVE_AGREED")==1,events
    if mode=="cancel":
     assert events.count("PASSWORD_CANCELLED")==1,events
    assert not qa_list.read_bytes(),"Wrong/cancelled password was saved"
    assert counts["NATIVE_PLAYLIST_OPEN"]==0 and counts["ACCESS_OPEN_OK"]==0 and \
           counts["ROUTE_STREAM"]==0 and counts["ROUTE_MEMORY"]==0,trace[-1800:]
    print("VLC_ACTUAL_LAUNCHER_REJECTED_NO_PASSWORD_SAVE_PASS",kind,mode,flush=True)
   assert not decoy_list.read_bytes(),"Inherited ambient password-list path was used"
   if configured:
    digest=hashlib.sha256(configured.read_bytes()).digest() if configured.is_file() else None
    assert digest==prior,"Original configured password list was mutated"
  finally:
   done.set()
   if worker:worker.join(timeout=2)
   launcher.stop_owned_processes(baseline)
   if saved.is_file():shutil.move(str(saved),str(MODULE))
   assert MODULE.is_file()==had,"Disposable VLC QA module restore failed"
 print("VLC_ACTUAL_LAUNCHER_SAVE_PROFILE_CLEANED_PASS",kind,mode,flush=True)

if __name__=="__main__":
 layouts=("7z-hidden","rar-headers")
 modes=("save","wrong-save","cancel")
 cases=[(kind,mode) for kind in layouts for mode in modes]
 if len(sys.argv)==3:
  cases=[(sys.argv[1],sys.argv[2])]
 for kind,mode in cases:
  run_case(kind,mode)
 print("VLC_ACTUAL_LAUNCHER_NATIVE_SAVE_MATRIX_PASS",len(cases),flush=True)
