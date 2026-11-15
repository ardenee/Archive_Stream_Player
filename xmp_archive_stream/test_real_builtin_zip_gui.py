"""Isolated XMPlay 4.1 native ZIP XMPARC priority investigation.
Test fixture and player profile disposable; no installed-player modification.
Optional AST_XMPLAY_ZIP_INSPECT=1 opens the host decoder options to show the builtin ZIP handler ahead of XMPARC.
"""
import os,sys,shutil,subprocess,tempfile,time,zipfile
from pathlib import Path
from test_real_native_gui_seek import AUTOMATION,XMPLAY_SOURCE,PLUGIN,AUDIO,wait_for,player_window,progress
os.add_dll_directory(str(AUTOMATION/"pywin32_system32"))
sys.path[:0]=[str(AUTOMATION),str(AUTOMATION/"win32"),str(AUTOMATION/"win32"/"lib"),str(AUTOMATION/"Pythonwin")]
from pywinauto import Desktop,mouse
from pywinauto.keyboard import send_keys
import win32gui,win32process,win32con

def windows(pid):
 result=[]
 def visit(hwnd,ctx):
  if win32gui.IsWindowVisible(hwnd) and win32process.GetWindowThreadProcessId(hwnd)[1]==pid:
   result.append((hwnd,win32gui.GetClassName(hwnd),win32gui.GetWindowText(hwnd),win32gui.GetWindowRect(hwnd)))
 win32gui.EnumWindows(visit,None)
 return result

def screenshot(filename):
 shot=("Add-Type -AssemblyName System.Drawing;"
       "$b=New-Object System.Drawing.Bitmap 1366,850;"
       "$g=[System.Drawing.Graphics]::FromImage($b);"
       "$g.CopyFromScreen(0,0,0,0,$b.Size);"
       "$b.Save('"+str(filename).replace("'","''")+"',[System.Drawing.Imaging.ImageFormat]::Jpeg);"
       "$g.Dispose();$b.Dispose()")
 result=subprocess.run(["powershell.exe","-NoProfile","-Command",shot],capture_output=True,text=True,timeout=9)
 assert result.returncode==0,result.stderr[-400:]

