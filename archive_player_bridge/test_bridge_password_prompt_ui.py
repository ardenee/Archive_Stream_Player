"""Real Windows password dialog acceptance for the shared HTTP archive bridge.
Runs Win32 or x64 bridge with temporary 7z AES fixtures (default) or RAR5
encrypted filenames (third CLI argument: rar-headers). Synthetic passwords and
isolated empty password lists are never copied into the real player profile.
Always declines saving; verifies 128 decoded WAV bytes through HTTP Range.
"""
import os
import queue
import secrets
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import wave
from pathlib import Path

sdk=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
_dll=os.add_dll_directory(str(sdk/"pywin32_system32"))
sys.path[:0]=[str(sdk),str(sdk/"win32"),str(sdk/"win32"/"lib"),str(sdk/"Pythonwin")]
from pywinauto import Desktop
import win32gui
import win32process

assert len(sys.argv) in (2,3) and sys.argv[1] in ("Win32","x64")
arch=sys.argv[1]
rar_headers=len(sys.argv)==3 and sys.argv[2]=="rar-headers"
if len(sys.argv)==3 and not rar_headers:
    raise SystemExit("Supported additional mode: rar-headers")
ROOT=Path(__file__).resolve().parent
EXE=ROOT/("Release" if arch=="Win32" else "x64/Release")/"archive_player_bridge.exe"
SEVEN=Path(r"C:\Program Files\7-Zip\7z.exe")
assert EXE.is_file() and SEVEN.is_file()

def owned_windows(pid):
    handles=[]
    def enumerate_one(hwnd,_):
        try:
            _,actual=win32process.GetWindowThreadProcessId(hwnd)
            if actual==pid and win32gui.IsWindowVisible(hwnd):
                handles.append((hwnd,win32gui.GetWindowText(hwnd)))
        except Exception:pass
    win32gui.EnumWindows(enumerate_one,None)
    return handles

def handle_dialogs(pid,passwd):
    handled=[]
    for hwnd,title in owned_windows(pid):
        if title not in ("Archive Stream Player - Password Required","Archive Stream Player"):
            continue
        window=Desktop(backend="win32").window(handle=hwnd)
        if title=="Archive Stream Player - Password Required":
            fields=[w for w in window.descendants() if w.class_name()=="Edit"]
            assert fields,"Credentials dialog has no password input"
            fields[-1].set_edit_text(passwd)
            next(w for w in window.descendants() if w.window_text()=="OK").click()
            handled.append("PASSWORD_ENTERED")
        elif title=="Archive Stream Player":
            no=next((w for w in window.descendants() if w.window_text() in ("No","&No")),None)
            assert no,"Consent dialog has no decline action"
            no.click()
            handled.append("SAVE_DECLINED")
    return handled

with tempfile.TemporaryDirectory(prefix="asp-bridge-cred-ui-",dir=r"D:\Temp") as temp:
    root=Path(temp)
    profile=root/"profile"
    profile.mkdir()
    password="AspBridgePassword"+secrets.token_hex(12)
    wav=root/"PromptBridge.wav"
    with wave.open(str(wav),"wb") as w:
        w.setparams((1,2,44100,0,"NONE","not compressed"))
        w.writeframes(struct.pack("<h",1000)*44100*2)
    archive=root/("PromptBridge.rar" if rar_headers else "PromptBridge.7z")
    if rar_headers:
        rar=Path(r"C:\Program Files\WinRAR\Rar.exe")
        assert rar.is_file()
        cmd=[str(rar),"a","-idq","-y","-ma5","-m0","-s-","-ep",
             "-hp"+password,str(archive),str(wav)]
    else:
        cmd=[str(SEVEN),"a","-t7z","-m0=LZMA2","-mhe=on","-ms=off",
             "-p"+password,str(archive),str(wav)]
    result=subprocess.run(cmd, capture_output=True,text=True,timeout=25)
    assert result.returncode==0 and archive.is_file(),result.stderr[-450:]
    password_list=root/"empty-passwords.txt"
    password_list.write_bytes(b"")
    env=os.environ.copy()
    env.update(LOCALAPPDATA=str(profile))
    proc=subprocess.Popen([str(EXE),"--archive",str(archive),
        "--password-list",str(password_list),"--prompt-password","1",
        "--cache-limit-mb","0","--playlist-dir",str(root),"--lifetime","50"],
        env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
        errors="replace",bufsize=1)
    lines=queue.Queue()
    def collect_output():
        for line in proc.stdout:
            lines.put(line.strip())
    threading.Thread(target=collect_output,daemon=True).start()
    events=[]
    values={}
    try:
        deadline=time.monotonic()+34
        while time.monotonic()<deadline:
            events.extend(handle_dialogs(proc.pid,password))
            try:
                while True:
                    line=lines.get_nowait()
                    if "=" in line:
                        k,v=line.split("=",1)
                        values[k]=v
            except queue.Empty:pass
            if values.get("BRIDGE_READY")=="1":break
            if proc.poll() is not None:break
            time.sleep(.12)
        assert "PASSWORD_ENTERED" in events,(arch,events,values,owned_windows(proc.pid))
        assert values.get("BRIDGE_READY")=="1",(arch,events,values,proc.poll())
        playlist=Path(values["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig")
        url=next(s for s in playlist.splitlines() if s.startswith("http://127.0.0.1:"))
        received={}
        def request_track():
            try:
                with urllib.request.urlopen(urllib.request.Request(url,
                    headers={"Range":"bytes=0-127"}),timeout=25) as r:
                    received["status"]=r.status
                    received["bytes"]=r.read()
            except Exception as err:
                received["error"]=str(err)
        worker=threading.Thread(target=request_track,daemon=True)
        worker.start()
        until=time.monotonic()+32
        while worker.is_alive() and time.monotonic()<until:
            events.extend(handle_dialogs(proc.pid,password))
            time.sleep(.12)
        worker.join(timeout=2)
        assert not worker.is_alive() and received.get("status")==206 and len(received.get("bytes",b""))==128,(arch,events,received)
        assert received["bytes"][:4]==b"RIFF",(arch,received["bytes"][:12])
        assert password_list.read_bytes()==b"", "Password saved without consent"
        print("PASS_BRIDGE_CREDENTIALS_UI",arch,"rar_headers="+str(rar_headers),
              "prompts",events,"range_bytes=128","no_saved_password",flush=True)
        shutdown=values["URL_M3U8"].replace("/playlist.m3u8","/shutdown")
        with urllib.request.urlopen(urllib.request.Request(shutdown,method="POST"),timeout=5) as resp:
            assert resp.status==200
        proc.wait(timeout=10)
        assert proc.returncode==0,proc.returncode
        print("PASS_BRIDGE_PASSWORD_UI_AND_AUDIO",arch,"rar_headers="+str(rar_headers),flush=True)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
