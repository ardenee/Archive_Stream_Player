"""Exercise shared per-host logging without touching user registry or profiles.
Uses a private LOCALAPPDATA and the documented DEV debug override.
"""
import os
import subprocess
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ZIP = Path(r"D:\Temp\ArchiveStreamNewPlayers\bridge-test.zip")

def main():
    assert ZIP.is_file()
    with tempfile.TemporaryDirectory(prefix="archive-shared-log-") as base:
        root = Path(base)
        local = root / "profile_日本語"
        local.mkdir()
        logdir = local / "ArchiveStreamPlayer" / "Logs"
        env = os.environ.copy()
        env["LOCALAPPDATA"] = str(local)
        env["ARCHIVE_STREAM_ADAPTER_DEBUG"] = "1"
        for arch, subdir in (("Win32","Release"),("x64","x64/Release")):
            exe = ROOT / "archive_player_bridge" / subdir / "archive_player_bridge.exe"
            folder = root / ("playlist-"+arch)
            folder.mkdir()
            proc = subprocess.Popen([str(exe),"--archive",str(ZIP),"--playlist-dir",str(folder),
                                     "--lifetime","25","--cache-limit-mb","0","--prompt-password","0"],
                                     env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
            try:
                info={}
                for _ in range(36):
                    line=proc.stdout.readline().strip()
                    if "=" in line:
                        key,value=line.split("=",1)
                        info[key]=value
                    if line=="BRIDGE_READY=1":
                        break
                assert info.get("BRIDGE_READY")=="1",info
                playlist=Path(info["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig")
                track=next(line for line in playlist.splitlines() if line.startswith("http://127.0.0.1:"))
                with urllib.request.urlopen(urllib.request.Request(track,headers={"Range":"bytes=0-4095"}),timeout=15) as request:
                    assert request.status==206 and len(request.read())==4096
                shutdown=info["URL_M3U8"].replace("/playlist.m3u8","/shutdown")
                with urllib.request.urlopen(urllib.request.Request(shutdown,method="POST"),timeout=6) as request:
                    assert request.status==200
                proc.wait(timeout=10)
                assert proc.returncode==0,(proc.returncode,proc.stderr.read())
                logfile=logdir/"archive_stream_bridge.log"
                assert logfile.is_file(),(arch,logdir)
                lines=logfile.read_text(encoding="utf-8",errors="replace")
                assert "FORMAT_VERSION" in lines and "SESSION_OPEN" in lines and "ROUTE_STREAM" in lines,(arch,lines[-1000:])
                assert "password=" not in lines.lower(),(arch,"password material unexpectedly logged")
                print("PASS_SHARED_DEBUG_LOG",arch,"per-host diagnostic events and range stream",flush=True)
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait(timeout=10)

        # Native adapters must use the same shared logging policy and
        # Unicode-safe per-player log filenames, not test-only temp logs.
        for arch, subdir in (("Win32", "Release"), ("x64", "x64/Release")):
            aimp = ROOT / "aimp_archive_stream" / subdir
            completed = subprocess.run([str(aimp/"aimp_adapter_test.exe"),
                str(aimp/"aimp_archive_stream.dll"), str(ZIP)], env=env,
                capture_output=True, text=True, errors="replace", timeout=90)
            assert completed.returncode == 0 and "PASS AIMP plugin contract" in completed.stdout, (
                arch,completed.stdout[-650:],completed.stderr[-650:])
            log = logdir / "archive_stream_aimp.log"
            assert log.is_file() and "PLUGIN_INIT" in log.read_text(encoding="utf-8",errors="replace")
            print("PASS_AIMP_SHARED_LOG", arch, "Unicode path and native host contract", flush=True)

        xmplay = ROOT / "xmp_archive_stream" / "Release"
        wav = Path(r"D:\Temp\ArchiveStreamNewPlayers\bridge-wav.zip")
        completed = subprocess.run([str(xmplay/"xmplay_adapter_test.exe"),
            str(xmplay/"xmp-archive-stream.dll"), str(wav)], env=env,
            capture_output=True,text=True,errors="replace",timeout=90)
        assert completed.returncode == 0 and "PASS XMPlay archive callback" in completed.stdout, (
            completed.stdout[-650:], completed.stderr[-650:])
        log = logdir / "archive_stream_xmplay.log"
        assert log.is_file() and "PLUGIN_INIT" in log.read_text(encoding="utf-8",errors="replace")
        print("PASS_XMPLAY_SHARED_LOG native archive callback and Unicode path",flush=True)

        # Test the C# MusicBee adapter's own logging routine in an isolated
        # x86 PowerShell process; no MusicBee installation is started or edited.
        musicbee_dll = ROOT/"musicbee_archive_stream"/"Release"/"MB_ArchiveStream.dll"
        assert musicbee_dll.is_file(), musicbee_dll
        power32 = Path(r"C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe")
        code = ("$assembly=[Reflection.Assembly]::LoadFrom('" + str(musicbee_dll).replace("'", "''") +
                "'); $type=$assembly.GetType('MusicBeePlugin.Plugin'); "
                "$trace=$type.GetMethod('Trace',[Reflection.BindingFlags]'Static,NonPublic'); "
                "$trace.Invoke($null,@('TEST_LOG_RECORD'))")
        completed = subprocess.run([str(power32),"-NoProfile","-NonInteractive","-Command",code],
            env=env,capture_output=True,text=True,errors="replace",timeout=35)
        assert completed.returncode == 0, (completed.stdout,completed.stderr)
        log = logdir / "archive_stream_musicbee.log"
        assert log.is_file() and "TEST_LOG_RECORD" in log.read_text(encoding="utf-8",errors="replace")
        print("PASS_MUSICBEE_SHARED_LOG x86 reflection smoke and Unicode path",flush=True)

        sentinel=logdir/"user_notes.txt"
        sentinel.write_text("do not erase unrelated files",encoding="utf-8")
        ui=ROOT/"archive_player_bridge"/"configure-adapters.ps1"
        shell=subprocess.run(["powershell.exe","-NoProfile","-ExecutionPolicy","Bypass","-File",str(ui),"-ClearLogs"],
                             env=env,capture_output=True,text=True,timeout=25)
        assert shell.returncode==0 and "LOG_FILES_REMOVED=4" in shell.stdout,(shell.stdout,shell.stderr)
        assert sentinel.is_file() and not (logdir/"archive_stream_bridge.log").exists()
        print("PASS_SHARED_DEBUG_LOG_CLEANUP safe-generated-files-only",flush=True)
    print("ALL_SHARED_LOGGING_REGRESSIONS_PASS",flush=True)

if __name__ == "__main__":
    main()
