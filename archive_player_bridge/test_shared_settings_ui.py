"""Interactively verify shared settings without modifying the user's registry."""
import os,sys,tempfile,subprocess,time,winreg
from pathlib import Path
sdk=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
dll=os.add_dll_directory(str(sdk/"pywin32_system32"))
sys.path[:0]=[str(sdk),str(sdk/"win32"),str(sdk/"win32"/"lib"),str(sdk/"Pythonwin")]
from pywinauto import Application
import win32gui, win32con, win32process
def snapshot():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r"Software\ArchiveStreamPlayer\Adapters") as key:
            return [winreg.EnumValue(key,i) for i in range(winreg.QueryInfoKey(key)[1])]
    except FileNotFoundError:return []
before=snapshot()
with tempfile.TemporaryDirectory(prefix="asp-ui-",dir=r"D:\Temp") as tmp:
    local=Path(tmp)/"profile"
    cache=local/"ArchiveStreamPlayer"/"ManagedCache"
    logs=local/"ArchiveStreamPlayer"/"Logs"
    cache.mkdir(parents=True);logs.mkdir(parents=True)
    testCache=cache/"0000000000000001_123.mp3"
    keepCache=cache/"user_notes.txt"
    testCache.write_bytes(b"cached");keepCache.write_bytes(b"keep")
    testLog=logs/"archive_stream_bridge.log"
    keepLog=logs/"user_notes.txt"
    testLog.write_text("test",encoding="utf-8");keepLog.write_text("keep",encoding="utf-8")
    env=os.environ.copy();env["LOCALAPPDATA"]=str(local)
    exe=Path(__file__).with_name("configure-adapters.ps1")
    p=subprocess.Popen(["powershell.exe","-NoProfile","-ExecutionPolicy","Bypass","-File",str(exe)],env=env)
    try:
        app=Application(backend="win32").connect(process=p.pid,timeout=15)
        dlg=app.window(title="Archive Stream Player - Settings (all players)")
        dlg.wait("visible",timeout=15)
        def content():
            return "\n".join(x.window_text() for x in dlg.descendants())
        for label in ("Memory threshold","Maximum managed cache","Prepare remaining tracks",
            "Prompt when a saved password","Offer to save passwords","track-title format",
            "Enable diagnostic logging","Clear managed cache","Clear all logs"):
            assert label.lower() in content().lower(),("MISSING_SETTINGS_CONTROL",label)
        print("PASS_SETTINGS_GUI_CONTROLS",flush=True)
        combos=[x for x in dlg.descendants() if "COMBOBOX" in x.class_name().upper()]
        assert len(combos)==1,[(w.class_name(),w.window_text()) for w in dlg.descendants()]
        combos[0].select(4)
        time.sleep(.2)
        assert "Example title: 7Z: 1 - Sample.mp3" in content(),content()[-800:]
        print("PASS_SETTINGS_TITLE_LIVE_PREVIEW",flush=True)
        for button,confirmation,target,retained in (
            ("Clear managed cache","Clear managed cache",testCache,keepCache),
            ("Clear all logs","Clear Archive Stream logs",testLog,keepLog)):
            targetButton=next(w for w in dlg.descendants() if w.window_text()==button)
            if button == "Clear all logs":
                win32gui.PostMessage(targetButton.handle,win32con.BM_CLICK,0,0)
            else:
                targetButton.click_input()
            popup=app.window(title=confirmation)
            try:popup.wait("visible",timeout=8)
            except Exception:
                from pywinauto import Desktop
                print("DEBUG_WINDOWS",[(w.window_text(),w.class_name()) for w in Desktop(backend="win32").windows() if w.is_visible()],flush=True)
                raise
            hwnd=win32gui.FindWindow(None,confirmation)
            assert hwnd,(confirmation,"confirmation window missing")
            win32gui.SendMessage(hwnd,win32con.WM_COMMAND,6,0)
            deadline=time.monotonic()+8
            while target.exists() and time.monotonic()<deadline:
                time.sleep(.1)
            assert not target.exists() and retained.exists(),button
            # Close all result dialogs created by the test script; otherwise
            # a modal notice can block subsequent buttons in the main form.
            def close_result(hwnd,_):
                if win32gui.GetWindowText(hwnd) != "Archive Stream":
                    return
                _,owner=win32process.GetWindowThreadProcessId(hwnd)
                if owner==p.pid:
                    win32gui.PostMessage(hwnd,win32con.WM_CLOSE,0,0)
            win32gui.EnumWindows(close_result,None)
            time.sleep(.4)
            print("PASS_SETTINGS_UI_SAFE_ACTION",button,flush=True)
        next(w for w in dlg.descendants() if w.window_text()=="Cancel").click_input()
        p.wait(timeout=12)
        assert p.returncode==0 and snapshot()==before,"Cancel modified shared user settings"
        print("PASS_SETTINGS_UI_CANCEL_NO_REGISTRY_CHANGES",flush=True)
    finally:
        if p.poll() is None:p.terminate();p.wait(timeout=10)
print("ALL_SHARED_SETTINGS_GUI_TESTS_PASS",flush=True)
