"""DEV-only Unicode password-list regression with a synthetic encrypted 7z.

Requires 7z.exe to CREATE the fixture; playback uses Archive Stream's static
backend. Never accesses or modifies a real player's saved passwords.
"""
import subprocess
import tempfile
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SEVENZIP = Path(r"C:\Program Files\7-Zip\7z.exe")
KEY = "ArchiveStreamFixtureUnicode2026"

def check():
    assert SEVENZIP.is_file(), "7z.exe required to generate the test fixture"
    with tempfile.TemporaryDirectory(prefix="ArchiveStreamPwdUnicode-") as workspace:
        root = Path(workspace) / "Café-日本語"
        root.mkdir()
        original = root / "エントリ.wav"
        with wave.open(str(original), "wb") as wav:
            wav.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
            wav.writeframes(b"\0\0" * 6400)
        password_path = root / "秘密-keeps.txt"
        password_path.write_text(KEY + "\r\n", encoding="utf-8")
        archive = root / "音楽-encrypted.7z"
        subprocess.run([str(SEVENZIP), "a", "-t7z", "-mx=3", "-ms=off", "-m0=lzma2",
                        "-mhe=off", "-y", "-p" + KEY, str(archive), str(original)],
                       capture_output=True, text=True, check=True)
        for platform in ("Release", "x64/Release"):
            program = REPO / "archive_backend_test" / platform / "archive_backend_test.exe"
            assert program.is_file(), f"Build archive_backend_test {platform} first"
            result = subprocess.run([str(program), "pwstreamtest", str(archive), str(password_path), "0"],
                                    capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", timeout=50)
            assert result.returncode == 0, (platform, result.stdout, result.stderr)
            assert "PASSWORD_STREAM_PASS" in result.stdout and "encrypted=1" in result.stdout
            print(f"PASS {platform} Unicode password-list path and encrypted decoded audio", flush=True)
    print("ALL_UNICODE_PASSWORD_ARCHIVE_REGRESSIONS_PASS", flush=True)

if __name__ == "__main__":
    check()
