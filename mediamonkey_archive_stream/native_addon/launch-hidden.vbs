' Launch an Archive Stream PowerShell task without a transient console.
' Script path and arguments are supplied by the trusted MediaMonkey addon.
Option Explicit

Dim shell, powershell, command, i, arg
If WScript.Arguments.Count < 1 Then WScript.Quit 2

Set shell = CreateObject("WScript.Shell")
powershell = shell.ExpandEnvironmentStrings("%SystemRoot%") & "\System32\WindowsPowerShell\v1.0\powershell.exe"
command = Chr(34) & powershell & Chr(34) & " -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File"

For i = 0 To WScript.Arguments.Count - 1
    arg = WScript.Arguments(i)
    If InStr(arg, Chr(34)) > 0 Or InStr(arg, vbCr) > 0 Or InStr(arg, vbLf) > 0 Then WScript.Quit 2
    command = command & " " & Chr(34) & arg & Chr(34)
Next

shell.Run command, 0, False
