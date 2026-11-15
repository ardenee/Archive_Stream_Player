"""Real VLC 3 x64 player seek/resume on seven codecs inside ZIP/7z/RAR.

Checks libVLC time API (six codecs) and position API (OGG), advancing clock,
post-seek 880 Hz PCM through paced libVLC audio callbacks, and native direct
archive streaming. ACCESS_SEEK may be satisfied by VLC's buffered data and is
not required in these short fixtures. No HTTP service or speaker output.
"""
import argparse
from array import array
import ctypes
import math
import os
from pathlib import Path
import shutil
import struct
import tempfile
import threading
import time
import wave

from test_real_vlc_seek import api
from test_real_vlc_seven_codecs import CODECS, FFMPEG, MODULE, RATE, RAR, SEVENZIP, VLC, check_run, make_archive


class TimedPcmSink:
    """libVLC 3 C audio callback, paced and retaining only recent decoded samples."""

    PLAYCB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_int64)

    def __init__(self):
        self.lock = threading.Lock()
        self.tail = bytearray()
        self.received = 0
        self.callbacks = 0
        self.callback = self.PLAYCB(self._play)

    def _play(self, ignored_opaque, samples, count, ignored_pts):
        if count > 0 and samples:
            pcm = ctypes.string_at(samples, count * 2)
            with self.lock:
                self.tail = (self.tail + pcm)[-(RATE * 2):]
                self.received += count
                self.callbacks += 1
        time.sleep(min(count / RATE, 0.2))

    def clear(self):
        with self.lock:
            self.tail.clear()

    def pcm(self):
        with self.lock:
            return bytes(self.tail), self.received, self.callbacks


def audio_api():
    lib = ctypes.CDLL(str(VLC.parent / "libvlc.dll"))
    callbacks = lib.libvlc_audio_set_callbacks
    callbacks.argtypes = [ctypes.c_void_p, TimedPcmSink.PLAYCB,
                          ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                          ctypes.c_void_p, ctypes.c_void_p]
    callbacks.restype = None
    set_format = lib.libvlc_audio_set_format
    set_format.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint]
    set_format.restype = None
    get_length = lib.libvlc_media_player_get_length
    get_length.argtypes = [ctypes.c_void_p]
    get_length.restype = ctypes.c_longlong
    return callbacks, set_format, get_length


