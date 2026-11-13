"""Run foobar2000's real SDK-backed archive file reader with injected failures.

Requires a separate portable foobar2000 runtime containing a DLL built with
ARCHIVE_STREAM_FOOBAR_FAULT_TEST. Never use this DLL in a normal player install.
"""
import ctypes
import os
import subprocess
import tempfile
import time
import zipfile
from pathlib import Path

SOURCE = Path(r"C:\audiotest\01.MP3File.mp3")
PORTABLE = Path(os.environ.get("ARCHIVE_FOOBAR_FAULT_PORTABLE_DIR", r"D:\Temp\ArchiveStreamFoobarFaultTest\portable"))
EXE = PORTABLE / "foobar2000.exe"
ARCH = "Win32" if "Win32" in str(PORTABLE) else "x64"


def close_foobar(process):
    user32 = ctypes.windll.user32
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def close_window(hwnd, unused):
        pid = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(pid))
        if pid.value == process.pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            user32.PostMessageW(ctypes.c_void_p(hwnd), 0x0010, 0, 0)
        return True

    callback = callback_type(close_window)
    user32.EnumWindows(callback, None)
    try:
        process.wait(timeout=7)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=7)


def run_case(archive, folder, route, memory_mb, mode):
    report = folder / f"foobar-{route}-{mode}.txt"
    env = os.environ.copy()
    env["AST_FOOBAR_FAULT_ARCHIVE"] = str(archive)
    env["AST_FOOBAR_FAULT_SOURCE"] = str(SOURCE)
    env["AST_FOOBAR_FAULT_MEMORY_MB"] = str(memory_mb)
    env["AST_FOOBAR_FAULT_MODE"] = mode
    env["AST_FOOBAR_FAULT_REPORT"] = str(report)

    # This is a dedicated test-only portable profile. A previous aborted test
    # may leave the instance-running sentinel behind and trigger a modal
    # recovery prompt before any on_init callbacks execute.
    sentinel = PORTABLE / "profile" / "running"
    if sentinel.is_file():
        sentinel.unlink()

    process = subprocess.Popen([str(EXE)], cwd=str(PORTABLE), env=env)
    try:
        deadline = time.monotonic() + 25
        while not report.is_file() and time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError(f"foobar exited before report with code {process.returncode}")
            time.sleep(0.1)
        if not report.is_file():
            raise AssertionError(f"foobar did not produce report for {route} {mode}")

        result = report.read_text(encoding="utf-8-sig", errors="replace").strip()
        assert result.startswith("PASS FOOBAR_NATIVE_RECOVERY_PASS"), result
        assert f"mode={mode}" in result and f"route={route}" in result, result
        assert "injected=1 recovered=1 byte_parity=ok seek=ok" in result, result
        print(f"FOOBAR_NATIVE_RECOVERY_PASS {ARCH} {route} {mode}: byte parity and seek", flush=True)
    finally:
        if process.poll() is None:
            time.sleep(1.0)
            close_foobar(process)
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


if __name__ == "__main__":
    assert SOURCE.is_file(), SOURCE
    assert EXE.is_file() and (PORTABLE / "components" / "foo_archive_stream.dll").is_file(), PORTABLE
    with tempfile.TemporaryDirectory(prefix="archive-foobar-native-fault-") as directory:
        folder = Path(directory)
        archive = folder / "foobar-native-fault.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as output:
            output.write(SOURCE, arcname=SOURCE.name)
        for route, limit in (("memory", 50), ("cache", 1)):
            for mode in ("read", "seek"):
                run_case(archive, folder, route, limit, mode)
    print("ALL_FOOBAR_NATIVE_RECOVERY_REGRESSIONS_PASS", flush=True)
