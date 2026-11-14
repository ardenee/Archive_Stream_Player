"""Real MusicBee 3.7 host acceptance: plugin Tools menu, queue, MP3 playback and seek.
DEV-only dependencies: installed MusicBee, pywinauto automation runtime and test audio.
Stages a fresh portable MusicBee copy and never edits the installed profile/library.
Run only on an interactive Windows desktop when no other MusicBee is running.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = Path(r"C:\Program Files (x86)\MusicBee")
AUTOMATION = Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
MP3 = Path(r"C:\audiotest\01.MP3File.mp3")
OGG = Path(r"C:\audiotest\02.OGGFile.ogg")
PLUGIN = ROOT / "musicbee_archive_stream" / "Release" / "MB_ArchiveStream.dll"
BRIDGE = ROOT / "archive_player_bridge" / "Release" / "archive_player_bridge.exe"
assert all(p.exists() for p in (APP, AUTOMATION, MP3, OGG, PLUGIN, BRIDGE)), "DEV host prerequisites missing"
assert not any(p.lower().endswith("musicbee.exe") for p in subprocess.check_output(
    ["powershell.exe", "-NoProfile", "-Command",
     "(Get-CimInstance Win32_Process | Where-Object {$_.Name -eq 'MusicBee.exe'}).ExecutablePath"],
    text=True).splitlines() if p.strip()), "Close active MusicBee instances before this isolated test"

os.add_dll_directory(str(AUTOMATION / "pywin32_system32"))
sys.path[:0] = [str(AUTOMATION), str(AUTOMATION / "win32"),
                str(AUTOMATION / "win32" / "lib"), str(AUTOMATION / "Pythonwin")]
from pywinauto import Desktop, mouse

def windows(pid):
    return [w for w in Desktop(backend="win32").windows()
            if w.process_id() == pid and w.is_visible()]

def await_window(pid, title, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if title == "MusicBee":
            found = next((w for w in windows(pid) if
                          w.class_name().startswith("WindowsForms10.Window") and
                          w.rectangle().width() > 1000 and w.rectangle().height() > 500), None)
        else:
            found = next((w for w in windows(pid) if w.window_text() == title), None)
        if found is not None:
            return found
        time.sleep(.2)
    details = [(w.window_text(), w.class_name()) for w in windows(pid)]
    raise AssertionError("MusicBee window missing: " + title + "; visible=" + repr(details))

def await_log(path, pattern, timeout=20):
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        if path.is_file():
            last = path.read_text(encoding="utf-8", errors="replace")
            if re.search(pattern, last):
                return last
        time.sleep(.25)
    raise AssertionError("MusicBee log event not reached: " + pattern + "\n" + last[-1600:])

def click(point):
    mouse.click(coords=point)

def capture_screen(label):
    if os.environ.get("AST_MUSICBEE_UI_DEBUG") != "1":
        return
    target = r"D:\Temp\musicbee-host-" + label + ".png"
    command = ("Add-Type -AssemblyName System.Drawing; " +
               "$b=New-Object System.Drawing.Bitmap 1750,1000; " +
               "$g=[System.Drawing.Graphics]::FromImage($b); " +
               "$g.CopyFromScreen(0,0,0,0,$b.Size); " +
               "$b.Save('" + target + "',[System.Drawing.Imaging.ImageFormat]::Png); " +
               "$g.Dispose();$b.Dispose()")
    subprocess.run(["powershell.exe", "-NoProfile", "-Command", command],
                   capture_output=True, text=True, timeout=8)

def cleanup(process):
    if process.poll() is None:
        subprocess.run(["taskkill.exe", "/F", "/T", "/PID", str(process.pid)],
                       capture_output=True, text=True)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)

with tempfile.TemporaryDirectory(prefix="asp-musicbee-ui-", dir=r"D:\Temp") as location:
    path = Path(location)
    staged = path / "MusicBee"
    shutil.copytree(APP, staged)
    (staged / "AppData").mkdir(exist_ok=True)
    (staged / "Library").mkdir(exist_ok=True)
    shutil.copy2(PLUGIN, staged / "Plugins" / "MB_ArchiveStream.dll")
    shutil.copy2(BRIDGE, staged / "Plugins" / "archive_player_bridge.exe")
    archive = path / "audio-test.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as out:
        for sound in (MP3, OGG):
            out.write(sound, arcname=sound.name)

    environment = os.environ.copy()
    environment["APPDATA"] = str(path / "Roaming")
    environment["LOCALAPPDATA"] = str(path / "Local")
    environment["TEMP"] = str(path / "SessionTemp")
    environment["TMP"] = str(path / "SessionTemp")
    environment["ARCHIVE_STREAM_ADAPTER_DEBUG"] = "1"
    (path / "Roaming").mkdir()
    (path / "Local").mkdir()
    (path / "SessionTemp").mkdir()
    process = subprocess.Popen([str(staged / "MusicBee.exe")], cwd=str(staged), env=environment)
    print("MUSICBEE_ISOLATED_PID", process.pid, flush=True)
    try:
        # Fresh profiles often display the main window before showing the
        # library wizard. Do not treat main-window visibility as readiness.
        log_root = path / "Local" / "ArchiveStreamPlayer" / "Logs"
        adapter_log = log_root / "archive_stream_musicbee.log"
        bridge_log = log_root / "archive_stream_bridge.log"
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError("MusicBee exited during startup")
            wizard = next((w for w in windows(process.pid) if w.window_text() == "Welcome"), None)
            if wizard:
                empty = next(c for c in wizard.descendants()
                             if "create an empty library" in c.window_text())
                box = empty.rectangle()
                click((box.left + 12, (box.top + box.bottom) // 2))
                proceed = next(c for c in wizard.descendants()
                               if "BUTTON" in c.class_name() and c.window_text() == "Proceed")
                proceed.click_input()
                print("MUSICBEE_EMPTY_LIBRARY_CREATED", flush=True)
                break
            if adapter_log.is_file() and "MENU_ITEM_REGISTERED" in adapter_log.read_text(
                    encoding="utf-8", errors="replace"):
                break
            time.sleep(.25)

        main = await_window(process.pid, "MusicBee")
        await_log(adapter_log, r"MENU_ITEM_REGISTERED", timeout=12)
        rect = main.rectangle()
        main.set_focus()
        click((rect.left + 103, rect.top + 14))  # MusicBee menu arrow
        time.sleep(.2)
        mouse.move(coords=(rect.left + 99, rect.top + 119))  # Tools submenu
        time.sleep(.4)
        click((rect.left + 236, rect.top + 363))  # Archive Stream: Add Archive Audio
        dialog = await_window(process.pid, "Archive Stream: choose an audio archive")
        filename = next(c for c in dialog.descendants()
                        if c.class_name() == "Edit" and c.rectangle().top > dialog.rectangle().top + 240)
        filename.set_edit_text(str(archive))
        button = next(c for c in dialog.descendants()
                      if c.class_name() == "Button" and "Open" in c.window_text())
        button.click_input()
        await_log(adapter_log, r"QUEUE_RESULT accepted=1 tracks=2", timeout=25)
        await_log(adapter_log, r"OGG_LOCAL_READY index=1 bytes=", timeout=10)
        copies = list((path / "SessionTemp").glob(
            "ArchiveStreamMusicBee-*/archive-entry-000001.ogg"))
        assert len(copies) == 1, ("Missing MusicBee isolated OGG materialization", copies)
        assert copies[0].read_bytes() == OGG.read_bytes(), "MusicBee OGG bytes not preserved"
        print("MUSICBEE_NATIVE_QUEUE_ACCEPTED tracks=2", flush=True)
        print("MUSICBEE_OGG_LOCAL_BYTE_PARITY_PASS", copies[0].stat().st_size, flush=True)

        # Real host dispatch, not a direct HTTP client: select first queued track,
        # muted before playback to avoid unexpected audio on the DEV machine.
        main = await_window(process.pid, "MusicBee")
        main.set_focus()
        rect = main.rectangle()
        print("MUSICBEE_MAIN_RECT", rect, flush=True)
        click((rect.left + 189, rect.bottom - 26))  # Mute icon, inside content not shadow
        mouse.double_click(coords=(rect.right - 200, rect.top + 90))
        await_log(bridge_log, r"ROUTE_STREAM[^\n]*index=0[^\n]*start=0", timeout=15)
        time.sleep(4)
        capture_screen("before-seek")
        main = await_window(process.pid, "MusicBee")
        main.set_focus()
        rect = main.rectangle()
        print("MUSICBEE_SEEK_RECT", rect, flush=True)
        click((rect.left + 895, rect.bottom - 14))  # Seek bar, above Win32 shadow border
        time.sleep(.8)
        capture_screen("after-seek")
        trace = await_log(bridge_log, r"ROUTE_STREAM[^\n]*index=0[^\n]*start=(?!0(?:\D|$))\d+", timeout=15)
        offsets = [int(x) for x in re.findall(r"ROUTE_STREAM[^\n]*index=0[^\n]*start=(\d+)", trace)]
        assert any(offset > 100000 for offset in offsets), offsets
        print("MUSICBEE_REAL_HOST_SEEK_PASS", max(offsets), flush=True)

        # MusicBee rejected this same Vorbis entry when queued over HTTP.
        # The local-file fallback must open through the real host without
        # displaying the "source file could not be found" error dialog.
        main = await_window(process.pid, "MusicBee")
        main.set_focus()
        rect = main.rectangle()
        mouse.double_click(coords=(rect.right - 200, rect.top + 135))
        time.sleep(2.5)
        capture_screen("ogg-playback")
        unexpected = [w.window_text() for w in windows(process.pid)
                      if w.window_text() in ("Error", "Archive Stream Player")]
        assert not unexpected, ("MusicBee OGG playback displayed an error", unexpected)
        print("MUSICBEE_OGG_HOST_NO_ERROR_PASS", flush=True)
        main = await_window(process.pid, "MusicBee")
        main.set_focus()
        rect = main.rectangle()
        click((rect.left + 895, rect.bottom - 14))
        time.sleep(1.5)
        capture_screen("ogg-after-seek")
        unexpected = [w.window_text() for w in windows(process.pid)
                      if w.window_text() == "Error"]
        assert not unexpected, ("MusicBee OGG GUI seek displayed an error", unexpected)
        print("MUSICBEE_OGG_GUI_SEEK_NO_ERROR_PASS", flush=True)
        print("ALL_MUSICBEE_HOST_QUEUE_MP3_SEEK_AND_OGG_FALLBACK_CHECKS_PASS", flush=True)
    finally:
        cleanup(process)
