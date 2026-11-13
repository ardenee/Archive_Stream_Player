"""Real foobar2000 Win32/x64 password-dialog acceptance using synthetic AES 7z.
Call with x64|x86 and optionally --submit-password. The default cancels.
The test declines password storage, checks that decoding follows the expected
success/failure route, and preserves the user password-list contents.
"""
import os,sys,time,tempfile,subprocess,struct,wave,secrets
from pathlib import Path
sdk=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
_dll=os.add_dll_directory(str(sdk/"pywin32_system32"))
sys.path[:0]=[str(sdk),str(sdk/"win32"),str(sdk/"win32"/"lib"),str(sdk/"Pythonwin")]
from pywinauto import Application,Desktop
import win32gui,win32process,win32con
assert len(sys.argv) in (2,3) and sys.argv[1] in ("x64","x86")
submit=(len(sys.argv)==3 and sys.argv[2]=="--submit-password")
arch=sys.argv[1]
runtime=Path(r"D:\Temp\ArchiveStreamFoobar64Runtime" if arch=="x64" else r"D:\Temp\ArchiveStreamFoobar32Runtime")
exe=runtime/"portable"/"foobar2000.exe"
seven=Path(r"C:\Program Files\7-Zip\7z.exe")
assert exe.is_file() and seven.is_file()
active=subprocess.run(["powershell.exe","-NoProfile","-Command","@(Get-Process foobar2000 -ErrorAction SilentlyContinue).Count"],capture_output=True,text=True).stdout.strip()
assert active in ("","0"), "Another foobar2000 instance is running; refusing to alter it"
with tempfile.TemporaryDirectory(prefix="asp-pwd-ui-",dir=r"D:\Temp") as temp:
    root=Path(temp)
    key="AspPasswordPrompt"+secrets.token_hex(12)
    wav=root/"PromptFixture.wav"
    with wave.open(str(wav),"wb") as w:
        w.setparams((1,2,44100,0,"NONE","not compressed"))
        w.writeframes(struct.pack("<h",1000)*44100*2)
    archive=root/"PromptFixture.7z"
    result=subprocess.run([str(seven),"a","-t7z","-m0=LZMA2","-mhe=on","-ms=off","-p"+key,
        str(archive),str(wav)],capture_output=True,text=True,timeout=20)
    assert result.returncode==0 and archive.exists(),result.stdout[-500:]
    proc=subprocess.Popen([str(exe)],cwd=str(exe.parent))
    # Do not modify user credentials, even on a failed or interrupted test.
    import winreg
    password_file=None
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r"Software\ArchiveStreamPlayer\Adapters") as reg:
            password_file=Path(winreg.QueryValueEx(reg,"PasswordList")[0])
    except (OSError,ValueError):pass
    original_bytes=password_file.read_bytes() if password_file and password_file.is_file() else None
    app=None
    try:
        time.sleep(1)
        app=Application(backend="win32").connect(process=proc.pid,timeout=10)
        for initial in Desktop(backend="win32").windows():
            if initial.is_visible() and initial.process_id()==proc.pid and initial.window_text()=="foobar2000":
                print("FOOBAR_STARTUP_MODAL",[(child.window_text(),child.class_name())
                    for child in initial.descendants() if child.window_text()],flush=True)
                normally=next((child for child in initial.descendants()
                    if child.window_text()=="Start foobar2000 normally"),None)
                if normally:
                    normally.click()
                    print("FOOBAR_RECOVERY_START_NORMALLY",flush=True)
        subprocess.run([str(exe),"/command:Archive Stream: Verify archive decoding..."],
            cwd=str(exe.parent),timeout=10,check=True)
        picker=app.window(title="Archive Stream - Verify Audio Decoding")
        try:
            picker.wait("visible",timeout=12)
        except Exception:
            print("STARTUP_VISIBLE_WINDOWS",
                [(w.window_text(),w.class_name()) for w in Desktop(backend="win32").windows()
                if w.is_visible() and w.process_id()==proc.pid],flush=True)
            raise
        picker.child_window(control_id=1148,class_name="Edit").set_edit_text(str(archive))
        submit_button=picker.child_window(control_id=1,class_name="Button").wrapper_object()
        win32gui.PostMessage(submit_button.handle,win32con.BM_CLICK,0,0)
        print("FOOBAR_ENCRYPTED_FIXTURE_SUBMITTED",arch,flush=True)
        match=[]
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            allwindows=[w for w in Desktop(backend="win32").windows() if w.is_visible() and w.process_id()==proc.pid]
            match=[w for w in allwindows if ("password" in w.window_text().lower() or "security" in w.window_text().lower())]
            if match:break
            time.sleep(.3)
        print("FOOBAR_WINDOW_TITLES",[(w.window_text(),w.class_name()) for w in allwindows],flush=True)
        assert match,"PASSWORD_UI_NOT_FOUND"
        window=match[0]
        print("FOOBAR_PASSWORD_WINDOW",repr(window.window_text()),window.class_name(),flush=True)
        print("PASSWORD_CONTROLS",[(w.window_text()[:55],w.class_name()) for w in window.descendants()][:40],flush=True)
        if not submit:
            cancel=next((c for c in window.descendants() if c.window_text().strip().lower() == "cancel"),None)
            assert cancel,"No cancel control in native password prompt"
            cancel.click()
            print("FOOBAR_PASSWORD_PROMPT_VISIBLE_AND_CANCELLED",flush=True)
            resultWindow=app.window(title="Archive Stream - Verify Audio")
            resultWindow.wait("visible",timeout=22)
            result=resultWindow.child_window(control_id=1077,class_name="Edit").window_text()
            assert "1 decoded, 0 failed" not in result and ("SZ_ERROR" in result or "failed" in result.lower()),repr(result[-1100:])
            print("FOOBAR_CANCEL_NO_DECODE_PASS",arch,flush=True)
            resultWindow.close()
        else:
            fields=[w for w in window.descendants() if w.class_name()=="Edit"]
            assert len(fields)>=2,[(x.class_name(),x.window_text()) for x in fields]
            fields[-1].set_edit_text(key)
            next(w for w in window.descendants() if w.window_text()=="OK").click()
            save=None
            end=time.monotonic()+10
            while time.monotonic()<end:
                matches=[]
                def on_window(hwnd,arg):
                    try:
                        _,pid=win32process.GetWindowThreadProcessId(hwnd)
                        if pid==proc.pid and win32gui.IsWindowVisible(hwnd) and win32gui.GetWindowText(hwnd)=="Archive Stream Player":
                            matches.append(hwnd)
                    except Exception:pass
                win32gui.EnumWindows(on_window,None)
                if matches:
                    save=Desktop(backend="win32").window(handle=matches[0])
                    break
                time.sleep(.2)
            if save is not None:
                no=next((w for w in save.descendants() if w.window_text() in ("No","&No")),None)
                assert no,"Password save consent dialog missing No"
                no.click()
                print("FOOBAR_PASSWORD_SAVE_DECLINED",arch,flush=True)
            popup=app.window(title="Archive Stream - Verify Audio")
            popup.wait("visible",timeout=24)
            result=popup.child_window(control_id=1077,class_name="Edit").window_text()
            assert "1 decoded, 0 failed" in result,repr(result[-1100:])
            print("FOOBAR_PASSWORD_PROMPT_DECODER_PASS",arch,flush=True)
            try:popup.close()
            except Exception:pass
    finally:
        if proc.poll() is None:
            try:app.window(title_re="foobar2000 v2.*").close()
            except Exception:pass
            try:proc.wait(timeout=7)
            except subprocess.TimeoutExpired:proc.terminate();proc.wait(timeout=7)
        if password_file is not None:
            if original_bytes is not None:
                assert password_file.read_bytes()==original_bytes,"Saved password list was modified by test"
            else:
                assert not password_file.exists(),"A password list was created without consent"
        print("FOOBAR_PASSWORD_LIST_UNCHANGED",flush=True)
        print("FOOBAR_EXIT",proc.returncode,flush=True)
