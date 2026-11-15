"""Real native VLC 3 v1.3 PowerShell launcher preferences and route controls.

Uses one disposable VLC installation, synthetic silent WAV ZIP/solid 7z,
fresh LOCALAPPDATA and automatic process cleanup. No HKCU setting changes.
"""
import hashlib,os,re,shutil,struct,subprocess,tempfile,time,wave,zipfile
from pathlib import Path
import test_real_vlc_launcher_inprocess as host

ROOT=Path(__file__).resolve().parents[1]
LAUNCH=host.SCRIPT
VLC=host.VLC
DLL=host.DLL
INSTALLED=host.INSTALLED
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")
TEMPLATE="<archivetype>: <#> - <filename>"
def sha(path):
 return hashlib.sha256(path.read_bytes()).digest()
def make_wave(path,seconds):
 with wave.open(str(path),"wb") as w:
  w.setnchannels(1);w.setsampwidth(2);w.setframerate(44100)
  chunk=struct.pack("<h",0)*44100
  for _ in range(seconds):
   w.writeframes(chunk)
 return path
def launch(archive,qa,options,expect,criterion,timeout=25):
 for x in ("Roaming","Local","Temp"):(qa/x).mkdir(parents=True,exist_ok=True)
 env=os.environ.copy()
 env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
            TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),ARCHIVE_STREAM_ADAPTER_DEBUG="1",
            ARCHIVE_STREAM_VLC_MEMORY_MB="7",
            ARCHIVE_STREAM_VLC_CACHE_LIMIT_MB="1500",
            ARCHIVE_STREAM_VLC_TITLE_TEMPLATE="<filename> [<filetype>]")
 args=["powershell.exe","-NoProfile","-NonInteractive","-File",str(LAUNCH),
       "-Player","VLC","-PlayerPath",str(VLC),"-Archive",str(archive),*options]
 result=subprocess.run(args,env=env,capture_output=True,text=True,errors="replace",timeout=26)
 assert result.returncode==0 and "NATIVE_VLC_READY=1 mode=in_process http=0" in result.stdout,(args,result.stdout,result.stderr)
 assert "NATIVE_VLC_PREF_OVERRIDES=1" in result.stdout,(args,result.stdout,result.stderr)
 assert "VLC native archive playback requested" in result.stdout
 log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
 deadline=time.monotonic()+timeout
 trace=""
 while time.monotonic()<deadline:
  if log.is_file():
   trace=log.read_text(encoding="utf-8-sig",errors="replace")
   if criterion(trace):break
  time.sleep(.25)
 assert criterion(trace),("timed out",expect,trace[-2400:],result.stdout)
 assert "http://127.0.0.1:" not in trace
 assert expect in trace,("effective preferences missing",expect,trace[-2100:])
 return trace
def main():
 assert all(p.is_file() for p in (LAUNCH,VLC,DLL,SEVENZIP))
 baseline=host.pids()
 assert not baseline,"Disposable VLC test installation in use: refusing to alter"
 with tempfile.TemporaryDirectory(prefix="AST-VLC-v13-Preferences-",dir=r"D:\Temp") as scratch:
  qa=Path(scratch)
  saved=qa/"old-access-module.dll"
  had=INSTALLED.is_file()
  if had:shutil.move(str(INSTALLED),str(saved))
  try:
   archive=qa/"album.zip"
   with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as z:
    for idx in range(2):
     wav=make_wave(qa/f"track{idx}.wav",2)
     z.write(wav,wav.name)
   title_qa=qa/"Title"
   try:
    trace=launch(archive,title_qa,
        ["-MemoryMB","2","-CacheLimitMB","0","-TitleTemplate",TEMPLATE],
        "memoryMb=2 | cacheLimitMb=0 | titleTemplate="+TEMPLATE,
        lambda text:text.count("NATIVE_PLAYLIST_TITLE")>=2 and text.count("ACCESS_OPEN_OK")>=2)
    for idx in range(2):
     expected=f"title=ZIP: {idx+1} - track{idx}.wav"
     assert expected in trace,("VLC actual M3U8 title template mismatch",expected,trace[-2900:])
    assert trace.count("NATIVE_PLAYLIST_OPEN")==1
    assert trace.count("ROUTE_STREAM")==2 and trace.count("ROUTE_CACHE")==0
    print("VLC_V13_REAL_LAUNCHER_ZIP_TITLE_TEMPLATE_AND_STREAM_ROUTE_PASS",flush=True)
   finally:
    host.stop_owned_processes(baseline)
   large=[]
   for idx,sec in enumerate((15,16)):
    large.append(make_wave(qa/f"solid{idx}.wav",sec))
   solid=qa/"solid.7z"
   generated=subprocess.run([str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=on",
                   str(solid),*(str(f) for f in large)],
                   cwd=qa,capture_output=True,text=True,timeout=65)
   assert generated.returncode==0 and solid.is_file(),generated.stderr[-600:]
   assert all(p.stat().st_size>1048576 for p in large)
   deny_qa=qa/"NoDisk"
   try:
    trace=launch(solid,deny_qa,
      ["-MemoryMB","1","-CacheLimitMB","0"],
      "memoryMb=1 | cacheLimitMb=0 | titleTemplate=",
      lambda text:"ROUTE_FAIL" in text and "NATIVE_PLAYLIST_OPEN" in text)
    assert "ROUTE_MEMORY" not in trace and "ROUTE_CACHE" not in trace,trace[-1600:]
    cache=deny_qa/"Local"/"ArchiveStreamPlayer"/"ManagedCache"
    assert not cache.is_dir() or not [f for f in cache.iterdir() if f.is_file()],"Cache used when disabled"
    print("VLC_V13_REAL_LAUNCHER_SOLID_MEMORY_1MB_CACHE_0_FAIL_CLOSED_PASS",flush=True)
   finally:
    host.stop_owned_processes(baseline)
   cache_qa=qa/"DiskFallback"
   try:
    trace=launch(solid,cache_qa,
        ["-MemoryMB","1","-CacheLimitMB","5"],
        "memoryMb=1 | cacheLimitMb=5 | titleTemplate=",
        lambda text:"ROUTE_CACHE" in text and "ACCESS_OPEN_OK" in text,timeout=30)
    assert "ROUTE_MEMORY" not in trace,"Memory limit incorrectly bypassed"
    cache=cache_qa/"Local"/"ArchiveStreamPlayer"/"ManagedCache"
    assert cache.is_dir()
    files=[f for f in cache.iterdir() if f.is_file()]
    assert files and any(sha(f) in {sha(src) for src in large} for f in files),("Cached file not byte-identical",files)
    print("VLC_V13_REAL_LAUNCHER_SOLID_MEMORY_1MB_CACHE_5_DISK_LAST_RESORT_PCM_BYTES_PASS",flush=True)
   finally:
    host.stop_owned_processes(baseline)
  finally:
   host.stop_owned_processes(baseline)
   if saved.is_file():shutil.move(str(saved),str(INSTALLED))
   assert INSTALLED.is_file()==had,"Disposable VLC native access module restoration failed"
 print("VLC_V13_REAL_LAUNCHER_PREFERENCES_DISPOSABLE_HOST_CLEANED_PASS",flush=True)
if __name__=="__main__":main()