def main():
 check=subprocess.run(["tasklist","/FI","IMAGENAME eq xmplay.exe"],capture_output=True,text=True).stdout.lower()
 assert "xmplay.exe" not in check,"Existing XMPlay running: do not interfere"
 with tempfile.TemporaryDirectory(prefix="AST-XMPlay-ZIP-Priority-",dir=r"D:\Temp") as folder:
  qa=Path(folder); player=qa/"XMPlay";shutil.copytree(XMPLAY_SOURCE,player)
  shutil.copy2(PLUGIN,player/"xmp-archive-stream.dll")
  for f in ("xmplay.pls","xmplay.library","xmplay.library~"):(player/f).unlink(missing_ok=True)
  for n in ("Local","Roaming","Temp"):(qa/n).mkdir()
  env=os.environ.copy()
  env.update(LOCALAPPDATA=str(qa/"Local"),APPDATA=str(qa/"Roaming"),TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),ARCHIVE_STREAM_ADAPTER_DEBUG="1")
  source=qa/"AST_ZIP_NATIVE.mp3"
  shutil.copy2(AUDIO,source)
  archive=qa/"native.zip"
  compression = zipfile.ZIP_DEFLATED if os.environ.get("AST_XMPLAY_ZIP_DEFLATE")=="1" else zipfile.ZIP_STORED
  with zipfile.ZipFile(archive,"w",compression) as out:
   out.write(source,source.name)
  proc=None
  try:
   proc=subprocess.Popen([str(player/"xmplay.exe"),"-play",str(archive)],cwd=player,env=env)
   window=wait_for(lambda:player_window(proc.pid),25,"XMPlay main")
   time.sleep(2)
   log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_xmplay.log"
   trace=log.read_text(encoding="utf-8-sig",errors="replace") if log.is_file() else ""
   print("XMPLAY_ZIP_PRIOR_BASELINE","init",trace.count("PLUGIN_INIT"),"list",trace.count("LIST_OK"),"stream",trace.count("STREAM_OK"),flush=True)
   if os.environ.get("AST_XMPLAY_ZIP_INSPECT")=="1":
    main_window=Desktop(backend="win32").window(handle=window)
    main_window.set_focus()
    send_keys("{F9}")
    time.sleep(1.4)
    print("XMPLAY_ZIP_PRIOR_OPTIONS_WINDOWS",windows(proc.pid),flush=True)
    if os.environ.get("AST_XMPLAY_DECODER_INSPECT")=="1":
     dialog=next((x for x in windows(proc.pid) if x[2]=="Options"),None)
     assert dialog is not None
     win32gui.SetWindowPos(dialog[0],win32con.HWND_TOPMOST,25,25,0,0,win32con.SWP_NOSIZE|win32con.SWP_SHOWWINDOW)
     l,t,r,b=win32gui.GetWindowRect(dialog[0])
     mouse.click(coords=(l+55,t+178))
     time.sleep(.8)
     print("XMPLAY_ZIP_DECODER_MODULE_PAGE_SELECTED",flush=True)
     if os.environ.get("AST_XMPLAY_ZIP_SELECT_ARCHIVE_PLUGIN")=="1":
      mouse.click(coords=(l+230,t+198))
      time.sleep(.5)
      print("XMPLAY_ZIP_ARCHIVE_PLUGIN_SELECTED",flush=True)
      def inspect_child(hwnd,unused):
       cr=win32gui.GetWindowRect(hwnd)
       if cr[1]>=t+290:
        print("XMPLAY_ZIP_ARCHIVE_PRIORITY_CONTROL",win32gui.GetClassName(hwnd),win32gui.GetWindowText(hwnd)[:70],win32gui.IsWindowEnabled(hwnd),cr,flush=True)
      win32gui.EnumChildWindows(dialog[0],inspect_child,None)
      if os.environ.get("AST_XMPLAY_ZIP_SET_PRIORITY")=="1":
       mouse.click(coords=(l+352,t+345))
       send_keys("zip")
       time.sleep(.35)
       print("XMPLAY_ZIP_PRIORITY_TYPED",flush=True)
    for hwnd,cls,title,rect in windows(proc.pid):
     if hwnd==window:continue
     try:
      w=Desktop(backend="win32").window(handle=hwnd)
      print("XMPLAY_ZIP_PRIOR_OPTION_CONTROLS",title,
            [(x.class_name(),x.window_text()[:50],x.rectangle()) for x in w.descendants() if x.window_text()][:65],flush=True)
     except Exception as e:
      print("XMPLAY_ZIP_PRIOR_OPTION_INSPECTION_ERROR",type(e).__name__,flush=True)
    screenshot(Path(r"D:\Temp\ast-xmplay-zip-options.jpg"))
    print("XMPLAY_ZIP_OPTIONS_SCREENSHOT_PASS",flush=True)
    return
   # XMPlay 4.1 contains its own ZIP archive decoder, ahead of XMPARC.
   # Verify real host playback and GUI seeking independently of plugin logs.
   win32gui.SetWindowPos(window,win32con.HWND_TOPMOST,0,0,0,0,win32con.SWP_NOMOVE|win32con.SWP_NOSIZE|win32con.SWP_SHOWWINDOW)
   l,t,r,b=win32gui.GetWindowRect(window)
   mouse.move(coords=(l+30,t+180))
   time.sleep(5)
   first=progress(window,qa)
   time.sleep(4)
   second=progress(window,qa)
   assert 0<first<second<r-l-40,(first,second,r-l)
   mouse.click(coords=(l+int((r-l)*.73),t+56))
   mouse.move(coords=(l+30,t+180))
   time.sleep(3)
   jumped=progress(window,qa)
   time.sleep(4)
   resumed=progress(window,qa)
   assert jumped>second+90 and resumed>=jumped+3,(first,second,jumped,resumed)
   assert not log.is_file() or ("LIST_OK" not in log.read_text(encoding="utf-8-sig",errors="replace") and "STREAM_OK" not in log.read_text(encoding="utf-8-sig",errors="replace"))
   print("XMPLAY_REAL_BUILTIN_ZIP_MP3_GUI_SEEK_PASS",f"pixels={first}->{second}->{jumped}->{resumed}","http=0",flush=True)
  finally:
   if proc and proc.poll() is None:
    subprocess.run(["taskkill","/PID",str(proc.pid),"/T","/F"],capture_output=True,timeout=10)
    proc.wait(timeout=5)
    time.sleep(.35)

if __name__=="__main__":main()
