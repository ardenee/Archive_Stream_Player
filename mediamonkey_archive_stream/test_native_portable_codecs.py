r"""Real MediaMonkey 2024 portable host acceptance for Archive Stream Player.

The player is staged with [Portable] IsPortable=1 and an explicit database
under the disposable install. The installed player's profile is never copied.
Uses pywinauto on DEV, without requiring interaction with a personal library.

Run: py -3.13 mediamonkey_archive_stream/test_native_portable_codecs.py
For actual encrypted host dialogs, set AST_MM5_QA_AES=accept|cancel|wrong
and AST_MM5_QA_ARCHIVE=7z|rar (default 7z). Enable GUI seek acceptance with
AST_MM5_VERIFY_GUI_SEEK=1. Native tests require a real decoder seek callback
and Media Foundation seek success; elapsed GUI clock movement alone is not
accepted as seeking. AST_MM5_QA_AUTOPLAY=1 starts an isolated single-track
queue directly through the player SDK when portable UI rows are inaccessible.
Set AST_MM5_QA_RAR_HEADERS=1 for first-use RAR5
encrypted filenames (with no saved password). Only the archive picker result
is substituted for the test; the packaged bridge and native queue run as-is.
Set AST_MM5_QA_RAR_MULTIPART=1 to create >=3 adjacent RAR5 volumes and
verify that the first .part1.rar file decodes across their boundaries.
Set AST_MM5_QA_RAR_SOLID=1 for a two-track solid RAR5 fixture; the test
plays and seeks both archive entries, including the second solid item.
For real native encrypted RAR5 AAC, AST_MM5_QA_SDK_SEEK=1 performs an
isolated seekMSAsync(150000) via the supported MediaMonkey Player SDK, and
requires actual native decoder seek/Media Foundation return and resumed clock;
AST_MM5_VERIFY_GUI_SEEK=1 checks actual visible UIA seek-bar clicking instead.
AST_MM5_QA_INJECT_DURATION=1 tests a duration override in the disposable
playlist only (requires a local ffprobe executable); production is unchanged.
AST_MM5_QA_DIAG_UI=1 logs bounded, non-secret UI/seek-bar and dialog traces.
No persistent personal player database or password file is read or changed.
"""
import hashlib
import ctypes
import os
import secrets
import threading
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
import win32gui
import win32process
import win32api
import win32con

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
AES_MODE = os.environ.get("AST_MM5_QA_AES", "").lower()
ARCHIVE_KIND = os.environ.get("AST_MM5_QA_ARCHIVE", "7z").lower()
RAR_HEADERS = os.environ.get("AST_MM5_QA_RAR_HEADERS", "0") == "1"
RAR_MULTIPART = os.environ.get("AST_MM5_QA_RAR_MULTIPART", "0") == "1"
RAR_SOLID = os.environ.get("AST_MM5_QA_RAR_SOLID", "0") == "1"
if RAR_SOLID and (ARCHIVE_KIND != "rar" or not AES_MODE):
    raise SystemExit("AST_MM5_QA_RAR_SOLID requires encrypted RAR5 host QA")
if RAR_MULTIPART and (ARCHIVE_KIND != "rar" or not AES_MODE):
    raise SystemExit("AST_MM5_QA_RAR_MULTIPART requires AST_MM5_QA_AES and AST_MM5_QA_ARCHIVE=rar")
TITLE_REFRESH_QA = os.environ.get("AST_MM5_QA_TITLE_REFRESH", "0") == "1"
NATIVE_INPUT_QA = os.environ.get("AST_MM5_QA_NATIVE", "0") == "1"
TEST_SAVE_CONSENT = os.environ.get("AST_MM5_QA_SAVE", "0") == "1"
if TEST_SAVE_CONSENT and (not RAR_HEADERS or AES_MODE != "accept"):
    raise SystemExit("AST_MM5_QA_SAVE requires RAR encrypted headers and accepted password")
if RAR_HEADERS and (ARCHIVE_KIND != "rar" or not AES_MODE):
    raise SystemExit("AST_MM5_QA_RAR_HEADERS requires AST_MM5_QA_AES and AST_MM5_QA_ARCHIVE=rar")
if ARCHIVE_KIND not in ("7z", "rar"):
    raise SystemExit("AST_MM5_QA_ARCHIVE must be 7z or rar")
