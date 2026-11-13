"""Native AIMP COM-plugin password dialog regression, Win32 / x64.
Automates the real Windows credential prompt using synthetic encrypted 7z WAVs.
Declines saving and checks the existing candidate-list file is unmodified.
"""
import os,secrets,struct,subprocess,sys,tempfile,time,wave,winreg
from pathlib import Path
sdk=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
_dll=os.add_dll_directory(str(sdk/"pywin32_system32"))
sys.path[:0]=[str(sdk),str(sdk/"win32"),str(sdk/"win32"/"lib"),str(sdk/"Pythonwin")]
from pywinauto import Desktop
import win32gui,win32process

assert len(sys.argv)==2 and sys.argv[1] in ("Win32","x64")
arch=sys.argv[1]
ROOT=Path(__file__).resolve().parent
folder=ROOT/("Release" if arch=="Win32" else "x64/Release")
HOST=folder/"aimp_adapter_test.exe"
PLUGIN=folder/"aimp_archive_stream.dll"
SEVEN=Path(r"C:\Program Files\7-Zip\7z.exe")
assert all(p.is_file() for p in (HOST,PLUGIN,SEVEN))

def windows_for(pid):
    handles=[]
    def cb(hwnd,_):
        try:
            _,owner=win32process.GetWindowThreadProcessId(hwnd)
            if owner==pid and win32gui.IsWindowVisible(hwnd):
                handles.append((hwnd,win32gui.GetWindowText(hwnd)))
        except Exception:pass
    win32gui.EnumWindows(cb,None)
    return handles

password_path=None
try:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r"Software\ArchiveStreamPlayer\Adapters") as key:
        password_path=Path(winreg.QueryValueEx(key,"PasswordList")[0])
except (OSError,ValueError):pass
saved_before=password_path.read_bytes() if password_path and password_path.is_file() else None
with tempfile.TemporaryDirectory(prefix="asp-aimp-credui-",dir=r"D:\Temp") as temp:
    root=Path(temp)
    profile=root/"profile"
    profile.mkdir()
    password="AspAimpPassword"+secrets.token_hex(12)
    wav=root/"AimpPrompt.wav"
    with wave.open(str(wav),"wb") as writer:
        writer.setparams((1,2,44100,0,"NONE","not compressed"))
        writer.writeframes(struct.pack("<h",1249)*44100*2)
    archive=root/"AimpPrompt.7z"
    run=subprocess.run([str(SEVEN),"a","-t7z","-m0=LZMA2","-mhe=on",
        "-ms=off","-p"+password,str(archive),str(wav)],
        capture_output=True,text=True,timeout=25)
    assert run.returncode==0 and archive.is_file(),run.stderr[-500:]
    env=os.environ.copy()
    env["LOCALAPPDATA"]=str(profile)
    proc=subprocess.Popen([str(HOST),str(PLUGIN),str(archive)],
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,errors="replace",env=env)
    events=[]
    try:
        until=time.monotonic()+42
        while time.monotonic()<until and proc.poll() is None:
            for hwnd,title in windows_for(proc.pid):
                if title not in ("Archive Stream - AIMP Password","Archive Stream - AIMP"):
                    continue
                dlg=Desktop(backend="win32").window(handle=hwnd)
                if title=="Archive Stream - AIMP Password":
                    fields=[child for child in dlg.descendants() if child.class_name()=="Edit"]
                    assert fields,"Windows credential dialog missing password input"
                    fields[-1].set_edit_text(password)
                    next(child for child in dlg.descendants() if child.window_text()=="OK").click()
                    events.append("PASSWORD_ENTERED")
                elif title=="Archive Stream - AIMP":
                    no=next((child for child in dlg.descendants() if child.window_text() in ("No","&No")),None)
                    assert no,"Password consent dialog missing No action"
                    no.click()
                    events.append("SAVE_DECLINED")
            time.sleep(.1)
        if proc.poll() is None:
            proc.terminate()
        output,error=proc.communicate(timeout=8)
        assert proc.returncode==0 and "PASS AIMP plugin contract expanded=1 streamed=1" in output, (
            arch,proc.returncode,events,output[-1200:],error[-750:])
        assert "PASSWORD_ENTERED" in events,(arch,events,output)
        if password_path is not None:
            if saved_before is not None:
                assert password_path.read_bytes()==saved_before,"Saved password list modified without consent"
            else:
                assert not password_path.exists(),"Saved password list unexpectedly created"
        print("PASS_AIMP_NATIVE_PASSWORD_PROMPT",arch,"events",events,
            "expanded=1","read_seek=PASS","password_file_unchanged",flush=True)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=7)
