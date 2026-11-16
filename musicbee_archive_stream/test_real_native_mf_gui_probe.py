"""Build a test-only MusicBee plugin from the real Plugin.cs with a bounded startup test hook.

Output is isolated under AST_MUSICBEE_GUI_ARTIFACT_DIR, not the Release installer.
Uses the local MusicBee SDK interface on DEV; never ships the QA hook.
"""
from pathlib import Path
import os
import subprocess
r=Path(os.environ["AST_MUSICBEE_GUI_ARTIFACT_DIR"])
source=Path(__file__).resolve().parent/"Plugin.cs"
s=source.read_text(encoding="utf-8-sig")
old="public void ReceiveNotification(string sourceFileUrl, NotificationType type) {}"
new="""public void ReceiveNotification(string sourceFileUrl, NotificationType type)
        {
            if (type != NotificationType.PluginStartup)
                return;
            System.Threading.ThreadPool.QueueUserWorkItem(_ =>
            {
                System.Threading.Thread.Sleep(11000);
                try
                {
                    string archive = Environment.GetEnvironmentVariable("AST_NATIVE_ZIP_PATH");
                    string[] tracks = StartArchiveSession(archive);
                    string selectedText = Environment.GetEnvironmentVariable("AST_GUI_TRACK_INDEX");
                    if (!String.IsNullOrEmpty(selectedText))
                    {
                        int selected;
                        if (!Int32.TryParse(selectedText, out selected) || selected < 0 || selected >= tracks.Length)
                            throw new IndexOutOfRangeException("Invalid QA archive track selection");
                        tracks = new string[] { tracks[selected] };
                    }
                    Trace("QA_NATIVE_SESSION tracks=" + tracks.Length);
                    bool accepted = api.NowPlayingList_QueueFilesLast(tracks);
                    Trace("QA_NATIVE_QUEUE accepted=" + (accepted ? "1" : "0"));
                    if (accepted)
                        api.Player_PlayPause();
                    for (int i = 0; i < 21; ++i)
                    {
                        System.Threading.Thread.Sleep(1000);
                        Trace("QA_NATIVE_HOST state=" + api.Player_GetPlayState() + " position=" + api.Player_GetPosition() + " duration=" + api.NowPlaying_GetDuration());
                    }
                }
                catch (Exception e)
                {
                    Trace("QA_NATIVE_EXCEPTION type=" + e.GetType().Name + " message=" + e.Message);
                }
                Trace("QA_NATIVE_TRACE_COMPLETE");
            });
        }"""
assert s.count(old)==1
s=s.replace(old,new)
p=r/"PluginProductionProbe.cs"
p.write_text(s,encoding="utf-8")
csc=Path(r"C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe")
out=r/"MB_ArchiveStream_ProductionProbe.dll"
result=subprocess.run([str(csc),"/nologo","/target:library","/platform:x86","/optimize+","/r:System.Windows.Forms.dll","/r:System.Core.dll","/r:System.Drawing.dll",f"/out:{out}",str(p),r"D:\Temp\ArchiveStreamNewPlayers\MusicBeeInterface.cs"],capture_output=True,text=True)
print(result.stdout,result.stderr)
assert result.returncode==0,(result.returncode,result.stderr)
print("MUSICBEE_NATIVE_GUI_DISPOSABLE_PROBE_BUILD_PASS",out.stat().st_size)
