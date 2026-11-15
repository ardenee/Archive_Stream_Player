"""VLC native-first launcher selection tests, no user VLC installation touched."""
import os,shutil,subprocess,tempfile,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SCRIPT=ROOT/"archive_player_bridge"/"launch-archive.ps1"
VLC=Path(r"D:\Temp\AST-VLC-ProbeBuild\VLC\vlc.exe")
PLUGIN=ROOT/"vlc_archive_stream"/"x64"/"Release"/"libarchive_stream_plugin.dll"
AUDIO=Path(r"C:\audiotest\Archives\7Z_NonSolid.7z")
def run(player,*extra,script=SCRIPT):
 cmd=["powershell.exe","-NoProfile","-NonInteractive","-File",str(script),
      "-Player","VLC","-PlayerPath",str(player),"-Archive",str(AUDIO),
      "-NoLaunch",*extra]
 return subprocess.run(cmd,capture_output=True,text=True,errors="replace",timeout=24)
def main():
 assert all(p.is_file() for p in (SCRIPT,VLC,PLUGIN,AUDIO))
 with tempfile.TemporaryDirectory(prefix="AST-VLC-Launcher-",dir=r"D:\Temp") as scratch:
  qa=Path(scratch)
  player=qa/"vlc"/"vlc.exe";player.parent.mkdir()
  shutil.copy2(VLC,player)
  plugin=qa/"vlc"/"plugins"/"access"/"libarchive_stream_plugin.dll"
  plugin.parent.mkdir(parents=True)
  shutil.copy2(PLUGIN,plugin)
  valid=run(player)
  assert valid.returncode==0 and "DRY_RUN_PASS player=VLC mode=native_sdk http=0" in valid.stdout,(
      valid.returncode,valid.stdout,valid.stderr)
  assert "NATIVE_VLC_PLAYLIST_ENTRIES=21" in valid.stdout,valid.stdout
  print("VLC_NATIVE_LAUNCHER_21_TRACKS_NO_HTTP_PASS",flush=True)
  forced=run(player,"-UseHttpBridge")
  assert forced.returncode==0 and "DRY_RUN_PASS player=VLC" in forced.stdout and "NATIVE_VLC_READY" not in forced.stdout,(
      forced.returncode,forced.stdout,forced.stderr)
  print("VLC_EXPLICIT_HTTP_BRIDGE_COMPATIBILITY_PASS",flush=True)
  overr=run(player,"-MemoryMB","1")
  assert overr.returncode==0 and "VLC_BRIDGE_FALLBACK reason=per_launch_overrides_require_bridge" in overr.stdout,(
      overr.returncode,overr.stdout,overr.stderr)
  print("VLC_CLI_OVERRIDES_PRESERVE_HTTP_ROUTE_PASS",flush=True)
  plugin.unlink()
  external=run(player)
  assert external.returncode==0 and "DRY_RUN_PASS player=VLC mode=native_sdk http=0" in external.stdout,(
      external.returncode,external.stdout,external.stderr)
  print("VLC_NATIVE_SOURCE_DLL_WITHOUT_VLC_INSTALL_PASS",flush=True)

  # In a distributed copy with neither local DLL nor playlist builder,
  # the original HTTP bridge remains functional with no silent failure.
  standalone=qa/"bridge-only"/"archive_player_bridge"
  standalone.mkdir(parents=True)
  isolated_script=standalone/"launch-archive.ps1"
  shutil.copy2(SCRIPT,isolated_script)
  existing=ROOT/"archive_player_bridge"/"x64"/"Release"/"archive_player_bridge.exe"
  bridge=standalone/"x64"/"Release"/"archive_player_bridge.exe"
  bridge.parent.mkdir(parents=True)
  shutil.copy2(existing,bridge)
  absent=run(player,script=isolated_script)
  assert absent.returncode==0 and "VLC_BRIDGE_FALLBACK reason=native_dll_or_playlist_builder_not_available" in absent.stdout and "DRY_RUN_PASS player=VLC" in absent.stdout,(
      absent.returncode,absent.stdout,absent.stderr)
  print("VLC_MISSING_ALL_NATIVE_COMPONENTS_SAFE_HTTP_FALLBACK_PASS",flush=True)
  package=ROOT/"vlc_archive_stream"/"Release"/"ArchiveStreamPlayer-VLC3-x64.zip"
  assert package.is_file(),"Build the native package before running this integration test"
  extracted=qa/"extracted-native-package"
  with zipfile.ZipFile(package) as archive:
   archive.extractall(extracted)
  bundled=run(player,script=extracted/"launch-archive.ps1")
  assert bundled.returncode==0 and "DRY_RUN_PASS player=VLC mode=native_sdk http=0" in bundled.stdout,(bundled.returncode,bundled.stdout,bundled.stderr)
  print("VLC_STANDALONE_NATIVE_PACKAGE_WITHOUT_INSTALLED_DLL_PASS",flush=True)
 print("VLC_LAUNCHER_DISPOSABLE_PROFILE_CLEANED_PASS",flush=True)
if __name__=="__main__":main()
