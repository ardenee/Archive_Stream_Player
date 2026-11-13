"""Real foobar2000 portable UI test: archive title field refresh in loaded playlist.
Uses an isolated fixture and a process-local settings file, not the user's registry.
"""
import os, sys, subprocess, struct, tempfile, time, wave, zipfile
from pathlib import Path
assert len(sys.argv)==2 and sys.argv[1] in ("x86","x64")
arch=sys.argv[1]
sdk=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
_dll=os.add_dll_directory(str(sdk/"pywin32_system32"))
sys.path[:0]=[str(sdk),str(sdk/"win32"),str(sdk/"win32"/"lib"),str(sdk/"Pythonwin")]
from pywinauto import Application
base=Path(r"D:\Temp\ArchiveStreamFoobar32Runtime") if arch=="x86" else Path(r"D:\Temp\ArchiveStreamFoobar64Runtime")
exe=base/"portable"/"foobar2000.exe"
assert exe.is_file()
def wait_for(report,want,timeout=22):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        text=report.read_text(encoding="utf-8",errors="replace") if report.is_file() else ""
        if want in text:
            return text
        time.sleep(.25)
    raise AssertionError(("No title refresh",want,text[-1000:]))
with tempfile.TemporaryDirectory(prefix="asp-foobar-title-",dir=r"D:\Temp") as tmp:
    root=Path(tmp)
    source=root/("ArchiveStreamTitleParity"+arch+".wav")
    with wave.open(str(source),"wb") as writer:
        writer.setparams((1,2,44100,0,"NONE","not compressed"))
        writer.writeframes(struct.pack("<h",9090)*44100)
    archive=root/("ArchiveStreamTitleParity"+arch+".zip")
    with zipfile.ZipFile(archive,"w") as z:z.write(source,source.name)
    preset=root/"title.txt"
    preset.write_text("<filename>\n",encoding="ascii")
    report=root/"refresh.txt"
    env=os.environ.copy()
    env.update(AST_FOOBAR_TITLE_TEST_INPUT=str(preset),
        AST_FOOBAR_TITLE_TEST_REPORT=str(report),
        AST_FOOBAR_TITLE_TEST_ARCHIVE_FILTER=archive.name)
    p=subprocess.Popen([str(exe)],cwd=str(exe.parent),env=env)
    app=None
    try:
        time.sleep(1.1)
        app=Application(backend="win32").connect(process=p.pid,timeout=12)
        subprocess.run([str(exe),"/command:Archive Stream: Add archive audio..."],
            cwd=str(exe.parent),timeout=12,check=True)
        picker=app.window(title="Archive Stream - Add Archive Audio")
        picker.wait("visible",timeout=12)
        picker.child_window(control_id=1148,class_name="Edit").set_edit_text(str(archive))
        picker.child_window(control_id=1,class_name="Button").click()
        initial=wait_for(report,source.name)
        assert "found=1" in initial,initial
        print(arch,"FOOBAR_TITLE_FIELD_PASS",source.name,flush=True)
        preset.write_text("<archivetype>: <#> - <filename>\n",encoding="ascii")
        updated=wait_for(report,"ZIP: 1 - "+source.name)
        assert "found=1" in updated,updated
        print(arch,"FOOBAR_LIVE_METADATA_REFRESH_PASS",flush=True)
    finally:
        if p.poll() is None:
            try:app.window(title_re="foobar2000 v2.*").close()
            except Exception:pass
            try:p.wait(timeout=7)
            except subprocess.TimeoutExpired:p.terminate();p.wait(timeout=7)
        print(arch,"PLAYER_EXIT",p.returncode,flush=True)
print(arch,"FOOBAR_NATIVE_TITLE_ACCEPTANCE_PASS",flush=True)
