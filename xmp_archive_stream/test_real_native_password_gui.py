"""Real XMPlay 4.1 XMPARC encrypted-7z password GUI and seek regression.

Starts an isolated portable XMPlay x86 copy, creates a random-password
non-solid AES+LZMA2 7z MP3, enters the password via genuine Windows CredUI,
declines saving, checks backend stream into host-owned memory, and seeks through
XMPlay's real progress slider. No personal player configuration is changed.
"""
import hashlib
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import winreg
from pathlib import Path

import test_real_native_gui_seek as base

from pywinauto import Desktop, mouse
import win32gui
import win32process
import win32con

SOURCE = base.XMPLAY_SOURCE
PLUGIN = base.PLUGIN
AUDIO = base.AUDIO
SEVENZIP = base.SEVENZIP


def password_list_fingerprint():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\ArchiveStreamPlayer\Adapters") as key:
            target = Path(winreg.QueryValueEx(key, "PasswordList")[0])
        return target, (hashlib.sha256(target.read_bytes()).digest() if target.is_file() else None)
    except (OSError, ValueError):
        return None, None


def credential_worker(pid, password, events, shutdown):
    seen = set()
    while not shutdown.is_set():
        found = []
        def visit(hwnd, _):
            try:
                if win32gui.IsWindowVisible(hwnd) and win32process.GetWindowThreadProcessId(hwnd)[1] == pid:
                    found.append((hwnd, win32gui.GetWindowText(hwnd)))
            except Exception:
                pass
        win32gui.EnumWindows(visit, None)
        for hwnd, title in found:
            if hwnd in seen or title not in ("Archive Stream Password", "Archive Stream - XMPlay"):
                continue
            try:
                dialog = Desktop(backend="win32").window(handle=hwnd)
                if title == "Archive Stream Password":
                    fields = [item for item in dialog.descendants() if item.class_name() == "Edit"]
                    if not fields:
                        continue
                    fields[-1].set_edit_text(password)
                    next(item for item in dialog.descendants() if item.window_text() == "OK").click_input()
                    events.append("PASSWORD_ENTERED")
                else:
                    next(item for item in dialog.descendants()
                         if item.window_text().replace("&", "") == "No").click_input()
                    events.append("SAVE_DECLINED")
                print("XMPLAY_REAL_PASSWORD_DIALOG_HANDLED", title, flush=True)
                seen.add(hwnd)
            except Exception:
                # Native host often shows the window before its fields.
                pass
        time.sleep(.13)


