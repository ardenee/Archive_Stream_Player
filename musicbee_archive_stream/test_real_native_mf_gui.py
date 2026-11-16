"""DEV-only isolated real MusicBee 3.7 x86 AAC/M4A/WMA GUI-seek acceptance.

Compiles no shipped hooks, uses test-only managed queue probe and production
bass_archive_stream.dll; requires an unlocked interactive Windows desktop.
This checks actual GUI timeline + PCM read/seek, not speaker audibility or
injected midstream fault handling. Cleans the disposable profile.
"""
import os,sys,time,re,shutil,subprocess,tempfile
from pathlib import Path
import ctypes
ROOT=Path(os.environ["AST_MUSICBEE_GUI_ARTIFACT_DIR"])
APP=Path(r"C:\Program Files (x86)\MusicBee")
REPO=Path(__file__).resolve().parents[1]
AUT=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
ZIP=Path(os.environ["AST_NATIVE_ARCHIVE_TEST_PATH"])
BASS_DLL=Path(os.environ.get("AST_GUI_BASS_DLL",str(REPO/"musicbee_archive_stream"/"Release"/"bass_archive_stream.dll")))
RECOVERY_ROUTE=os.environ.get("AST_GUI_EXPECT_RECOVERY_ROUTE")
GUI_SEEK_REQUIRED=os.environ.get("AST_GUI_REQUIRE_SEEK","1")=="1"
EXPECTED_TRACKS=int(os.environ.get("AST_GUI_EXPECT_TRACKS","1"))
EXPECTED_ROUTE=os.environ.get("AST_GUI_EXPECT_ROUTE","STREAM")
assert all(p.is_file() for p in [
    ZIP,ROOT/"MB_ArchiveStream_ProductionProbe.dll",
    BASS_DLL,
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
    shutil.copy2(BASS_DLL,staged/"bass_archive_stream.dll")
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
    process=subprocess.Popen([str(staged/"MusicBee.exe")],cwd=staged,env=env)
    print("MUSICBEE_PRODUCTION_NATIVE_HOST_PID",process.pid,flush=True)
    try:
        deadline=time.monotonic()+85
        clicked=False
        data=""
        gui_clicked=False
        gui_attempts=0
        last_gui_attempt=0
        before_click=0
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
            log=musiclog.read_text(encoding="utf-8",errors="replace") if musiclog.is_file() else ""
            positions=[int(q) for q in re.findall(r"QA_NATIVE_HOST state=Playing position=(\d+)",log)]
            progressed=gui_attempts>0 and max(positions,default=0)>before_click+30000
            ready=len(positions)>=4 and max(positions)-min(positions)>2000
            retry=gui_attempts>0 and gui_attempts<3 and not progressed and time.monotonic()-last_gui_attempt>=2.5
            if GUI_SEEK_REQUIRED and ((gui_attempts==0 and ready) or retry):
                active=next((w for w in windows(process.pid) if w.window_text().endswith("MusicBee") and w.class_name().startswith("WindowsForms10.Window") and w.rectangle().width()>550 and w.rectangle().height()>350),None)
                if not active and gui_attempts==0:
                    print("MUSICBEE_GUI_WINDOW_NOT_FOUND",[(w.window_text(),w.class_name(),w.rectangle().width(),w.rectangle().height()) for w in windows(process.pid)][:12],flush=True)
                if active:
                    active.maximize()
                    rect=active.rectangle()
                    if gui_attempts==0:
                        before_click=positions[-1]
                    active.set_focus()
                    mouse.click(coords=(rect.left+895,rect.bottom-14))
                    gui_attempts+=1
                    last_gui_attempt=time.monotonic()
                    print("MUSICBEE_NATIVE_REAL_GUI_SEEK_CLICKED",before_click,(rect.left+895,rect.bottom-14),"attempt="+str(gui_attempts),flush=True)
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
            if any(x in line for x in ["OPEN_OK","PCM_FIRST","STREAM_VALIDATED","SEEK","CREATE_BEGIN","MANIFEST_READ","RECOVERY"]):
                print("MUSICBEE_BASS_RUNTIME_LOG",line,flush=True)
        assert "NATIVE_BASS_PLUGIN loaded=1" in data, "MusicBee did not load native BASS through production C#"
        assert "NATIVE_MANIFEST_READY mode=AUDIO archive=" + ZIP.suffix.lower() + " tracks=" + str(EXPECTED_TRACKS) + " helper=DLL http=0 cache=shared" in data, "production native route did not select"
        assert "SESSION_READY route=NATIVE tracks=" + str(EXPECTED_TRACKS) in data, "production native path not used"
        assert "QA_NATIVE_QUEUE accepted=1" in data, "host rejected native .ast"
        assert "AST_BASS NATIVE_ENUM_OK route=IN_PROCESS" in native, "In-process enumerator did not execute"
        assert ("AST_BASS OPEN_OK codec=" + os.environ["AST_NATIVE_CODEC"]) in native and ("AST_BASS PCM_FIRST route=" + EXPECTED_ROUTE) in native, "host did not decode expected native PCM route: " + EXPECTED_ROUTE
        playing=[int(p) for p in re.findall(r"QA_NATIVE_HOST state=Playing position=(\d+)",data)]
        assert len(playing)>=2 and max(playing)-min(playing)>1000, "MusicBee playback position did not progress: "+str(playing)
        if GUI_SEEK_REQUIRED:
            assert gui_clicked,"Native GUI seek was not clicked"
            assert max(playing)>before_click+30000,("MusicBee GUI seek did not change timeline",before_click,playing)
            assert "AST_BASS SEEK_OK" in native,"Native BASS decoder did not receive GUI seek"
        if RECOVERY_ROUTE:
            assert "AST_BASS PCM_FIRST route=STREAM" in native, "Fault test did not start in streaming route"
            assert "AST_BASS RECOVERY_START" in native, "Injected compressed-source interruption did not start recovery"
            assert (("AST_BASS RECOVERY_OK route=" + RECOVERY_ROUTE) in native or
                    ("AST_BASS RECOVERY_OK codec=OGG route=" + RECOVERY_ROUTE) in native), "Expected recovery route missing: "+RECOVERY_ROUTE
            if os.environ["AST_NATIVE_CODEC"]!="OGG":
                assert "AST_BASS RECOVERY_PCM_RESUMED" in native, "Native PCM recovery did not resume"
            assert len(playing)>=6 and max(playing)-min(playing)>6000, "Recovery host did not sustain playback"
            print("MUSICBEE_REAL_HOST_MIDSTREAM_RECOVERY_PASS",{"codec":os.environ["AST_NATIVE_CODEC"],"route":RECOVERY_ROUTE,"archive":ZIP.suffix,"gui_seek":GUI_SEEK_REQUIRED,"http":False},flush=True)
        if GUI_SEEK_REQUIRED:
            post=[t for t in playing if t>before_click+30000]
            assert len(post)>=2 and post[-1]>post[0]+900,post
            print("MUSICBEE_NATIVE_REAL_HOST_GUI_SEEK_AND_RESUME_PASS",{"before_ms":before_click,"after_ms":post[:3],"seek_logged":True},flush=True)
            print("MUSICBEE_NATIVE_MF_REAL_HOST_GUI_SEEK_PASS",{"samples":len(playing),"range_ms":max(playing)-min(playing)},flush=True)
    finally:
        if process.poll() is None:
            subprocess.run(["taskkill.exe","/F","/T","/PID",str(process.pid)],capture_output=True,text=True,timeout=12)
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=10)
