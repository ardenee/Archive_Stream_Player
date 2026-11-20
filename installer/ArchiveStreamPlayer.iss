; Open in Inno Setup 7 and choose Build > Compile.
#define AppName "Archive Stream Player"
#define AppVersion "0.1.11-dev"
#define HomeURL "https://www.arden.ie/projects/archive-stream-player/"

[Setup]
AppId={{B8C3136D-DFAF-4A70-9E3F-3913109487C5}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=arden
AppPublisherURL={#HomeURL}
AppSupportURL={#HomeURL}
AppUpdatesURL={#HomeURL}
DefaultDirName={autopf}\Archive Stream Player
DefaultGroupName=Archive Stream Player
OutputDir=output
OutputBaseFilename=ArchiveStreamPlayer-Setup-DEV
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=admin
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UninstallDisplayName=Archive Stream Player
MinVersion=10.0
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE

[Types]
Name: "custom"; Description: "Choose installed players"; Flags: iscustom

[Components]
Name: "winamp"; Description: "Winamp 32-bit - native playback and playlists"; Types: custom
Name: "wacup"; Description: "WACUP 32-bit - native Winamp-compatible plugins"; Types: custom
Name: "foobar32"; Description: "foobar2000 32-bit - native component"; Types: custom
Name: "foobar64"; Description: "foobar2000 64-bit - native component"; Types: custom
Name: "aimp32"; Description: "AIMP 32-bit - native plugin"; Types: custom
Name: "aimp64"; Description: "AIMP 64-bit - native plugin"; Types: custom
Name: "xmplay"; Description: "XMPlay 32-bit - native 7z/RAR plugin"; Types: custom
Name: "musicbee"; Description: "MusicBee 32-bit - native plugin and Tools menu"; Types: custom
Name: "mediamonkey"; Description: "MediaMonkey 5/2024 - native playback and archive Tools menu"; Types: custom
Name: "vlc"; Description: "VLC 3 64-bit - native ZIP/7z/RAR Open File and drag-and-drop plugin"; Types: custom

[Tasks]
Name: "wacup_zip_override"; Description: "Use Archive Stream for ZIP files in WACUP (disable WACUP built-in ZIP plugin; restore on uninstall)"; Components: wacup; Flags: checkedonce

[Files]
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\mediamonkey_archive_stream\native_addon\launch-hidden.vbs"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}\Notices"; Flags: ignoreversion
Source: "..\LICENSING.md"; DestDir: "{app}\Notices"; Flags: ignoreversion
Source: "..\third_party\README.md"; DestDir: "{app}\Notices\third_party"; Flags: ignoreversion
Source: "..\third_party\libarchive\COPYING"; DestDir: "{app}\Notices\third_party\libarchive"; Flags: ignoreversion
Source: "..\third_party\libarchive\README.md"; DestDir: "{app}\Notices\third_party\libarchive"; Flags: ignoreversion
Source: "..\third_party\lzma-sdk\README.md"; DestDir: "{app}\Notices\third_party\lzma-sdk"; Flags: ignoreversion
Source: "..\third_party\unrar\license.txt"; DestDir: "{app}\Notices\third_party\unrar"; Flags: ignoreversion
Source: "..\third_party\unrar\README.md"; DestDir: "{app}\Notices\third_party\unrar"; Flags: ignoreversion
Source: "..\third_party\zlib\LICENSE"; DestDir: "{app}\Notices\third_party\zlib"; Flags: ignoreversion
Source: "..\third_party\zlib\README.md"; DestDir: "{app}\Notices\third_party\zlib"; Flags: ignoreversion
Source: "..\third_party\dr_libs\README.md"; DestDir: "{app}\Notices\third_party\dr_libs"; Flags: ignoreversion
Source: "..\third_party\stb\README.md"; DestDir: "{app}\Notices\third_party\stb"; Flags: ignoreversion
Source: "..\third_party\winamp\README.md"; DestDir: "{app}\Notices\third_party\winamp"; Flags: ignoreversion

Source: "..\in_archive_stream\Release\in_archive_stream.dll"; DestDir: "{code:GetPlayerDir|winamp}\Plugins"; Components: winamp; Flags: ignoreversion
Source: "..\gen_archive_stream\Release\gen_archive_stream.dll"; DestDir: "{code:GetPlayerDir|winamp}\Plugins"; Components: winamp; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{code:GetPlayerDir|winamp}\Plugins"; Components: winamp; Flags: ignoreversion
Source: "..\in_archive_stream\Release\in_archive_stream.dll"; DestDir: "{code:GetPlayerDir|wacup}\Plugins"; Components: wacup; Flags: ignoreversion
Source: "..\gen_archive_stream\Release\gen_archive_stream.dll"; DestDir: "{code:GetPlayerDir|wacup}\Plugins"; Components: wacup; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{code:GetPlayerDir|wacup}\Plugins"; Components: wacup; Flags: ignoreversion

Source: "..\foo_archive_stream\Release\foo_archive_stream.dll"; DestDir: "{code:GetPlayerDir|foobar32}\components"; Components: foobar32; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{code:GetPlayerDir|foobar32}\components"; Components: foobar32; Flags: ignoreversion
Source: "..\foo_archive_stream\x64\Release\foo_archive_stream.dll"; DestDir: "{code:GetPlayerDir|foobar64}\components"; Components: foobar64; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{code:GetPlayerDir|foobar64}\components"; Components: foobar64; Flags: ignoreversion

Source: "..\aimp_archive_stream\Release\aimp_archive_stream.dll"; DestDir: "{code:GetPlayerDir|aimp32}\Plugins\aimp_archive_stream"; Components: aimp32; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{code:GetPlayerDir|aimp32}\Plugins\aimp_archive_stream"; Components: aimp32; Flags: ignoreversion
Source: "..\aimp_archive_stream\x64\Release\aimp_archive_stream.dll"; DestDir: "{code:GetPlayerDir|aimp64}\Plugins\aimp_archive_stream"; Components: aimp64; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{code:GetPlayerDir|aimp64}\Plugins\aimp_archive_stream"; Components: aimp64; Flags: ignoreversion

Source: "..\xmp_archive_stream\Release\xmp-archive-stream.dll"; DestDir: "{code:GetPlayerDir|xmplay}"; Components: xmplay; Flags: ignoreversion

Source: "..\musicbee_archive_stream\Release\bass_archive_stream.dll"; DestDir: "{code:GetPlayerDir|musicbee}"; Components: musicbee; Flags: ignoreversion
Source: "..\musicbee_archive_stream\Release\MB_ArchiveStream.dll"; DestDir: "{code:GetPlayerDir|musicbee}\Plugins"; Components: musicbee; Flags: ignoreversion
Source: "..\archive_player_bridge\Release\archive_player_bridge.exe"; DestDir: "{code:GetPlayerDir|musicbee}\Plugins"; Components: musicbee; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{code:GetPlayerDir|musicbee}\Plugins"; Components: musicbee; Flags: ignoreversion

; MediaMonkey: install the add-on itself, not just the MMIP files.
; Check the currently logged-in user profile and MediaMonkey program folder.
; CloseApplications=yes lets Windows Restart Manager handle loaded DLLs.
Source: "..\in_archive_stream\Release\in_archive_stream.dll"; DestDir: "{userappdata}\MediaMonkey5\Plugins"; Components: mediamonkey; Flags: ignoreversion
Source: "..\in_archive_stream\Release\in_archive_stream.dll"; DestDir: "{code:GetPlayerDir|mediamonkey}\Plugins"; Components: mediamonkey; Flags: ignoreversion
Source: "..\mediamonkey_archive_stream\native_addon\info.json"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\mediamonkey_archive_stream\native_addon\actions_add.js"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\mediamonkey_archive_stream\native_addon\launch-mediamonkey.ps1"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\mediamonkey_archive_stream\native_addon\choose-archive.ps1"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\mediamonkey_archive_stream\native_addon\launch-hidden.vbs"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\archive_player_bridge\x64\Release\archive_player_bridge.exe"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\in_archive_stream\Release\in_archive_stream.dll"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\LICENSING.md"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
Source: "..\third_party\README.md"; DestName: "THIRD-PARTY-NOTICES.md"; DestDir: "{userappdata}\MediaMonkey5\Scripts\archiveStreamPlayerNative"; Components: mediamonkey; Flags: ignoreversion
; Retain optional MMIPs for users with portable MediaMonkey configurations.
Source: "..\mediamonkey_archive_stream\Release\ArchiveStreamPlayer-MediaMonkey5.mmip"; DestDir: "{app}\MediaMonkey"; Components: mediamonkey; Flags: ignoreversion
Source: "..\mediamonkey_archive_stream\Release\ArchiveStreamPlayer-MediaMonkey5-Native.mmip"; DestDir: "{app}\MediaMonkey"; Components: mediamonkey; Flags: ignoreversion

; VLC 3 x64 SDK access module is installed INTO VLC itself, so normal
; Open File and archive drag/drop work without a script, shortcut or HTTP.
Source: "..\vlc_archive_stream\x64\Release\libarchive_stream_plugin.dll"; DestDir: "{code:GetPlayerDir|vlc}\plugins\access"; Components: vlc; Flags: ignoreversion


; Upgrade cleanup for retired Microsoft.ZuneMusic DEV preview only.
[InstallDelete]
Type: files; Name: "{app}\MediaPlayer2022\modern_media_drop.exe"
Type: files; Name: "{app}\MediaPlayer2022\archive_player_bridge.exe"
Type: dirifempty; Name: "{app}\MediaPlayer2022"
Type: files; Name: "{group}\Media Player (2022) - Drop Archives.lnk"
Type: files; Name: "{autodesktop}\Archive Stream - Media Player (2022).lnk"

[Icons]
Name: "{group}\Configure Archive Stream Player"; Filename: "{sys}\wscript.exe"; Parameters: "//B //Nologo ""{app}\launch-hidden.vbs"" ""{app}\configure-adapters.ps1"""
Name: "{group}\Project homepage"; Filename: "{#HomeURL}"
Name: "{group}\MediaMonkey add-on packages (portable installs)"; Filename: "{app}\MediaMonkey"; Components: mediamonkey

[Run]
Filename: "{code:GetPlayerDir|vlc}\vlc-cache-gen.exe"; Parameters: """{code:GetPlayerDir|vlc}\plugins"""; StatusMsg: "Registering Archive Stream in VLC"; Flags: runhidden waituntilterminated; Components: vlc; Check: HasVlcCacheGen
Filename: "{#HomeURL}"; Description: "Visit the project website"; Flags: shellexec postinstall skipifsilent unchecked

[Code]
var
  PlayerPages: array[0..9] of TInputDirWizardPage;
  PlayerFound: array[0..9] of String;

function GetBinaryTypeW(const AppName: String; var BinaryType: Cardinal): Boolean;
  external 'GetBinaryTypeW@kernel32.dll stdcall';

function KeyOf(Index: Integer): String;
begin
  case Index of
    0: Result := 'winamp';
    1: Result := 'wacup';
    2: Result := 'foobar32';
    3: Result := 'foobar64';
    4: Result := 'aimp32';
    5: Result := 'aimp64';
    6: Result := 'xmplay';
    7: Result := 'musicbee';
    8: Result := 'mediamonkey';
    9: Result := 'vlc';
  else
    Result := '';
  end;
end;

function AppExe(Index: Integer): String;
begin
  case Index of
    0: Result := 'winamp.exe';
    1: Result := 'wacup.exe';
    2, 3: Result := 'foobar2000.exe';
    4, 5: Result := 'AIMP.exe';
    6: Result := 'xmplay.exe';
    7: Result := 'MusicBee.exe';
    8: Result := 'MediaMonkey.exe';
    9: Result := 'vlc.exe';
  end;
end;

function IsExpectedArchitecture(const FileName: String; Index: Integer): Boolean;
var
  Kind: Cardinal;
  Want64: Boolean;
begin
  Result := False;
  if not GetBinaryTypeW(FileName, Kind) then Exit;
  Want64 := (Index = 3) or (Index = 5) or (Index = 9);
  if Index = 8 then
    Result := Kind = 0
  else if Want64 then
    Result := Kind = 6
  else
    Result := Kind = 0;
end;

function ValidPlayerDir(const Folder: String; Index: Integer): Boolean;
var
  ExePath: String;
  VersionMS, VersionLS: Cardinal;
begin
  Result := False;
  if Folder = '' then Exit;
  ExePath := AddBackslash(Folder) + AppExe(Index);
  Result := FileExists(ExePath) and IsExpectedArchitecture(ExePath, Index);
  if Result and (Index = 9) then
  begin
    if not GetVersionNumbers(ExePath, VersionMS, VersionLS) then
      Result := False
    else
      Result := (VersionMS shr 16) = 3;
  end;
end;

procedure TryPlayerDir(Index: Integer; Candidate: String);
begin
  if (PlayerFound[Index] = '') and ValidPlayerDir(Candidate, Index) then
    PlayerFound[Index] := Candidate;
end;

function HasParameterPrefix(Prefix: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if Pos(Uppercase(Prefix), Uppercase(ParamStr(I))) = 1 then
    begin
      Result := True;
      Exit;
    end;
end;

function GetDirectoryOverride(Index: Integer): String;
var
  I: Integer;
  Prefix, Arg: String;
begin
  Result := '';
  Prefix := '/PLAYERDIR_' + Uppercase(KeyOf(Index)) + '=';
  for I := 1 to ParamCount do
  begin
    Arg := ParamStr(I);
    if Pos(Prefix, Uppercase(Arg)) = 1 then
    begin
      Result := Copy(Arg, Length(Prefix) + 1, Length(Arg));
      Exit;
    end;
  end;
end;

procedure FindPlayers;
var
  I: Integer;
  P32, P64, RegPath, OverrideDir: String;
begin
  P32 := ExpandConstant('{pf32}');
  if IsWin64 then P64 := ExpandConstant('{pf64}') else P64 := P32;
  TryPlayerDir(0, P32 + '\Winamp');
  TryPlayerDir(1, P32 + '\WACUP');
  TryPlayerDir(1, P64 + '\WACUP');
  TryPlayerDir(2, P32 + '\foobar2000');
  TryPlayerDir(2, P64 + '\foobar2000');
  TryPlayerDir(3, P64 + '\foobar2000');
  TryPlayerDir(3, P32 + '\foobar2000');
  TryPlayerDir(4, P32 + '\AIMP');
  TryPlayerDir(4, P64 + '\AIMP');
  TryPlayerDir(5, P64 + '\AIMP');
  TryPlayerDir(5, P32 + '\AIMP');
  TryPlayerDir(6, P32 + '\XMPlay');
  TryPlayerDir(6, P64 + '\XMPlay');
  TryPlayerDir(7, P32 + '\MusicBee');
  TryPlayerDir(7, P64 + '\MusicBee');
  TryPlayerDir(8, P32 + '\MediaMonkey 5');
  TryPlayerDir(8, P64 + '\MediaMonkey 5');
  TryPlayerDir(8, P32 + '\MediaMonkey');
  TryPlayerDir(9, P64 + '\VideoLAN\VLC');
  TryPlayerDir(9, P32 + '\VideoLAN\VLC');
  for I := 0 to 9 do
  begin
    if RegQueryStringValue(HKLM, 'Software\Microsoft\Windows\CurrentVersion\App Paths\' + AppExe(I), '', RegPath) then
      TryPlayerDir(I, ExtractFileDir(RegPath));
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\App Paths\' + AppExe(I), '', RegPath) then
      TryPlayerDir(I, ExtractFileDir(RegPath));
    OverrideDir := GetDirectoryOverride(I);
    if OverrideDir <> '' then
      PlayerFound[I] := OverrideDir;
  end;
end;

procedure InitializeWizard;
var
  I, LastPage: Integer;
begin
  FindPlayers;
  LastPage := wpSelectComponents;
  for I := 0 to 9 do
  begin
    PlayerPages[I] := CreateInputDirPage(LastPage,
      'Locate ' + AppExe(I), 'Select the matching player folder',
      'Select the directory containing ' + AppExe(I) + '.'#13#10 +
      'For portable players, use Browse. This page appears only if that player is selected.',
      False, '');
    PlayerPages[I].Add('Player folder:');
    PlayerPages[I].Values[0] := PlayerFound[I];
    LastPage := PlayerPages[I].ID;
  end;
  if not HasParameterPrefix('/COMPONENTS=') then
  begin
    for I := 0 to 9 do
      WizardForm.ComponentsList.Checked[I] := PlayerFound[I] <> '';
    { Modern Media Player is a separate opt-in preview component, not a native plug-in. }
    WizardForm.ComponentsList.Checked[10] := False;
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 0 to 9 do
    if PageID = PlayerPages[I].ID then
    begin
      Result := not WizardIsComponentSelected(KeyOf(I));
      Exit;
    end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  I: Integer;
begin
  Result := True;
  { In silent mode, skip interactive validation; PrepareToInstall enforces it and returns exit code 7. }
  if WizardSilent then Exit;
  if CurPageID = wpSelectComponents then
  begin
    Result := False;
    for I := 0 to 9 do
      if WizardIsComponentSelected(KeyOf(I)) then Result := True;
    if not Result then
      MsgBox('Select at least one player.', mbError, MB_OK);
    Exit;
  end;
  for I := 0 to 9 do
    if (CurPageID = PlayerPages[I].ID) and WizardIsComponentSelected(KeyOf(I)) then
    begin
      Result := ValidPlayerDir(PlayerPages[I].Values[0], I);
      if not Result then
        MsgBox('Cannot find a compatible ' + AppExe(I) + ' in this folder. Check its 32-bit/64-bit version.', mbError, MB_OK);
      Exit;
    end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  I: Integer;
  AnySelected: Boolean;
begin
  Result := '';
  AnySelected := False;
  for I := 0 to 9 do
    if WizardIsComponentSelected(KeyOf(I)) then AnySelected := True;
  if not AnySelected then
  begin
    Result := 'Select at least one supported player.';
    Exit;
  end;
  for I := 0 to 9 do
    if WizardIsComponentSelected(KeyOf(I)) and not ValidPlayerDir(PlayerPages[I].Values[0], I) then
    begin
      Result := 'Installation stopped: ' + AppExe(I) +
        ' is missing or has the wrong architecture in ' + PlayerPages[I].Values[0];
      Exit;
    end;
end;

function GetPlayerDir(Param: String): String;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to 9 do
    if CompareText(KeyOf(I), Param) = 0 then
    begin
      Result := PlayerPages[I].Values[0];
      Exit;
    end;
end;

function GetPlayerExe(Param: String): String;
begin
  Result := AddBackslash(GetPlayerDir(Param)) + 'vlc.exe';
end;

function HasVlcCacheGen: Boolean;
begin
  Result := FileExists(AddBackslash(GetPlayerDir('vlc')) + 'vlc-cache-gen.exe');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  OriginalZip, DisabledZip, Marker: String;
begin
  if (CurStep <> ssInstall) or not WizardIsComponentSelected('wacup') or
     not WizardIsTaskSelected('wacup_zip_override') then Exit;

  OriginalZip := AddBackslash(GetPlayerDir('wacup')) + 'Plugins\in_zip.dll';
  DisabledZip := OriginalZip + '.archive_stream_disabled';
  Marker := ExpandConstant('{app}\wacup-zip-restore-path.txt');

  // Do not overwrite a previous backup or a manually disabled WACUP plugin.
  if not FileExists(OriginalZip) then Exit;
  if FileExists(DisabledZip) then
    RaiseException('WACUP ZIP handler backup already exists: ' + DisabledZip);

  if not CreateDir(ExpandConstant('{app}')) then
    RaiseException('Cannot create Archive Stream installation directory.');
  if not RenameFile(OriginalZip, DisabledZip) then
    RaiseException('Cannot disable WACUP built-in ZIP plugin. Close WACUP and retry.');
  if not SaveStringToFile(Marker, OriginalZip, False) then
  begin
    RenameFile(DisabledZip, OriginalZip);
    RaiseException('Cannot register the WACUP ZIP handler for uninstall restoration.');
  end;
  Log('WACUP ZIP: original handler backed up to ' + DisabledZip);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Marker, OriginalZip, DisabledZip: String;
  StoredPath: AnsiString;
begin
  if CurUninstallStep <> usPostUninstall then Exit;

  Marker := ExpandConstant('{app}\wacup-zip-restore-path.txt');
  if not LoadStringFromFile(Marker, StoredPath) then Exit;
  OriginalZip := Trim(StoredPath);
  if CompareText(ExtractFileName(OriginalZip), 'in_zip.dll') <> 0 then Exit;

  DisabledZip := OriginalZip + '.archive_stream_disabled';
  if FileExists(OriginalZip) then
  begin
    Log('WACUP ZIP: an active ZIP plugin already exists; leaving backup untouched.');
    Exit;
  end;
  if not FileExists(DisabledZip) then Exit;

  if RenameFile(DisabledZip, OriginalZip) then
  begin
    DeleteFile(Marker);
    Log('WACUP ZIP: original handler restored.');
  end
  else
    Log('WACUP ZIP: restore failed; original handler remains at ' + DisabledZip);
end;
