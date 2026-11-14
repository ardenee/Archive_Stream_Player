"""Password prompt isolation: one stalled encrypted RAR dialog must not
block an unrelated saved-password RAR decode in the same native process.
Run after Release Win32/x64 archive_backend_test builds. No UI is displayed.
"""
from pathlib import Path
import secrets
import subprocess
import tempfile
import wave

ROOT=Path(__file__).resolve().parents[1]
RAR=Path(r"C:\Program Files\WinRAR\Rar.exe")

def create_fixture(temp, name, password, seed):
    wav=temp/(name+".wav")
    pattern=bytes((i*seed % 251 for i in range(32768)))
    with wave.open(str(wav),"wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(22050)
        for _ in range(6):
            writer.writeframesraw(pattern)
        writer.writeframes(b"")
    rar=temp/(name+".rar")
    built=subprocess.run([str(RAR),"a","-idq","-y","-ma5","-m0","-s-","-ep",
                          "-p"+password,str(rar),str(wav)],
                         capture_output=True,text=True,timeout=40)
    assert built.returncode==0 and rar.is_file(),built.stderr[-1100:]
    return rar

def main():
    assert RAR.is_file()
    with tempfile.TemporaryDirectory(prefix="AST-RAR5-Prompt-Isolation-",dir=r"D:\Temp") as folder:
        temp=Path(folder)
        pending_password="pending-qa-"+secrets.token_hex(12)
        saved_password="saved-qa-"+secrets.token_hex(12)
        pending=create_fixture(temp,"01-uncached-prompt",pending_password,13)
        saved=create_fixture(temp,"02-other-saved",saved_password,29)
        plist=temp/"saved-candidates.txt"
        plist.write_text(saved_password+"\n",encoding="utf-8")
        original=plist.read_bytes()
        for platform in ("Release","x64/Release"):
            exe=ROOT/"archive_backend_test"/platform/"archive_backend_test.exe"
            assert exe.is_file(),exe
            result=subprocess.run([str(exe),"pwparallelprompt",str(pending),
                                   str(saved),str(plist)],
                                  capture_output=True,text=True,timeout=24)
            assert result.returncode==0 and "PASSWORD_PARALLEL_ISOLATION_PASS:" in result.stdout, \
                   f"{platform}: {result.stdout[-1500:]} {result.stderr[-1000:]}"
            assert plist.read_bytes()==original,"A QA password was persisted"
            print("RAR5_NATIVE_PROMPT_DOES_NOT_HOLD_UNRAR_MUTEX_PASS",
                  "x64" if "x64" in platform else "Win32",result.stdout.strip(),flush=True)
    print("RAR5_BLOCKED_PROMPT_INDEPENDENT_DECODE_BOTH_ARCH_PASS",flush=True)

if __name__=="__main__":
    main()
