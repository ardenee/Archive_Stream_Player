# Winamp output sink — QA only

`out_archive_stream_test.cpp` is a small **test-only** Winamp output plugin implementing the public `Out_Module` interface. It provides a bounded, simulated two-second audio queue, tracks PCM buffers delivered by the real Winamp input plugin and reports initial PCM writes and `Flush()` seek events to a temporary file specified by `AST_WINAMP_TEST_SINK_REPORT`.

**It does not play sound or verify bitwise decoded PCM equality.** The host tests in [`../test_password_prompt_ui.py`](../test_password_prompt_ui.py) use it to verify end-to-end input-plugin output delivery and seeking when the DEV system has no functional DirectSound device. Tests create an isolated Winamp/WACUP copy and choose this sink only in that copy.

Build on DEV with Visual Studio MSBuild:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' '.\in_archive_stream\test_support\out_archive_stream_test.vcxproj' /p:Configuration=Release /p:Platform=Win32
```

The resulting `Release/out_archive_stream_test.dll` is **not** an Archive Stream Player distribution component. It must not be copied into real Winamp/WACUP installations or included by release packaging. Generated Release files must not be committed.

This original test utility is covered by the repository MIT licence. The Winamp SDK interface header under `third_party/winamp` retains its own licence terms.
