"""Real VLC 3 x64 one-process in-plugin album enumeration + password reuse."""
import hashlib,os,secrets,shutil,struct,subprocess,sys,tempfile,threading,wave,zipfile
from pathlib import Path
import test_real_vlc_password as pw

ROOT=Path(__file__).resolve().parents[1]
VLC=pw.VLC
DLL=pw.DLL
SEVENZIP=pw.SEVENZIP
RAR=pw.RAR

def test(kind):
 assert kind in ("7z-hidden","rar-headers")
 assert all(x.is_file() for x in (VLC,DLL,SEVENZIP,RAR))
 shutil.copy2(DLL,VLC.parent/"plugins"/"access"/DLL.name)
 prior,digest=pw.user_password_file()
 with tempfile.TemporaryDirectory(prefix="AST-VLC-InProcHidden-",dir=r"D:\Temp") as scratch:
  qa=Path(scratch)
  for key in ("Local","Roaming","Temp"):(qa/key).mkdir()
  env=os.environ.copy()
  env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
             TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
             ARCHIVE_STREAM_ADAPTER_DEBUG="1")
  songs=[]
  for i,value in enumerate((1400,2800)):
   song=qa/("song"+str(i)+".wav")
   with wave.open(str(song),"wb") as w:
    w.setnchannels(1);w.setsampwidth(2);w.setframerate(44100)
    w.writeframes(struct.pack("<h",value)*88200)
   songs.append(song)
  secret="AST-native-inproc-"+secrets.token_hex(11)
  if kind=="7z-hidden":
   archive=qa/"album.7z"
   cmd=[str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off","-mhe=on",
        "-p"+secret,str(archive),*(str(song) for song in songs)]
  else:
   archive=qa/"album.rar"
   cmd=[str(RAR),"a","-ma5","-m0","-s-","-ep",
        "-hp"+secret,str(archive),*(str(song) for song in songs)]
  generated=subprocess.run(cmd,cwd=qa,capture_output=True,text=True,timeout=55)
  assert generated.returncode==0 and archive.is_file(),generated.stderr[-1000:]
  uri="ast://"+str(archive).encode("utf-8").hex().upper()+"/list/album.m3u8"
  output=qa/"output.wav"
  cmd=[str(VLC),"-I","dummy","--no-one-instance","--ignore-config",
       "--no-plugins-cache","--no-video","--play-and-exit",
       "--aout=afile","--audiofile-format=s16",
       "--audiofile-file="+str(output),uri]
  process=subprocess.Popen(cmd,cwd=VLC.parent,env=env,stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE,text=True,errors="replace")
  stop=threading.Event();events=[]
  watcher=threading.Thread(target=pw.worker,args=(process.pid,secret,stop,events,"accept"),daemon=True)
  watcher.start()
  try:
   try:
    stdout,stderr=process.communicate(timeout=35)
   except subprocess.TimeoutExpired:
    process.kill();stdout,stderr=process.communicate(timeout=5)
    raise AssertionError("In-process encrypted VLC album playback timed out")
   stop.set();watcher.join(timeout=2)
   log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
   trace=log.read_text(encoding="utf-8-sig",errors="replace") if log.is_file() else ""
   counts={key:trace.count(key) for key in ("NATIVE_PLAYLIST_OPEN","ACCESS_OPEN_OK","ROUTE_STREAM","ROUTE_MEMORY","ROUTE_CACHE","ACCESS_CLOSE")}
   print("VLC_INPROC_ENCRYPTED_ALBUM",kind,"rc",process.returncode,"events",events,
         "counts",counts,"audio_bytes",output.stat().st_size if output.is_file() else 0,
         "trace_tail",[l[-190:] for l in trace.splitlines() if "VLC" in l][-10:],flush=True)
   assert process.returncode==0 and counts["NATIVE_PLAYLIST_OPEN"]==1 and counts["ACCESS_OPEN_OK"]==2,(
       kind,events,counts,trace[-1600:],stderr[-1000:])
   assert events.count("PASSWORD_ENTERED")==1 and events.count("SAVE_DECLINED")==1,events
   assert counts["ROUTE_CACHE"]==0
   if kind=="rar-headers":
    assert counts["ROUTE_MEMORY"]==2 and counts["ROUTE_STREAM"]==0,counts
   else:
    assert counts["ROUTE_STREAM"]==2 and counts["ROUTE_MEMORY"]==0,counts
   assert output.is_file()
   with wave.open(str(output),"rb") as rendered:
    decoded=rendered.readframes(rendered.getnframes())
   assert decoded==struct.pack("<h",2800)*88200,"Last album track PCM mismatch"
   if prior is not None:
    after=hashlib.sha256(prior.read_bytes()).digest() if prior.is_file() else None
    assert after==digest,"Configured original password list changed"
   print("VLC_INPROC_ENCRYPTED_ONE_PROMPT_TWO_TRACKS_EXACT_PCM_PASS",kind,flush=True)
  finally:
   stop.set()
   if process.poll() is None:process.kill()
   process.wait(timeout=5)
   watcher.join(timeout=2)

if __name__=="__main__":
 for typ in ("7z-hidden","rar-headers"):
  test(typ)
 print("VLC_INPROC_ENCRYPTED_ALBUM_BOTH_FORMATS_PASS",flush=True)
