"""Real AIMP x86/x64 native encrypted-7z prompt, playback and GUI seek.

Uses disposable portable player copies, random synthetic credentials, and
the real file-expander + IAIMPStream plugin. No live user profile is altered.
This complements, but does not replace, the official COM contract tests.
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

import test_real_player_gui_seek as base

ROOT = base.ROOT
AUDIO = base.AUDIO
SEVENZIP = Path(r"C:\Program Files\7-Zip\7z.exe")


def password_file_digest():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\ArchiveStreamPlayer\Adapters") as settings:
            target = Path(winreg.QueryValueEx(settings, "PasswordList")[0])
        return target, hashlib.sha256(target.read_bytes()).digest() if target.is_file() else None
    except (OSError, ValueError):
        return None, None


def prompt_worker(pid, password, events, stop):
    from pywinauto import Desktop
    import win32gui
    import win32process

    handled = set()
    while not stop.is_set():
        for hwnd in list_windows(pid, win32gui, win32process):
            if hwnd in handled:
                continue
            name = win32gui.GetWindowText(hwnd)
            if name == "Skin Settings":
                try:
                    Desktop(backend="win32").window(handle=hwnd).close()
                    events.append("SKIN_SETTINGS_CLOSED")
                    handled.add(hwnd)
                except Exception:
                    pass
                continue
            if name not in ("Archive Stream - AIMP Password", "Archive Stream - AIMP"):
                continue
            print("AIMP_REAL_HOST_DIALOG_SEEN",pid,repr(name),flush=True)
            try:
                dialog = Desktop(backend="win32").window(handle=hwnd)
                if name == "Archive Stream - AIMP Password":
                    fields = [c for c in dialog.descendants() if c.class_name() == "Edit"]
                    assert fields, "Native Windows credential dialog missing password input"
                    fields[-1].set_edit_text(password)
                    print("AIMP_QA_PASSWORD_FIELD_POPULATED",pid,flush=True)
                    next(c for c in dialog.descendants() if c.window_text() == "OK").click_input()
                    events.append("PASSWORD_ENTERED")
                    print("AIMP_QA_PASSWORD_SUBMITTED",pid,flush=True)
                else:
                    button = next(c for c in dialog.descendants()
                                  if c.window_text().replace("&", "") == "No")
                    button.click()
                    events.append("SAVE_DECLINED")
                handled.add(hwnd)
            except Exception as exc:
                # Chromium/Win32 may publish a partial dialog before controls.
                print("AIMP_REAL_HOST_DIALOG_HANDLER_PENDING",repr(exc),flush=True)
                if "ERROR" not in events:
                    events.append("DIALOG_CONTROLS_PENDING")
        time.sleep(.13)


def list_windows(pid, gui, process):
    found = []
    def visit(hwnd, unused):
        try:
            if gui.IsWindowVisible(hwnd) and process.GetWindowThreadProcessId(hwnd)[1] == pid:
                found.append(hwnd)
        except Exception:
            pass
    gui.EnumWindows(visit, None)
    return found


def run(arch):
    from pywinauto import Desktop, mouse
    import win32gui
    source = base.INSTALL / ("aimp-" + arch + "-portable") / "AIMP"
    dll = ROOT / "aimp_archive_stream" / ("Release" if arch == "x86" else "x64/Release") / "aimp_archive_stream.dll"
    assert all(p.is_file() for p in (source/"AIMP.exe", dll, AUDIO, SEVENZIP))
    password_path, before_digest = password_file_digest()
    with tempfile.TemporaryDirectory(prefix="AST-AIMP-HostAES-" + arch + "-", dir=r"D:\Temp") as folder:
        qa = Path(folder)
        dest = qa / "AIMP"
        shutil.copytree(source, dest)
        plugin = dest/"Plugins"/"aimp_archive_stream"/"aimp_archive_stream.dll"
        plugin.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dll, plugin)
        for name in ("Local", "Roaming", "Temp"):
            (qa/name).mkdir()
        isolated_password_list=qa/"Local"/"WinampArchiveStream"/"keeps.txt"
        isolated_password_list.parent.mkdir(parents=True)
        isolated_password_list.write_bytes(b"")
        password = "AST-Aimp-Host-QA-" + secrets.token_hex(12)
        target = qa/("AST_AIMP_ENCRYPTED_" + arch + ".mp3")
        shutil.copyfile(AUDIO, target)
        archive = qa/"native-password-test.7z"
        created = subprocess.run([str(SEVENZIP), "a", "-t7z", "-m0=LZMA2", "-ms=off",
                                  "-mhe=off", "-p"+password, str(archive), str(target)],
                                 capture_output=True, text=True, timeout=75)
        assert created.returncode == 0 and archive.is_file(), created.stderr[-900:]
        env = os.environ.copy()
        env.update(APPDATA=str(qa/"Roaming"), LOCALAPPDATA=str(qa/"Local"),
                   TEMP=str(qa/"Temp"), TMP=str(qa/"Temp"), ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        proc = None
        watcher = None
        passed=False
        stop = threading.Event()
        events = []
        try:
            proc = subprocess.Popen([str(dest/"AIMP.exe"), "/ADD_PLAY", str(archive)],
                                    cwd=dest, env=env)
            watcher = threading.Thread(target=prompt_worker,
                                       args=(proc.pid,password,events,stop),daemon=True)
            watcher.start()
            def ready():
                main, legacy = base.aimp_windows(proc.pid)
                if not main or not legacy:
                    return None
                if "AST_AIMP_ENCRYPTED_" + arch not in win32gui.GetWindowText(main[0]):
                    return None
                return main[0], legacy[0]
            main, legacy = base.poll(ready, 55, "encrypted entry expanded in genuine AIMP")
            for w in Desktop(backend="win32").windows():
                if w.process_id() == proc.pid and w.window_text() == "Skin Settings":
                    w.close()
            first = base.poll(lambda: base.ipc(legacy,0) if base.ipc(legacy,1)>100 and base.ipc(legacy,0)>2000 else None,
                              35, "encrypted archive playback progress")
            time.sleep(3)
            before = base.ipc(legacy,0)
            l,t,r,b = win32gui.GetWindowRect(main)
            assert r-l>=700 and b-t>=450
            mouse.click(coords=(l+int((r-l)*.70),t+142))
            time.sleep(3)
            jumped = base.ipc(legacy,0)
            time.sleep(3)
            resumed = base.ipc(legacy,0)
            assert before>=first and jumped>before+40000 and resumed>jumped+1300, (
                arch,first,before,jumped,resumed,events)
            assert "PASSWORD_ENTERED" in events and "SAVE_DECLINED" in events, (arch,events)
            log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_aimp.log"
            assert log.is_file(), "Missing isolated native diagnostic log"
            trace=log.read_text(encoding="utf-8-sig",errors="replace")
            assert "EXPAND_OK" in trace and ("ROUTE_STREAM" in trace or "ROUTE_MEMORY" in trace),trace[-1800:]
            assert "ROUTE_CACHE" not in trace, trace[-1800:]
            if password_path is not None:
                after=hashlib.sha256(password_path.read_bytes()).digest() if password_path.is_file() else None
                assert before_digest==after, "AIMP changed the configured password list"
            assert isolated_password_list.read_bytes()==b"", "Password persisted into isolated no-save list"
            passed=True
            print("AIMP_REAL_NATIVE_PASSWORD_GUI_PLAYBACK_SEEK_PASS", arch,
                  f"position_ms={first}->{before}->{jumped}->{resumed}",
                  f"events={events}", "cache=0", flush=True)
        finally:
            if not passed:
                print("AIMP_QA_FAILURE_EVENTS",arch,events,flush=True)
                debuglog=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_aimp.log"
                if debuglog.is_file():
                    print("AIMP_QA_FAILURE_LOG",arch,debuglog.read_text(encoding="utf-8-sig",errors="replace")[-2400:],flush=True)
            stop.set()
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=7)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=7)
            if watcher:
                watcher.join(timeout=2)
    print("AIMP_REAL_PASSWORD_ISOLATED_PROFILE_CLEANED_PASS",arch,flush=True)


if __name__=="__main__":
    architectures = (sys.argv[1],) if len(sys.argv)==2 else ("x86", "x64")
    for arch in architectures:
        run(arch)
    print("AIMP_X86_X64_REAL_ENCRYPTED_PASSWORD_GUI_ALL_PASS" if len(architectures)==2 else
          "AIMP_REAL_ENCRYPTED_PASSWORD_GUI_SELECTED_ARCH_PASS",flush=True)
