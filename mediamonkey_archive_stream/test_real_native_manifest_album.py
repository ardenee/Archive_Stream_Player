r"""Real MediaMonkey 2024 native input playlist/seek acceptance without HTTP.

Stages a disposable portable player, generates .ast references through the
static Archive Stream enumerator, and queues them through the official MM5 JS
API. Native x86 Winamp decoder directly streams the original archive audio.
No personal database changes. No real speaker output (QA player muted).
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

REPO=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(REPO/"mediamonkey_archive_stream"))
from test_native_portable_codecs import INSTALLED,wait_for,player_window,dismiss_first_run_wizard,doc

DLL=REPO/"in_archive_stream"/"Release"/"in_archive_stream.dll"
BUILDER=REPO/"archive_player_bridge"/"x64"/"Release"/"archive_player_bridge.exe"
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")
RAR=Path(r"C:\Program Files\WinRAR\Rar.exe")
SOURCES=(("MP3","01.MP3File.mp3"),("OGG","02.OGGFile.ogg"))

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None

def make_archive(kind,qa):
    files=[]
    for _,source_name in SOURCES:
        source=Path(r"C:\audiotest")/source_name
        assert source.is_file(),source
        local=qa/source_name
        shutil.copy2(source,local)
        files.append(local)
    archive=qa/("mm5-native-album."+kind)
    if kind=="zip":
        with zipfile.ZipFile(archive,"w",zipfile.ZIP_STORED) as z:
            for file in files:
                z.write(file,file.name)
    else:
        cmd=([str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off",str(archive),*(str(x) for x in files)]
             if kind=="7z" else
             [str(RAR),"a","-ma5","-m0","-s-","-ep",str(archive),*(str(x) for x in files)])
        run=subprocess.run(cmd,cwd=qa,capture_output=True,text=True,timeout=90)
        assert run.returncode==0 and archive.is_file(),(kind,run.stderr[-800:])
    return archive

def build_manifests(kind,qa,env):
    archive=make_archive(kind,qa)
    manifest_dir=qa/"NativeTracks"
    cmd=[str(BUILDER),"--archive",str(archive),"--native-manifest-dir",str(manifest_dir),
         "--prompt-password","0","--password-list",str(qa/"Temp"/"keeps.txt"),
         "--title-template","<#>. <filename>"]
    result=subprocess.run(cmd,cwd=qa,env=env,capture_output=True,text=True,timeout=45)
    assert result.returncode==0 and "NATIVE_MANIFEST_READY=1" in result.stdout,(result.stdout,result.stderr)
    playlist=manifest_dir/"archive-stream-native.m3u8"
    assert playlist.is_file()
    tracks=[]
    title=""
    for line in playlist.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("#EXTINF:"):
            title=line.partition(",")[2]
        elif line and not line.startswith("#"):
            path=Path(line)
            assert title and path.is_file() and path.suffix.lower()==".ast",(title,path)
            assert path.read_bytes().startswith(b"ARCHIVE_STREAM_NATIVE_TRACK_V1\n")
            tracks.append({"path":str(path),"title":title})
            title=""
    assert len(tracks)==2 and "http" not in result.stdout.lower(),tracks
    print("MM5_NATIVE_MANIFEST_LIST_ONLY_NO_HTTP",kind,[t["title"] for t in tracks],flush=True)
    return tracks

def run(kind):
    assert DLL.is_file() and BUILDER.is_file() and INSTALLED.is_dir()
    existing=subprocess.run(["tasklist","/FI","IMAGENAME eq MediaMonkey.exe"],
                            capture_output=True,text=True,timeout=10).stdout
    if "MediaMonkey.exe" in existing:
        raise RuntimeError("MediaMonkey already active: refusing to touch it")
    profile=Path(os.environ["APPDATA"])/"MediaMonkey5"
    protected=[profile/name for name in ("MM5.DB","MediaMonkey.ini","persistent.json")]
    before={str(p):digest(p) for p in protected}
    with tempfile.TemporaryDirectory(prefix="AST-MM5-Native-Album-",dir=r"D:\Temp") as folder:
        qa=Path(folder)
        for part in ("Roaming","Local","Temp"):
            (qa/part).mkdir()
        env=os.environ.copy()
        env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
                   TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1",
                   ARCHIVE_STREAM_VLC_PASSWORD_LIST=str(qa/"Temp"/"keeps.txt"))
        tracks=build_manifests(kind,qa,env)
        host=qa/"MediaMonkey"
        host.mkdir()
        copied=subprocess.run(["robocopy",str(INSTALLED),str(host),"/E",
                               "/NFL","/NDL","/NJH","/NJS","/NP","/R:1","/W:1"],
                              capture_output=True,text=True,timeout=160)
        assert copied.returncode<8,copied.stdout[-1000:]
        portable=host/"Portable"
        portable.mkdir(exist_ok=True)
        (portable/"MediaMonkey.ini").write_text("[Portable]\nIsPortable=1\n[System]\nDBName="+str(portable/"MM5.DB")+"\n",encoding="utf-8")
        shutil.copy2(DLL,host/"Plugins"/"in_archive_stream.dll")
        addon=host/"Scripts"/"astMm5NativeAlbumQa"
        addon.mkdir(parents=True,exist_ok=True)
        (addon/"info.json").write_text(json.dumps({"title":"AST MM5 Native Album QA","id":"astMm5NativeAlbumQa","description":"Ephemeral native playback test","version":"1.0.0","type":"general","author":"Archive Stream QA"}),encoding="utf-8")
        marker=qa/"qa-started.txt"
        result_path=qa/"qa-result.json"
        error_path=qa/"qa-error.txt"
        js="""(function(){'use strict';window.whenReady(()=>setTimeout(async()=>{
try{
if(await app.filesystem.fileExistsAsync(MARKER))return;
await app.filesystem.saveTextToFileAsync(MARKER,'1');
const tlist=app.utils.createTracklist(true);
for(const item of INPUTS){
const t=app.utils.createEmptyTrack();
t.path=item.path;t.title=item.title;tlist.add(t);
}
app.player.mute=true;
await app.player.addTracksAsync(tlist,{withClear:true,startPlayback:true});
const nap=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const results=[];
for(let index=0;index<INPUTS.length;index++){
if(index>0)await app.player.nextAsync();
for(let tick=0;tick<70 && app.player.trackPositionMS<1800;tick++)await nap(250);
const first=app.player.trackPositionMS;
await nap(1700);
const before=app.player.trackPositionMS;
await app.player.seekMSAsync(90000);
await nap(1500);
const jumped=app.player.trackPositionMS;
await nap(2300);
const resumed=app.player.trackPositionMS;
results.push({index,title:INPUTS[index].title,first,before,jumped,resumed,playing:app.player.isPlaying});
}
await app.filesystem.saveTextToFileAsync(RESULT,JSON.stringify(results));
}catch(e){await app.filesystem.saveTextToFileAsync(ERROR,String(e&&e.stack||e));}
},16000));})();"""
        for key,value in (("MARKER",str(marker)),("INPUTS",tracks),
                          ("RESULT",str(result_path)),("ERROR",str(error_path))):
            js=js.replace(key,json.dumps(value))
        (addon/"mainwindow_add.js").write_text(js,encoding="utf-8")
        verify=subprocess.run(["node","--check",str(addon/"mainwindow_add.js")],
                              capture_output=True,text=True,timeout=9)
        assert verify.returncode==0,verify.stderr
        proc=None
        try:
            proc=subprocess.Popen([str(host/"MediaMonkey.exe")],cwd=host,env=env)
            window=wait_for(lambda:player_window(proc.pid),95,"portable MM5 player window")
            dismiss_first_run_wizard(window)
            wait_for(lambda:portable/"MM5.DB" if (portable/"MM5.DB").exists() else None,30,"QA portable database")
            wait_for(lambda:marker if marker.is_file() else None,55,"native addon JS dispatch")
            deadline=time.monotonic()+70
            while time.monotonic()<deadline and not result_path.is_file() and not error_path.is_file():
                time.sleep(.4)
            if error_path.is_file():
                raise AssertionError(("Native SDK QA addon failed",error_path.read_text(encoding="utf-8")[-1700:]))
            assert result_path.is_file(),"Native playback/seek results did not finish"
            results=json.loads(result_path.read_text(encoding="utf-8"))
            print("MM5_NATIVE_TWO_TRACK_PLAYBACK_SEEK_SDK_RESULTS",kind,results,flush=True)
            assert len(results)==len(tracks)==2
            for row in results:
                assert row["first"]>=1200 and row["before"]>row["first"]+800,row
                assert row["jumped"]>85000 and row["resumed"]>row["jumped"]+1000,row
                assert row["playing"],row
            log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_winamp.log"
            assert log.is_file(),"Winamp decoder did not log native tracks"
            trace=log.read_text(encoding="utf-8-sig",errors="replace")
            metrics={value:trace.count(value) for value in
                     ("NATIVE_MANIFEST_ACCEPTED","DECODER_OPEN_OK","FIRST_WRITE","SEEK_REQUEST","mode=STREAM","mode=CACHE","ROUTE_CACHE")}
            print("MM5_NATIVE_TWO_TRACK_BACKEND_COUNTS",kind,metrics,flush=True)
            # The MP3 decoder reports DECODER_OPEN_OK/FIRST_WRITE; OGG's push decoder
            # has different log events, so require its real clock/seek success and
            # absence of decoder/output failures rather than MP3-only log names.
            assert metrics["NATIVE_MANIFEST_ACCEPTED"]==2 and metrics["SEEK_REQUEST"]>=2 and metrics["mode=STREAM"]>=2,metrics
            assert metrics["DECODER_OPEN_OK"]>=1 and metrics["FIRST_WRITE"]>=1,metrics
            initial_writes=[int(match) for match in re.findall(r"FIRST_WRITE[^\n]*\bbytes=(\d+)",trace)]
            assert initial_writes and all(0 < n <= 8192 for n in initial_writes),initial_writes
            assert "PCM_WRITE_INVALID" not in trace,trace[-2000:]
            print("MM5_NATIVE_OUTPUT_HOST_MAX_8192_BYTE_WRITES_PASS",kind,initial_writes,flush=True)
            assert "DECODER_OPEN_FAIL" not in trace and "OUTPUT | OPEN_FAIL" not in trace,trace[-2000:]
            assert metrics["mode=CACHE"]==metrics["ROUTE_CACHE"]==0,metrics
            print("MM5_REAL_NATIVE_ALBUM_TWO_TRACK_PLAY_SEEK_NO_HTTP_PASS",kind,flush=True)
        finally:
            if proc and proc.poll() is None:
                subprocess.run(["taskkill","/PID",str(proc.pid),"/T","/F"],
                               capture_output=True,text=True,timeout=20)
                try:proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=8)
            # The x86 Winamp decoder is hosted in MediaMonkeyEngine.exe, not
            # necessarily the front-end process. Terminate ONLY executables
            # located inside this disposable QA installation before cleanup.
            isolated=str(host).replace("'","''")
            cleanup_script=("$folder='"+isolated+"';"+
                "Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and " +
                "$_.ExecutablePath.StartsWith($folder+'\\',[System.StringComparison]::OrdinalIgnoreCase) } | " +
                "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }")
            subprocess.run(["powershell","-NoProfile","-NonInteractive","-Command",cleanup_script],
                           capture_output=True,text=True,timeout=20)
            time.sleep(1.5)
    assert before=={str(p):digest(p) for p in protected},"Personal MediaMonkey profile modified"
    print("MM5_REAL_NATIVE_ALBUM_ISOLATED_PROFILE_CLEANED_PASS",kind,flush=True)

if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--kind",choices=("zip","7z","rar"),default="zip")
    run(parser.parse_args().kind)
