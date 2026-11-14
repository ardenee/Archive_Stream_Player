r"""Real MediaMonkey 2024 portable host acceptance for Archive Stream Player.

The player is staged with [Portable] IsPortable=1 and an explicit database
under the disposable install. The installed player's profile is never copied.
Uses pywinauto on DEV, without requiring interaction with a personal library.

Run: py -3.13 mediamonkey_archive_stream/test_native_portable_codecs.py
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import json
import zipfile
from pathlib import Path

AUTOMATION = Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
if not (AUTOMATION / "pywinauto").is_dir():
    raise SystemExit("Missing DEV pywinauto runtime at " + str(AUTOMATION))
os.add_dll_directory(str(AUTOMATION / "pywin32_system32"))
sys.path[:0] = [str(AUTOMATION), str(AUTOMATION / "win32"),
                str(AUTOMATION / "win32" / "lib"), str(AUTOMATION / "Pythonwin")]
from pywinauto import Desktop, mouse

REPO = Path(__file__).resolve().parents[1]
PACKAGE = REPO / "mediamonkey_archive_stream" / "Release" / "ArchiveStreamPlayer-MediaMonkey5-Native.mmip"
INSTALLED = Path(r"C:\Program Files (x86)\MediaMonkey 5")
AUDIO = Path(r"C:\audiotest")
SOURCES = (
    ("MP3", "01.MP3File.mp3"),
    ("OGG", "02.OGGFile.ogg"),
    ("FLAC", "03.FlacFile.flac"),
    ("WAV", "04.WAVFile.wav"),
    ("WMA", "05.WMAFile.wma"),
    ("M4A", "06.M4AFile.m4a"),
    ("AAC", "07.AACFile.aac"),
)
if os.environ.get("AST_MM5_QA_ONLY"):
    keep = {x.strip().upper() for x in os.environ["AST_MM5_QA_ONLY"].split(",")}
    SOURCES = tuple(x for x in SOURCES if x[0] in keep)
    if not SOURCES:
        raise SystemExit("AST_MM5_QA_ONLY selected no known codecs")

def wait_for(get, timeout, what):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            v = get()
            if v:
                return v
        except Exception as exc:
            last = str(exc)
        time.sleep(.25)
    raise AssertionError(f"TIMEOUT {what}; last exception {last}")

def player_window(pid):
    return next((w for w in Desktop(backend="uia").windows()
                 if w.process_id() == pid and "MediaMonkey 2024" in w.window_text()), None)

def doc(window):
    for element in window.descendants():
        if element.element_info.control_type == "Document":
            contents = element.window_text()
            if "Playing" in contents:
                return contents
    return ""

def dismiss_first_run_wizard(window):
    # A fresh portable profile opens a modal wizard by default. Do not press
    # Next: it offers to scan the user's Music/Videos directories. Cancel
    # instead so QA never imports or alters personal media/library entries.
    def wizard_control():
        return next((c for c in window.descendants()
                     if c.element_info.control_type == "Window" and
                     c.window_text().strip() == "Setup Wizard"), None)
    wizard = wait_for(wizard_control, 40, "portable first-run Setup Wizard")
    cancel = next((c for c in wizard.descendants()
                   if c.element_info.control_type == "Button" and
                   c.window_text().strip().upper() == "CANCEL"), None)
    if not cancel:
        raise AssertionError("Fresh portable setup wizard has no safe Cancel button")
    cancel.click_input()
    wait_for(lambda: not wizard_control(), 25, "first-run wizard dismissal")
    print("MM5_PORTABLE_WIZARD_CANCELED_WITHOUT_MEDIA_SCAN",flush=True)


def elapsed(window, label):
    source = doc(window)
    if not source or label not in source:
        return -1
    # The player's footer is after the queue's Selected section, and positive
    # mm:ss occurs immediately before the currently playing title.
    footer = source.rsplit("Selected", 1)[-1]
    values = re.findall(r"(?<![-\d])(\d{2}):(\d{2})\s+" + re.escape(label), footer)
    if not values:
        return -1
    m,s = values[-1]
    return 60*int(m)+int(s)

def start_track(window, label):
    boundary = window.rectangle()
    controls = []
    for c in window.descendants():
        if c.element_info.control_type != "Text" or c.window_text().strip() != label or not c.is_visible():
            continue
        rect=c.rectangle()
        if rect.left > boundary.right - 350 and 66 < rect.top < boundary.bottom - 260:
            controls.append(c)
    if not controls:
        # MediaMonkey's compact Now Playing panel virtualizes offscreen rows.
        # Scroll only that panel, not the main library view.
        for _ in range(4):
            mouse.scroll(coords=(boundary.right-110, boundary.top+230), wheel_dist=-3)
            time.sleep(.25)
            controls=[c for c in window.descendants()
                      if c.element_info.control_type=="Text" and
                      c.window_text().strip()==label and c.is_visible() and
                      c.rectangle().left>boundary.right-350 and
                      66<c.rectangle().top<boundary.bottom-260]
            if controls:
                break
    if not controls:
        print("NO_VISIBLE_TRACK", label, "TAIL", repr(doc(window)[-500:]), flush=True)
        raise AssertionError("No visible MediaMonkey playlist row " + label)
    controls[0].double_click_input()
    print("SELECTED_CODEC", label, "row", str(controls[0].rectangle()), flush=True)

def main():
    for required in [PACKAGE, INSTALLED / "MediaMonkey.exe"] + [AUDIO / name for _,name in SOURCES]:
        if not required.is_file():
            raise SystemExit("Missing DEV fixture " + str(required))
    if subprocess.run(["tasklist", "/FI", "IMAGENAME eq MediaMonkey.exe"],
                      capture_output=True,text=True).stdout.count("MediaMonkey.exe"):
        raise SystemExit("Refusing host QA because MediaMonkey is already running.")
    # The existing roaming-profile database and options are private data.
    # Prove they are unchanged by this test's portable player lifecycle.
    roaming = Path(os.environ["APPDATA"]) / "MediaMonkey5"
    protected = [roaming / name for name in ("MM5.DB", "MediaMonkey.ini", "persistent.json")]
    def fingerprint(file):
        return hashlib.sha256(file.read_bytes()).hexdigest() if file.is_file() else None
    before_profile = {str(file): fingerprint(file) for file in protected}
    workspace = Path(tempfile.mkdtemp(prefix="AST-MM5-Portable-",dir=r"D:\Temp"))
    exe_dir = workspace / "MediaMonkey"
    process = None
    try:
        exe_dir.mkdir()
        copied=subprocess.run(["robocopy",str(INSTALLED),str(exe_dir),
                               "/E","/NFL","/NDL","/NJH","/NJS","/NP","/R:1","/W:1"],
                              capture_output=True,text=True)
        if copied.returncode >= 8:
            raise RuntimeError("Could not stage MediaMonkey: " + copied.stdout[-1000:])
        portable = exe_dir / "Portable"
        portable.mkdir(exist_ok=True)
        (portable / "MediaMonkey.ini").write_text(
            "[Portable]\nIsPortable=1\n[System]\nDBName="+str(portable / "MM5.DB")+"\n", encoding="utf-8")
        addons = exe_dir / "Scripts" / "archiveStreamPlayerNative"
        addons.mkdir(parents=True,exist_ok=True)
        with zipfile.ZipFile(PACKAGE) as mmip:
            mmip.extractall(addons)
        print("MM5_PORTABLE_PLAYER_AND_PLUGIN_STAGED",workspace,flush=True)

        archive_path=workspace/"host-7-codec.zip"
        with zipfile.ZipFile(archive_path,"w",zipfile.ZIP_STORED) as archive:
            for key,source in SOURCES:
                archive.write(AUDIO / source, f"AST_MM5_QA_{key}.{source.rsplit('.',1)[-1].lower()}")

        # Exact archive choice is injected only into this throwaway QA copy;
        # all production archive bridge + player queue code remains unchanged.
        # The real Windows picker itself passed separate host acceptance.
        script=addons/"actions_add.js"
        js=script.read_text(encoding="utf-8-sig")
        begin=js.index("const archive = await window.localPromise(app.utils.dialogOpenFile(")
        end=js.index("));",begin)+3
        js=js[:begin]+"const archive = "+json.dumps(str(archive_path))+";"+js[end:]
        # Keep a lightweight test-only trace to tell bridge setup failure from
        # native player queue rejection and host decode limitations.
        hook = ("function qaWrite(name, value){app.filesystem.saveTextToFileAsync("+
                json.dumps(str(workspace)+"\\qa-")+"+name+'.txt',String(value));}\n")
        js=hook+js
        js=js.replace("await app.player.addTracksAsync(tracks.list, {withClear: false, startPlayback: false});",
                      "await app.player.addTracksAsync(tracks.list, {withClear: false, startPlayback: false});\n"
                      "await qaWrite('queued',String(tracks.count));")
        js=js.replace("notice(error && error.message ? error.message : 'Archive selection or playback failed.');",
                      "await qaWrite('error',error && error.stack ? error.stack : String(error));\n"
                      "notice(error && error.message ? error.message : 'Archive selection or playback failed.');")
        # MediaMonkey loads addon scripts in multiple Chromium surfaces; an
        # on-disk marker ensures only the first initialized surface runs QA.
        marker=str(workspace / "qa-started.txt")
        js += ("\nsetTimeout(async () => { const marker = " + json.dumps(marker) +
               "; if (await app.filesystem.fileExistsAsync(marker)) return;" +
               " await app.filesystem.saveTextToFileAsync(marker, '1');" +
               " await actions.archiveStreamNativeAdd.execute(); }, 7000);\n")
        script.write_text(js,encoding="utf-8")
        for folder in ("Roaming","Local","Temp"):
            (workspace/folder).mkdir()
        env=os.environ.copy()
        env.update(APPDATA=str(workspace/"Roaming"), LOCALAPPDATA=str(workspace/"Local"),
                   TEMP=str(workspace/"Temp"), TMP=str(workspace/"Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        process=subprocess.Popen([str(exe_dir/"MediaMonkey.exe")],cwd=str(exe_dir),env=env)
        window=wait_for(lambda: player_window(process.pid),90,"portable MediaMonkey UI")
        dismiss_first_run_wizard(window)
        # Fail closed if MediaMonkey ignored its portable marker.
        db=wait_for(lambda: portable/"MM5.DB" if (portable/"MM5.DB").exists() else None,
                    30,"portable MediaMonkey database")
        print("MM5_PORTABLE_PROFILE_ISOLATION_PASS",db,flush=True)

        def ready():
            matches=list((workspace/"Temp").glob("ArchiveBridge-*/archive-stream.m3u8"))
            for file in matches:
                lines=file.read_text(encoding="utf-8-sig").splitlines()
                labels=[row.split(",",1)[1] for row in lines if row.startswith("#EXTINF:") and "," in row]
                if len(labels)==len(SOURCES) and all("AST_MM5_QA" in x for x in labels):
                    return file,labels,lines
            return None
        playlist,labels,lines=wait_for(ready,65,"native addon bridge playlist")
        wait_for(lambda: (workspace/"qa-queued.txt").is_file(),35,"native player queue acknowledgement")
        print("MM5_SEVEN_CODEC_NATIVE_QUEUE_ACK_PASS", (workspace/"qa-queued.txt").read_text(),flush=True)
        window.set_focus()
        window.maximize()
        playlist_out=portable / "mediamonkey.m3u8"
        wait_for(lambda: playlist_out.is_file() and all(label in
                         playlist_out.read_text(encoding="utf-8-sig",errors="replace")
                         for label in labels),25,"seven persisted portable playlist titles")
        print("MM5_SEVEN_CODEC_PORTABLE_QUEUE_TITLES_PASS",flush=True)
        view=doc(window)
        visible=[label for label in labels if label in view]
        print("MM5_VISIBLE_QUEUE_TITLES",visible,flush=True)
        if not visible:
            raise AssertionError("Native Now Playing view has no archive titles")

        results={}
        for key,label in zip((p[0] for p in SOURCES),labels):
            try:
                start_track(window,label)
                before=elapsed(window,label)
                time.sleep(4)
                after=elapsed(window,label)
                # Some codecs need loading time to open MF or resolve a header.
                if after < 2:
                    time.sleep(7)
                    after=elapsed(window,label)
                results[key]={"before":before,"after":after,"working":after>=2 and after>before}
                if os.environ.get("AST_MM5_VERIFY_GUI_SEEK") == "1" and results[key]["working"]:
                    bounds=window.rectangle()
                    # Only on the maximized portable QA player. This is the
                    # actual MediaMonkey GUI seek bar, not a bridge API seek.
                    window.set_focus()
                    print("MM5_GUI_SEEK_WINDOW_BOUNDS",str(bounds),flush=True)
                    attempts = []
                    for offset in (95, 93, 91, 89, 87, 84):
                        coord=(bounds.left + int(bounds.width()*.62),
                               bounds.bottom-offset)
                        mouse.click(coords=coord)
                        time.sleep(1)
                        observed=elapsed(window,label)
                        attempts.append({"offset":offset,"coord":coord,
                                         "position":observed})
                        if observed>=after+15:
                            break
                    jumped=elapsed(window,label)
                    time.sleep(3)
                    resumed=elapsed(window,label)
                    results[key]["gui_seek"]={"before":after,"jumped":jumped,
                        "resumed":resumed,"working":jumped>=after+15 and resumed>=jumped+2,
                        "attempts":attempts}
                    print("MM5_GUI_SEEK_RESULT",key,results[key]["gui_seek"],flush=True)
            except Exception as error:
                results[key]={"error":str(error),"working":False}
            print("MM5_CODEC_RESULT",key,results[key],flush=True)

        logfile=workspace/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_bridge.log"
        if logfile.exists():
            data=logfile.read_text(encoding="utf-8",errors="replace")
            print("MM5_BRIDGE_STREAM_COUNTS",
                  {key:len(re.findall(r"ROUTE_STREAM\s+.*index="+str(i),data))
                   for i,(key,_) in enumerate(SOURCES)},flush=True)
            print("MM5_BRIDGE_MEMORY_COUNT",data.count("ROUTE_MEMORY"),flush=True)
            print("MM5_BRIDGE_CACHE_COUNT",data.count("ROUTE_CACHE"),flush=True)
        else:
            print("MM5_BRIDGE_LOG_MISSING",flush=True)

        print("MM5_PORTABLE_CODEC_MATRIX",json.dumps(results),flush=True)
        if all(x.get("working") for x in results.values()):
            print("MM5_REAL_HOST_CODEC_PLAYBACK_PROGRESS_ALL_PASS",flush=True)
        else:
            raise AssertionError("Real MediaMonkey portable playback did not progress for every selected codec; consult per-codec results")
        if os.environ.get("AST_MM5_VERIFY_GUI_SEEK") == "1":
            failures = [name for name, test in results.items()
                        if not test.get("gui_seek", {}).get("working")]
            if failures:
                raise AssertionError("Real MediaMonkey GUI seek/resume failed: " + ", ".join(failures))
            print("MM5_REAL_HOST_ALL_SELECTED_CODEC_GUI_SEEK_RESUME_PASS",flush=True)
    finally:
        if process and process.poll() is None:
            subprocess.run(["taskkill","/PID",str(process.pid),"/T","/F"],
                           capture_output=True,timeout=25)
            try: process.wait(timeout=12)
            except subprocess.TimeoutExpired: process.kill()
        changed = [str(file) for file in protected
                   if fingerprint(file) != before_profile[str(file)]]
        if changed:
            print("MM5_ORIGINAL_PROFILE_CHANGED", changed, flush=True)
        else:
            print("MM5_ORIGINAL_PROFILE_HASHES_UNCHANGED_PASS", flush=True)
        if os.environ.get("AST_MM5_KEEP_QA") == "1":
            print("MM5_QA_WORKSPACE_RETAINED",workspace,flush=True)
        else:
            for attempt in range(8):
                try:
                    shutil.rmtree(workspace)
                    print("MM5_PORTABLE_QA_TEMP_REMOVED",flush=True)
                    break
                except OSError:
                    time.sleep(.6)
            else:
                print("MM5_PORTABLE_QA_CLEANUP_INCOMPLETE",workspace,flush=True)
        if changed:
            raise AssertionError("Portable QA touched the original MediaMonkey profile; inspect files before any further tests")

if __name__=="__main__":
    main()
