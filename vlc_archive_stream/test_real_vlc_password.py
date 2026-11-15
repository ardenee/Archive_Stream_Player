"""Real isolated VLC 3 x64 native archive password and PCM regression.

Uses a random synthetic password. Handles only dialogs owned by the
disposable VLC test process and declines password storage. The configured
password list may be outside the temporary profile (HKCU); hash it before
and after and fail if it changes. Captures output to a disposable audio file.
Requires pywin32 and pywinauto in the test environment.
"""
import hashlib,os,secrets,shutil,struct,subprocess,sys,tempfile,threading,time,win32gui,win32process,wave,winreg
from pathlib import Path
from pywinauto import Desktop

ROOT=Path(__file__).resolve().parents[1]
VLC=Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC\vlc.exe")
DLL=ROOT/"vlc_archive_stream"/"x64"/"Release"/"libarchive_stream_plugin.dll"
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")
RAR=Path(r"C:\Program Files\WinRAR\Rar.exe")
RATE=44100

def user_password_file():
 try:
  with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r"Software\ArchiveStreamPlayer\Adapters") as key:
   path=Path(winreg.QueryValueEx(key,"PasswordList")[0])
  return path,hashlib.sha256(path.read_bytes()).digest() if path.is_file() else None
 except (OSError,ValueError):
  return None,None

def worker(pid,password,stop,events,mode):
 handled=set()
 while not stop.is_set():
  windows=[]
  def find(hwnd,unused):
   try:
    if win32gui.IsWindowVisible(hwnd) and win32process.GetWindowThreadProcessId(hwnd)[1]==pid:
     windows.append((hwnd,win32gui.GetWindowText(hwnd)))
   except Exception:
    pass
  win32gui.EnumWindows(find,None)
  for hwnd,title in windows:
   if hwnd in handled or title not in ("Archive Stream Player - VLC Password","Archive Stream Player - VLC"):
    continue
   try:
    win=Desktop(backend="win32").window(handle=hwnd)
    if title=="Archive Stream Player - VLC Password":
     if mode=="cancel" or (mode=="wrong" and "PASSWORD_WRONG_ENTERED" in events):
      next(c for c in win.descendants() if c.window_text()=="Cancel").click()
      time.sleep(.2)
      if win32gui.IsWindowVisible(hwnd):continue
      events.append("PASSWORD_CANCELLED")
     else:
      edits=[c for c in win.descendants() if c.class_name()=="Edit"]
      if len(edits)<2:continue
      edits[0].set_edit_text("ArchiveStream")
      edits[-1].set_edit_text(("Incorrect-"+secrets.token_hex(8)) if mode=="wrong" else password)
      next(c for c in win.descendants() if c.window_text()=="OK").click()
      time.sleep(.2)
      if win32gui.IsWindowVisible(hwnd):continue
      events.append("PASSWORD_WRONG_ENTERED" if mode=="wrong" else "PASSWORD_ENTERED")
    else:
     next(c for c in win.descendants() if c.window_text().replace("&","")=="No").click()
     time.sleep(.2)
     if win32gui.IsWindowVisible(hwnd):continue
     events.append("SAVE_DECLINED")
    print("VLC_NATIVE_PASSWORD_GUI_HANDLED",title,flush=True)
    handled.add(hwnd)
   except Exception:
    continue
  time.sleep(.12)

