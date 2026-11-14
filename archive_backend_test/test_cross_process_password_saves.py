r"""Windows cross-process shared password-list deduplication regression.

Starts both 32-bit and 64-bit native backend executables as *separate*
processes, releases them against a common barrier, and inspects physical
UTF-8 password lines (not the API's duplicate-filtered list). Uses only
random synthetic passwords under D:\Temp. No player profile is modified.
"""
from pathlib import Path
import secrets
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
EXES={
    "Win32": ROOT/"archive_backend_test"/"Release"/"archive_backend_test.exe",
    "x64": ROOT/"archive_backend_test"/"x64"/"Release"/"archive_backend_test.exe",
}

def run_writers(path, workers, label):
    barrier=path.parent/(label+"-start.signal")
    if barrier.exists():
        raise AssertionError("Refusing to reuse old test barrier")
    jobs=[]
    try:
        for index,(architecture,synthetic) in enumerate(workers):
            exe=EXES[architecture]
            # On half the processes change path casing and spelling. Windows
            # should still resolve the same physical file and mutex key.
            spelling=str(path)
            if index % 3 == 1:
                spelling=spelling.upper()
            elif index % 3 == 2:
                spelling=str(path.parent)+r"\."+"\\"+path.name
            jobs.append(subprocess.Popen([str(exe),"pwappendtest",spelling,synthetic,str(barrier)],
                                         stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                         text=True,errors="replace"))
        barrier.write_text("release",encoding="ascii")
        for index,job in enumerate(jobs):
            output,errors=job.communicate(timeout=36)
            if job.returncode!=0 or "PASSWORD_APPEND_TEST_PASS" not in output:
                raise AssertionError(f"{label} worker {index}: rc={job.returncode} "
                                     f"stdout={output[-900:]} stderr={errors[-900:]}")
    finally:
        for job in jobs:
            if job.poll() is None:
                job.kill()
            job.wait(timeout=6)
        barrier.unlink(missing_ok=True)

def main():
    assert all(exe.is_file() for exe in EXES.values()),"Build Win32 and x64 backend test executables"
    with tempfile.TemporaryDirectory(prefix="AST-Password-MultiProcess-",dir=r"D:\Temp") as folder:
        root=Path(folder)
        path=root/"Café-秘密-keeps.txt"
        same="synthetic-shared-"+secrets.token_hex(15)
        workers=[("Win32" if i%2==0 else "x64",same) for i in range(36)]
        run_writers(path,workers,"one-key")
        raw=path.read_bytes()
        assert raw==same.encode("utf-8")+b"\r\n",f"Duplicate or broken shared key: {raw!r}"
        print("PASSWORD_CROSS_PROCESS_DEDUP_PASS","Win32+x64","writers=36","physical_entries=1",flush=True)

        # Existing files may not end in a newline. The append API must
        # preserve the old line and write each new candidate exactly once.
        seed="existing-synthetic-"+secrets.token_hex(6)
        path.write_text(seed,encoding="utf-8")
        first="synthetic-first-"+secrets.token_hex(12)
        second="synthetic-second-"+secrets.token_hex(12)
        workers=[
            ("Win32" if i%2==0 else "x64",first if i%3 else second)
            for i in range(36)
        ]
        run_writers(path,workers,"two-keys")
        lines=path.read_text(encoding="utf-8").splitlines()
        assert len(lines)==3 and len(set(lines))==3 and set(lines)=={seed,first,second}, \
               f"Concurrent keys corrupt, duplicated or missing: {lines!r}"
        print("PASSWORD_CROSS_PROCESS_MIXED_KEY_AND_MISSING_NEWLINE_PASS",
              "Win32+x64","writers=36","physical_entries=3",flush=True)

        # UTF-8 synthetic credentials must survive argv conversion, loading
        # and cross-bitness deduplication without codepage substitutions.
        unicode_key="Café-秘密-Пример-"+secrets.token_hex(9)
        run_writers(path,[("x64" if i%2 else "Win32",unicode_key) for i in range(12)],"unicode-key")
        unicode_lines=path.read_text(encoding="utf-8").splitlines()
        assert set(unicode_lines)=={seed,first,second,unicode_key} and len(unicode_lines)==4,                f"Unicode password was lost or duplicated: {unicode_lines!r}"
        print("PASSWORD_CROSS_PROCESS_UTF8_CREDENTIAL_PASS","Win32+x64","writers=12",flush=True)

        # A directory path is never treated as a valid password-list file.
        forbidden=root/"directory-instead-of-keeps.txt"
        forbidden.mkdir()
        result=subprocess.run([str(EXES["x64"]),"pwappendtest",str(forbidden),first],
                              capture_output=True,text=True,timeout=15)
        assert result.returncode!=0 and forbidden.is_dir()
        print("PASSWORD_CROSS_PROCESS_SAVE_FAIL_CLOSED_PASS","path_is_directory",flush=True)
    print("ALL_PASSWORD_CROSS_PROCESS_PERSISTENCE_REGRESSIONS_PASS",flush=True)

if __name__=="__main__":
    main()
