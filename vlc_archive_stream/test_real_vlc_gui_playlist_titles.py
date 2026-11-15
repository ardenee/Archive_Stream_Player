r"""Real VLC 3 x64 native album titles in its actual Qt player window.

The production access module generates #EXTINF titles for the in-process
ast:// album. This test confirms VLC displays both custom titles as consecutive
tracks play, without HTTP or extraction. It does not claim live repaint.
"""
import os
import subprocess
import tempfile
import time
import wave
import struct
from pathlib import Path

from test_real_vlc_gui_seek import VLC, MODULE, poll, dismiss_privacy, player_window
from test_real_vlc_seven_codecs import SEVENZIP, RAR, check_run, make_archive

TITLE_PRESET = "<#>. <filename>"

def create_tracks(qa):
    tracks = []
    for index, value in enumerate((1200, 2700), 1):
        path = qa / f"track{index}.wav"
        with wave.open(str(path), "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(44100)
            out.writeframes(struct.pack("<h", value) * 44100 * 8)
        tracks.append(path)
    return tracks

def test_album(kind, qa, files, env):
    archive = make_archive(kind, qa, files)
    log = qa / "Local" / "ArchiveStreamPlayer" / "Logs" / "archive_stream_vlc.log"
    if log.is_file():
        log.unlink()
    uri = "ast://" + str(archive).encode("utf-8").hex().upper() + "/list/album.m3u8"
    cmd = [str(VLC), "--no-one-instance", "--ignore-config", "--no-plugins-cache",
           "--no-video", "--aout=dummy", "--no-media-library", uri]
    proc = subprocess.Popen(cmd, cwd=VLC.parent, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    expected = ["1. track1.wav", "2. track2.wav"]
    try:
        window = poll(lambda: player_window(proc.pid), 18, "VLC native album window")
        dismiss_privacy(proc.pid)
        for title in expected:
            def shown():
                current = window.window_text()
                if current == title + " - VLC media player":
                    return current
                return None
            poll(shown, 16, f"real VLC window showing {title} ({kind})")
            print("VLC_NATIVE_REAL_GUI_ALBUM_TITLE_VISIBLE", kind, title,flush=True)
        def finished():
            if not log.is_file():
                return None
            contents = log.read_text(encoding="utf-8-sig",errors="replace")
            if contents.count("ACCESS_OPEN_OK") >= 2:
                return contents
            return None
        trace = poll(finished, 10, f"VLC native {kind} album opened both tracks")
        for title in expected:
            assert f"title={title}" in trace, ("native #EXTINF mismatch", kind, title,trace[-1500:])
        assert trace.count("NATIVE_PLAYLIST_OPEN") == 1, (kind, trace[-1500:])
        assert trace.count("ACCESS_OPEN_OK") == 2, (kind, trace[-1500:])
        assert trace.count("ROUTE_STREAM") == 2, (kind, trace[-1500:])
        assert "ROUTE_MEMORY" not in trace and "ROUTE_CACHE" not in trace, (kind,trace[-1500:])
        assert "http://127.0.0.1" not in trace and "localhost:" not in trace.lower()
        print("VLC_NATIVE_REAL_GUI_ALBUM_TITLE_SEQUENCE_PASS",kind,
              "titles=2", "stream=2 memory=0 cache=0 http=0",flush=True)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=7)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=7)

def main():
    assert VLC.is_file() and MODULE.is_file() and SEVENZIP.is_file() and RAR.is_file()
    running = subprocess.run(["tasklist","/FI","IMAGENAME eq vlc.exe"],
                             capture_output=True,text=True,timeout=10).stdout
    if "vlc.exe" in running.lower():
        raise SystemExit("VLC already running; refusing to affect the existing player")
    with tempfile.TemporaryDirectory(prefix="AST-VLC-GUI-Titles-",dir=r"D:\Temp") as scratch:
        qa = Path(scratch)
        for name in ("Local","Roaming","Temp"):
            (qa/name).mkdir()
        tracks = create_tracks(qa)
        env = os.environ.copy()
        env.update(APPDATA=str(qa/"Roaming"),LOCALAPPDATA=str(qa/"Local"),
                   TEMP=str(qa/"Temp"),TMP=str(qa/"Temp"),
                   VLC_PLUGIN_PATH=str(MODULE.parent),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1",
                   ARCHIVE_STREAM_VLC_TITLE_TEMPLATE=TITLE_PRESET,
                   ARCHIVE_STREAM_VLC_PASSWORD_LIST=str(qa/"Temp"/"keeps.txt"))
        for kind in ("zip","7z","rar"):
            test_album(kind,qa,tracks,env)
        print("VLC_NATIVE_REAL_GUI_ALBUM_TITLE_MATRIX_PASS",3,flush=True)
    print("VLC_NATIVE_GUI_TITLE_ISOLATED_PROFILE_CLEANED_PASS",flush=True)

if __name__ == "__main__":
    main()
