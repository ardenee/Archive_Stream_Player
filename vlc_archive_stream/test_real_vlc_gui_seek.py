r"""Real VLC 3.0.24 x64 GUI seek/resume of native ast:// archive audio.

An isolated VLC process receives a real mouse click on the accessible Qt
playback-position slider. We verify elapsed-time jump and continued advancing
playback, plus the production native archive plugin's stream-first route.
No HTTP, sound device, personal profile, network metadata or test PCM assertions.
Run only from an unlocked DEV interactive session.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

AUTOMATION = Path(r"D:\Temp\ArchiveStreamFoobar32Runtime\automation")
os.add_dll_directory(str(AUTOMATION / "pywin32_system32"))
sys.path[:0] = [str(AUTOMATION), str(AUTOMATION / "win32"),
                str(AUTOMATION / "win32" / "lib"), str(AUTOMATION / "Pythonwin")]
from pywinauto import Desktop, mouse

from test_real_vlc_seven_codecs import CODECS, MODULE, SEVENZIP, RAR, VLC, check_run, make_archive
from test_real_vlc_seven_codec_seek import source_tracks

def poll(fn, seconds, what):
    end = time.monotonic() + seconds
    last = None
    while time.monotonic() < end:
        try:
            result = fn()
            if result is not None and result is not False:
                return result
        except Exception as error:
            last = error
        time.sleep(0.20)
    raise AssertionError(("VLC GUI wait timeout", what, repr(last)))

def windows_for(pid):
    return [window for window in Desktop(backend="uia").windows()
            if window.process_id() == pid]

def dismiss_privacy(pid):
    for root in windows_for(pid):
        candidates = [root] + root.descendants(control_type="Window")
        for window in candidates:
            if window.window_text() != "Privacy and Network Access Policy":
                continue
            for option in ("Allow metadata network access", "Regularly check for VLC updates"):
                matches = [c for c in window.descendants(control_type="CheckBox") if c.window_text() == option]
                if matches:
                    checkbox = matches[0]
                    if checkbox.get_toggle_state():
                        checkbox.click_input()
            buttons = [c for c in window.descendants(control_type="Button") if "Continue" in c.window_text()]
            assert len(buttons) == 1, ("unknown VLC first-use privacy dialog", [b.window_text() for b in buttons])
            buttons[0].click_input()
            print("VLC_GUI_ISOLATED_PRIVACY_NETWORK_DISABLED",flush=True)
            return

def player_window(pid):
    found = [window for window in windows_for(pid)
             if window.window_text().endswith("- VLC media player")]
    return found[0] if len(found) == 1 else None

def clock_seconds(label):
    label = label.strip()
    if not re.fullmatch(r"\d{1,2}:\d\d(?::\d\d)?", label):
        return None
    parts = [int(x) for x in label.split(":")]
    seconds = 0
    for number in parts:
        seconds = seconds * 60 + number
    return seconds

def gui_controls(window):
    descendants = window.descendants()
    sliders = [c for c in descendants if c.element_info.control_type == "Slider"
               and c.rectangle().width() >= 240]
    if len(sliders) != 1:
        return None
    seek = sliders[0]
    parent = seek.parent()
    labels = [c for c in parent.children() if c.element_info.control_type == "Text"]
    times = [clock_seconds(label.window_text()) for label in labels]
    if len(times) != 2 or any(value is None for value in times):
        return None
    return seek, times[0], times[1]

def play_one(kind, ext, archive, index, qa, env):
    log = qa / "Local" / "ArchiveStreamPlayer" / "Logs" / "archive_stream_vlc.log"
    if log.exists():
        log.unlink()
    uri = "ast://" + str(archive).encode("utf-8").hex().upper() + f"/{index}/seek-codec.{ext}"
    cmd = [str(VLC), "--no-one-instance", "--ignore-config", "--no-plugins-cache",
           "--no-video", "--aout=dummy", "--no-media-library", uri]
    proc = subprocess.Popen(cmd, cwd=VLC.parent, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        window = poll(lambda: player_window(proc.pid), 18, "real player window")
        dismiss_privacy(proc.pid)
        def starting():
            controls = gui_controls(window)
            if controls and controls[1] >= 2 and controls[2] >= 110:
                return controls
            return None
        seek, initial, duration = poll(starting, 20, f"native audio starting {kind}/{ext}")
        time.sleep(1.5)
        before = gui_controls(window)[1]
        assert before >= initial + 1, ("GUI playback clock not advancing", kind, ext, initial, before)
        rect = seek.rectangle()
        assert rect.width() >= 240 and rect.height() > 8, ("unexpected Qt seek slider", rect)
        target = 0.70
        print("VLC_GUI_BEFORE_CLICK",kind,ext,"elapsed",before,"duration",duration,"slider",rect,flush=True)
        mouse.click(coords=(rect.left + int(rect.width() * target), rect.top + rect.height() // 2))
        mouse.move(coords=(rect.left, rect.top - 35))
        def jumped():
            controls = gui_controls(window)
            if controls and controls[1] >= int(duration * 0.58):
                return controls[1]
            return None
        moved = poll(jumped, 8, f"VLC GUI click seek jump {kind}/{ext}")
        time.sleep(2.4)
        resumed = gui_controls(window)[1]
        assert moved > before + 30, ("GUI seek did not jump", kind, ext, before, moved)
        assert resumed >= moved + 2, ("GUI seek did not resume", kind, ext, moved, resumed)
        assert log.is_file(), ("no native plugin log", kind, ext)
        trace = log.read_text(encoding="utf-8-sig", errors="replace")
        opened = trace.count("ACCESS_OPEN_OK")
        streamed = trace.count("ROUTE_STREAM")
        assert opened >= 1 and streamed >= 1, (
            "missing direct native route", kind, ext, trace[-1400:])
        assert "ROUTE_MEMORY" not in trace and "ROUTE_CACHE" not in trace, (
            "unexpected backend fallback", kind, ext, trace[-1400:])
        assert "http://127.0.0.1" not in trace and "localhost:" not in trace.lower(), (
            "unexpected HTTP transport", kind, ext)
        print("VLC_REAL_NATIVE_GUI_SEEK_RESUME_PASS", kind, ext,
              f"seconds={initial}->{before}->{moved}->{resumed}",
              f"duration={duration}", f"access_open={opened}", f"stream={streamed}",
              "memory=0 cache=0 http=0", flush=True)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=7)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=7)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", choices=("zip", "7z", "rar", "all"), default="all")
    parser.add_argument("--only", choices=tuple(ext for ext, _ in CODECS), default=None)
    args = parser.parse_args()
    assert VLC.is_file() and MODULE.is_file()
    preexisting = subprocess.run(["tasklist", "/FI", "IMAGENAME eq vlc.exe"],
                                 capture_output=True, text=True, timeout=10).stdout
    if "vlc.exe" in preexisting.lower():
        raise SystemExit("VLC already running; refusing to affect the existing player")
    kinds = ("zip", "7z", "rar") if args.kind == "all" else (args.kind,)
    with tempfile.TemporaryDirectory(prefix="AST-VLC-GUI-Seek-", dir=r"D:\Temp") as folder:
        qa = Path(folder)
        for name in ("Local", "Roaming", "Temp", "plugins"):
            (qa / name).mkdir()
        env = os.environ.copy()
        env.update(APPDATA=str(qa / "Roaming"), LOCALAPPDATA=str(qa / "Local"),
                   TEMP=str(qa / "Temp"), TMP=str(qa / "Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1",
                   ARCHIVE_STREAM_VLC_PASSWORD_LIST=str(qa / "Temp" / "keeps.txt"),
                   VLC_PLUGIN_PATH=str(MODULE.parent))
        tracks = source_tracks(qa)
        count = 0
        for kind in kinds:
            archive = make_archive(kind, qa, tracks)
            listing = check_run([str(SEVENZIP), "l", "-slt", str(archive)], cwd=qa).stdout
            order = [line[len("Path = "):].strip() for line in listing.splitlines()
                     if line.startswith("Path = seek-codec.")]
            assert len(order) == len(tracks) and set(order) == {p.name for p in tracks}, (
                kind, order)
            for track in tracks:
                ext = track.suffix.lstrip(".")
                if args.only and args.only != ext:
                    continue
                play_one(kind, ext, archive, order.index(track.name), qa, env)
                count += 1
        expected = len(kinds) * (1 if args.only else len(CODECS))
        assert count == expected, (count, expected)
        print("VLC_REAL_NATIVE_GUI_SEEK_MATRIX_PASS", count, flush=True)
    print("VLC_REAL_NATIVE_GUI_PROFILE_CLEANED_PASS", flush=True)

if __name__ == "__main__":
    main()
