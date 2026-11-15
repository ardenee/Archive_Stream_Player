"""Real VLC 3 x64 native Save=Yes password persistence + rejection QA.

Uses libVLC from the disposable DEV test installation. A Windows process-local
RegOverridePredefKey redirects HKCU to a unique disposable QA subtree INSIDE
THIS PYTHON PROCESS ONLY. Existing ArchiveStreamPlayer registry settings and
the user's configured password list are never edited. This is not a mock:
the production native access DLL opens, prompts, decodes, and attempts the save.
Physical speaker audio is never used (VLC afile output).
"""
import contextlib,ctypes,hashlib,os,secrets,shutil,struct,subprocess,sys,tempfile,threading,time,wave,winreg
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import test_real_vlc_password as ui
import test_real_vlc_seek as lib
from pywinauto import Desktop
import win32gui,win32process

ROOT=Path(__file__).resolve().parents[1]
VLC=lib.VLC
MODULE=lib.MODULE
RATE=44100
ADVAPI=ctypes.WinDLL("Advapi32.dll",use_last_error=True)
REG=ADVAPI.RegOverridePredefKey
REG.argtypes=[ctypes.c_void_p,ctypes.c_void_p]
REG.restype=ctypes.c_long

@contextlib.contextmanager
def private_registry(qa,password_path):
    unique="VLCPasswordSaveQA_"+secrets.token_hex(10)
    root_name=r"Software\ArchiveStreamPlayer"+"\\"+unique
    root=winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER,root_name,0,winreg.KEY_ALL_ACCESS)
    settings=winreg.CreateKeyEx(root,r"Software\ArchiveStreamPlayer\Adapters",0,winreg.KEY_ALL_ACCESS)
    for name,val in (("PasswordList",str(password_path)),("MemoryMB",64),
                     ("AskToSavePassword",1),("PromptPasswords",1),("DebugLogging",1)):
        winreg.SetValueEx(settings,name,0,winreg.REG_SZ if isinstance(val,str) else winreg.REG_DWORD,val)
    engaged=False
    try:
        assert winreg.QueryInfoKey(root)[0]>=1,"QA registry subtree missing"
        ret=REG(ctypes.c_void_p(winreg.HKEY_CURRENT_USER),ctypes.c_void_p(root.handle))
        if ret:raise RuntimeError(f"Unable to isolate HKCU, Win32={ret} handle={root.handle}")
        engaged=True
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r"Software\ArchiveStreamPlayer\Adapters") as mapped:
            actual=winreg.QueryValueEx(mapped,"PasswordList")[0]
            assert actual==str(password_path),("Private registry view mismatch",actual)
        print("VLC_PASSWORD_SAVE_PROCESS_LOCAL_REGISTRY_ISOLATED_PASS",flush=True)
        yield
    finally:
        if engaged:
            ret=REG(ctypes.c_void_p(winreg.HKEY_CURRENT_USER),None)
            if ret:raise RuntimeError(f"Failed to remove HKCU process override {ret}")
        settings.Close()
        root.Close()
        for suffix in (r"\Software\ArchiveStreamPlayer\Adapters",
                       r"\Software\ArchiveStreamPlayer",r"\Software",""):
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER,root_name+suffix)
        print("VLC_PASSWORD_SAVE_ORIGINAL_HKCU_VIEW_RESTORED_PASS",flush=True)

