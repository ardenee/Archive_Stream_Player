"""Real VLC 3 x64 source-vs-native archive decoder parity for seven audio codecs.

Uses a disposable VLC 3 installation, synthetic audio, silent-file output and
isolated user settings. Compares exact VLC-decoded PCM for ordinary source files
against ast:// native playback; lossy formats are compared to their own original
compressed source, NOT to pre-encode PCM. No HTTP bridge or sound hardware.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import wave
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VLC = Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC\vlc.exe")
MODULE = ROOT / "vlc_archive_stream" / "x64" / "Release" / "libarchive_stream_plugin.dll"
FFMPEG = Path(r"C:\Program Files\AudioBookConverter\app\external\ffmpeg.exe")
SEVENZIP = Path(r"C:\Program Files\7-Zip\7z.exe")
RAR = Path(r"C:\Program Files\WinRAR\Rar.exe")
RATE = 44100
CODECS = (("wav", "pcm_s16le"), ("mp3", "libmp3lame"), ("ogg", "libvorbis"),
          ("flac", "flac"), ("wma", "wmav2"), ("m4a", "aac"), ("aac", "aac"))


def check_run(cmd, *, cwd, env=None, timeout=35):
    result = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True,
                            errors="replace", timeout=timeout)
    assert result.returncode == 0, (cmd[0], result.returncode, result.stderr[-1800:])
    return result


def pcm_output(path):
    assert path.is_file() and path.stat().st_size > 1000, ("missing audio output", path)
    with wave.open(str(path), "rb") as decoded:
        frames = decoded.getnframes()
        assert decoded.getnchannels() in (1, 2) and decoded.getsampwidth() == 2, (
            path, decoded.getnchannels(), decoded.getsampwidth())
        pcm = decoded.readframes(frames)
    assert frames > 40000 and frames < 120000, ("unexpected duration", path, frames)
    assert any(pcm), ("unexpected all-zero output", path)
    return frames, pcm


def make_sources(qa):
    original = qa / "ast-source.wav"
    with wave.open(str(original), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        for sample in (1234, -2345, 3210, -4321):
            wav.writeframes(struct.pack("<h", sample) * (RATE // 2))
    files = []
    for ext, encoder in CODECS:
        destination = qa / ("ast-codec." + ext)
        check_run([str(FFMPEG), "-hide_banner", "-loglevel", "error", "-y",
                   "-i", str(original), "-map", "0:a:0", "-c:a", encoder,
                   "-ar", str(RATE), "-ac", "1", str(destination)], cwd=qa)
        assert destination.is_file() and destination.stat().st_size > 100
        files.append(destination)
    return files


def make_archive(kind, qa, files):
    archive = qa / ("seven-codecs." + kind)
    if kind == "zip":
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_STORED) as output:
            for file in files:
                output.write(file, file.name)
    elif kind == "7z":
        check_run([str(SEVENZIP), "a", "-t7z", "-m0=LZMA2", "-ms=off",
                   str(archive), *(str(file) for file in files)], cwd=qa, timeout=75)
    elif kind == "rar":
        check_run([str(RAR), "a", "-ma5", "-m0", "-s-", "-ep",
                   str(archive), *(str(file) for file in files)], cwd=qa, timeout=75)
    assert archive.is_file()
    return archive


def render(uri, output, env):
    args = [str(VLC), "-I", "dummy", "--no-one-instance", "--ignore-config",
            "--no-plugins-cache", "--no-video", "--play-and-exit",
            "--aout=afile", "--audiofile-format=s16",
            "--audiofile-file=" + str(output), uri]
    check_run(args, cwd=VLC.parent, env=env, timeout=35)
    return pcm_output(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", choices=("zip", "7z", "rar", "all"), default="all")
    options = parser.parse_args()
    assert all(path.is_file() for path in (VLC, MODULE, FFMPEG, SEVENZIP, RAR))
    access_dir = VLC.parent / "plugins" / "access"
    access_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(MODULE, access_dir / MODULE.name)
    kinds = ("zip", "7z", "rar") if options.kind == "all" else (options.kind,)
    with tempfile.TemporaryDirectory(prefix="AST-VLC-SevenCodecs-", dir=r"D:\Temp") as tmp:
        qa = Path(tmp)
        for folder in ("Local", "Roaming", "Temp"):
            (qa / folder).mkdir()
        env = os.environ.copy()
        env.update(APPDATA=str(qa / "Roaming"), LOCALAPPDATA=str(qa / "Local"),
                   TEMP=str(qa / "Temp"), TMP=str(qa / "Temp"),
                   ARCHIVE_STREAM_ADAPTER_DEBUG="1")
        log = qa / "Local" / "ArchiveStreamPlayer" / "Logs" / "archive_stream_vlc.log"
        tracks = make_sources(qa)
        reference = {}
        for file in tracks:
            reference[file.name] = render(str(file), qa / (file.suffix[1:] + "-reference.wav"), env)
            print("VLC_CODEC_REFERENCE_PASS", file.suffix, reference[file.name][0], flush=True)
        for kind in kinds:
            archive = make_archive(kind, qa, tracks)
            listing = check_run([str(SEVENZIP), "l", "-slt", str(archive)], cwd=qa).stdout
            order = [line[len("Path = "):].strip() for line in listing.splitlines()
                     if line.startswith("Path = ast-codec.")]
            assert len(order) == len(tracks) and set(order) == {file.name for file in tracks}, (kind, order)
            print("VLC_CODEC_ARCHIVE_MEMBER_ORDER", kind, order, flush=True)
            for file in tracks:
                index = order.index(file.name)
                if log.is_file():
                    log.unlink()
                uri = "ast://" + str(archive).encode("utf-8").hex().upper() + f"/{index}/" + file.name
                frames, pcm = render(uri, qa / (kind + "-" + file.suffix[1:] + ".wav"), env)
                expected_frames, expected_pcm = reference[file.name]
                assert (frames, pcm) == (expected_frames, expected_pcm), (
                    kind, file.suffix, frames, expected_frames,
                    hashlib.sha256(pcm).hexdigest(), hashlib.sha256(expected_pcm).hexdigest())
                assert log.is_file(), (kind, file.suffix, "native access plugin not invoked")
                trace = log.read_text(encoding="utf-8-sig", errors="replace")
                assert trace.count("ACCESS_OPEN_OK") == 1, (kind, file.suffix, trace[-1600:])
                assert trace.count("ROUTE_STREAM") == 1, (kind, file.suffix, trace[-1600:])
                assert "ROUTE_MEMORY" not in trace and "ROUTE_CACHE" not in trace, (
                    kind, file.suffix, trace[-1600:])
                assert "http://127.0.0.1" not in trace and "localhost" not in trace.lower()
                print("VLC_NATIVE_SEVEN_CODEC_PCM_PARITY_PASS", kind, file.suffix,
                      "frames", frames, "sha256", hashlib.sha256(pcm).hexdigest()[:16],
                      "stream=1 memory=0 cache=0 http=0", flush=True)
        print("VLC_NATIVE_SEVEN_CODEC_MATRIX_PASS", len(kinds) * len(tracks),
              "isolated_profile_removed=1", flush=True)


if __name__ == "__main__":
    main()