def source_tracks(qa):
    source = qa / "native-seek-source.wav"
    with wave.open(str(source), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        for freq, duration in ((220, 30), (440, 30), (880, 60)):
            cycle = b"".join(struct.pack("<h", round(12000 * math.sin(2 * math.pi * freq * i / RATE)))
                             for i in range(RATE))
            for _ in range(duration):
                out.writeframes(cycle)
    tracks = []
    for ext, encoder in CODECS:
        path = qa / ("seek-codec." + ext)
        check_run([str(FFMPEG), "-hide_banner", "-loglevel", "error", "-y", "-i", str(source),
                   "-map", "0:a:0", "-c:a", encoder, "-ar", str(RATE),
                   "-ac", "1", str(path)], cwd=qa, timeout=60)
        assert path.is_file() and path.stat().st_size > 1000, path
        tracks.append(path)
    return tracks


def native_seek(player_api, qa, env, kind, ext, uri, archive_log, use_position=False):
    if archive_log.exists():
        archive_log.unlink()
    args = ["-I", "dummy", "--no-one-instance", "--ignore-config", "--no-plugins-cache", "--no-video"]
    encoded = (ctypes.c_char_p * len(args))(*[value.encode("utf-8") for value in args])
    instance = player_api["new"](len(args), encoded)
    assert instance, ("libvlc_new", kind, ext)
    media = None
    player = None
    before = after = resumed = -1
    sink = TimedPcmSink()
    callbacks, set_format, get_length = audio_api()
    try:
        media = player_api["media"](instance, uri.encode("utf-8"))
        assert media, ("libvlc_media_new_location", kind, ext)
        player = player_api["player"](media)
        assert player, ("libvlc_media_player_new_from_media", kind, ext)
        callbacks(player, sink.callback, None, None, None, None, None)
        set_format(player, b"S16N", RATE, 1)
        assert player_api["play"](player) == 0, ("libvlc_play", kind, ext)
        end = time.monotonic() + 14
        while time.monotonic() < end:
            if player_api["state"](player) == 7:
                raise AssertionError(("VLC player error", kind, ext))
            before = player_api["time"](player)
            if before >= 150 and player_api["state"](player) == 3:
                break
            time.sleep(0.03)
        assert before >= 150 and player_api["state"](player) == 3, ("preseek did not advance", kind, ext, before)
        length = get_length(player)
        assert length >= 110000, ("invalid source duration", kind, ext, length)
        sink.clear()
        if use_position:
            lib = ctypes.CDLL(str(VLC.parent / "libvlc.dll"))
            set_position = lib.libvlc_media_player_set_position
            set_position.argtypes = [ctypes.c_void_p, ctypes.c_float]
            set_position.restype = None
            set_position(player, ctypes.c_float(95 / 120))
        else:
            player_api["set_time"](player, 95000)
        end = time.monotonic() + 7
        while time.monotonic() < end:
            if player_api["state"](player) == 7:
                raise AssertionError(("VLC player error after seek", kind, ext))
            after = player_api["time"](player)
            if after >= 90000:
                break
            time.sleep(0.1)
        assert after >= 90000, ("seek position failed", kind, ext, before, after)
        time.sleep(1.4)
        resumed = player_api["time"](player)
        if resumed < after + 500:
            trace = archive_log.read_text(encoding="utf-8-sig", errors="replace") if archive_log.is_file() else ""
            print("VLC_CODEC_SEEK_RESUME_DIAG", kind, ext, "state", player_api["state"](player),
                  "before", before, "after", after, "resumed", resumed, "length", get_length(player),
                  "pcm_frames", sink.pcm()[1], "callbacks", sink.pcm()[2],
                  "trace", trace[-1800:], flush=True)
        assert resumed >= after + 500, ("playback did not resume", kind, ext, before, after, resumed)
    finally:
        if player:
            player_api["stop"](player)
            player_api["player_release"](player)
        if media:
            player_api["media_release"](media)
        player_api["release"](instance)
    trace = archive_log.read_text(encoding="utf-8-sig", errors="replace") if archive_log.is_file() else ""
    seek_events = [s for s in trace.splitlines() if "ACCESS_SEEK" in s]
    if kind != "source":
        assert archive_log.is_file(), ("missing native trace", kind, ext)
        if not seek_events:
            print("VLC_NATIVE_SEEK_WITHOUT_ACCESS_CALLBACK", kind, ext, flush=True)
        assert "ACCESS_OPEN_OK" in trace and "ROUTE_STREAM" in trace, ("missing native direct stream", kind, ext, trace[-2000:])
        assert "ROUTE_MEMORY" not in trace and "ROUTE_CACHE" not in trace, ("unexpected fallback", kind, ext, trace[-2000:])
    raw, delivered, callbacks_count = sink.pcm()
    assert delivered > RATE and callbacks_count > 0, ("no host PCM callbacks", kind, ext, delivered)
    pcm = array("h")
    pcm.frombytes(raw[-round(RATE * .2) * 2:])
    samples = pcm
    assert len(samples) >= 3000, ("insufficient output PCM", kind, ext, len(samples))
    level = max(abs(sample) for sample in samples)
    assert level > 1000, ("post-seek audio silent", kind, ext, level)
    crossings = sum((samples[i - 1] < 0) != (samples[i] < 0)
                    for i in range(1, len(samples)))
    # 880 Hz => ~350 crossings in 0.2s, whereas 440 Hz => ~176 and 220 Hz => ~88.
    assert 270 < crossings < 440, ("wrong post-seek audio frequency", kind, ext, crossings, level,
                                    before, after, resumed, seek_events[-2:])
    prefix = "VLC_DIRECT_FILE_SEEK_PCM_PASS" if kind == "source" else "VLC_NATIVE_CODEC_SEEK_PCM_PASS"
    route = "original_file=1" if kind == "source" else "stream=1 memory=0 cache=0 http=0"
    print(prefix, kind, ext, "seek_api", "position" if use_position else "time",
          "before_ms", before, "after_ms", after, "resumed_ms", resumed,
          "access_seeks", len(seek_events), "pcm_crossings_0.2s", crossings,
          "level", level, "callback_frames", delivered, route, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", choices=("zip", "7z", "rar", "all"), default="all")
    parser.add_argument("--only", choices=tuple(ext for ext, _ in CODECS))
    parser.add_argument("--exclude", action="append", choices=tuple(ext for ext, _ in CODECS), default=[])
    parser.add_argument("--baseline", action="store_true", help="diagnose direct-file host seek")
    parser.add_argument("--ogg-time", action="store_true", help="reproduce libVLC direct-file OGG time-seek ending")
    options = parser.parse_args()
    assert all(path.is_file() for path in (VLC, MODULE, FFMPEG, SEVENZIP, RAR))
    path = VLC.parent / "plugins" / "access"
    path.mkdir(parents=True, exist_ok=True)
    shutil.copy2(MODULE, path / MODULE.name)
    kinds = ("zip", "7z", "rar") if options.kind == "all" else (options.kind,)
    with tempfile.TemporaryDirectory(prefix="AST-VLC-CodecsSeek-", dir=r"D:\Temp") as tmp:
        qa = Path(tmp)
        for folder in ("Local", "Roaming", "Temp"):
            (qa / folder).mkdir()
        os.environ.update(APPDATA=str(qa / "Roaming"), LOCALAPPDATA=str(qa / "Local"),
                          TEMP=str(qa / "Temp"), TMP=str(qa / "Temp"),
                          ARCHIVE_STREAM_ADAPTER_DEBUG="1",
                          ARCHIVE_STREAM_VLC_PASSWORD_LIST=str(qa / "Temp" / "keeps.txt"))
        log = qa / "Local" / "ArchiveStreamPlayer" / "Logs" / "archive_stream_vlc.log"
        tracks = source_tracks(qa)
        native = api()
        if options.baseline:
            for track in tracks:
                ext = track.suffix.removeprefix(".")
                if options.only and options.only != ext:
                    continue
                native_seek(native, qa, os.environ, "source", ext, track.as_uri(), log, ext == "ogg" and not options.ogg_time)
            return
        count = 0
        for kind in kinds:
            archive = make_archive(kind, qa, tracks)
            listing = check_run([str(SEVENZIP), "l", "-slt", str(archive)], cwd=qa).stdout
            order = [line.split("Path = ", 1)[1].strip() for line in listing.splitlines()
                     if line.startswith("Path = seek-codec.")]
            assert len(order) == len(tracks) and set(order) == {file.name for file in tracks}, (kind, order)
            for track in tracks:
                ext = track.suffix.removeprefix(".")
                if (options.only and options.only != ext) or ext in options.exclude:
                    continue
                uri = ("ast://" + str(archive).encode("utf-8").hex().upper()
                       + f"/{order.index(track.name)}/{track.name}")
                native_seek(native, qa, os.environ, kind, ext, uri, log, ext == "ogg" and not options.ogg_time)
                count += 1
        expected_count = len(kinds) * sum(1 for ext, _ in CODECS if ext not in options.exclude
                                          and (not options.only or options.only == ext))
        assert count == expected_count and count > 0, (count, expected_count)
        print("VLC_NATIVE_CODEC_SEEK_MATRIX_PASS", count, "isolated_profile_removed=1", flush=True)


if __name__ == "__main__":
    main()
