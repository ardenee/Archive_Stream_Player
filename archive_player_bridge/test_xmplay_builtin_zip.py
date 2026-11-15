"""Conservative native XMPlay ZIP eligibility contract regression.

Use only an ordinary, unencrypted, non-ZIP64 ZIP with local known Store/Deflate
member sizes; other ZIP archive layouts must retain HTTP Archive Stream backend.
Tests archive HEADERS ONLY; native player seek is tested separately in
xmp_archive_stream/test_real_zip_priority.py.
"""
import base64,io,os,re,shutil,struct,subprocess,sys,tempfile,time,zipfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"xmp_archive_stream"))
import test_real_native_gui_seek as native
import win32gui,win32process,win32api

ROOT=Path(__file__).resolve().parents[1]
LAUNCH=ROOT/"archive_player_bridge"/"launch-archive.ps1"
PLAYER=Path(r"D:\Temp\ArchiveStreamPlayerAdapters\xmplay\xmplay.exe")
MP3=Path(r"C:\audiotest\01.MP3File.mp3")

class NoSeek(io.BytesIO):
    def seekable(self):return False
    def seek(self,*args):raise OSError("stream is non-seekable")

def run_ps(code,env=None):
    encoded=base64.b64encode(code.encode("utf-16le")).decode("ascii")
    return subprocess.run(["powershell.exe","-NoProfile","-NonInteractive",
                           "-EncodedCommand",encoded],capture_output=True,
                          text=True,errors="replace",timeout=70,env=env)

def run_launch(archive,*opts,player=PLAYER,env=None):
    code=f"""& '{str(LAUNCH).replace("'","''")}' -Player XMPlay -PlayerPath '{str(player).replace("'","''")}' -Archive '{str(archive).replace("'","''")}' {' '.join(opts)}; if (!$?) {{ exit 3 }}"""
    return run_ps(code,env=env)

