"""Winamp 5.9 native password prompt test in an isolated cloned Winamp installation.
Uses a generated AES 7z WAV, a separate Winamp ini directory and LOCALAPPDATA.
Does not alter the installed player or user's saved password list.
"""
import os,sys,time,secrets,struct,subprocess,tempfile,shutil,wave,winreg,re,zipfile
from pathlib import Path
sdk=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
_dll=os.add_dll_directory(str(sdk/"pywin32_system32"))
sys.path[:0]=[str(sdk),str(sdk/"win32"),str(sdk/"win32"/"lib"),str(sdk/"Pythonwin")]
from pywinauto import Desktop
import win32gui,win32process,win32con
assert len(sys.argv) in (2,3,4) and sys.argv[1] in ("accept","cancel","codec")
mode=sys.argv[1]
host=sys.argv[2] if len(sys.argv)>=3 else "winamp"
codec=sys.argv[3].lower() if len(sys.argv)==4 else ""
assert host in ("winamp","wacup"),host
if mode=="codec":
    assert codec in ("mp3","wav","flac","ogg","wma","m4a","aac"),"Expected codec mp3|wav|flac|ogg|wma|m4a|aac"
winamp=Path(r"D:\Temp\ArchiveStreamPlayerAdapters\wacup-x86-test") if host=="wacup" else Path(r"C:\Program Files (x86)\Winamp")
seven=Path(r"C:\Program Files\7-Zip\7z.exe")
assert winamp.is_dir() and seven.is_file()

def wins(pid):
    out=[]
    def cb(hwnd,_):
        try:
            _,p=win32process.GetWindowThreadProcessId(hwnd)
            if p==pid and win32gui.IsWindowVisible(hwnd):
                out.append((hwnd,win32gui.GetWindowText(hwnd),win32gui.GetClassName(hwnd)))
        except Exception:pass
    win32gui.EnumWindows(cb,None)
    return out