def interactive_dialogs(secret, mode, stop, events):
    """Operate only password dialogs owned by this single isolated QA process."""
    handled=set()
    wrong_submitted=False
    while not stop.is_set():
        windows=[]
        def visit(hwnd,unused):
            try:
                if win32gui.IsWindowVisible(hwnd) and win32process.GetWindowThreadProcessId(hwnd)[1]==os.getpid():
                    windows.append((hwnd,win32gui.GetWindowText(hwnd)))
            except Exception:pass
        win32gui.EnumWindows(visit,None)
        handled.intersection_update(h for h,_ in windows)
        for hwnd,title in windows:
            if hwnd in handled or title not in ("Archive Stream Player - VLC Password","Archive Stream Player - VLC"):
                continue
            try:
                window=Desktop(backend="win32").window(handle=hwnd)
                if title.endswith("Password"):
                    if mode=="cancel" or (mode=="wrong" and wrong_submitted):
                        next(x for x in window.descendants() if x.window_text()=="Cancel").click()
                        events.append("PASSWORD_CANCELLED")
                    else:
                        edits=[x for x in window.descendants() if x.class_name()=="Edit"]
                        if len(edits)<2:continue
                        edits[0].set_edit_text("ArchiveStreamQA")
                        edits[-1].set_edit_text("wrong-"+secrets.token_hex(9) if mode=="wrong" else secret)
                        next(x for x in window.descendants() if x.window_text()=="OK").click()
                        wrong_submitted=mode=="wrong"
                        events.append("PASSWORD_WRONG_ENTERED" if wrong_submitted else "PASSWORD_ENTERED")
                else:
                    button="Yes" if mode in ("accept","wrong") else "No"
                    next(x for x in window.descendants() if x.window_text().replace("&","")==button).click()
                    events.append("SAVE_AGREED" if button=="Yes" else "SAVE_DECLINED")
                time.sleep(.15)
                if not win32gui.IsWindowVisible(hwnd):
                    handled.add(hwnd)
            except Exception:
                continue
        time.sleep(.09)