def main():
    for file in (SOURCE / "xmplay.exe", PLUGIN, AUDIO, SEVENZIP):
        assert file.is_file(), file
    tasklist = subprocess.run(["tasklist", "/FI", "IMAGENAME eq xmplay.exe"],
                              capture_output=True, text=True).stdout.lower()
    if "xmplay.exe" in tasklist:
        raise SystemExit("Existing XMPlay running: refusing to alter an active player")
    configured_path, saved_fingerprint = password_list_fingerprint()
    with tempfile.TemporaryDirectory(prefix="AST-XMPlay-RealAES-", dir=r"D:\Temp") as temp:
        qa = Path(temp)
        target = qa / "XMPlay"
        shutil.copytree(SOURCE, target)
        shutil.copy2(PLUGIN, target / "xmp-archive-stream.dll")
        for filename in ("xmplay.pls", "xmplay.library", "xmplay.library~"):
            (target / filename).unlink(missing_ok=True)
        for name in ("Local", "Roaming", "Temp"):
            (qa / name).mkdir()
        local_password_list = qa / "Local" / "WinampArchiveStream" / "keeps.txt"
        local_password_list.parent.mkdir(parents=True)
        local_password_list.write_bytes(b"")
        password = "AST-XMPlay-Real-" + secrets.token_hex(10)
        source = qa / "AST_XMPLAY_AES_GUI.mp3"
        shutil.copyfile(AUDIO, source)
        archive = qa / "native-encrypted-test.7z"
        creation = subprocess.run(
            [str(SEVENZIP), "a", "-t7z", "-m0=LZMA2", "-ms=off", "-mhe=off",
             "-p" + password, str(archive), str(source)],
            cwd=qa, capture_output=True, text=True, timeout=80)
        assert creation.returncode == 0 and archive.is_file(), creation.stderr[-1100:]
        env = os.environ.copy()
        env.update(LOCALAPPDATA=str(qa / "Local"), APPDATA=str(qa / "Roaming"),
                   TEMP=str(qa / "Temp"), TMP=str(qa / "Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        player = None
        watcher = None
        stop = threading.Event()
        events = []
        passed = False
        try:
            player = subprocess.Popen([str(target / "xmplay.exe"), "-play", str(archive)],
                                      cwd=target, env=env)
            watcher = threading.Thread(target=credential_worker,
                                       args=(player.pid, password, events, stop), daemon=True)
            watcher.start()
            window = base.wait_for(lambda: base.player_window(player.pid), 45,
                                   "XMPlay native password host")
            win32gui.SetWindowPos(window, win32con.HWND_TOP, 0, 0, 0, 0,
                                 win32con.SWP_NOMOVE | win32con.SWP_NOSIZE | win32con.SWP_SHOWWINDOW)
            log = qa / "Local" / "ArchiveStreamPlayer" / "Logs" / "archive_stream_xmplay.log"
            def decoded():
                if not log.is_file():
                    return False
                content = log.read_text(encoding="utf-8-sig", errors="replace")
                return "LIST_OK" in content and "STREAM_OK" in content
            base.wait_for(decoded, 70, "XMPARC encrypted 7z decoded host buffer")
            assert "PASSWORD_ENTERED" in events, events
            time.sleep(8)
            before = base.progress(window, qa)
            time.sleep(4)
            advancing = base.progress(window, qa)
            assert advancing >= before + 3, (before, advancing)
            left, top, right, bottom = win32gui.GetWindowRect(window)
            mouse.click(coords=(left + int((right - left) * .73), top + 56))
            mouse.move(coords=(left + 30, top + 180))
            time.sleep(3)
            jumped = base.progress(window, qa)
            time.sleep(5)
            resumed = base.progress(window, qa)
            assert jumped > advancing + 90 and resumed >= jumped + 4, (
                before, advancing, jumped, resumed, events)
            assert "SAVE_DECLINED" in events, events
            trace = log.read_text(encoding="utf-8-sig", errors="replace")
            assert "STREAM_OK" in trace and "CACHE_OK" not in trace, trace[-2000:]
            assert local_password_list.read_bytes() == b"", "Declined password saved to isolated list"
            if configured_path is not None:
                after = hashlib.sha256(configured_path.read_bytes()).digest() if configured_path.is_file() else None
                assert after == saved_fingerprint, "Shared password file was changed"
            passed = True
            print("XMPLAY_REAL_NATIVE_PASSWORD_GUI_PLAYBACK_SEEK_PASS",
                  f"pixels={before}->{advancing}->{jumped}->{resumed}",
                  "events=" + ",".join(events), "disk_cache=0", flush=True)
        finally:
            if not passed:
                print("XMPLAY_PASSWORD_QA_FAILURE_EVENTS", events, flush=True)
                log = qa / "Local" / "ArchiveStreamPlayer" / "Logs" / "archive_stream_xmplay.log"
                if log.exists():
                    print("XMPLAY_PASSWORD_QA_FAILURE_LOG",
                          log.read_text(encoding="utf-8-sig", errors="replace")[-2400:], flush=True)
            stop.set()
            if player and player.poll() is None:
                player.terminate()
                try:
                    player.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    player.kill()
                    player.wait(timeout=6)
            if watcher:
                watcher.join(timeout=2)
    print("XMPLAY_REAL_NATIVE_ENCRYPTED_GUI_PROFILE_CLEANED_PASS", flush=True)


if __name__ == "__main__":
    main()