with tempfile.TemporaryDirectory(prefix="asp-winamp-ui-",dir=r"D:\Temp") as temp:
    root=Path(temp)
    stage=root/("WACUP" if host=="wacup" else "Winamp")
    shutil.copytree(winamp,stage)
    rebuilt=Path(__file__).resolve().parent/"Release"/"in_archive_stream.dll"
    assert rebuilt.is_file(), "Build Release|Win32 input DLL on DEV before running"
    shutil.copy2(rebuilt,stage/"Plugins"/"in_archive_stream.dll")
    sink=Path(__file__).resolve().parent/"test_support"/"Release"/"out_archive_stream_test.dll"
    assert sink.is_file(),"Build the test-only Winamp output sink Release|Win32 before running"
    shutil.copy2(sink,stage/"Plugins"/"out_archive_stream_test.dll")
    if host=="wacup":
        config=stage/"WACUP_Profile"/"winamp.ini"
        assert config.is_file(),"WACUP portable profile is missing"
        contents=config.read_text(encoding="latin-1")
        contents,n=re.subn(r"(?m)^\[Winamp\]$", "[Winamp]\noutname=out_archive_stream_test.dll",contents,count=1)
        assert n==1,"Could not configure portable WACUP output sink"
        config.write_text(contents,encoding="latin-1")
    else:
        ini=root/"Ini"
        ini.mkdir()
        original_ini=Path(os.environ.get("APPDATA",""))/"Winamp"/"winamp.ini"
        if original_ini.is_file():
            shutil.copyfile(original_ini,ini/"winamp.ini")
        else:
            raise RuntimeError("No installed Winamp profile to seed the isolated test")
        contents=(ini/"winamp.ini").read_text(encoding="latin-1")
        contents,n=re.subn(r"(?m)^outname=.*$", "outname=out_archive_stream_test.dll",contents)
        assert n==1,"Could not configure the isolated Winamp output sink"
        (ini/"winamp.ini").write_text(contents,encoding="latin-1")
        (stage/"paths.ini").write_text("[Winamp]\ninidir="+str(ini)+"\n",encoding="ascii")
    env=os.environ.copy()
    env["LOCALAPPDATA"]=str(root/"Local")
    env["ARCHIVE_STREAM_ADAPTER_DEBUG"]="1"
    sink_report=root/"output-sink.txt"
    env["AST_WINAMP_TEST_SINK_REPORT"]=str(sink_report)
    env["APPDATA"]=str(root/"Roaming")
    (root/"Local").mkdir()
    (root/"Roaming").mkdir()
    key="WinampPrompt"+secrets.token_hex(10)
    if mode=="codec":
        fixtures={
            "mp3":"01.MP3File.mp3","ogg":"02.OGGFile.ogg",
            "flac":"03.FlacFile.flac","wav":"04.WAVFile.wav",
            "wma":"05.WMAFile.wma","m4a":"06.M4AFile.m4a",
            "aac":"07.AACFile.aac"
        }
        source=Path(r"C:\audiotest")/fixtures[codec]
        assert source.is_file(),source
        arch=root/"Codec.zip"
        with zipfile.ZipFile(arch,"w",compression=zipfile.ZIP_STORED) as z:
            z.write(source,source.name)
        track_name=source.name
    else:
        wav=root/"PromptTrack.wav"
        with wave.open(str(wav),"wb") as w:
            w.setparams((1,2,44100,0,"NONE","not compressed"))
            w.writeframes(struct.pack("<h",1200)*44100*30)
        arch=root/"PromptTrack.7z"
        made=subprocess.run([str(seven),"a","-t7z","-m0=LZMA2","-ms=off","-mhe=off",
            "-p"+key,str(arch),str(wav)],capture_output=True,text=True,timeout=18)
        assert made.returncode==0,(made.stdout,made.stderr)
        track_name=wav.name
    # Canonical format: archive.zip.archive_stream|0|name.mp3
    virtual=str(arch)+".archive_stream|0|"+track_name
    proc=subprocess.Popen([str(stage/("wacup.exe" if host=="wacup" else "winamp.exe")),virtual],cwd=str(stage),env=env)
    password_file=None
    before=None
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r"Software\ArchiveStreamPlayer\Adapters") as settings:
            password_file=Path(winreg.QueryValueEx(settings,"PasswordList")[0])
            before=password_file.read_bytes() if password_file.is_file() else None
    except (OSError,ValueError):pass
    events=[]
    try:
        deadline=time.monotonic()+32
        while time.monotonic()<deadline:
            if mode=="codec" and sink_report.is_file() and "WRITE_FIRST" in sink_report.read_text(encoding="ascii"):
                events.append("PCM_WRITTEN")
                break
            for hwnd,title,klass in wins(proc.pid):
                if title=="Archive Password":
                    assert mode!="codec","Unexpected password prompt for an unencrypted ZIP"
                    print("WINAMP_PASSWORD_PROMPT_VISIBLE",mode,flush=True)
                    descendants=Desktop(backend="win32").window(handle=hwnd).descendants()
                    if mode=="accept":
                        field=next(w for w in descendants if w.class_name()=="Edit")
                        field.set_edit_text(key)
                        win32gui.PostMessage(hwnd,win32con.WM_COMMAND,1,0)
                        events.append("PASSWORD_ENTERED")
                    else:
                        win32gui.PostMessage(hwnd,win32con.WM_COMMAND,2,0)
                        events.append("CANCELLED")
                elif title and ("Setup" in title or "First" in title or "License" in title):
                    if not any(e.startswith("SETUP") for e in events):
                        events.append("SETUP_BLOCKED")
                        print("WINAMP_STARTUP_BLOCKED",title,flush=True)
                    break
            if "SETUP_BLOCKED" in events:
                break
            if any(x in events for x in ("PASSWORD_ENTERED","CANCELLED","PCM_WRITTEN")):
                break
            if proc.poll() is not None:break
            time.sleep(.12)
        print("WINAMP_VISIBLE_WINDOWS",wins(proc.pid),flush=True)
        assert events,("WINAMP_PROMPT_NOT_OBSERVED",mode,proc.poll(),wins(proc.pid))
        time.sleep(3 if mode=="accept" else .9 if mode=="codec" else 1.5)
        assert proc.poll() is None,"Winamp quit during test"
        hwnd=win32gui.FindWindow("Winamp v1.x",None)
        assert hwnd,"Cannot locate real Winamp IPC main window"
        _,owned=win32process.GetWindowThreadProcessId(hwnd)
        assert owned==proc.pid,("IPC window owned by another Winamp instance",owned,proc.pid)
        isplaying=win32gui.SendMessage(hwnd,win32con.WM_USER,0,104)
        output_time=win32gui.SendMessage(hwnd,win32con.WM_USER,0,105)
        print("WINAMP_IPC_STATE",{"playing":isplaying,"output_ms":output_time},flush=True)
        if mode in ("accept","codec"):
            assert isplaying==1 and output_time>0,"Native Winamp archive PCM playback did not progress"
            report=sink_report.read_text(encoding="ascii") if sink_report.is_file() else ""
            assert "OPEN" in report and "WRITE_FIRST" in report,("Native PCM output sink never received decoded samples",report)
            print("WINAMP_SILENT_PCM_OUTPUT",report.strip().splitlines()[-6:],flush=True)
            target=18000
            result=win32gui.SendMessage(hwnd,win32con.WM_USER,target,106)
            samples=[]
            until=time.monotonic()+4.5
            while time.monotonic()<until:
                time.sleep(.3)
                sought=win32gui.SendMessage(hwnd,win32con.WM_USER,0,105)
                samples.append(sought)
                if sought>=17500:
                    break
            print("WINAMP_IPC_SEEK",{"request_ms":target,"result":result,"samples":samples},flush=True)
            log=root/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_winamp.log"
            if log.is_file():
                events_log=[x for x in log.read_text(encoding="utf-8",errors="replace").splitlines() if any(k in x for k in ("SEEK_REQUEST", "SEEK_APPLIED", "SEEK_FAIL"))]
                print("WINAMP_SEEK_EVENTS",[x.split("|")[2:5] for x in events_log[-5:]],flush=True)
            assert result==0 and samples and 17500<=samples[-1]<=24000,"Real Winamp seek to encrypted archive track failed"
            report=sink_report.read_text(encoding="ascii") if sink_report.is_file() else ""
            assert "FLUSH 18000 0" in report and report.count("WRITE_FIRST")>=2,("PCM output not flushed/resumed on seek",report)
            print("WINAMP_PCM_SINK_SEEK_VERIFIED",report.strip().splitlines()[-8:],flush=True)
        else:
            assert isplaying==0,"Winamp kept playing after password cancellation"
            report=sink_report.read_text(encoding="ascii") if sink_report.is_file() else ""
            assert "WRITE_FIRST" not in report,"Cancelled Winamp entry unexpectedly decoded PCM"
        if password_file is not None:
            if before is None:
                assert not password_file.exists(),"Winamp unexpectedly created password file"
            else:
                assert password_file.read_bytes()==before,"Winamp modified saved password list"
        print("PASS_WINAMP_NATIVE_ARCHIVE",host,mode,codec,events,flush=True)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=7)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=7)
        print("WINAMP_TEST_PROCESS_EXIT",host,proc.returncode,flush=True)
