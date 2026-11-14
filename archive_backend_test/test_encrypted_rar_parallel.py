"""Concurrent RAR5 encrypted-header playback regression for the shared bridge.

Creates two distinct WAV entries with encrypted filenames. Both Win32 and x64
bridges must handle concurrent independent HTTP Range requests without corrupting
the runtime password vectors, blocking on prompts, or bypassing the RAM limit.
Only synthetic password files inside a disposable local profile are used.
"""
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import os
import secrets
import subprocess
import tempfile
import urllib.request
import wave

ROOT=Path(__file__).resolve().parents[1]
RAR=Path(r"C:\Program Files\WinRAR\Rar.exe")

def wav_fixture(path, seed):
    block=bytes(((x * seed + 13) % 253 for x in range(32768)))
    with wave.open(str(path),"wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(22050)
        for _ in range(12):
            writer.writeframesraw(block)
        writer.writeframes(b"")
    return path.read_bytes()

def run_for_arch(arch, archive, password_file, originals, temp):
    platform="Release" if arch=="Win32" else "x64/Release"
    exe=ROOT/"archive_player_bridge"/platform/"archive_player_bridge.exe"
    assert exe.is_file(),exe
    out=temp/("bridge-"+arch)
    out.mkdir()
    env=os.environ.copy()
    local=temp/("local-"+arch)
    local.mkdir()
    env["LOCALAPPDATA"]=str(local)
    env["TEMP"]=str(local)
    env["TMP"]=str(local)
    command=[str(exe),"--archive",str(archive),"--password-list",str(password_file),
             "--prompt-password","0","--memory-mb","8","--cache-limit-mb","0",
             "--playlist-dir",str(out),"--lifetime","70"]
    process=subprocess.Popen(command,env=env,stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,text=True,errors="replace",bufsize=1)
    try:
        ready={}
        for _ in range(35):
            line=process.stdout.readline()
            if not line:
                break
            line=line.strip()
            if "=" in line:
                k,v=line.split("=",1)
                ready[k]=v
            if line=="BRIDGE_READY=1":
                break
        assert ready.get("BRIDGE_READY")=="1",f"{arch} bridge never became ready: {ready}"
        lines=Path(ready["PLAYLIST_M3U8"]).read_text(encoding="utf-8-sig").splitlines()
        tracks=[line for line in lines if line.startswith("http://127.0.0.1:")]
        assert len(tracks)==len(originals),f"{arch}: {len(tracks)} entries, expected {len(originals)}"
        requests=[]
        for cycle in range(12):
            for index,content in enumerate(originals):
                begin=(cycle * 7919 + index * 23) % (len(content)-4097)
                stop=begin+4096
                requests.append((tracks[index],f"bytes={begin}-{stop}",content[begin:stop+1],index))
        def fetch(request):
            url,byte_range,expected,index=request
            with urllib.request.urlopen(urllib.request.Request(url,headers={"Range":byte_range}),timeout=25) as result:
                assert result.status==206,(arch,index,result.status)
                actual=result.read()
            assert actual==expected,f"{arch} byte mismatch for index {index} / {byte_range}"
            return index
        with ThreadPoolExecutor(max_workers=8) as pool:
            results=list(pool.map(fetch,requests))
        assert results.count(0)==12 and results.count(1)==12
        shutdown=ready["URL_M3U8"].replace("/playlist.m3u8","/shutdown")
        with urllib.request.urlopen(urllib.request.Request(shutdown,method="POST"),timeout=5) as result:
            assert result.status==200
        process.wait(timeout=12)
        output=process.stdout.read()
        assert process.returncode==0,output
        assert "ROUTE_COUNTS" in output and "stream=0" in output and "cache=0" in output,output
        assert password_file.read_text(encoding="utf-8").count("QA-") == 1, "Password file unexpectedly mutated"
        print("RAR5_CONCURRENT_ENCRYPTED_HEADERS_RANGE_PASS",arch,
              "requests",len(requests),"max_workers",8,output.strip(),flush=True)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=10)

def main():
    assert RAR.is_file()
    with tempfile.TemporaryDirectory(prefix="AST-RAR5-Parallel-",dir=r"D:\Temp") as folder:
        temp=Path(folder)
        sources=[temp/"01 Parallel.wav",temp/"02 Parallel.wav"]
        originals=[wav_fixture(source,seed) for source,seed in zip(sources,(13,29))]
        password="QA-"+secrets.token_hex(16)
        plist=temp/"synthetic-passwords.txt"
        plist.write_text(password+"\n",encoding="utf-8")
        archive=temp/"encrypted-headers.rar"
        result=subprocess.run([str(RAR),"a","-idq","-y","-ma5","-m0","-s-",
                               "-hp"+password,str(archive),*(str(s) for s in sources)],
                              capture_output=True,text=True,errors="replace",timeout=60)
        assert result.returncode==0 and archive.is_file(),result.stderr[-1000:]
        for arch in ("Win32","x64"):
            run_for_arch(arch,archive,plist,originals,temp)
    print("RAR5_PARALLEL_CREDENTIAL_STATE_AND_HTTP_RANGE_BOTH_ARCH_PASS",flush=True)

if __name__=="__main__":
    main()
