"""Prove adapter startup honors shared cache age limits without touching real user files."""
import os
import subprocess
import tempfile
import time
from pathlib import Path
import winreg

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = Path(r"D:\Temp\ArchiveStreamNewPlayers\bridge-test.zip")
REGISTRY = r"Software\ArchiveStreamPlayer\Adapters"

def settings():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY) as key:
            limit = winreg.QueryValueEx(key, "CacheLimitMB")[0]
            age = winreg.QueryValueEx(key, "CacheAgeDays")[0]
            auto = winreg.QueryValueEx(key, "CacheAutoCleanup")[0]
            return int(limit), int(age), bool(auto)
    except OSError:
        return 2048, 30, True

def main():
    limit, age, auto = settings()
    if not auto or not limit:
        print("SKIP_SHARED_CACHE_MAINTENANCE: user configured automatic cleanup disabled")
        return
    assert FIXTURE.is_file()
    with tempfile.TemporaryDirectory(prefix="archive-cache-maintenance-") as folder:
        root = Path(folder)
        local = root / "profile_日本語"
        cache = local / "ArchiveStreamPlayer" / "ManagedCache"
        cache.mkdir(parents=True)
        old = cache / "0000000000000001_123.mp3"
        new = cache / "0000000000000002_456.ogg"
        untouched = cache / "user_notes.txt"
        old.write_bytes(b"old cache")
        new.write_bytes(b"new cache")
        untouched.write_bytes(b"unrelated content")
        now=time.time()
        os.utime(old,(now-(age+5)*86400,now-(age+5)*86400))
        os.utime(new,(now-60,now-60))
        env=os.environ.copy()
        env["LOCALAPPDATA"]=str(local)
        for arch,subdir in (("Win32","Release"),("x64","x64/Release")):
            old.write_bytes(b"old cache")
            os.utime(old,(now-(age+5)*86400,now-(age+5)*86400))
            exe=ROOT/"archive_player_bridge"/subdir/"archive_player_bridge.exe"
            session=root/arch
            session.mkdir()
            process=subprocess.run([str(exe),"--archive",str(FIXTURE),"--playlist-dir",str(session),
                "--lifetime","1","--cache-limit-mb","0","--prompt-password","0"],env=env,
                capture_output=True,text=True,errors="replace",timeout=20)
            assert process.returncode==0,(arch,process.stdout[-300:],process.stderr[-300:])
            assert not old.exists(),(arch,"expired cache not removed")
            assert new.exists() and untouched.read_bytes()==b"unrelated content",(
                arch,"active/unrelated file unexpectedly deleted")
            print("PASS_STARTUP_CACHE_AGE",arch,"old_removed recent_and_unrelated_preserved",flush=True)
    print("ALL_SHARED_CACHE_MAINTENANCE_TESTS_PASS",flush=True)

if __name__=="__main__":
    main()
