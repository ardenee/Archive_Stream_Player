r"""Real XMPlay 4.1 native XMPARC archive playback and GUI-seek regression.

Uses the actual XMPlay seek bar (not host SDK Seek callbacks), reads only
its rendered progress strip, and verifies continued playback. The adapter
returns a host-owned decoded buffer as required by XMPARC; this test does
not claim on-demand player streaming, audible audio or PCM parity.
Requires an unlocked interactive DEV desktop.
    py -3.13 -B xmp_archive_stream/test_real_native_gui_seek.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

AUTOMATION=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
os.add_dll_directory(str(AUTOMATION/"pywin32_system32"))
sys.path[:0]=[str(AUTOMATION),str(AUTOMATION/"win32"),
             str(AUTOMATION/"win32"/"lib"),str(AUTOMATION/"Pythonwin")]
from pywinauto import mouse
import win32gui
import win32con
import win32process

ROOT=Path(__file__).resolve().parents[1]
XMPLAY_SOURCE=Path(r"D:\Temp\ArchiveStreamPlayerAdapters\xmplay")
PLUGIN=ROOT/"xmp_archive_stream"/"Release"/"xmp-archive-stream.dll"
AUDIO=Path(r"C:\audiotest\01.MP3File.mp3")
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")

def wait_for(callback,seconds,label):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        try:
            result=callback()
            if result:
                return result
        except Exception:
            pass
        time.sleep(.3)
    raise AssertionError("XMPlay host timeout: "+label)

def player_window(pid):
    found=[]
    def visit(hwnd,ignored):
        try:
            if (win32gui.GetClassName(hwnd)=="XMPLAY-MAIN" and
                win32process.GetWindowThreadProcessId(hwnd)[1]==pid):
                found.append(hwnd)
        except Exception:
            pass
    win32gui.EnumWindows(visit,None)
    return found[0] if found else None

def progress(window,qa):
    # Default XMPlay 4.1 skin exposes the seek bar at y=55..60 below the
    # top-left window corner. Played pixels are white (255) at rest and
    # pale grey (229) when the cursor hovers; remaining pixels are blue.
    l,t,r,b=win32gui.GetWindowRect(window)
    width=r-l
    if not 550<=width<=900 or b-t<240:
        raise AssertionError("Unexpected XMPlay skin geometry")
    bmp=qa/"progress.bmp"
    dest=str(bmp).replace("'","''")
    script=(
        "Add-Type -AssemblyName System.Drawing; "
        f"$b=New-Object System.Drawing.Bitmap {width},14; "
        "$g=[System.Drawing.Graphics]::FromImage($b); "
        f"$g.CopyFromScreen({l},{t+51},0,0,$b.Size); "
        f"$b.Save('{dest}',[System.Drawing.Imaging.ImageFormat]::Bmp); "
        "$g.Dispose(); $b.Dispose();"
    )
    run=subprocess.run(["powershell.exe","-NoProfile","-NonInteractive",
                        "-Command",script],capture_output=True,text=True,timeout=16)
    if run.returncode:
        raise AssertionError("XMPlay screen capture failed: "+run.stderr[-900:])
    content=bmp.read_bytes()
    start=int.from_bytes(content[10:14],"little")
    w=int.from_bytes(content[18:22],"little",signed=True)
    height=int.from_bytes(content[22:26],"little",signed=True)
    bpp=int.from_bytes(content[28:30],"little")
    if w!=width or abs(height)!=14 or bpp not in (24,32):
        raise AssertionError("Unexpected screenshot bitmap format")
    stride=((width*bpp+31)//32)*4
    row=55-51
    base=start+(height-1-row if height>0 else row)*stride
    count=0
    for x in range(10,width-16):
        offset=base+x*(bpp//8)
        blue,green,red=content[offset:offset+3]
        if red>=215 and green>=215 and blue>=215:
            count+=1
    return count

def run():
    for fixture in (XMPLAY_SOURCE/"xmplay.exe",PLUGIN,AUDIO,SEVENZIP):
        assert fixture.is_file(),fixture
    tasklist=subprocess.run(["tasklist","/FI","IMAGENAME eq xmplay.exe"],
                            capture_output=True,text=True).stdout
    if "xmplay.exe" in tasklist.lower():
        raise SystemExit("XMPlay already running; refusing to disturb a user player")
    with tempfile.TemporaryDirectory(prefix="AST-XMPlay-NativeGUI-",dir=r"D:\Temp") as folder:
        qa=Path(folder)
        target=qa/"XMPlay"
        shutil.copytree(XMPLAY_SOURCE,target)
        shutil.copy2(PLUGIN,target/"xmp-archive-stream.dll")
        for filename in ("xmplay.pls","xmplay.library","xmplay.library~"):
            (target/filename).unlink(missing_ok=True)
        for name in ("Local","Roaming","Temp"):
            (qa/name).mkdir()
        source=qa/"AST_XMPLAY_NATIVE_GUI.mp3"
        shutil.copyfile(AUDIO,source)
        archive=qa/"native-plugin-test.7z"
        created=subprocess.run([str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off",
                                str(archive),str(source)],cwd=qa,
                               capture_output=True,text=True,timeout=80)
        assert created.returncode==0 and archive.is_file(),created.stderr[-1100:]
        env=os.environ.copy()
        env.update(LOCALAPPDATA=str(qa/"Local"),APPDATA=str(qa/"Roaming"),
                   TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        process=None
        try:
            process=subprocess.Popen([str(target/"xmplay.exe"),"-play",str(archive)],
                                     cwd=target,env=env)
            window=wait_for(lambda:player_window(process.pid),40,"native XMPlay GUI window")
            win32gui.SetWindowPos(window,win32con.HWND_TOP,0,0,0,0,
                                 win32con.SWP_NOMOVE|win32con.SWP_NOSIZE|win32con.SWP_SHOWWINDOW)
            log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_xmplay.log"
            def loaded():
                if not log.is_file():
                    return False
                data=log.read_text(encoding="utf-8-sig",errors="replace")
                return "LIST_OK" in data and "STREAM_OK" in data
            wait_for(loaded,45,"native XMPARC entry extraction")
            time.sleep(8)
            before=progress(window,qa)
            time.sleep(4)
            advancing=progress(window,qa)
            assert advancing>=before+3,f"XMPlay did not advance {before} -> {advancing}"
            l,t,r,b=win32gui.GetWindowRect(window)
            mouse.click(coords=(l+int((r-l)*.73),t+56))
            # The default skin highlights the entire seek strip on hover;
            # a screenshot while the cursor stays over it is not progress.
            mouse.move(coords=(l+30,t+180))
            time.sleep(3)
            jumped=progress(window,qa)
            time.sleep(5)
            resumed=progress(window,qa)
            assert jumped>advancing+90 and resumed>=jumped+4, \
                f"Native GUI seek failed: {before}->{advancing}->{jumped}->{resumed}"
            trace=log.read_text(encoding="utf-8-sig",errors="replace")
            assert "STREAM_OK" in trace and "CACHE_OK" not in trace,trace[-1200:]
            print("XMPLAY_REAL_NATIVE_XMPARC_GUI_SEEK_RESUME_PASS",
                  f"seek_pixels={before}->{advancing}->{jumped}->{resumed}",
                  "7z_non_solid","x86","disk_cache=0",flush=True)
        finally:
            if process and process.poll() is None:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
    print("XMPLAY_ISOLATED_NATIVE_HOST_PROFILE_CLEANED_PASS",flush=True)

if __name__=="__main__":
    run()
