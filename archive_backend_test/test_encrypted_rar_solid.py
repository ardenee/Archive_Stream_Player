"""RAR5 solid encrypted two-entry regression, both Win32 and x64.

The test decodes the *second* solid entry independently. Earlier entries
must be handled correctly to restore the solid dictionary. Nothing is
extracted to persistent user audio folders.
"""
from pathlib import Path
import hashlib
import random
import secrets
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_encrypted_rar_memory import ROOT, RAR, EXECUTABLES, bridge_check

def build_wav(path, seed):
    block = random.Random(seed).randbytes(65536)
    with wave.open(str(path), "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(22050)
        for _ in range(22):
            writer.writeframesraw(block)
        writer.writeframes(b"")
    return hashlib.sha256(path.read_bytes()).digest()

def main():
    assert Path(RAR).is_file()
    assert all(p.is_file() for p in EXECUTABLES)
    with tempfile.TemporaryDirectory(prefix="ast-rar5-solid-",dir=r"D:\Temp") as folder:
        temp=Path(folder)
        tracks=[temp/"01 solid starter.wav", temp/"02 solid target.wav"]
        hashes=[build_wav(path, i+731) for i,path in enumerate(tracks)]
        pwd="SolidRarSyntheticQA-"+secrets.token_hex(12)
        pwfile=temp/"test-synthetic-passwords.txt"
        pwfile.write_text("wrong-candidate\n"+pwd+"\n",encoding="utf-8")
        for mode,flag in (("encrypted-data","-p"),("encrypted-names","-hp")):
            archive=temp/(mode+".rar")
            result=subprocess.run([str(RAR),"a","-idq","-y","-ma5","-s","-m3","-ep",
                                   flag+pwd,str(archive),*(str(p) for p in tracks)],
                                  capture_output=True,text=True,errors="replace",timeout=110)
            assert result.returncode==0 and archive.is_file(), result.stderr[-1100:]
            print("SOLID_RAR5_FIXTURE_READY",mode,"bytes",archive.stat().st_size,flush=True)
            for index in range(2):
                for binary in EXECUTABLES:
                    arch="x64" if binary.parent.parent.name=="x64" else "Win32"
                    output=temp/(mode+"-"+arch+"-"+str(index)+".wav")
                    r=subprocess.run([str(binary),"pwfiletest",str(archive),str(pwfile),
                                      str(index),str(output)],
                                     capture_output=True,text=True,timeout=90)
                    if r.returncode:
                        print("SOLID_RAR5_NATIVE_ERROR",mode,arch,index,
                              r.stdout[-1800:],r.stderr[-700:],flush=True)
                        raise AssertionError("Native solid RAR5 decoder failed")
                    assert hashlib.sha256(output.read_bytes()).digest()==hashes[index]
                    assert "PASSWORD_MEMORY_PASS" in r.stdout
                    print("SOLID_RAR5_NATIVE_PARITY_PASS",mode,arch,index,flush=True)
                for arch in ("Win32","x64"):
                    bridge_check(arch,archive,tracks[index],pwfile,temp,
                                 memory_mb=8,expected_route="memory",entry_index=index)
                    print("SOLID_RAR5_HTTP_RANGE_RAM_PASS",mode,arch,index,flush=True)
        print("ENCRYPTED_RAR5_SOLID_MULTI_ENTRY_X86_X64_PASS",flush=True)

if __name__=="__main__":
    main()
