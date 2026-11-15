r"""Real XMPlay native-first launcher regression (never touches personal player).

Tests native SDK/XMPARC dispatch, no loopback HTTP service, explicit failure
when the plugin is absent, and voluntary -UseHttpBridge compatibility mode.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"xmp_archive_stream"))
import test_real_native_gui_seek as native
import win32gui
import win32process
import win32api

ROOT=Path(__file__).resolve().parents[1]
LAUNCH=ROOT/"archive_player_bridge"/"launch-archive.ps1"
SOURCE=native.XMPLAY_SOURCE
PLUGIN=native.PLUGIN
AUDIO=native.AUDIO

def invoke(player, archive, *options, env=None):
    args=["powershell.exe","-NoProfile","-NonInteractive","-File",str(LAUNCH),
          "-Player","XMPlay","-PlayerPath",str(player),"-Archive",str(archive),*options]
    return subprocess.run(args,env=env,capture_output=True,text=True,errors="replace",timeout=65)

def main():
    assert all(x.is_file() for x in (LAUNCH,SOURCE/"xmplay.exe",PLUGIN,AUDIO))
    check=subprocess.run(["tasklist","/FI","IMAGENAME eq xmplay.exe"],capture_output=True,text=True).stdout.lower()
    if "xmplay.exe" in check:
        raise SystemExit("XMPlay already running; do not disturb a user session")
    with tempfile.TemporaryDirectory(prefix="AST-XMPlay-LauncherNative-",dir=r"D:\Temp",ignore_cleanup_errors=True) as temp:
        qa=Path(temp)
        player_folder=qa/"XMPlay"
        shutil.copytree(SOURCE,player_folder)
        for f in ("xmplay.pls","xmplay.library","xmplay.library~"):
            (player_folder/f).unlink(missing_ok=True)
        target=player_folder/"xmplay.exe"
        for name in ("Local","Roaming","Temp"):
            (qa/name).mkdir()
        env=os.environ.copy()
        env.update(LOCALAPPDATA=str(qa/"Local"),APPDATA=str(qa/"Roaming"),
                   TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        source=qa/"AST_LAUNCH_NATIVE_MP3.mp3"
        shutil.copyfile(AUDIO,source)
        kind=os.environ.get("AST_XMPLAY_LAUNCH_QA_FORMAT","7z").lower()
        assert kind in ("7z","zip","rar"),kind
        archive=qa/("real-launch."+kind)
        if kind=="7z":
            sevenzip=Path(r"C:\Program Files\7-Zip\7z.exe")
            assert sevenzip.is_file()
            result=subprocess.run([str(sevenzip),"a","-t7z","-m0=LZMA2","-ms=off",
                                   str(archive),str(source)],cwd=qa,capture_output=True,text=True,timeout=80)
            assert result.returncode==0 and archive.is_file(),result.stderr[-1000:]
        elif kind=="rar":
            rar=Path(r"C:\Program Files\WinRAR\Rar.exe")
            assert rar.is_file()
            result=subprocess.run([str(rar),"a","-ma5","-m0","-s-","-ep",str(archive),str(source)],
                                  cwd=qa,capture_output=True,text=True,timeout=80)
            assert result.returncode==0 and archive.is_file(),result.stderr[-1000:]
        else:
            with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as output:
                output.write(source,"AST_LAUNCH_NATIVE_MP3.mp3")
        plugin=player_folder/"xmp-archive-stream.dll"
        plugin.unlink(missing_ok=True)
        if kind=="zip":
            native_zip=invoke(target,archive,"-NoLaunch",env=env)
            assert native_zip.returncode==0 and "DRY_RUN_PASS player=XMPlay mode=native_builtin_zip http=0" in native_zip.stdout,(native_zip.stdout,native_zip.stderr)
            override=invoke(target,archive,"-UseHttpBridge","-NoLaunch",env=env)
            assert override.returncode==0 and "DRY_RUN_PASS player=XMPlay" in override.stdout and "native_builtin_zip" not in override.stdout,(override.stdout,override.stderr)
            print("XMPLAY_ZIP_NATIVE_BUILTIN_DEFAULT_AND_HTTP_OVERRIDE_PASS",flush=True)
            return
        missing=invoke(target,archive,"-NoLaunch",env=env)
        assert missing.returncode != 0 and "native Archive Stream XMPARC plugin is not installed" in (missing.stdout+missing.stderr),missing
        print("XMPLAY_LAUNCHER_NATIVE_PLUGIN_REQUIRED_FAIL_CLOSED_PASS",flush=True)
        fallback=invoke(target,archive,"-UseHttpBridge","-NoLaunch",env=env)
        assert fallback.returncode==0 and "DRY_RUN_PASS player=XMPlay" in fallback.stdout,(
            fallback.returncode,fallback.stdout[-900:],fallback.stderr[-900:])
        print("XMPLAY_LAUNCHER_EXPLICIT_HTTP_FALLBACK_PASS",flush=True)
        shutil.copy2(PLUGIN,plugin)
        dry=invoke(target,archive,"-NoLaunch",env=env)
        assert dry.returncode==0 and "DRY_RUN_PASS player=XMPlay mode=native_xmparc http=0" in dry.stdout,(
            dry.returncode,dry.stdout,dry.stderr)
        print("XMPLAY_LAUNCHER_NATIVE_ZERO_HTTP_DRY_RUN_PASS",flush=True)
        pid=None
        try:
            actual=invoke(target,archive,env=env)
            assert actual.returncode==0 and "no HTTP bridge was started" in actual.stdout,(
                actual.returncode,actual.stdout,actual.stderr)
            match=re.search(r"XMPLAY_NATIVE_PLAYER_PID=(\d+)",actual.stdout)
            assert match,"Launcher did not return a native process PID"
            pid=int(match.group(1))
            handle=win32api.OpenProcess(0x410,False,pid)
            try:
                started=Path(win32process.GetModuleFileNameEx(handle,0))
            finally:
                handle.Close()
            assert started.resolve()==target.resolve(),("Unexpected process",started,target)
            win=native.wait_for(lambda:native.player_window(pid),20,"XMPlay playback window")
            win32gui.SetWindowPos(win,native.win32con.HWND_TOP,0,0,0,0,
                                 native.win32con.SWP_NOMOVE|native.win32con.SWP_NOSIZE|native.win32con.SWP_SHOWWINDOW)
            left,top,right,bottom=win32gui.GetWindowRect(win)
            native.mouse.move(coords=(left+30,top+180))
            log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_xmplay.log"
            def read_state():
                if not log.is_file():return False
                body=log.read_text(encoding="utf-8-sig",errors="replace")
                return "LIST_OK" in body and "STREAM_OK" in body
            if kind in ("7z","rar"):
                native.wait_for(read_state,35,"native XMPARC list + decode requests")
            else:
                time.sleep(4)
                print("XMPLAY_ZIP_NATIVE_SDK_LOG_PRESENT",read_state(),flush=True)
            # A highlighted XMPlay skin can paint the entire seek strip white.
            # That returns ~570 counted pixels and is NOT playback progress.
            # Reject such captures, and recheck with the pointer away.
            win32gui.SetWindowPos(win,native.win32con.HWND_TOPMOST,0,0,0,0,
                                 native.win32con.SWP_NOMOVE|native.win32con.SWP_NOSIZE|native.win32con.SWP_SHOWWINDOW)
            left,top,right,bottom=win32gui.GetWindowRect(win)
            def stable_progress():
                native.mouse.move(coords=(left+30,top+180))
                time.sleep(.35)
                value=native.progress(win,qa)
                return value if 0<value<right-left-40 else None
            first=native.wait_for(stable_progress,20,"unobscured XMPlay playback progress")
            time.sleep(4)
            def advancing_progress():
                value=stable_progress()
                return value if value and value>=first+3 else None
            second=native.wait_for(advancing_progress,20,"real XMPlay progress advance")
            assert 0<first<second<right-left-40,(first,second,right-left)
            body=log.read_text(encoding="utf-8-sig",errors="replace") if log.is_file() else ""
            assert "CACHE_OK" not in body,body[-1200:]
            if kind in ("7z","rar"):
                print("XMPLAY_LAUNCHER_REAL_NATIVE_PLAYBACK_WITHOUT_HTTP_PASS",
                      "progress_pixels="+str(first)+"->"+str(second),"cache=0",flush=True)
            else:
                print("XMPLAY_ZIP_PLAYER_PLAYBACK_WITHOUT_SDK_LOG_PASS",
                      "progress_pixels="+str(first)+"->"+str(second),"log_tail="+body[-300:],flush=True)
        finally:
            if pid:
                subprocess.run(["taskkill","/PID",str(pid),"/T","/F"],capture_output=True,timeout=15)
                time.sleep(2)
    for attempt in range(15):
        if not qa.exists():
            break
        try:
            shutil.rmtree(qa)
        except PermissionError:
            time.sleep(1)
    assert not qa.exists(),"Disposable native XMPlay test folder still held open"
    print("XMPLAY_LAUNCHER_TEST_ISOLATED_PROFILE_REMOVED_PASS",flush=True)

if __name__=="__main__":
    main()