if AES_MODE:
    if AES_MODE not in ("accept", "cancel", "wrong"):
        raise SystemExit("AST_MM5_QA_AES must be accept, cancel, or wrong")
    if RAR_SOLID:
        SOURCES = (("MP3", "01.MP3File.mp3"), ("AAC", "07.AACFile.aac"))
    elif os.environ.get("AST_MM5_QA_AES_CODEC","AAC").upper() == "MP3":
        SOURCES = (("MP3", "01.MP3File.mp3"),)
    else:
        SOURCES = (("AAC", "07.AACFile.aac"),)

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
                     c.window_text().strip() == "Setup Wizard" and c.is_visible()), None)
    wait_for(wizard_control, 40, "portable first-run Setup Wizard")
    # Chromium renders the wizard shell before its buttons. Requery the
    # controls until a visible Cancel appears; do not click Next/Scan.
    def cancel_control():
        wizard=wizard_control()
        if not wizard:
            return None
        return next((c for c in wizard.descendants()
                     if c.element_info.control_type == "Button" and c.is_visible() and
                     c.window_text().replace("&","").strip().upper() == "CANCEL"), None)
    cancel = wait_for(cancel_control, 30, "portable first-run safe Cancel button")
    try:
        cancel.invoke()
    except Exception:
        cancel.click_input()
    try:
        wait_for(lambda: not wizard_control(), 8, "first-run wizard dismissal")
    except AssertionError:
        # First-run UI sometimes animates its cancellation and exposes a
        # second modal. Diagnose only visible controls; never click Next,
        # Scan or Finish since they could import personal media folders.
        print("MM5_FIRST_RUN_CANCEL_STILL_VISIBLE",
              [(c.element_info.control_type,c.window_text()[:100],str(c.rectangle()))
               for c in window.descendants()
               if c.is_visible() and c.element_info.control_type in ("Window","Button")][-30:],
              flush=True)
        print("MM5_FIRST_RUN_WINDOWS",
              [(w.window_text(),w.class_name()) for w in Desktop(backend="uia").windows()
               if w.process_id()==window.process_id() and w.is_visible()],flush=True)
        # Retry the same safe Cancel action if the setup panel is still open.
        wizard=wizard_control()
        if wizard:
            buttons=[c for c in wizard.descendants() if c.element_info.control_type=="Button"
                     and c.window_text().strip().upper()=="CANCEL" and c.is_visible()]
            if buttons:
                try:
                    buttons[-1].invoke()
                except Exception:
                    buttons[-1].click_input()
        wait_for(lambda: not wizard_control(), 12, "first-run wizard dismissal after retry")
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

