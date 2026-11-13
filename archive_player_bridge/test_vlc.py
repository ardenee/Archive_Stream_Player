import subprocess,os,re,time,urllib.request
binary=r"C:\Users\arden\source\repos\archive_player_bridge\x64\Release\archive_player_bridge.exe"
fixture=r"D:\Temp\ArchiveStreamNewPlayers\bridge-test.zip"
vlc=r"C:\Program Files\VideoLAN\VLC\vlc.exe"
p=subprocess.Popen([binary,"--archive",fixture,"--lifetime","11"],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
try:
    address=None
    for _ in range(30):
        line=p.stdout.readline().strip()
        if not line:continue
        print(line,flush=True)
        if line.startswith("URL_M3U8="):
            address=line.split("=",1)[1]
        if line=="BRIDGE_READY=1":break
    assert address and p.poll() is None
    with urllib.request.urlopen(address) as response:
        listing=response.read().decode("utf-8")
        assert "#EXTM3U" in listing
    print("START VLC",flush=True)
    test=subprocess.run([vlc,"-I","dummy","--no-video","--aout","dummy","--play-and-exit",
        "--stop-time","3","--no-one-instance",address],timeout=13,capture_output=True,text=True)
    print("VLC_RETURN",test.returncode,"OUT",test.stdout[-450:],"ERR",test.stderr[-450:],flush=True)
    assert test.returncode==0
    p.wait(timeout=14)
    report=p.stdout.read()
    print("VLC_BRIDGE_REPORT",report.strip(),flush=True)
    assert "ROUTE_COUNTS" in report
    print("VLC_REAL_HOST_PASS",flush=True)
finally:
    if p.poll() is None:
        p.terminate()
        try:p.wait(timeout=4)
        except subprocess.TimeoutExpired:p.kill()
    print("BRIDGE_RETURN",p.returncode,"TAIL",p.stdout.read()[-500:], "ERROR",p.stderr.read()[-300:],flush=True)
