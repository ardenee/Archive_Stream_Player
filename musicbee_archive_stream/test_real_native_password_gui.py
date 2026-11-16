"""DEV-only isolated real MusicBee 3.7 x86 AAC/M4A/WMA GUI-seek acceptance.

Uses a test-only BASS DLL to isolate the password list and a managed
queue probe built from production Plugin.cs. Exercises the actual Windows
CredUI dialog in a disposable MusicBee 3.7 host. Correct credentials prove
native queued PCM and playback progression; wrong/cancel fail closed. The
optional GUI seek click is a separate gate, not part of password acceptance.
No speaker audibility or production credential file is touched.
"""
import os,sys,time,re,shutil,subprocess,tempfile
from pathlib import Path
import ctypes
ROOT=Path(os.environ["AST_MUSICBEE_GUI_ARTIFACT_DIR"])
MODE=os.environ.get("AST_GUI_PASSWORD_MODE","accept").lower()
assert MODE in ("accept","save","wrong","cancel")
REQUIRE_SEEK=os.environ.get("AST_GUI_REQUIRE_SEEK")=="1"
APP=Path(r"C:\Program Files (x86)\MusicBee")
REPO=Path(__file__).resolve().parents[1]
AUT=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
ZIP=Path(os.environ["AST_NATIVE_ARCHIVE_TEST_PATH"])
assert all(p.is_file() for p in [
    ZIP,ROOT/"MB_ArchiveStream_ProductionProbe.dll",
    REPO/"musicbee_archive_stream"/"Release"/"bass_archive_stream.dll",
    REPO/"archive_player_bridge"/"configure-adapters.ps1",
])
existing=subprocess.check_output(["powershell.exe","-NoProfile","-Command",
    "(Get-CimInstance Win32_Process | Where-Object {$_.Name -eq 'MusicBee.exe'}).ExecutablePath"],text=True)
assert not existing.strip(),"Existing MusicBee process; do not risk user's active session"
# Do not try to move the mouse through a locked RDP, Winlogon or other
# secure desktop. This is not a codec failure and must never be a PASS.
class _CursorPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

point = _CursorPoint()
user32 = ctypes.windll.user32
if not user32.GetCursorPos(ctypes.byref(point)) or not user32.SetCursorPos(point.x, point.y):
    print("MUSICBEE_REAL_GUI_INPUT_DESKTOP_UNAVAILABLE", flush=True)
    raise SystemExit(77)

os.add_dll_directory(str(AUT/"pywin32_system32"))
sys.path[:0]=[str(AUT),str(AUT/"win32"),str(AUT/"win32"/"lib"),str(AUT/"Pythonwin")]
from pywinauto import Desktop, mouse
def windows(pid):
    try:
        return [w for w in Desktop(backend="win32").windows() if w.process_id()==pid and w.is_visible()]
    except Exception:
        return []