def case(kind,mode):
    assert kind in ("7z","7z-hidden","rar","rar-headers")
    assert mode in ("accept","cancel","wrong")
    assert ctypes.sizeof(ctypes.c_void_p)==8,"VLC production plugin needs 64-bit Python"
    assert all(x.is_file() for x in (MODULE,VLC/"vlc.exe",ui.SEVENZIP))
    if kind.startswith("rar"):assert ui.RAR.is_file()
    configured,before=ui.user_password_file()
    with tempfile.TemporaryDirectory(prefix="AST-VLC-NativeSaveQA-",dir=r"D:\Temp") as scratch:
        qa=Path(scratch)
        for d in ("Roaming","Local","Temp"):(qa/d).mkdir()
        local=qa/"Local"/"WinampArchiveStream"
        local.mkdir(parents=True)
        password_file=local/"keeps.txt"
        password_file.write_bytes(b"")
        secret="VLC-SaveQA-"+secrets.token_hex(15)
        source=qa/"sample.wav"
        pcm=struct.pack("<h",3900)*RATE*3
        with wave.open(str(source),"wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(RATE)
            output.writeframes(pcm)
        archive=qa/("encrypted."+("7z" if kind.startswith("7z") else "rar"))
        if kind.startswith("7z"):
            command=[str(ui.SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off",
                     "-mhe=on" if kind=="7z-hidden" else "-mhe=off",
                     "-p"+secret,str(archive),str(source)]
        else:
            command=[str(ui.RAR),"a","-ma5","-m0","-s-","-ep",
                     ("-hp" if kind=="rar-headers" else "-p")+secret,
                     str(archive),str(source)]
        created=subprocess.run(command,cwd=qa,capture_output=True,text=True,timeout=65)
        assert created.returncode==0 and archive.is_file(),created.stderr[-800:]
        target=VLC/"plugins"/"access"/MODULE.name
        assert target.parent.is_dir()
        shutil.copy2(MODULE,target)
        uri="ast://"+str(archive).encode("utf-8").hex().upper()+"/0/sample.wav"
        out=qa/"rendered.wav"
        args=["-I","dummy","--no-one-instance","--ignore-config","--no-plugins-cache",
              "--no-video","--aout=afile","--audiofile-format=s16","--audiofile-file="+str(out)]
        native=lib.api()
        encoded=(ctypes.c_char_p*len(args))(*[s.encode("utf-8") for s in args])
        old_env={name:os.environ.get(name) for name in ("APPDATA","LOCALAPPDATA","TEMP","TMP","ARCHIVE_STREAM_ADAPTER_DEBUG")}
        os.environ.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
                          TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        stop=threading.Event();events=[]
        watcher=None
        try:
            with private_registry(qa,password_file):
                instance=native["new"](len(args),encoded)
                assert instance,"libvlc_new failed"
                media=None;player=None
                try:
                    media=native["media"](instance,uri.encode("utf-8"))
                    assert media,"VLC could not allocate archive input"
                    player=native["player"](media)
                    assert player,"VLC failed to allocate player"
                    watcher=threading.Thread(target=interactive_dialogs,
                                             args=(secret,mode,stop,events),daemon=True)
                    watcher.start()
                    assert native["play"](player)==0,"VLC failed to request native playback"
                    deadline=time.monotonic()+27
                    states=[]
                    while time.monotonic()<deadline:
                        state=native["state"](player)
                        if not states or state!=states[-1]:states.append(state)
                        if state in (6,7):break
                        time.sleep(.12)
                    else:
                        raise AssertionError("VLC native Save=Yes playback timed out")
                    native["stop"](player)
                    stop.set();watcher.join(timeout=2)
                    log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
                    trace=log.read_text(encoding="utf-8-sig",errors="replace") if log.is_file() else ""
                    values=[x.strip() for x in password_file.read_text(encoding="utf-8-sig").splitlines() if x.strip()]
                    print("VLC_NATIVE_PASSWORD_SAVE_RESULT",kind,mode,"events",events,
                          "states",states,"routes",
                          {tag:trace.count(tag) for tag in ("ROUTE_STREAM","ROUTE_MEMORY","ROUTE_CACHE","ACCESS_OPEN_OK")},
                          "test_list_count",len(values),flush=True)
                    if mode=="accept":
                        assert "PASSWORD_ENTERED" in events and events.count("SAVE_AGREED")==1,events
                        assert state==6,("VLC audio did not finish",states,trace[-1000:])
                        assert values==[secret],"Accepted password not saved exactly once"
                        assert out.is_file(),trace[-1000:]
                        with wave.open(str(out),"rb") as result:
                            audio=result.readframes(result.getnframes())
                        assert audio==pcm,"VLC saved password but playback PCM did not match"
                        assert "ROUTE_CACHE" not in trace,"Encrypted audio unexpectedly extracted to disk"
                        print("VLC_NATIVE_PASSWORD_SAVE_YES_VERIFIED_PCM_AND_SINGLE_ENTRY_PASS",kind,flush=True)
                    else:
                        assert not values,("Rejected password was saved!",kind,mode)
                        assert "ROUTE_STREAM" not in trace and "ROUTE_MEMORY" not in trace,trace[-1200:]
                        if out.is_file():
                            with wave.open(str(out),"rb") as result:
                                assert result.getnframes()==0,"VLC emitted audio after failed authentication"
                        if mode=="wrong":
                            assert "PASSWORD_WRONG_ENTERED" in events and "SAVE_AGREED" in events,events
                        else:
                            assert "PASSWORD_CANCELLED" in events,events
                        print("VLC_NATIVE_PASSWORD_SAVE_REJECTED_NO_LIST_WRITE_PASS",kind,mode,flush=True)
                finally:
                    stop.set()
                    if watcher:watcher.join(timeout=2)
                    if player:
                        native["stop"](player)
                        native["player_release"](player)
                    if media:native["media_release"](media)
                    native["release"](instance)
        finally:
            stop.set()
            if watcher:watcher.join(timeout=2)
            for name,value in old_env.items():
                if value is None:os.environ.pop(name,None)
                else:os.environ[name]=value
        if configured is not None:
            after=hashlib.sha256(configured.read_bytes()).digest() if configured.is_file() else None
            assert after==before,"Existing configured password list changed"
        print("VLC_NATIVE_PASSWORD_SAVE_TEST_FILES_REMOVED_PASS",kind,mode,flush=True)

if __name__=="__main__":
    layouts=("7z","7z-hidden","rar","rar-headers")
    modes=("accept","wrong","cancel")
    cases=[(kind,mode) for kind in layouts for mode in modes]
    chosen=[x for x in cases if len(sys.argv)==1 or x==(sys.argv[1],sys.argv[2])]
    if not chosen:raise SystemExit("Use optional arguments: 7z|7z-hidden|rar|rar-headers accept|wrong|cancel")
    for kind,mode in chosen:
        case(kind,mode)
    print("VLC_NATIVE_PASSWORD_SAVE_REGRESSION_PASS",len(chosen),flush=True)