def handle_qa_password_windows(workspace, password, mode, events, stop, save_consent=False):
    """Operate only native/bridge password windows owned by this throwaway MM5 process."""
    events.append("QA_PASSWORD_MONITOR_STARTED")
    while not stop.is_set():
        windows = []
        def collect(hwnd, unused):
            if win32gui.IsWindowVisible(hwnd):
                title = win32gui.GetWindowText(hwnd)
                if title in ("Archive Stream Player - Password Required", "Archive Stream Player") or (NATIVE_INPUT_QA and title == "Archive Password"):
                    windows.append((hwnd, title))
        try:
            win32gui.EnumWindows(collect, None)
            if NATIVE_INPUT_QA:
                # Winamp's modal prompt may be an owned popup that the normal
                # top-level enumeration omits. Its debug log records the HWND,
                # which must still be verified to belong to this QA process.
                trace=workspace/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_winamp.log"
                if trace.is_file():
                    if "QA_PASSWORD_TRACE_PRESENT" not in events:
                        events.append("QA_PASSWORD_TRACE_PRESENT")
                    for raw in re.findall(r"NATIVE_PROMPT_CREATED[^\n]*hwnd=([0-9A-Fa-f]+)",trace.read_text(encoding="utf-8-sig",errors="replace")):
                        if "QA_PASSWORD_TRACE_HWND_MATCH" not in events:
                            events.append("QA_PASSWORD_TRACE_HWND_MATCH")
                        hwnd=int(raw,16)
                        if win32gui.IsWindow(hwnd) and (hwnd,"Archive Password") not in windows:
                            windows.append((hwnd,"Archive Password"))
                            if "NATIVE_DIALOG_LOG_HANDLE_FOUND" not in events:
                                events.append("NATIVE_DIALOG_LOG_HANDLE_FOUND")
            for hwnd, title in windows:
                try:
                    _, pid = win32process.GetWindowThreadProcessId(hwnd)
                    process_handle = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION | win32con.PROCESS_VM_READ, False, pid)
                    try:
                        exe = Path(win32process.GetModuleFileNameEx(process_handle, 0))
                    finally:
                        process_handle.Close()
                    isolated_owner = str(exe).lower().startswith(str(workspace).lower())
                    if title == "Archive Password" and "QA_NATIVE_WINDOW_OWNER_CHECKED" not in events:
                        events.append("QA_NATIVE_WINDOW_OWNER_CHECKED")
                        events.append("QA_NATIVE_OWNER_ISOLATED_" + str(isolated_owner))
                        events.append("QA_NATIVE_OWNER_EXE_" + exe.name.lower())
                    # Real MM5 may host Winamp-compatible decoders in a child
                    # executable. For this specific native HWND, the strictly
                    # sandbox-local process path is the ownership boundary.
                    if not isolated_owner or (title != "Archive Password" and exe.name.lower() != "archive_player_bridge.exe"):
                        continue
                    dialog = Desktop(backend="win32").window(handle=hwnd)
                    if title == "Archive Password":
                        if mode == "cancel":
                            next(c for c in dialog.descendants() if c.window_text() in ("Cancel","&Cancel")).click()
                            events.append("PASSWORD_CANCELED")
                        else:
                            field = next((c for c in dialog.descendants() if c.class_name() == "Edit"),None)
                            if field is None:
                                continue
                            field.set_edit_text(password if mode == "accept" else password + "-INVALID")
                            next(c for c in dialog.descendants() if c.window_text() == "OK").click()
                            events.append("PASSWORD_ENTERED" if mode == "accept" else "PASSWORD_WRONG_ENTERED")
                        continue
                    if title == "Archive Stream Player - Password Required":
                        if mode != "cancel":
                            edits = [c for c in dialog.descendants() if c.class_name() == "Edit"]
                            if not edits:
                                continue
                            edits[-1].set_edit_text(password if mode == "accept" else password + "-INVALID")
                            next(c for c in dialog.descendants() if c.window_text() == "OK").click()
                            events.append("PASSWORD_ENTERED" if mode == "accept" else "PASSWORD_WRONG_ENTERED")
                        else:
                            next(c for c in dialog.descendants() if c.window_text() in ("Cancel", "&Cancel")).click()
                            events.append("PASSWORD_CANCELED")
                    elif title == "Archive Stream Player":
                        choice = ("Yes", "&Yes") if save_consent else ("No", "&No")
                        action = next((c for c in dialog.descendants() if c.window_text() in choice), None)
                        if action:
                            action.click()
                            events.append("SAVE_APPROVED" if save_consent else "SAVE_DECLINED")
                except Exception as exc:
                    events.append("PROMPT_HANDLER_RETRY_" + type(exc).__name__)
        except Exception as exc:
            events.append("PROMPT_ENUM_RETRY_" + type(exc).__name__)
        stop.wait(.15)

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
    ui_stop = threading.Event()
    ui_thread = None
    password_events = []
    synthetic_password = ""
    isolated_password_list = None
    try:
        exe_dir.mkdir()
        copied=subprocess.run(["robocopy",str(INSTALLED),str(exe_dir),
                               "/E","/NFL","/NDL","/NJH","/NJS","/NP","/R:1","/W:1"],
                              capture_output=True,text=True)
        if copied.returncode >= 8:
            raise RuntimeError("Could not stage MediaMonkey: " + copied.stdout[-1000:])
        portable = exe_dir / "Portable"
        if NATIVE_INPUT_QA:
            native_dll=REPO/"in_archive_stream"/"Release"/"in_archive_stream.dll"
            assert native_dll.is_file(),native_dll
            shutil.copy2(native_dll,exe_dir/"Plugins"/"in_archive_stream.dll")
            print("MM5_NATIVE_MATCHING_DECODER_STAGED",flush=True)
        portable.mkdir(exist_ok=True)
        (portable / "MediaMonkey.ini").write_text(
            "[Portable]\nIsPortable=1\n[System]\nDBName="+str(portable / "MM5.DB")+"\n", encoding="utf-8")
        addons = exe_dir / "Scripts" / "archiveStreamPlayerNative"
        addons.mkdir(parents=True,exist_ok=True)
        with zipfile.ZipFile(PACKAGE) as mmip:
            mmip.extractall(addons)
        print("MM5_PORTABLE_PLAYER_AND_PLUGIN_STAGED",workspace,flush=True)

        archive_path=workspace/"host-7-codec.zip"
        if AES_MODE:
            synthetic_password="MM5HostQA-"+secrets.token_hex(18)
            isolated_password_list=workspace/"test-empty-password-list.txt"
            isolated_password_list.write_bytes(b"")
            source_files=[]
            for codec, fixture in SOURCES:
                source=workspace/("AST_MM5_QA_"+codec+"."+fixture.rsplit(".",1)[-1].lower())
                shutil.copyfile(AUDIO/fixture,source)
                source_files.append(str(source.name))
            archive_path=workspace/("host-encrypted-nonsolid."+ARCHIVE_KIND)
            if ARCHIVE_KIND == "7z":
                binary=Path(r"C:\Program Files\7-Zip\7z.exe")
                arguments=[str(binary),"a","-t7z","-m0=LZMA2","-ms=off",
                           "-mhe=off","-p"+synthetic_password,str(archive_path),*source_files]
            else:
                binary=Path(r"C:\Program Files\WinRAR\Rar.exe")
                arguments=[str(binary),"a","-ma5","-m3" if RAR_SOLID else "-m0",
                           "-s" if RAR_SOLID else "-s-","-ep",
                           *([] if not RAR_MULTIPART else ["-v1m"]),
                           ("-hp" if RAR_HEADERS else "-p")+synthetic_password,
                           str(archive_path),*source_files]
            if not binary.is_file():
                raise AssertionError("Missing DEV synthetic encrypted fixture builder")
            build=subprocess.run(arguments,cwd=workspace,capture_output=True,text=True,timeout=70)
            if RAR_MULTIPART:
                archive_path=archive_path.with_suffix(".part1.rar")
                volumes=sorted(workspace.glob("host-encrypted-nonsolid.part*.rar"))
                if len(volumes)<3:
                    raise AssertionError("Expected at least three synthetic RAR5 volumes")
                print("MM5_RAR_ENCRYPTED_MULTIPART_VOLUMES_STAGED",len(volumes),flush=True)
            if build.returncode != 0 or not archive_path.is_file():
                raise AssertionError("Failed to create synthetic encrypted "+ARCHIVE_KIND+": "+build.stderr[-200:])
            print("MM5_SYNTHETIC_AES_"+ARCHIVE_KIND.upper()+"_TEST_FIXTURE_READY",
                  "encrypted_headers="+str(RAR_HEADERS),"multipart="+str(RAR_MULTIPART),
                  "solid="+str(RAR_SOLID),flush=True)
            launcher=addons/"launch-mediamonkey.ps1"
            power=launcher.read_text(encoding="utf-8-sig")
            match="$start.Arguments = '--archive \"' + $Archive + '\" --lifetime 7200'"
            if match not in power:
                raise AssertionError("MM5 launcher changed; cannot safely isolate synthetic password list")
            isolated_args=match + " + ' --password-list \""+str(isolated_password_list)+"\" --prompt-password 1 --memory-mb 16 --cache-limit-mb 0'"
            launcher.write_text(power.replace(match,isolated_args,1),encoding="utf-8")
        else:
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
        if os.environ.get("AST_MM5_QA_INJECT_DURATION") == "1":
            # Disposable host A/B test only. Read the original fixture duration
            # independently: do NOT guess it, alter production M3U8, or decode
            # encrypted archive audio while generating the playlist.
            if not (NATIVE_INPUT_QA and AES_MODE == "accept" and len(SOURCES) == 1 and SOURCES[0][0] == "AAC"):
                raise AssertionError("QA duration injection requires a single native encrypted AAC entry")
            ffprobe=Path(r"C:\Program Files\AudioBookConverter\app\external\ffprobe.exe")
            duration_probe=subprocess.run([str(ffprobe),"-v","error","-show_entries","format=duration",
                "-of","default=noprint_wrappers=1:nokey=1",str(AUDIO/SOURCES[0][1])],
                capture_output=True,text=True,timeout=15)
            assert duration_probe.returncode == 0,duration_probe.stderr
            verified_seconds=int(float(duration_probe.stdout.strip()) + .5)
            assert 20 < verified_seconds < 86400,verified_seconds
            duration_anchor="const playlist = await app.filesystem.loadTextFromFileAsync(manifest.playlist);"
            assert js.count(duration_anchor) == 1,"Unexpected native playlist read operation"
            injected="const playlist = (await app.filesystem.loadTextFromFileAsync(manifest.playlist)).replace('#EXTINF:-1,', '#EXTINF:"+str(verified_seconds)+",');"
            js=js.replace(duration_anchor,injected,1)
            print("MM5_QA_ONLY_VERIFIED_AAC_PLAYLIST_DURATION_INJECTED",verified_seconds,"source=original_aac_fixture",flush=True)
        js=js.replace("await app.player.addTracksAsync(tracks.list, {withClear: false, startPlayback: false});",
                      "await app.player.addTracksAsync(tracks.list, {withClear: false, startPlayback: false});\n"
                      "await qaWrite('queued',String(tracks.count));")
        if NATIVE_INPUT_QA and os.environ.get("AST_MM5_QA_SDK_SEEK") == "1":
            # A/B diagnosis only: use MediaMonkey\'s documented player API
            # without touching the GUI seek bar or production add-on logic.
            seek_marker="await qaWrite(\'queued\',String(tracks.count));"
            assert js.count(seek_marker)==1,"Missing isolated queue acknowledgement"
            seek_action=(seek_marker+"\nsetTimeout(async () => { try { "+
                "const before=app.player.trackPositionMS; "+
                "await app.player.seekMSAsync(150000); "+
                "await qaWrite(\'sdk-seek-result\',JSON.stringify({before:before,after:app.player.trackPositionMS,target:150000})); "+
                "} catch(error) { await qaWrite(\'sdk-seek-result\',JSON.stringify({error:String(error)})); } }, 35000);")
            js=js.replace(seek_marker,seek_action,1)
            print("MM5_QA_ONLY_NATIVE_SDK_SEEK_ARMED",150000,flush=True)
        if NATIVE_INPUT_QA and os.environ.get("AST_MM5_QA_AUTOPLAY") == "1" and len(SOURCES) == 1:
            # Use the documented player queue SDK to begin playback directly.
            # Some fresh portable MM5 surfaces virtualize all accessible row
            # controls even though the track is present in Now Playing.
            old_queue="await app.player.addTracksAsync(tracks.list, {withClear: false, startPlayback: false});"
            assert old_queue in js,"Cannot find isolated MM5 queue SDK call"
            js=js.replace(old_queue,old_queue.replace("startPlayback: false","startPlayback: true"),1)
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
        if (TITLE_REFRESH_QA or os.environ.get("AST_MM5_QA_INJECT_DURATION") == "1" or
                os.environ.get("AST_MM5_QA_SDK_SEEK") == "1"):
            syntax=subprocess.run(["node","--check",str(script)],capture_output=True,text=True,timeout=12)
            if syntax.returncode:
                raise AssertionError("Staged QA title refresh JavaScript invalid: "+syntax.stderr[-1400:])
        for folder in ("Roaming","Local","Temp"):
            (workspace/folder).mkdir()
        env=os.environ.copy()
        env.update(APPDATA=str(workspace/"Roaming"), LOCALAPPDATA=str(workspace/"Local"),
                   TEMP=str(workspace/"Temp"), TMP=str(workspace/"Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        if AES_MODE:
            ui_thread=threading.Thread(target=handle_qa_password_windows,
                      args=(workspace,synthetic_password,AES_MODE,password_events,ui_stop,TEST_SAVE_CONSENT),daemon=True)
            ui_thread.start()
            print("MM5_PASSWORD_MONITOR_STARTED",ui_thread.is_alive(),flush=True)
        process=subprocess.Popen([str(exe_dir/"MediaMonkey.exe")],cwd=str(exe_dir),env=env)
        window=wait_for(lambda: player_window(process.pid),90,"portable MediaMonkey UI")
        dismiss_first_run_wizard(window)
        # Fail closed if MediaMonkey ignored its portable marker.
        db=wait_for(lambda: portable/"MM5.DB" if (portable/"MM5.DB").exists() else None,
                    30,"portable MediaMonkey database")
        print("MM5_PORTABLE_PROFILE_ISOLATION_PASS",db,flush=True)

        if RAR_HEADERS and AES_MODE in ("cancel", "wrong"):
            expected = "PASSWORD_CANCELED" if AES_MODE == "cancel" else "PASSWORD_WRONG_ENTERED"
            wait_for(lambda: expected if expected in password_events else None, 55,
                     "encrypted filename first-use credential dialog")
            wait_for(lambda: workspace/"qa-error.txt" if (workspace/"qa-error.txt").is_file() else None,
                     40, "native add-on rejection of inaccessible encrypted RAR filenames")
            if (workspace/"qa-queued.txt").exists():
                raise AssertionError("Native queue unexpectedly accepted encrypted RAR header after rejected password")
            if isolated_password_list.read_bytes() != b"":
                raise AssertionError("Rejected header password was saved to disk")
            if list((workspace/"Temp").glob("ArchiveBridge-*/archive-stream.m3u8")):
                raise AssertionError("Encrypted RAR headers exposed a playlist after rejected password")
            print("MM5_RAR_ENCRYPTED_HEADER_FIRST_USE_"+AES_MODE.upper()+"_FAIL_CLOSED_PASS",flush=True)
            return

        def ready():
            matches=(list((workspace/"Temp").glob("ArchiveStreamMM5Native-*/archive-stream-native.m3u8"))
                     if NATIVE_INPUT_QA else
                     list((workspace/"Temp").glob("ArchiveBridge-*/archive-stream.m3u8")))
            for file in matches:
                lines=file.read_text(encoding="utf-8-sig").splitlines()
                labels=[row.split(",",1)[1] for row in lines if row.startswith("#EXTINF:") and "," in row]
                if len(labels)==len(SOURCES) and all("AST_MM5_QA" in x for x in labels):
                    return file,labels,lines
            return None
        playlist,labels,lines=wait_for(ready,65,"native addon bridge playlist")
        wait_for(lambda: (workspace/"qa-queued.txt").is_file(),35,"native player queue acknowledgement")
        print("MM5_SELECTED_CODEC_NATIVE_QUEUE_ACK_PASS", (workspace/"qa-queued.txt").read_text(),flush=True)
        if TEST_SAVE_CONSENT:
            if "SAVE_APPROVED" not in password_events:
                raise AssertionError("Header password save consent dialog was not confirmed")
            if isolated_password_list.read_bytes() != b"":
                raise AssertionError("Header password saved before any audio bytes were decoded")
            print("MM5_RAR_HEADER_PASSWORD_SAVE_DEFERRED_UNTIL_DATA_PASS",flush=True)
        window.set_focus()
        window.maximize()
        playlist_out=portable / "mediamonkey.m3u8"
        wait_for(lambda: playlist_out.is_file() and all(label in
                         playlist_out.read_text(encoding="utf-8-sig",errors="replace")
                         for label in labels),25,"seven persisted portable playlist titles")
        print("MM5_SELECTED_CODEC_PORTABLE_QUEUE_TITLES_PASS",flush=True)
        view=doc(window)
        visible=[label for label in labels if label in view]
        if not visible and not (TITLE_REFRESH_QA and "AST_QA_TITLE_REFRESHED" in view):
            # A fresh portable Chromium surface can acknowledge the SDK queue
            # before its Now Playing accessibility text has been populated.
            # Wait for it, without selecting tracks or altering the real profile.
            for _ in range(16):
                time.sleep(.5)
                view=doc(window)
                visible=[label for label in labels if label in view]
                if visible:
                    break
        print("MM5_VISIBLE_QUEUE_TITLES",visible,flush=True)
        if not visible and not (TITLE_REFRESH_QA and "AST_QA_TITLE_REFRESHED" in view):
            print("MM5_NATIVE_QUEUE_ACCESSIBILITY_DELAY_TAIL",repr(view[-500:]),flush=True)
            raise AssertionError("Native Now Playing view has no archive titles")
        if TITLE_REFRESH_QA:
            # Exercise the production background refresh by atomically replacing
            # the real bridge playlist as a changed title preset would do.
            # Do not inject JavaScript metadata changes into the QA addon.
            original=playlist.read_text(encoding="utf-8-sig")
            updated=original
            replacements=[]
            for (key,_),previous_title in zip(SOURCES,labels):
                refreshed_title="AST_QA_REFRESHED_"+key
                needle=","+previous_title+"\n"
                if updated.count(needle)!=1:
                    raise AssertionError("Expected exactly one bridge title to refresh: "+key)
                updated=updated.replace(needle,","+refreshed_title+"\n",1)
                replacements.append(refreshed_title)
            draft=playlist.with_name(playlist.name+".qa-update")
            draft.write_text(updated,encoding="utf-8")
            os.replace(draft,playlist)
            if len(SOURCES)==1:
                wait_for(lambda: replacements[0] in doc(window),55,
                         "production single-entry in-place title repaint")
                labels[:]=replacements
                print("MM5_REAL_HOST_QUEUED_ARCHIVE_INPLACE_TITLE_REFRESH_PASS",
                      ",".join(key for key,_ in SOURCES),flush=True)
            else:
                # Host can invalidate decoder references when metadata on
                # multiple queued rows is rewritten. The production guard
                # must preserve all original row titles and their playback.
                time.sleep(8)
                view_after=doc(window)
                if replacements[0] in view_after or labels[0] not in view_after:
                    raise AssertionError("Multi-entry refresh unexpectedly mutated queued tracks")
                print("MM5_MULTI_ENTRY_TITLE_REFRESH_BLOCKED_FOR_PLAYBACK_SAFETY_PASS",
                      len(SOURCES),flush=True)

        results={}
        for key,label in zip((p[0] for p in SOURCES),labels):
            try:
                if NATIVE_INPUT_QA and os.environ.get("AST_MM5_QA_AUTOPLAY") == "1" and len(SOURCES) == 1:
                    print("MM5_NATIVE_SINGLE_TRACK_SDK_AUTOPLAY_SELECTED",label,flush=True)
                else:
                    start_track(window,label)
                if AES_MODE:
                    deadline=time.monotonic()+32
                    while not any(event in password_events for event in ("PASSWORD_ENTERED","PASSWORD_CANCELED","PASSWORD_WRONG_ENTERED")) and time.monotonic()<deadline:
                        time.sleep(.18)
                    print("MM5_ENCRYPTED_PLAYER_UI_EVENTS",password_events,flush=True)
                    if AES_MODE in ("cancel","wrong"):
                        time.sleep(3)
                        rejected_position=elapsed(window,label)
                        if rejected_position > 0:
                            raise AssertionError("Rejected password unexpectedly advanced playback")
                        results[key]={"working":False,"rejected_position":rejected_position}
                        print("MM5_ENCRYPTED_INVALID_CREDENTIAL_PLAYBACK_REJECTED",results[key],flush=True)
                        continue
                    if "PASSWORD_ENTERED" not in password_events:
                        raise AssertionError("Encrypted playback did not display its native credential dialog")
                    time.sleep(4)
                before=elapsed(window,label)
                time.sleep(4)
                after=elapsed(window,label)
                # Some codecs need loading time to open MF or resolve a header.
                if after < 2:
                    time.sleep(7)
                    after=elapsed(window,label)
                if TITLE_REFRESH_QA and after < 2:
                    # MM5 may consume the first double-click to focus the
                    # dynamically repainted Now Playing panel rather than play.
                    window.set_focus()
                    start_track(window,label)
                    time.sleep(5)
                    after=elapsed(window,label)
                    print("MM5_REFRESHED_ROW_RESELECT_RESULT",key,after,flush=True)
                results[key]={"before":before,"after":after,"working":after>=2 and after>before}
                if os.environ.get("AST_MM5_VERIFY_GUI_SEEK") == "1" and results[key]["working"]:
                    # MediaMonkey can restore the parent window when a native
                    # password dialog closes. Restore maximized geometry before
                    # selecting the actual host seek bar, otherwise a click may
                    # merely refocus the UI and natural time looks like a seek.
                    # pywin32 win32gui does not expose IsZoomed in all builds.
                    # Ask the Win32 window manager through ctypes instead.
                    if not ctypes.windll.user32.IsZoomed(int(window.handle)):
                        window.maximize()
                    window.set_focus()
                    time.sleep(.3)
                    bounds=window.rectangle()
                    print("MM5_GUI_SEEK_WINDOW_BOUNDS",str(bounds),flush=True)
                    if os.environ.get("AST_MM5_QA_DIAG_UI") == "1":
                        foreground=win32gui.GetForegroundWindow()
                        print("MM5_QA_TIMELINE_FOCUS",{"main_hwnd":window.handle,"foreground_hwnd":foreground,
                            "foreground_title":win32gui.GetWindowText(foreground),"main_enabled":win32gui.IsWindowEnabled(window.handle)},flush=True)
                        controls=[]
                        for control in window.descendants():
                            try:
                                rect=control.rectangle()
                                typ=control.element_info.control_type
                                if control.is_visible() and (typ in ("Slider","ProgressBar") or
                                    rect.top >= bounds.bottom-135 and typ in ("Text","Button")):
                                    controls.append((typ,control.window_text()[:50],str(rect),control.is_enabled()))
                            except Exception:
                                continue
                            if len(controls)>=65:
                                break
                        print("MM5_QA_TIMELINE_CONTROLS",controls,flush=True)
                    # Click the real accessible timeline, not a guessed offset
                    # from the window bottom. The seek bar can be hundreds of
                    # pixels higher in MediaMonkey's large-screen layout.
                    timeline=[]
                    for control in window.descendants():
                        try:
                            if (control.element_info.control_type=="Slider" and
                                control.window_text().strip().lower()=="seek bar" and
                                control.is_visible() and control.is_enabled()):
                                timeline.append(control)
                        except Exception:
                            continue
                    if len(timeline)!=1:
                        raise AssertionError("Expected one visible enabled MediaMonkey seek bar, found "+str(len(timeline)))
                    seek_bounds=timeline[0].rectangle()
                    if seek_bounds.width()<=20 or seek_bounds.height()<=0:
                        raise AssertionError("Accessible MediaMonkey seek bar has invalid geometry: "+str(seek_bounds))
                    coord=(seek_bounds.left+int(seek_bounds.width()*.62),
                           seek_bounds.top+seek_bounds.height()//2)
                    print("MM5_GUI_SEEK_ACCESSIBLE_SLIDER",str(seek_bounds),"click",coord,flush=True)
                    seek_started=time.monotonic()
                    mouse.click(coords=coord)
                    time.sleep(1)
                    jumped=elapsed(window,label)
                    attempts=[{"source":"accessible_seek_bar","coord":coord,"position":jumped}]
                    time.sleep(3)
                    resumed=elapsed(window,label)
                    results[key]["gui_seek"]={"before":after,"jumped":jumped,
                        "resumed":resumed,
                        "working":jumped>=after+max(15,int(time.monotonic()-seek_started)+12) and resumed>=jumped+2,
                        "attempts":attempts}
                    print("MM5_GUI_SEEK_RESULT",key,results[key]["gui_seek"],flush=True)
            except Exception as error:
                results[key]={"error":str(error),"working":False}
            print("MM5_CODEC_RESULT",key,results[key],flush=True)

        if NATIVE_INPUT_QA and os.environ.get("AST_MM5_QA_SDK_SEEK") == "1":
            sdk_result=wait_for(lambda: workspace/"qa-sdk-seek-result.txt" if (workspace/"qa-sdk-seek-result.txt").is_file() else None,55,
                                "isolated MM5 app.player.seekMSAsync result")
            sdk_seek_payload=json.loads(sdk_result.read_text(encoding="utf-8-sig",errors="replace"))
            assert "error" not in sdk_seek_payload and sdk_seek_payload.get("target")==150000,sdk_seek_payload
            assert isinstance(sdk_seek_payload.get("before"),(int,float)) and sdk_seek_payload["before"]<150000,sdk_seek_payload
            print("MM5_QA_ONLY_NATIVE_SDK_SEEK_RESULT",sdk_seek_payload,flush=True)
            time.sleep(3)
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
        if NATIVE_INPUT_QA:
            decoder_log=workspace/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_winamp.log"
            assert decoder_log.is_file(),"Missing production native input decoder trace"
            native_trace=decoder_log.read_text(encoding="utf-8-sig",errors="replace")
            native_counts={kind:native_trace.count(kind) for kind in
                           ("NATIVE_MANIFEST_ACCEPTED","THREAD_ENTER","mode=STREAM","mode=CACHE","ROUTE_CACHE","SEEK_REQUEST","MF_SEEK_DISPATCH","SEEK_READER_RETURN")}
            print("MM5_NATIVE_INPUT_STREAM_COUNTS",native_counts,flush=True)
            for duration_event in ("DURATION_METADATA", "OPEN_DURATION"):
                for source_line in native_trace.splitlines():
                    if duration_event in source_line:
                        print("MM5_NATIVE_MF_DURATION_EVIDENCE",source_line.split("|",4)[-1].strip(),flush=True)
            if os.environ.get("AST_MM5_QA_DIAG_UI") == "1":
                for event in ("NATIVE_PROMPT_BEGIN", "NATIVE_PROMPT_SHOWN", "PASSWORD_ENTERED"):
                    entries=[row.split("|",1)[0].strip() for row in native_trace.splitlines() if event in row]
                    print("MM5_QA_NATIVE_DIALOG_TIMELINE",event,entries,flush=True)
            assert native_counts["NATIVE_MANIFEST_ACCEPTED"]>=len(SOURCES),native_counts
            assert native_counts["THREAD_ENTER"]>=len(SOURCES),native_counts
            if os.environ.get("AST_MM5_QA_SDK_SEEK") == "1":
                assert native_counts["SEEK_REQUEST"]>=1 and native_counts["MF_SEEK_DISPATCH"]>=1 and native_counts["SEEK_READER_RETURN"]>=1,native_counts
                assert re.search(r"SEEK_READER_RETURN[^\n]*hr=0x00000000",native_trace),native_trace[-2500:]
                sdk_played=elapsed(window,labels[0])
                assert 150 <= sdk_played <= 260,sdk_played
                print("MM5_NATIVE_RAR5_AAC_SDK_SEEK_CALLBACK_AND_RESUME_PASS",{"seek_target_ms":150000,"host_seconds":sdk_played,"reader_hr":"0x00000000"},flush=True)
            # Encrypted RAR5 may not support direct streaming. The native MF
            # decoder (AAC/WMA/M4A) cannot yet consume a bounded memory buffer,
            # so managed cache is the legitimate final fallback after trying
            # stream and memory. Never confuse this with HTTP playback.
            encrypted_rar5_aac_cache=(AES_MODE=="accept" and ARCHIVE_KIND=="rar" and
                                     not RAR_SOLID and len(SOURCES)==1 and SOURCES[0][0]=="AAC")
            encrypted_rar5_mp3_memory=(AES_MODE=="accept" and ARCHIVE_KIND=="rar" and
                                      not RAR_SOLID and len(SOURCES)==1 and SOURCES[0][0]=="MP3")
            if encrypted_rar5_aac_cache:
                assert native_counts["mode=CACHE"]>=1 and native_counts["mode=STREAM"]==0,native_counts
                assert "route=CACHE" in native_trace and "mf_memory_decoder_unavailable" in native_trace,native_trace[-3000:]
                print("MM5_NATIVE_ENCRYPTED_RAR5_AAC_LAST_RESORT_CACHE_NO_HTTP_PASS",flush=True)
            elif encrypted_rar5_mp3_memory:
                assert native_counts["mode=CACHE"]==0 and "route=BUFFER" in native_trace,native_counts
                print("MM5_NATIVE_ENCRYPTED_RAR5_MP3_BOUNDED_MEMORY_NO_CACHE_NO_HTTP_PASS",flush=True)
            else:
                assert native_counts["mode=STREAM"]>=len(SOURCES),native_counts
                assert native_counts["mode=CACHE"]==native_counts["ROUTE_CACHE"]==0,native_counts
            assert "NATIVE_MANIFEST_REJECTED" not in native_trace and "DECODER_OPEN_FAIL" not in native_trace,native_trace[-2500:]
            assert "PCM_WRITE_INVALID" not in native_trace,native_trace[-2500:]
            if any(key == "MP3" for key,_ in SOURCES):
                accepted_writes=[int(value) for value in re.findall(r"FIRST_WRITE[^\n]*\bbytes=(\d+)",native_trace)]
                assert accepted_writes and all(0 < size <= 8192 for size in accepted_writes),accepted_writes
                print("MM5_NATIVE_MP3_OUTPUT_8192_BYTE_CONTRACT_PASS",accepted_writes,flush=True)
            if os.environ.get("AST_MM5_VERIFY_GUI_SEEK") == "1":
                if native_counts["SEEK_REQUEST"] < len(SOURCES):
                    print("MM5_NATIVE_GUI_CLOCK_ONLY_NOT_SEEK_CERTIFIED",{"native_seek_requests":native_counts["SEEK_REQUEST"],"expected_tracks":len(SOURCES)},flush=True)
                    raise AssertionError("GUI clock movement alone is not a decoder seek; native SEEK_REQUEST callback missing: "+str(native_counts))
                mf_track_count=sum(key in ("AAC","M4A","WMA") for key,_ in SOURCES)
                if mf_track_count:
                    if native_counts["MF_SEEK_DISPATCH"] < mf_track_count or native_counts["SEEK_READER_RETURN"] < mf_track_count:
                        print("MM5_NATIVE_MF_GUI_SEEK_UNCERTIFIED",native_counts,flush=True)
                        raise AssertionError("Media Foundation seek not dispatched and returned for every MF track: "+str(native_counts))
                    if len(re.findall(r"SEEK_READER_RETURN[^\n]*hr=0x00000000",native_trace)) < mf_track_count:
                        raise AssertionError("Media Foundation did not confirm successful decoder seek for every MF track")
                if AES_MODE == "accept" and SOURCES[0][0] == "AAC" and ARCHIVE_KIND == "7z":
                    print("MM5_NATIVE_ENCRYPTED_AAC_MF_SEEK_HANDOFF",
                          {"request":native_counts["SEEK_REQUEST"],
                           "dispatched":native_counts["MF_SEEK_DISPATCH"],
                           "reader_returned":native_counts["SEEK_READER_RETURN"]},flush=True)
            if encrypted_rar5_aac_cache:
                print("MM5_NATIVE_INPUT_ENCRYPTED_RAR5_MF_CACHE_LAST_RESORT_ROUTE_PASS",flush=True)
            elif encrypted_rar5_mp3_memory:
                print("MM5_NATIVE_INPUT_ENCRYPTED_RAR5_MEMORY_SECONDARY_ROUTE_PASS",flush=True)
            else:
                print("MM5_NATIVE_INPUT_CODEC_ARCHIVE_STREAMING_WITHOUT_CACHE_PASS",flush=True)

        print("MM5_PORTABLE_CODEC_MATRIX",json.dumps(results),flush=True)
        if AES_MODE:
            if TEST_SAVE_CONSENT:
                if isolated_password_list.read_text(encoding="utf-8").splitlines() != [synthetic_password]:
                    raise AssertionError("Confirmed header password was not saved after successful audio decode")
                print("MM5_RAR_HEADER_VERIFIED_PASSWORD_SAVE_CONSENT_PASS",flush=True)
            elif isolated_password_list.read_bytes() != b"":
                raise AssertionError("Synthetic password was saved without user consent")
            if "PASSWORD_ENTERED" not in password_events and AES_MODE == "accept":
                raise AssertionError("Did not exercise encrypted host password acceptance")
            if "PASSWORD_CANCELED" not in password_events and AES_MODE == "cancel":
                raise AssertionError("Did not exercise encrypted host password cancellation")
            if AES_MODE == "wrong" and "PASSWORD_WRONG_ENTERED" not in password_events:
                raise AssertionError("Did not exercise an incorrect encrypted host password")
            if AES_MODE in ("cancel", "wrong"):
                if any(test.get("working") for test in results.values()):
                    raise AssertionError("Rejected password unexpectedly decoded the test audio")
                print("MM5_ENCRYPTED_PLAYER_PASSWORD_"+AES_MODE.upper()+"_REJECTED_NO_SAVE_PASS",flush=True)
                return
            print("MM5_ENCRYPTED_PLAYER_DIALOG_ENTERED_NO_PLAINTEXT_PASSWORD_SAVE",flush=True)
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
        if AES_MODE == "accept":
            if os.environ.get("AST_MM5_VERIFY_GUI_SEEK") == "1":
                print("MM5_ENCRYPTED_HOST_PASSWORD_ACCEPTED_PLAYBACK_AND_GUI_SEEK_PASS",flush=True)
            elif os.environ.get("AST_MM5_QA_SDK_SEEK") == "1":
                print("MM5_ENCRYPTED_HOST_PASSWORD_ACCEPTED_PLAYBACK_AND_SDK_SEEK_PASS_GUI_SEEK_NOT_TESTED",flush=True)
            else:
                print("MM5_ENCRYPTED_HOST_PASSWORD_ACCEPTED_PLAYBACK_PASS_SEEK_NOT_TESTED",flush=True)
    finally:
        ui_stop.set()
        if ui_thread is not None:
            ui_thread.join(timeout=3)
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
