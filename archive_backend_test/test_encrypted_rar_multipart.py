"""Encrypted RAR5 multipart regression: Win32/x64 RAM, byte parity and HTTP.
Creates only synthetic WAV and temporary RAR5 volume sets; no player data used.
Run after Release builds:
    py -3.13 -B archive_backend_test/test_encrypted_rar_multipart.py
"""
from pathlib import Path
import hashlib
import secrets
import subprocess
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_encrypted_rar_memory import bridge_check

RAR = Path(r"C:\Program Files\WinRAR\Rar.exe")
EXES = [
    ROOT / "archive_backend_test" / "Release" / "archive_backend_test.exe",
    ROOT / "archive_backend_test" / "x64" / "Release" / "archive_backend_test.exe",
]

def invoke(args, timeout=95):
    result = subprocess.run([str(arg) for arg in args], capture_output=True,
                            text=True, errors="replace", timeout=timeout)
    if result.returncode:
        raise AssertionError(f"Executable {Path(str(args[0])).name} failed "
                             f"with exit={result.returncode}; stderr={result.stderr[-1200:]}")
    return result.stdout

def make_wav(path):
    # >3 MiB, intentionally stored (RAR -m0) so a real audio entry crosses
    # multiple physical volumes; content is deterministic and PCM-decodable.
    frame = bytes((v % 251 for v in range(8192)))
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(22050)
        for _ in range(420):
            wav.writeframesraw(frame)
        wav.writeframes(b"")
    assert path.stat().st_size > 3 * 1024 * 1024
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    assert RAR.is_file(), "WinRAR fixture generator not installed"
    assert all(p.is_file() for p in EXES), "Release Win32/x64 backend tests must be built first"
    with tempfile.TemporaryDirectory(prefix="ast-rar5-multipart-", dir=r"D:\Temp") as folder:
        temp = Path(folder)
        original = temp / "multipart-synthetic.wav"
        original_hash = make_wav(original)
        synthetic = "RarMultiSyntheticQA-" + secrets.token_hex(14)
        password_file = temp / "regression-synthetic-passwords.txt"
        password_file.write_text("wrong-synthetic\n" + synthetic + "\n", encoding="utf-8")
        for mode, flag in (("data", "-p"), ("encrypted-filenames", "-hp")):
            output_archive = temp / (mode + ".rar")
            invoke([RAR, "a", "-idq", "-y", "-ma5", "-m0", "-s-", "-ep",
                    "-v1m", flag + synthetic, output_archive, original], timeout=90)
            part1 = temp / (mode + ".part1.rar")
            parts = sorted(temp.glob(mode + ".part*.rar"))
            assert part1.is_file() and len(parts) >= 3, \
                f"Expected multi-volume archive, instead got {len(parts)}"
            print("MULTIPART_RAR5_VOLUME_SET_READY", mode, "volumes", len(parts),
                  "sizes", [p.stat().st_size for p in parts], flush=True)
            for binary in EXES:
                platform = "x64" if binary.parent.parent.name == "x64" else "Win32"
                decoded = temp / (mode + "-" + platform + ".wav")
                output = invoke([binary, "pwfiletest", part1, password_file, "0", decoded])
                assert "PASSWORD_MEMORY_PASS:" in output and "PASSWORD_TEST_PASS:" in output
                assert hashlib.sha256(decoded.read_bytes()).hexdigest() == original_hash
                print("PASS_MULTIPART_RAR5_MEMORY_AND_BYTE_PARITY", mode, platform, flush=True)
            for platform in ("Win32", "x64"):
                bridge_check(platform, part1, original, password_file, temp,
                             memory_mb=8, expected_route="memory")
                print("PASS_MULTIPART_RAR5_HTTP_RANGE_RAM_NO_CACHE", mode, platform, flush=True)
                bridge_check(platform, part1, original, password_file, temp,
                             memory_mb=1, expected_route="cache")
                print("PASS_MULTIPART_RAR5_HTTP_RANGE_MANAGED_CACHE", mode, platform, flush=True)
            # A missing continuation volume must fail closed, not request an
            # interactive retry or expose a seemingly complete corrupted file.
            final_volume = parts[-1]
            detached = final_volume.with_suffix(".volume-missing")
            final_volume.rename(detached)
            try:
                for binary in EXES:
                    platform = "x64" if binary.parent.parent.name == "x64" else "Win32"
                    broken_output = temp / (mode + "-missing-" + platform + ".wav")
                    broken = subprocess.run([str(binary), "pwfiletest", str(part1),
                                             str(password_file), "0", str(broken_output)],
                                            capture_output=True, text=True, timeout=25)
                    if broken.returncode == 0:
                        raise AssertionError("Native decoder accepted incomplete multipart archive")
                    if broken_output.is_file() and hashlib.sha256(broken_output.read_bytes()).hexdigest() == original_hash:
                        raise AssertionError("Missing multipart volume unexpectedly produced the full source")
                    print("PASS_MULTIPART_RAR5_MISSING_VOLUME_FAIL_CLOSED", mode, platform, flush=True)
            finally:
                detached.rename(final_volume)
        print("ENCRYPTED_RAR5_MULTIPART_X86_X64_REGRESSIONS_PASS", flush=True)

if __name__ == "__main__":
    main()
