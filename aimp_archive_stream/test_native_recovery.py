"""Fault-inject AIMP's real IAIMPStream read/seek recovery, x86/x64.
Build a separate test-only DLL with ARCHIVE_STREAM_AIMP_FAULT_TEST.
No production plugin settings or password files are changed.
"""
import os
import subprocess
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(r"C:\audiotest\01.MP3File.mp3")
FAULT_BIN = Path(os.environ.get("ARCHIVE_AIMP_FAULT_TEST_BIN_DIR", r"D:\Temp\ArchiveStreamAimpFaultTest"))


def main():
    assert SOURCE.is_file(), SOURCE
    with tempfile.TemporaryDirectory(prefix="archive-aimp-native-fault-") as folder:
        archive = Path(folder) / "native-fault.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as output:
            output.write(SOURCE, arcname=SOURCE.name)

        for architecture in ("Win32", "x64"):
            location = "x64/Release" if architecture == "x64" else "Release"
            host = ROOT / "aimp_archive_stream" / location / "aimp_adapter_test.exe"
            plugin = FAULT_BIN / architecture / "aimp_archive_stream.dll"
            assert host.is_file() and plugin.is_file(), (host, plugin)
            for route, memory in (("memory", "50"), ("cache", "1")):
                for mode in ("read", "seek"):
                    env = os.environ.copy()
                    env["AST_AIMP_FAULT_MODE"] = mode
                    env["AST_AIMP_FAULT_MEMORY_MB"] = memory
                    result = subprocess.run(
                        [str(host), str(plugin), str(archive), "faulttest", str(SOURCE)],
                        capture_output=True, text=True, errors="replace", timeout=100, env=env,
                    )
                    assert result.returncode == 0 and "injected=1 recovered=1 byte_parity=ok seek=ok" in result.stdout, (
                        architecture, route, mode, result.returncode, result.stdout, result.stderr
                    )
                    print(f"AIMP_NATIVE_RECOVERY_PASS {architecture} {route} {mode}: exact source-byte parity and seeking", flush=True)
    print("ALL_AIMP_NATIVE_RECOVERY_REGRESSIONS_PASS", flush=True)


if __name__ == "__main__":
    main()
