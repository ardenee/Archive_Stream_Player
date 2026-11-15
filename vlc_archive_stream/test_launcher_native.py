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
  assert "NATIVE_VLC_READY=1 mode=in_process http=0" in valid.stdout and "NATIVE_VLC_PLAYLIST_ENTRIES" not in valid.stdout,valid.stdout
  print("VLC_VERSIONED_INPROCESS_NATIVE_LAUNCHER_NO_HTTP_PASS",flush=True)
  qa_password_list=qa/"temporary-password-list"/"keeps.txt"
  qa_password_list.parent.mkdir()
  native_list=run(player,"-PasswordList",str(qa_password_list))
  assert native_list.returncode==0 and "NATIVE_VLC_PASSWORD_LIST_OVERRIDE=1" in native_list.stdout and "NATIVE_VLC_READY=1 mode=in_process http=0" in native_list.stdout,(native_list.returncode,native_list.stdout,native_list.stderr)
  print("VLC_V12_NATIVE_PER_LAUNCH_PASSWORD_LIST_NO_HTTP_PASS",flush=True)
  missing_parent=run(player,"-PasswordList",str(qa/"nonexistent-password-folder"/"keeps.txt"))
  assert missing_parent.returncode!=0 or "VLC native password-list parent directory must exist" in (missing_parent.stdout+missing_parent.stderr),(missing_parent.returncode,missing_parent.stdout,missing_parent.stderr)
  assert "DRY_RUN_PASS player=VLC mode=native_sdk http=0" not in missing_parent.stdout
  print("VLC_V12_INVALID_PASSWORD_LIST_FAIL_CLOSED_PASS",flush=True)
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
  assert "NATIVE_VLC_READY=1 mode=in_process http=0" in external.stdout,external.stdout
  print("VLC_NATIVE_SOURCE_DLL_WITHOUT_VLC_INSTALL_PASS",flush=True)

  # Previous unversioned adapters still select the external M3U8 builder.
  old_plugin=Path(r"D:\Temp\AST-VLC-ProbeBuild\libarchive_stream_plugin.dll")
  if old_plugin.is_file():
   shutil.copy2(old_plugin,plugin)
   older=run(player)
   assert older.returncode==0 and "NATIVE_VLC_READY=1 mode=external_playlist" in older.stdout and "NATIVE_VLC_PLAYLIST_ENTRIES=21" in older.stdout,(older.returncode,older.stdout,older.stderr)
   print("VLC_LEGACY_UNVERSIONED_NATIVE_DLL_BUILDER_FALLBACK_PASS",flush=True)
   old_with_list=run(player,"-PasswordList",str(qa_password_list))
   assert old_with_list.returncode==0 and "VLC_BRIDGE_FALLBACK reason=per_launch_overrides_require_bridge" in old_with_list.stdout and "DRY_RUN_PASS player=VLC" in old_with_list.stdout,(old_with_list.returncode,old_with_list.stdout,old_with_list.stderr)
   print("VLC_OLD_NATIVE_DLL_PASSWORD_LIST_USES_HTTP_PASS",flush=True)
   plugin.unlink()

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
  assert "NATIVE_VLC_READY=1 mode=in_process http=0" in bundled.stdout,bundled.stdout
  print("VLC_STANDALONE_NATIVE_PACKAGE_WITHOUT_INSTALLED_DLL_PASS",flush=True)
  (extracted/"vlc_native_playlist.exe").unlink()
  no_builder=run(player,script=extracted/"launch-archive.ps1")
  assert no_builder.returncode==0 and "NATIVE_VLC_READY=1 mode=in_process http=0" in no_builder.stdout,(no_builder.returncode,no_builder.stdout,no_builder.stderr)
  print("VLC_INPROCESS_NATIVE_ALBUM_WITHOUT_EXTERNAL_BUILDER_PASS",flush=True)
 print("VLC_LAUNCHER_DISPOSABLE_PROFILE_CLEANED_PASS",flush=True)
if __name__=="__main__":main()