with tempfile.TemporaryDirectory(prefix="ast-musicbee-production-host-",dir=r"D:\Temp") as tmp:
    root=Path(tmp)
    staged=root/"MusicBee"
    shutil.copytree(APP,staged)
    for folder in ("AppData","Library"):
        (staged/folder).mkdir(exist_ok=True)
    original=staged/"Plugins"/"MB_ArchiveStream.dll"
    if original.is_file(): original.unlink()
    shutil.copy2(ROOT/"MB_ArchiveStream_ProductionProbe.dll",staged/"Plugins"/"MB_ArchiveStream.dll")
    shutil.copy2(REPO/"archive_player_bridge"/"configure-adapters.ps1",staged/"Plugins"/"configure-adapters.ps1")
    shutil.copy2(ROOT/"bass_archive_stream.dll",staged/"bass_archive_stream.dll")
    if (staged/"Plugins"/"archive_player_bridge.exe").exists(): (staged/"Plugins"/"archive_player_bridge.exe").unlink()  # only disposable staged copy
    assert not (staged/"Plugins"/"archive_player_bridge.exe").exists(), "Test must not have helper executable"
    env=os.environ.copy()
    for name,folder in [("APPDATA","Roaming"),("LOCALAPPDATA","Local"),("TEMP","Temp"),("TMP","Temp")]:
        dest=root/folder;dest.mkdir(exist_ok=True);env[name]=str(dest)
    basslog=root/"native-bass-runtime.log"
    musiclog=root/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_musicbee.log"
    env["AST_NATIVE_ZIP_PATH"]=str(ZIP)
    env["AST_BASS_ADDON_LOG"]=str(basslog)
    env["ARCHIVE_STREAM_ADAPTER_DEBUG"]="1"
    env["AST_NATIVE_TEST_PASSWORD_LIST"]=str(root/"empty-test-credentials.txt")
    (root/"empty-test-credentials.txt").write_bytes(b"")
    # The isolated BASS DLL is a test build, but no password override is set:
    # the real production CredUI dialog is the only way to provide credentials.
    env.pop("AST_NATIVE_TEST_PASSWORD",None)
    env.pop("AST_NATIVE_TEST_PASSWORD_MODE",None)
    process=subprocess.Popen([str(staged/"MusicBee.exe")],cwd=staged,env=env)
    print("MUSICBEE_PRODUCTION_NATIVE_HOST_PID",process.pid,flush=True)
    try:
        deadline=time.monotonic()+85
        clicked=False
        data=""
        gui_clicked=False
        before_click=0
        prompted=False
        filled=False
        while time.monotonic()<deadline:
            if process.poll() is not None:
                raise AssertionError("MusicBee exited unexpectedly "+str(process.returncode))
            welcome=next((w for w in windows(process.pid) if w.window_text()=="Welcome"),None)
            if welcome and not clicked:
                empty=next((c for c in welcome.descendants() if "create an empty library" in c.window_text()),None)
                proceed=next((c for c in welcome.descendants() if "BUTTON" in c.class_name().upper() and c.window_text()=="Proceed"),None)
                if empty and proceed:
                    rect=empty.rectangle()
                    mouse.click(coords=(rect.left+12,(rect.top+rect.bottom)//2))
                    proceed.click_input()
                    clicked=True
                    print("MUSICBEE_PRODUCTION_ISOLATED_EMPTY_LIBRARY_READY",flush=True)
            credential=next((w for w in windows(process.pid) if "Archive Stream Player - Password Required" in w.window_text()),None)
            if credential and not filled:
                prompted=True
                fields=[c for c in credential.descendants() if c.class_name().lower()=="edit"]
                buttons=[c for c in credential.descendants() if c.class_name().lower()=="button"]
                if MODE=="cancel":
                    cancel=next((c for c in buttons if c.window_text() in ("Cancel","&Cancel")),None)
                    if cancel:
                        cancel.click_input()
                        filled=True
                        print("MUSICBEE_REAL_GUI_NATIVE_PASSWORD_CANCELLED",flush=True)
                elif len(fields)>=2:
                    value=os.environ["AST_GUI_SYNTHETIC_PASSWORD"]
                    if MODE=="wrong":
                        value += "-wrong"
                    fields[-1].set_edit_text(value)
                    if MODE=="save":
                        checkboxes=[c for c in buttons if c.style() & 0x0000000f in (2,3)]
                        if not checkboxes:
                            raise AssertionError("Credential UI does not expose a save-checkbox control")
                        checkboxes[-1].click_input()
                        print("MUSICBEE_REAL_GUI_PASSWORD_SAVE_CHECKED",flush=True)
                    ok=next((c for c in buttons if c.window_text() in ("OK","&OK")),None)
                    if ok:
                        ok.click_input()
                        filled=True
                        print("MUSICBEE_REAL_GUI_NATIVE_PASSWORD_ENTERED",flush=True)
            log=musiclog.read_text(encoding="utf-8",errors="replace") if musiclog.is_file() else ""
            if REQUIRE_SEEK and MODE in ("accept","save") and not gui_clicked:
                positions=[int(q) for q in re.findall(r"QA_NATIVE_HOST state=Playing position=(\d+)",log)]
                if len(positions)>=4 and max(positions)-min(positions)>2000:
                    active=next((w for w in windows(process.pid) if w.window_text().endswith("MusicBee")),None)
                    if active:
                        rect=active.rectangle()
                        before_click=positions[-1]
                        active.set_focus()
                        mouse.click(coords=(rect.left+895,rect.bottom-14))
                        print("MUSICBEE_NATIVE_REAL_GUI_SEEK_CLICKED",before_click,(rect.left+895,rect.bottom-14),flush=True)
                        gui_clicked=True
            if "QA_NATIVE_TRACE_COMPLETE" in log:
                data=log
                break
            time.sleep(.3)
        if not data and musiclog.is_file():
            data=musiclog.read_text(encoding="utf-8",errors="replace")
        native=basslog.read_text(encoding="utf-8",errors="replace") if basslog.is_file() else ""
        for line in data.splitlines():
            if any(x in line for x in ["NATIVE_BASS_PLUGIN","SESSION_READY","NATIVE_MANIFEST","QA_NATIVE","HTTP"]):
                print("MUSICBEE_PRODUCTION_HOST_LOG",line,flush=True)
        for line in native.splitlines():
            if any(x in line for x in ["OPEN_OK","PCM_FIRST","STREAM_VALIDATED","SEEK","CREATE_BEGIN","MANIFEST_READ"]):
                print("MUSICBEE_BASS_RUNTIME_LOG",line,flush=True)
        assert prompted and filled, "Real Windows CredUI password prompt was not shown and handled"
        assert "AST_BASS NATIVE_PASSWORD_PROMPT source=windows_credui" in native, "Missing actual Windows CredUI call"
        credentials=root/"empty-test-credentials.txt"
        passwords=credentials.read_bytes()
        if MODE in ("wrong","cancel"):
            expected="NATIVE_PASSWORD_CANCELLED" if MODE=="cancel" else "NATIVE_PASSWORD_REJECTED"
            assert expected+" bridge=0" in data, "Rejected/cancelled credentials did not fail closed without HTTP"
            assert "SESSION_READY route=NATIVE tracks=0" in data, "Rejected password queued a track"
            assert "NATIVE_MANIFEST_READY" not in data, "Rejected password accepted a native track"
            assert "AST_BASS OPEN_OK" not in native, "Rejected password decoded PCM"
            assert not passwords, "Rejected credentials were persisted"
            print("MUSICBEE_REAL_HOST_PASSWORD_REJECTION_FAIL_CLOSED_PASS",
                {"mode":MODE,"native":True,"helper":False},flush=True)
        else:
            assert "AST_BASS NATIVE_PASSWORD_ENTERED source=windows_credui" in native, "Actual Windows credential input not accepted"
        if MODE in ("wrong","cancel"):
            assert "NATIVE_BASS_PLUGIN loaded=1" in data, "Native BASS plugin not loaded"
            assert not (staged/"Plugins"/"archive_player_bridge.exe").exists()
            print("MUSICBEE_REAL_HOST_ENCRYPTED_PASSWORD_DIALOG_FAIL_CLOSED_PASS",MODE,flush=True)
        else:
            assert "NATIVE_BASS_PLUGIN loaded=1" in data, "MusicBee did not load native BASS through production C#"
            assert "NATIVE_MANIFEST_READY mode=AUDIO archive=" + ZIP.suffix.lower() + " tracks=1 helper=DLL http=0 cache=0" in data, "production native route did not select"
            assert "SESSION_READY route=NATIVE tracks=1" in data, "production native path not used"
            assert "QA_NATIVE_QUEUE accepted=1" in data, "host rejected native .ast"
            assert "AST_BASS NATIVE_ENUM_OK route=IN_PROCESS" in native, "In-process enumerator did not execute"
            assert ("AST_BASS OPEN_OK codec=" + os.environ["AST_NATIVE_CODEC"]) in native and "AST_BASS PCM_FIRST" in native, "host did not decode native PCM entry"
            playing=[int(p) for p in re.findall(r"QA_NATIVE_HOST state=Playing position=(\d+)",data)]
            assert len(playing)>=2 and max(playing)-min(playing)>1000, "MusicBee playback position did not progress: "+str(playing)
            if REQUIRE_SEEK:
                assert gui_clicked,"Native GUI seek was not clicked"
            if REQUIRE_SEEK:
                assert max(playing)>before_click+30000,("MusicBee GUI seek did not change timeline",before_click,playing)
                assert "AST_BASS SEEK_OK" in native,"Native BASS decoder did not receive GUI seek"
                post=[t for t in playing if t>before_click+30000]
                assert len(post)>=2 and post[-1]>post[0]+900,post
                print("MUSICBEE_NATIVE_REAL_HOST_GUI_SEEK_AND_RESUME_PASS",{"before_ms":before_click,"after_ms":post[:3],"seek_logged":True},flush=True)
            if MODE=="save":
                saved=passwords.decode("utf-8-sig").splitlines()
                assert saved==[os.environ["AST_GUI_SYNTHETIC_PASSWORD"]], "Save=Yes did not persist exactly one verified password"
            else:
                assert not passwords, "Credential was saved without user consent"
            print("MUSICBEE_REAL_HOST_ENCRYPTED_PASSWORD_DIALOG_PCM_PASS",
                {"mode":MODE,"playing_samples":len(playing),"range_ms":max(playing)-min(playing)},flush=True)
    finally:
        if process.poll() is None:
            subprocess.run(["taskkill.exe","/F","/T","/PID",str(process.pid)],capture_output=True,text=True,timeout=12)
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=10)