def main():
    assert all(x.is_file() for x in (LAUNCH,PLAYER,MP3))
    with tempfile.TemporaryDirectory(prefix="AST-XMPLAY-ZIP-Eligibility-",dir=r"D:\Temp") as tmp:
        folder=Path(tmp)
        fixture=MP3.read_bytes()[:250_000]
        def make(name,compression=zipfile.ZIP_STORED,entry="audio.mp3",nonseek=False):
            path=folder/name
            if nonseek:
                target=NoSeek()
                with zipfile.ZipFile(target,"w",compression) as file:
                    file.writestr(entry,fixture)
                path.write_bytes(target.getvalue())
            else:
                with zipfile.ZipFile(path,"w",compression) as file:
                    file.writestr(entry,fixture)
            return path
        stored=make("stored.zip")
        deflate=make("deflate.zip",zipfile.ZIP_DEFLATED)
        descriptor=make("descriptor.zip",zipfile.ZIP_DEFLATED,nonseek=True)
        text=make("non_audio.zip",entry="README.txt")
        encrypted=folder/"encrypted_flag.zip"
        e=bytearray(stored.read_bytes());struct.pack_into("<H",e,6,1);encrypted.write_bytes(e)
        zip64=folder/"zip64_local_marker.zip"
        z=bytearray(stored.read_bytes());struct.pack_into("<I",z,18,0xFFFFFFFF);zip64.write_bytes(z)
        method=folder/"unsupported_method.zip"
        m=bytearray(stored.read_bytes());struct.pack_into("<H",m,8,99);method.write_bytes(m)
        truncated=folder/"truncated.zip";truncated.write_bytes(stored.read_bytes()[:29])
        truncated_central=folder/"truncated_central.zip"
        truncated_central.write_bytes(stored.read_bytes()[:-16])
        no_central=folder/"no_central.zip"
        no_central.write_bytes(stored.read_bytes().split(b"PK\x01\x02",1)[0])
        fixtures={
            stored:True,deflate:True,descriptor:False,text:False,
            encrypted:False,zip64:False,method:False,truncated:False,
            truncated_central:False,no_central:False}
        root=str(LAUNCH).replace("'","''")
        q=f"""$code=Get-Content -LiteralPath '{root}' -Raw;
$a=$code.IndexOf('function Test-XmplayNativeSimpleZip');
$b=$code.IndexOf('# Native ZIP and XMPARC use', $a);
if ($a -lt 0 -or $b -le $a) {{ throw 'Cannot locate ZIP detection contract' }}
Invoke-Expression $code.Substring($a,$b-$a);
"""
        for path in fixtures:
            quoted=str(path).replace("'","''")
            q+=f"""Write-Output ('ZIP_ELIGIBILITY|{path.name}|' + (Test-XmplayNativeSimpleZip -Path '{quoted}'));\n"""
        check=run_ps(q)
        assert check.returncode==0,(check.stdout,check.stderr)
        observed={}
        for line in check.stdout.splitlines():
            if line.startswith("ZIP_ELIGIBILITY|"):
                _,name,value=line.split("|",2)
                observed[name]=value.strip().lower()=="true"
        for path,expected in fixtures.items():
            assert observed.get(path.name)==expected,(path.name,expected,observed,check.stderr)
        print("XMPLAY_NATIVE_ZIP_ELIGIBILITY_PASS",len(fixtures),"cases",flush=True)
        for path in (stored,deflate):
            result=run_launch(path,"-NoLaunch")
            assert result.returncode==0 and "DRY_RUN_PASS player=XMPlay mode=native_builtin_zip http=0" in result.stdout,(path,result.stdout,result.stderr)
            print("XMPLAY_ZIP_NATIVE_LAUNCH_DRYRUN_PASS",path.name,flush=True)
        fallback=run_launch(stored,"-UseHttpBridge","-NoLaunch")
        assert fallback.returncode==0 and "DRY_RUN_PASS player=XMPlay" in fallback.stdout and "native_builtin_zip" not in fallback.stdout,(fallback.stdout,fallback.stderr)
        print("XMPLAY_ZIP_EXPLICIT_HTTP_COMPATIBILITY_PASS",flush=True)
        if os.environ.get("AST_XMPLAY_ZIP_SKIP_HOST")!="1":
            running=subprocess.run(["tasklist","/FI","IMAGENAME eq xmplay.exe"],capture_output=True,text=True).stdout.lower()
            assert "xmplay.exe" not in running,"Existing XMPlay: never interfere with user instance"
            player_dir=folder/"XMPlay"
            shutil.copytree(native.XMPLAY_SOURCE,player_dir)
            for plugin in (player_dir/"xmp-archive-stream.dll",player_dir/"Plugins"/"xmp-archive-stream.dll"):
                plugin.unlink(missing_ok=True)
            for old in ("xmplay.pls","xmplay.library","xmplay.library~"):
                (player_dir/old).unlink(missing_ok=True)
            for n in ("Local","Roaming","Temp"):(folder/n).mkdir(exist_ok=True)
            full=folder/"full_native.zip"
            with zipfile.ZipFile(full,"w",zipfile.ZIP_STORED) as output:
                output.write(MP3,MP3.name)
            env=os.environ.copy()
            env.update(APPDATA=str(folder/"Roaming"),LOCALAPPDATA=str(folder/"Local"),
                       TEMP=str(folder/"Temp"),TMP=str(folder/"Temp"),
                       ARCHIVE_STREAM_ADAPTER_DEBUG="1")
            pid=None
            try:
                actual=run_launch(full,player=player_dir/"xmplay.exe",env=env)
                assert actual.returncode==0 and "no HTTP bridge was started" in actual.stdout and "handler=xmplay_builtin_zip" in actual.stdout,(actual.stdout,actual.stderr)
                match=re.search(r"XMPLAY_NATIVE_PLAYER_PID=(\d+)",actual.stdout)
                assert match,actual.stdout
                pid=int(match.group(1))
                win=native.wait_for(lambda:native.player_window(pid),20,"native builtin ZIP launcher GUI window")
                handle=win32api.OpenProcess(0x410,False,pid)
                try:executable=Path(win32process.GetModuleFileNameEx(handle,0))
                finally:handle.Close()
                assert executable.resolve()==(player_dir/"xmplay.exe").resolve(),executable
                win32gui.SetWindowPos(win,native.win32con.HWND_TOPMOST,0,0,0,0,
                      native.win32con.SWP_NOMOVE|native.win32con.SWP_NOSIZE|native.win32con.SWP_SHOWWINDOW)
                l,t,r,b=win32gui.GetWindowRect(win)
                native.mouse.move(coords=(l+30,t+180))
                time.sleep(5)
                first=native.progress(win,folder)
                time.sleep(4)
                next_count=native.progress(win,folder)
                assert 0<first<next_count<r-l-40,(first,next_count)
                native.mouse.click(coords=(l+int((r-l)*.73),t+56))
                native.mouse.move(coords=(l+30,t+180))
                time.sleep(3)
                jump=native.progress(win,folder)
                time.sleep(4)
                resume=native.progress(win,folder)
                assert jump>next_count+90 and resume>=jump+3,(first,next_count,jump,resume)
                debug=folder/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_xmplay.log"
                log=debug.read_text(encoding="utf-8-sig",errors="replace") if debug.is_file() else ""
                assert "LIST_OK" not in log and "STREAM_OK" not in log,log[-900:]
                print("XMPLAY_REAL_LAUNCHER_BUILTIN_ZIP_NO_PLUGIN_NO_HTTP_GUI_SEEK_PASS",
                      f"pixels={first}->{next_count}->{jump}->{resume}",flush=True)
            finally:
                if pid is not None:
                    subprocess.run(["taskkill","/PID",str(pid),"/T","/F"],capture_output=True,timeout=12)
                    time.sleep(.5)
    print("XMPLAY_NATIVE_ZIP_ELIGIBILITY_FIXTURES_CLEANED_PASS",flush=True)

if __name__=="__main__":main()
