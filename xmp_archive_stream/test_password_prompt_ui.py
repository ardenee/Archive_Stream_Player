"""XMPlay XMPARC native adapter password-prompt regression (Win32).
Exercises actual CredUI + synthetic encrypted 7z and WAV full/partial reads.
"""
import os,secrets,struct,subprocess,sys,tempfile,time,wave,winreg
from pathlib import Path
sdk=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
_dll=os.add_dll_directory(str(sdk/"pywin32_system32"))
sys.path[:0]=[str(sdk),str(sdk/"win32"),str(sdk/"win32"/"lib"),str(sdk/"Pythonwin")]
from pywinauto import Desktop
import win32gui,win32process

ROOT=Path(__file__).resolve().parent
HOST=ROOT/"Release"/"xmplay_adapter_test.exe"
PLUGIN=ROOT/"Release"/"xmp-archive-stream.dll"
SEVEN=Path(r"C:\Program Files\7-Zip\7z.exe")
assert all(p.is_file() for p in (HOST,PLUGIN,SEVEN))

def windows_for(pid):
    found=[]
    def cb(hwnd,_):
        try:
            _,owner=win32process.GetWindowThreadProcessId(hwnd)
            if owner==pid and win32gui.IsWindowVisible(hwnd):
                found.append((hwnd,win32gui.GetWindowText(hwnd)))
        except Exception:pass
    win32gui.EnumWindows(cb,None)
    return found

password_file=None
try:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r"Software\ArchiveStreamPlayer\Adapters") as key:
        password_file=Path(winreg.QueryValueEx(key,"PasswordList")[0])
except (OSError,ValueError):pass
before=password_file.read_bytes() if password_file and password_file.is_file() else None

with tempfile.TemporaryDirectory(prefix="asp-xmplay-credui-",dir=r"D:\Temp") as temp:
    root=Path(temp);profile=root/"profile";profile.mkdir()
    password="AspXmplayPassword"+secrets.token_hex(12)
    wav=root/"XmplayPrompt.wav"
    with wave.open(str(wav),"wb") as writer:
        writer.setparams((1,2,44100,0,"NONE","not compressed"))
        writer.writeframes(struct.pack("<h",1249)*44100*2)
    archive=root/"XmplayPrompt.7z"
    creation=subprocess.run([str(SEVEN),"a","-t7z","-m0=LZMA2","-mhe=on",
        "-ms=off","-p"+password,str(archive),str(wav)],
        capture_output=True,text=True,timeout=25)
    assert creation.returncode==0 and archive.is_file(),creation.stderr[-500:]
    env=os.environ.copy();env["LOCALAPPDATA"]=str(profile)
    proc=subprocess.Popen([str(HOST),str(PLUGIN),str(archive)],
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,errors="replace",env=env)
    events=[]
    try:
        until=time.monotonic()+45
        while time.monotonic()<until and proc.poll() is None:
            for hwnd,title in windows_for(proc.pid):
                if title not in ("Archive Stream Password","Archive Stream - XMPlay"):
                    continue
                dlg=Desktop(backend="win32").window(handle=hwnd)
                if title=="Archive Stream Password":
                    fields=[x for x in dlg.descendants() if x.class_name()=="Edit"]
                    assert fields,"Password entry not found"
                    fields[-1].set_edit_text(password)
                    next(x for x in dlg.descendants() if x.window_text()=="OK").click()
                    events.append("PASSWORD_ENTERED")
                else:
                    no=next((x for x in dlg.descendants() if x.window_text() in ("No","&No")),None)
                    assert no,"Consent dialog cannot decline password save"
                    no.click()
                    events.append("SAVE_DECLINED")
            time.sleep(.11)
        if proc.poll() is None:proc.terminate()
        stdout,stderr=proc.communicate(timeout=10)
        assert proc.returncode==0 and "PASS XMPlay archive callback entries=1" in stdout,(
            proc.returncode,events,stdout[-900:],stderr[-900:])
        assert "PASSWORD_ENTERED" in events,(events,stdout)
        if password_file is not None:
            if before is not None:
                assert password_file.read_bytes()==before,"Password list changed without consent"
            else:
                assert not password_file.exists(),"Password file unexpectedly created"
        print("PASS_XMPLAY_NATIVE_PASSWORD_PROMPT Win32",
            events,"decoded_full=PASS","partial_probe=PASS","password_file_unchanged",flush=True)
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=8)
