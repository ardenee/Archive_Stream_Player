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
        archive=qa/"real-launch.7z"
        sevenzip=Path(r"C:\Program Files\7-Zip\7z.exe")
        assert sevenzip.is_file()
        result=subprocess.run([str(sevenzip),"a","-t7z","-m0=LZMA2","-ms=off",
                               str(archive),str(source)],cwd=qa,capture_output=True,text=True,timeout=80)
        assert result.returncode==0 and archive.is_file(),result.stderr[-1000:]
        plugin=player_folder/"xmp-archive-stream.dll"
        plugin.unlink(missing_ok=True)
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
            log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_xmplay.log"
            def read_state():
                if not log.is_file():return False
                body=log.read_text(encoding="utf-8-sig",errors="replace")
                return "LIST_OK" in body and "STREAM_OK" in body
            native.wait_for(read_state,35,"native XMPARC list + decode requests")
            time.sleep(5)
            first=native.progress(win,qa)
            time.sleep(4)
            second=native.progress(win,qa)
            assert second>=first+3,(first,second)
            body=log.read_text(encoding="utf-8-sig",errors="replace")
            assert "CACHE_OK" not in body,body[-1200:]
            print("XMPLAY_LAUNCHER_REAL_NATIVE_PLAYBACK_WITHOUT_HTTP_PASS",
                  "progress_pixels="+str(first)+"->"+str(second),"cache=0",flush=True)
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