def main(mode='accept',kind='7z'):
 assert mode in ('accept','cancel','wrong')
 assert kind in ('7z','7z-hidden','rar','rar-headers')
 assert all(p.is_file() for p in (VLC,DLL,SEVENZIP))
 if kind.startswith('rar'):assert RAR.is_file()
 shutil.copy2(DLL,VLC.parent/"plugins"/"access"/DLL.name)
 protected,digest=user_password_file()
 with tempfile.TemporaryDirectory(prefix="AST-VLC-NativeAES-",dir=r"D:\Temp") as folder:
  qa=Path(folder)
  for name in ("Roaming","Local","Temp"):
   (qa/name).mkdir()
  isolated=qa/"Local"/"WinampArchiveStream"/"keeps.txt"
  isolated.parent.mkdir(parents=True);isolated.write_bytes(b"")
  env=os.environ.copy()
  env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
             TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
             ARCHIVE_STREAM_ADAPTER_DEBUG="1")
  source=qa/"encrypted.wav"
  with wave.open(str(source),"wb") as output:
   output.setnchannels(1);output.setsampwidth(2);output.setframerate(RATE)
   output.writeframes(struct.pack("<h",2100)*RATE*4)
  password="AST-VLC-NativeAES-"+secrets.token_hex(10)
  ext="rar" if kind.startswith("rar") else "7z"
  archive=qa/("encrypted."+ext)
  if ext=="7z":
   cmd=[str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off",
        "-mhe=on" if kind=="7z-hidden" else "-mhe=off",
        "-p"+password,str(archive),str(source)]
  else:
   # -p sets file-data encryption; -hp additionally encrypts filenames.
   cmd=[str(RAR),"a","-ma5","-m0","-s-","-ep",
        ("-hp" if kind=="rar-headers" else "-p")+password,
        str(archive),str(source)]
  make=subprocess.run(cmd,capture_output=True,text=True,timeout=55,cwd=qa)
  assert make.returncode==0 and archive.is_file(),make.stderr[-800:]
  url="ast://"+str(archive).encode("utf-8").hex().upper()+"/0/encrypted.wav"
  print("VLC_QA_ENCRYPTED_LAYOUT",kind,mode,flush=True)
  pcm=qa/"audio.wav"
  args=[str(VLC),"-I","dummy","--no-one-instance","--ignore-config",
        "--no-plugins-cache","--no-video","--play-and-exit","--aout=afile",
        "--audiofile-format=s16","--audiofile-file="+str(pcm),url]
  process=subprocess.Popen(args,cwd=VLC.parent,env=env,stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE,text=True,errors="replace")
  done=threading.Event()
  events=[]
  watcher=threading.Thread(target=worker,args=(process.pid,password,done,events,mode),daemon=True)
  watcher.start()
  try:
   try:
    stdout,stderr=process.communicate(timeout=36)
   except subprocess.TimeoutExpired:
    process.kill();stdout,stderr=process.communicate(timeout=5)
    raise AssertionError("Native VLC password prompt/player did not terminate")
   # VLC may terminate immediately on Cancel before the GUI watcher has
   # recorded its own completed button click. Join before assertions.
   done.set()
   watcher.join(timeout=2)
   log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
   trace=log.read_text(encoding="utf-8-sig",errors="replace") if log.is_file() else ""
   print("VLC_NATIVE_PASSWORD_TRACE",events,process.returncode,
         [x for x in trace.splitlines() if "VLC" in x][-12:],flush=True)
   if mode=="accept":
    assert process.returncode==0 and events.count("PASSWORD_ENTERED")==1 and events.count("SAVE_DECLINED")==1,(
        process.returncode,events,stderr[-500:])
    assert "ROUTE_CACHE" not in trace,trace[-1100:]
    if kind.startswith("rar"):
     assert "ROUTE_MEMORY" in trace and "ROUTE_STREAM" not in trace,trace[-1600:]
     assert events.count("PASSWORD_ENTERED")==1 and events.count("SAVE_DECLINED")==1,events
    else:
     assert "ROUTE_STREAM" in trace,trace[-1100:]
    assert pcm.is_file()
    with wave.open(str(pcm),"rb") as output:
     data=output.readframes(output.getnframes())
     assert output.getnframes()==RATE*4
    assert data==struct.pack("<h",2100)*RATE*4,"Native VLC AES PCM parity mismatch"
    print("VLC_NATIVE_ENCRYPTED_PASSWORD_DECLINED_SAVE_EXACT_PCM_PASS",kind,flush=True)
   else:
    assert "PASSWORD_ENTERED" not in events,events
    if mode=="wrong":
     assert "PASSWORD_WRONG_ENTERED" in events,events
    else:
     assert "PASSWORD_CANCELLED" in events,events
    assert "ROUTE_STREAM" not in trace and "ROUTE_MEMORY" not in trace and "ROUTE_CACHE" not in trace,trace[-1600:]
    if pcm.is_file():
     with wave.open(str(pcm),"rb") as output:
      assert output.getnframes()==0,"Audio was emitted after password cancellation"
    print("VLC_NATIVE_ENCRYPTED_PASSWORD_REJECT_FAIL_CLOSED_PASS",kind,mode,flush=True)
   assert isolated.read_bytes()==b"","Password unexpectedly persisted to disposable list"
   if protected is not None:
    after=hashlib.sha256(protected.read_bytes()).digest() if protected.is_file() else None
    assert digest==after,"User configured password file mutated"
  finally:
   done.set()
   if process.poll() is None:
    process.kill()
   process.wait(timeout=4)
   watcher.join(timeout=2)
 print("VLC_NATIVE_AES_TEST_PROFILE_CLEANED_PASS",flush=True)
if __name__=="__main__":
 layout=next((x.split("=",1)[1] for x in sys.argv[1:] if x.startswith("--layout=")),"7z")
 mode="cancel" if "--cancel" in sys.argv else "wrong" if "--wrong" in sys.argv else "accept"
 main(mode,layout)
