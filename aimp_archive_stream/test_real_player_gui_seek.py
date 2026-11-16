r"""Real AIMP x86/x64 native-plugin archive playback and GUI seeking.

Staged AIMP copies are fully disposable. Uses AIMP's native seek bar for
the action and its read-only Winamp-compatible position IPC to verify
a jump followed by continued playback. Does not certify audible PCM.
Run in an unlocked interactive DEV desktop:
    py -3.13 -B aimp_archive_stream/test_real_player_gui_seek.py
"""
import ctypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

AUTOMATION=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
os.add_dll_directory(str(AUTOMATION/"pywin32_system32"))
sys.path[:0]=[str(AUTOMATION),str(AUTOMATION/"win32"),
             str(AUTOMATION/"win32"/"lib"),str(AUTOMATION/"Pythonwin")]
from pywinauto import Desktop, mouse
import win32gui
import win32process
import win32con

ROOT=Path(__file__).resolve().parents[1]
INSTALL=Path(r"D:\Temp\ArchiveStreamPlayerAdapters")
AUDIO=Path(r"C:\audiotest\01.MP3File.mp3")
FAULT_ROUTE=os.environ.get("AST_AIMP_GUI_FAULT_ROUTE","")
FAULT_MODE=os.environ.get("AST_AIMP_GUI_FAULT_MODE","read")
assert FAULT_ROUTE in ("","memory","cache") and FAULT_MODE in ("read","seek")
USER32=ctypes.windll.user32
USER32.SendMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_ssize_t]
USER32.SendMessageW.restype=ctypes.c_ssize_t

def poll(check, seconds, label):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        try:
            result=check()
            if result:
                return result
        except Exception:
            pass
        time.sleep(.25)
    raise AssertionError("AIMP real host timeout: "+label)

def aimp_windows(pid):
    frames=[]
    hidden=[]
    def inspect(hwnd, unused):
        try:
            if win32process.GetWindowThreadProcessId(hwnd)[1] != pid:
                return
            cls=win32gui.GetClassName(hwnd)
            if cls=="TAIMPMainForm":
                frames.append(hwnd)
            if cls=="Winamp v1.x":
                hidden.append(hwnd)
        except Exception:
            pass
    win32gui.EnumWindows(inspect,None)
    return frames,hidden

def ipc(window, query):
    return int(USER32.SendMessageW(window,0x400,query,105))

def run(arch):
    src=INSTALL/("aimp-"+arch+"-portable")/"AIMP"
    dll=(Path(os.environ.get("ARCHIVE_AIMP_FAULT_TEST_BIN_DIR",r"D:\Temp\ArchiveStreamAimpFaultTest"))/("Win32" if arch=="x86" else "x64")/"aimp_archive_stream.dll" if FAULT_ROUTE else ROOT/"aimp_archive_stream"/("Release" if arch=="x86" else "x64/Release")/"aimp_archive_stream.dll")
    assert (src/"AIMP.exe").is_file() and dll.is_file() and AUDIO.is_file()
    with tempfile.TemporaryDirectory(prefix="AST-AIMP-Host-"+arch+"-",dir=r"D:\Temp") as folder:
        qa=Path(folder)
        dest=qa/"AIMP"
        shutil.copytree(src,dest)
        target=dest/"Plugins"/"aimp_archive_stream"/"aimp_archive_stream.dll"
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(dll,target)
        for name in ("Local","Roaming","Temp"):
            (qa/name).mkdir()
        archive=qa/"test-archive-native.zip"
        with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as output:
            output.write(AUDIO,"AST_AIMP_GUI_"+arch+".mp3")
        env=os.environ.copy()
        env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
                   TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        if FAULT_ROUTE:
            env.update(AST_AIMP_FAULT_MODE=FAULT_MODE,AST_AIMP_FAULT_MEMORY_MB="50" if FAULT_ROUTE=="memory" else "1")
        proc=None
        try:
            proc=subprocess.Popen([str(dest/"AIMP.exe"),"/ADD_PLAY",str(archive)],
                                  cwd=dest,env=env)
            def host():
                main,legacy=aimp_windows(proc.pid)
                if not main or not legacy:
                    return None
                title=win32gui.GetWindowText(main[0])
                if "AST_AIMP_GUI_"+arch not in title:
                    return None
                return main[0],legacy[0]
            main,legacy=poll(host,45,"native archive expanded and queued")
            for w in Desktop(backend="win32").windows():
                if w.process_id()==proc.pid and w.window_text()=="Skin Settings":
                    w.close()
            def playing():
                return ipc(legacy,0) if ipc(legacy,1)>100 and ipc(legacy,0)>2000 else None
            first=poll(playing,30,"actual MP3 playback with a nonzero position")
            duration=ipc(legacy,1)
            time.sleep(3)
            before=ipc(legacy,0)
            assert before>first+1200,(first,before)
            l,t,r,b=win32gui.GetWindowRect(main)
            if r-l<700 or b-t<450:
                raise AssertionError("Unexpected AIMP native layout for GUI seek")
            # Default Standard layout: the waveform/seek strip is 142px
            # below the top edge; seek target is ~70% of its visible width.
            mouse.click(coords=(l+int((r-l)*.70),t+142))
            time.sleep(3)
            jumped=ipc(legacy,0)
            time.sleep(3)
            after=ipc(legacy,0)
            assert jumped>before+40000 and after>jumped+1300, \
                f"{arch} native GUI seek did not jump and resume: {first}->{before}->{jumped}->{after}"
            log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_aimp.log"
            assert log.is_file(),"Missing opt-in native AIMP route log"
            trace=log.read_text(encoding="utf-8-sig",errors="replace")
            assert "EXPAND_OK" in trace and "ROUTE_STREAM" in trace,trace[-1500:]
            if FAULT_ROUTE:
                assert "STREAM_RECOVER_OK" in trace and ("route="+FAULT_ROUTE) in trace,trace[-1500:]
                assert ("ROUTE_MEMORY" if FAULT_ROUTE=="memory" else "ROUTE_CACHE") in trace,trace[-1500:]
                print("AIMP_REAL_NATIVE_RECOVERY_GUI_SEEK_PASS",arch,"mode="+FAULT_MODE,"route="+FAULT_ROUTE,
                      f"position_ms={first}->{before}->{jumped}->{after}","http=0",flush=True)
            else:
                assert not re.search(r"\bROUTE_CACHE\b",trace),trace[-1500:]
                print("AIMP_REAL_NATIVE_GUI_SEEK_RESUME_DIRECT_STREAM_PASS",arch,
                      f"position_ms={first}->{before}->{jumped}->{after}",
                      f"duration_s={duration}","cache=0",flush=True)
        finally:
            if proc and proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=7)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
    print("AIMP_REAL_NATIVE_ISOLATED_PROFILE_CLEANED_PASS",arch,flush=True)

def main():
    for arch in ("x86","x64"):
        if subprocess.run(["tasklist","/FI","IMAGENAME eq AIMP.exe"],
                          capture_output=True,text=True).stdout.lower().count("aimp.exe")>0:
            raise SystemExit("AIMP is already running; refusing to disturb the user process")
        run(arch)
    if FAULT_ROUTE:
        print("AIMP_X86_X64_NATIVE_RECOVERY_GUI_SEEK_VERIFIED_PASS mode="+FAULT_MODE+" route="+FAULT_ROUTE,flush=True)
    else:
        print("AIMP_X86_X64_NATIVE_GUI_SEEK_VERIFIED_PASS",flush=True)

if __name__=="__main__":
    main()
