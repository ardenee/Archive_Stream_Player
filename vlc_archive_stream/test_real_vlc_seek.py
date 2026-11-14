"""Native VLC 3 SDK access module: actual media-player byte seek and PCM test.

Uses libVLC player API in an isolated process rather than treating a Range or
file-open call as a GUI seek. Decodes into a dummy audio file, no speakers.
"""
import ctypes, os, shutil, struct, subprocess, tempfile, time, wave
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
VLC=Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC")
MODULE=ROOT/"vlc_archive_stream"/"x64"/"Release"/"libarchive_stream_plugin.dll"
SEVENZIP=Path(r"C:\Program Files\7-Zip\7z.exe")
RATE=44100

def api():
    os.add_dll_directory(str(VLC))
    lib=ctypes.CDLL(str(VLC/"libvlc.dll"))
    def bind(name,args,result):
        fn=getattr(lib,name)
        fn.argtypes=args
        fn.restype=result
        return fn
    ptr=ctypes.c_void_p
    cint=ctypes.c_int
    return {
        "new":bind("libvlc_new",[cint,ctypes.POINTER(ctypes.c_char_p)],ptr),
        "release":bind("libvlc_release",[ptr],None),
        "media":bind("libvlc_media_new_location",[ptr,ctypes.c_char_p],ptr),
        "media_release":bind("libvlc_media_release",[ptr],None),
        "player":bind("libvlc_media_player_new_from_media",[ptr],ptr),
        "player_release":bind("libvlc_media_player_release",[ptr],None),
        "play":bind("libvlc_media_player_play",[ptr],cint),
        "stop":bind("libvlc_media_player_stop",[ptr],None),
        "time":bind("libvlc_media_player_get_time",[ptr],ctypes.c_longlong),
        "set_time":bind("libvlc_media_player_set_time",[ptr,ctypes.c_longlong],None),
        "state":bind("libvlc_media_player_get_state",[ptr],cint)
    }

def main():
    assert MODULE.is_file() and SEVENZIP.is_file()
    shutil.copy2(MODULE,VLC/"plugins"/"access"/MODULE.name)
    with tempfile.TemporaryDirectory(prefix="AST-VLC-NativeSeek-",dir=r"D:\Temp") as temp:
        qa=Path(temp)
        for name in ("Roaming","Local","Temp"):
            (qa/name).mkdir()
        os.environ.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
                          TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
                          ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        source=qa/"seek.wav"
        with wave.open(str(source),"wb") as w:
            w.setnchannels(1);w.setsampwidth(2);w.setframerate(RATE)
            for value,seconds in ((1000,300),(2000,300),(3000,600)):
                w.writeframes(struct.pack("<h",value)*RATE*seconds)
        archive=qa/"native-seek.7z"
        cmd=[str(SEVENZIP),"a","-t7z","-m0=LZMA2","-ms=off",str(archive),str(source)]
        r=subprocess.run(cmd,check=True,capture_output=True,text=True,cwd=qa,timeout=45)
        uri="ast://"+str(archive).encode("utf-8").hex().upper()+"/0/seek.wav"
        out=qa/"result.wav"
        args=["-I","dummy","--no-one-instance","--ignore-config",
              "--no-plugins-cache","--no-video","--aout=afile",
              "--audiofile-format=s16","--audiofile-file="+str(out)]
        native=api()
        encoded=(ctypes.c_char_p*len(args))(*[x.encode("utf-8") for x in args])
        instance=native["new"](len(args),encoded)
        assert instance,"libvlc_new failed"
        media=None;player=None
        try:
            media=native["media"](instance,uri.encode("utf-8"))
            assert media
            player=native["player"](media)
            assert player and native["play"](player)==0
            deadline=time.monotonic()+22
            before=-1
            while time.monotonic()<deadline:
                if native["state"](player)==7:
                    raise AssertionError("VLC player reported Error")
                before=native["time"](player)
                if before>=800:
                    break
                time.sleep(.12)
            assert before>=800,("initial playback did not advance",before)
            native["set_time"](player,850000)
            time.sleep(1.2)
            after=native["time"](player)
            assert after>750000,("native player seek did not reach target",before,after)
            time.sleep(1.7)
            resumed=native["time"](player)
            assert resumed>=after+500,("native seek did not resume",before,after,resumed)
            print("VLC_NATIVE_LIBVLC_GUI_API_SEEK_RESUME_PASS",before,after,resumed,flush=True)
        finally:
            if player:
                native["stop"](player)
                native["player_release"](player)
            if media:
                native["media_release"](media)
            native["release"](instance)
        log=qa/"Local"/"ArchiveStreamPlayer"/"Logs"/"archive_stream_vlc.log"
        trace=log.read_text(encoding="utf-8-sig",errors="replace") if log.is_file() else ""
        seeks=[line for line in trace.splitlines() if "ACCESS_SEEK" in line]
        print("VLC_NATIVE_ACCESS_SEEK_CALLBACKS",len(seeks),seeks[-5:],flush=True)
        assert seeks,"VLC playback seek did not reach Archive Stream native access callback"
        assert "ROUTE_STREAM" in trace and "ROUTE_CACHE" not in trace
        if out.is_file():
            with wave.open(str(out),"rb") as decoded:
                samples=decoded.readframes(decoded.getnframes())
                values=[int.from_bytes(samples[k:k+2],"little",signed=True)
                        for k in range(0,len(samples)-1,2)]
            print("VLC_NATIVE_SEEK_PCM",len(values),"first",values[:4],"last",values[-4:],flush=True)
            assert values and values[-1]==3000,"post-seek PCM did not reach last segment"
        print("VLC_NATIVE_SEEK_ISOLATED_PROFILE_CLEANED_PASS",flush=True)

if __name__=="__main__":
    main()
