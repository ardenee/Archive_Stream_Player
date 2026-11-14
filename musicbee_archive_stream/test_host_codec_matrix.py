"""Real MusicBee 3.7 seven-codec Tools-menu, host playback and GUI seek regression.
Checks native SDK Playing state/time progression, seek-resume, streaming HTTP ranges,
local-file SHA-256 parity, and safe isolated host queueing. Not speaker/PCM certification.
DEV-only dependencies: installed MusicBee, pywinauto automation runtime and test audio.
Stages a fresh portable MusicBee copy; never edits the installed profile/library.
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


# Isolated seven-codec host smoke test; a requested URL is not decoded-audio proof.
SOURCES = [MP3, OGG, Path(r"C:\audiotest\03.FlacFile.flac"),
           Path(r"C:\audiotest\04.WAVFile.wav"),
           Path(r"C:\audiotest\05.WMAFile.wma"),
           Path(r"C:\audiotest\06.M4AFile.m4a"),
           Path(r"C:\audiotest\07.AACFile.aac")]
assert all(p.is_file() for p in SOURCES), "Audio codec fixture missing"

with tempfile.TemporaryDirectory(prefix="asp-musicbee-codecs-", dir=r"D:\Temp") as location:
    path = Path(location)
    staged = path / "MusicBee"
    shutil.copytree(APP, staged)
    (staged / "AppData").mkdir(exist_ok=True)
    (staged / "Library").mkdir(exist_ok=True)
    shutil.copy2(PLUGIN, staged / "Plugins" / "MB_ArchiveStream.dll")
    shutil.copy2(BRIDGE, staged / "Plugins" / "archive_player_bridge.exe")
    archive = path / "audio-codecs.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as out:
        for sound in SOURCES:
            out.write(sound, arcname=sound.name)

    environment = os.environ.copy()
    environment["APPDATA"] = str(path / "Roaming")
    environment["LOCALAPPDATA"] = str(path / "Local")
    environment["TEMP"] = str(path / "SessionTemp")
    environment["TMP"] = str(path / "SessionTemp")
    environment["ARCHIVE_STREAM_ADAPTER_DEBUG"] = "1"
    for name in ("Roaming", "Local", "SessionTemp"):
        (path / name).mkdir()
    process = subprocess.Popen([str(staged / "MusicBee.exe")], cwd=str(staged), env=environment)
    print("MUSICBEE_CODEC_ISOLATED_PID", process.pid, flush=True)
    try:
        log_root = path / "Local" / "ArchiveStreamPlayer" / "Logs"
        adapter_log = log_root / "archive_stream_musicbee.log"
        bridge_log = log_root / "archive_stream_bridge.log"
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError("MusicBee exited during startup")
            wizard = next((w for w in windows(process.pid) if w.window_text() == "Welcome"), None)
            if wizard:
                empty = next(c for c in wizard.descendants() if
                             "create an empty library" in c.window_text())
                rect = empty.rectangle()
                click((rect.left + 12, (rect.top + rect.bottom) // 2))
                next_button = next(c for c in wizard.descendants() if
                                   "BUTTON" in c.class_name() and c.window_text() == "Proceed")
                next_button.click_input()
                print("MUSICBEE_EMPTY_LIBRARY_CREATED", flush=True)
                break
            if adapter_log.is_file() and "MENU_ITEM_REGISTERED" in adapter_log.read_text(
                    encoding="utf-8", errors="replace"):
                break
            time.sleep(.25)

        main = await_window(process.pid, "MusicBee")
        await_log(adapter_log, r"MENU_ITEM_REGISTERED", timeout=30)
        rect = main.rectangle()
        main.set_focus()
        click((rect.left + 103, rect.top + 14))
        time.sleep(.2)
        mouse.move(coords=(rect.left + 99, rect.top + 119))
        time.sleep(.4)
        click((rect.left + 236, rect.top + 363))
        dialog = await_window(process.pid, "Archive Stream: choose an audio archive")
        filename = next(c for c in dialog.descendants()
                        if c.class_name() == "Edit" and c.rectangle().top > dialog.rectangle().top + 240)
        filename.set_edit_text(str(archive))
        button = next(c for c in dialog.descendants()
                      if c.class_name() == "Button" and "Open" in c.window_text())
        button.click_input()
        await_log(adapter_log, r"QUEUE_RESULT accepted=1 tracks=7", timeout=45)
        await_log(adapter_log, r"OGG_LOCAL_READY index=1 bytes=", timeout=10)
        for index in (1, 2, 3, 4, 5):
            sound = SOURCES[index]
            local_track = next(iter((path / "SessionTemp").glob(
                "ArchiveStreamMusicBee-*/archive-entry-" + str(index).zfill(6) + sound.suffix.lower())), None)
            assert local_track is not None, ("MusicBee local fallback missing", sound)
            assert local_track.read_bytes() == sound.read_bytes(), ("MusicBee local bytes differ", sound)
            print("LOCAL_CODEC_SHA256_PARITY", index, sound.suffix, local_track.stat().st_size, flush=True)
        print("MUSICBEE_CODEC_QUEUE_ACCEPTED tracks=7", flush=True)

        selected_indices = [int(os.environ["AST_MUSICBEE_CODEC_INDEX"])] if os.environ.get("AST_MUSICBEE_CODEC_INDEX") else range(len(SOURCES))
        for step, i in enumerate(selected_indices):
            sound = SOURCES[i]
            if process.poll() is not None:
                raise AssertionError("MusicBee exited before codec " + sound.suffix)
            main = await_window(process.pid, "MusicBee")
            main.set_focus()
            rect = main.rectangle()
            if step == 0:
                click((rect.left + 189, rect.bottom - 26))  # Mute before playback.
            mouse.double_click(coords=(rect.right - 200, rect.top + 90 + 45 * i))
            time.sleep(3)
            dialogs = [w for w in windows(process.pid)
                       if w.window_text() in ("Error", "Archive Stream Player", "MusicBee Error")]
            modal = [(w.window_text(), [c.window_text() for c in w.descendants()
                                        if c.window_text()][:12]) for w in dialogs]
            logs = bridge_log.read_text(encoding="utf-8", errors="replace") if bridge_log.is_file() else ""
            index_requests = re.findall(r"ROUTE_\w+[^\n]*index=" + str(i) + r"(?:\D|$)[^\n]*", logs)
            playback_log = adapter_log.read_text(encoding="utf-8", errors="replace") if adapter_log.is_file() else ""
            samples = re.findall(r"HOST_PLAYBACK index=" + str(i) + r" codec=" + re.escape(sound.suffix.lower()) +
                                 r" route=(HTTP|LOCAL) state=(\w+) position_ms=(-?\d+)", playback_log)
            print("CODEC_HOST_OBSERVATION", i, sound.suffix, "requests=" + str(len(index_requests)),
                  "modal=" + repr(modal), "playback_samples=" + repr(samples[-5:]), flush=True)
            assert not modal, ("MusicBee reported playback error", i, sound.suffix, modal)
            expected_route = "HTTP" if i in (0, 6) else "LOCAL"
            positions = [int(position) for route, state, position in samples
                         if route == expected_route and state == "Playing"]
            assert len(positions) >= 2 and max(positions) - min(positions) >= 500, (
                "MusicBee host playback did not advance", i, sound.suffix, samples)
            if i in (0, 6):
                assert index_requests, ("Expected MusicBee live archive-backed HTTP request", i, sound.suffix)
            if os.environ.get("AST_MUSICBEE_UI_DEBUG") == "1":
                capture_screen("codec-" + str(i))
            click((rect.left + 895, rect.bottom - 14))
            time.sleep(float(os.environ.get("AST_MUSICBEE_SEEK_WAIT", "3.5")))
            after_log = adapter_log.read_text(encoding="utf-8", errors="replace")
            after_samples = re.findall(r"HOST_PLAYBACK index=" + str(i) + r" codec=" + re.escape(sound.suffix.lower()) +
                                   r" route=(HTTP|LOCAL) state=(\w+) position_ms=(-?\d+)", after_log)
            after_positions = [int(position) for route, state, position in after_samples
                               if route == expected_route and state == "Playing"]
            bridge_after = bridge_log.read_text(encoding="utf-8", errors="replace") if bridge_log.is_file() else ""
            after_routes = re.findall(r"ROUTE_\w+[^\n]*index=" + str(i) + r"(?:\D|$)[^\n]*", bridge_after)
            print("CODEC_GUI_SEEK_OBSERVATION", i, sound.suffix,
                  "before_ms=" + str(max(positions)), "after_ms=" + repr(after_positions[-3:]),
                  "last_state=" + repr(after_samples[-3:]), "bridge_routes=" + repr(after_routes[-4:]), flush=True)
            resumed = [int(position) for route, state, position in after_samples[len(samples):]
                       if route == expected_route and state == "Playing"]
            seek_resumed = [position for position in resumed if position >= max(positions) + 30000]
            assert len(seek_resumed) >= 2, (
                "Host GUI seek did not resume at expected position", i, sound.suffix, resumed)
            assert seek_resumed[-1] > seek_resumed[0], (
                "Host did not advance after GUI seek", i, sound.suffix, seek_resumed)
            if expected_route == "HTTP":
                route_offsets = [int(value) for value in re.findall(
                    r"ROUTE_STREAM[^\n]*index=" + str(i) + r"[^\n]*start=(\d+)", bridge_after)]
                assert any(offset > 0 for offset in route_offsets), (
                    "Host seek did not produce a byte-range read", i, sound.suffix, route_offsets)
            print("CODEC_HOST_PLAYBACK_AND_GUI_SEEK_PASS", i, sound.suffix, resumed[-2:], flush=True)
        print("ALL_MUSICBEE_SEVEN_CODEC_HOST_PLAYBACK_AND_GUI_SEEK_CHECKS_PASS", flush=True)
    finally:
        cleanup(process)
