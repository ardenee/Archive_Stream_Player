; Open in Inno Setup 7 and choose Build > Compile.
#define AppName "Archive Stream Player"
#define AppVersion "0.1.1"
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
OutputBaseFilename=ArchiveStreamPlayer-Setup
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
Name: "mediamonkey"; Description: "MediaMonkey 5/2024 - two companion add-on packages"; Types: custom
Name: "vlc"; Description: "VLC 3 64-bit - native archive launcher"; Types: custom

[Files]
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{app}"; Flags: ignoreversion
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
Source: "..\in_archive_stream\Release\in_archive_stream.dll"; DestDir: "{code:GetPlayerDir|wacup}\Plugins"; Components: wacup; Flags: ignoreversion
Source: "..\gen_archive_stream\Release\gen_archive_stream.dll"; DestDir: "{code:GetPlayerDir|wacup}\Plugins"; Components: wacup; Flags: ignoreversion

Source: "..\foo_archive_stream\Release\foo_archive_stream.dll"; DestDir: "{code:GetPlayerDir|foobar32}\components"; Components: foobar32; Flags: ignoreversion
Source: "..\foo_archive_stream\x64\Release\foo_archive_stream.dll"; DestDir: "{code:GetPlayerDir|foobar64}\components"; Components: foobar64; Flags: ignoreversion

Source: "..\aimp_archive_stream\Release\aimp_archive_stream.dll"; DestDir: "{code:GetPlayerDir|aimp32}\Plugins\aimp_archive_stream"; Components: aimp32; Flags: ignoreversion
Source: "..\aimp_archive_stream\x64\Release\aimp_archive_stream.dll"; DestDir: "{code:GetPlayerDir|aimp64}\Plugins\aimp_archive_stream"; Components: aimp64; Flags: ignoreversion

Source: "..\xmp_archive_stream\Release\xmp-archive-stream.dll"; DestDir: "{code:GetPlayerDir|xmplay}"; Components: xmplay; Flags: ignoreversion

Source: "..\musicbee_archive_stream\Release\bass_archive_stream.dll"; DestDir: "{code:GetPlayerDir|musicbee}"; Components: musicbee; Flags: ignoreversion
Source: "..\musicbee_archive_stream\Release\MB_ArchiveStream.dll"; DestDir: "{code:GetPlayerDir|musicbee}\Plugins"; Components: musicbee; Flags: ignoreversion
Source: "..\archive_player_bridge\Release\archive_player_bridge.exe"; DestDir: "{code:GetPlayerDir|musicbee}\Plugins"; Components: musicbee; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{code:GetPlayerDir|musicbee}\Plugins"; Components: musicbee; Flags: ignoreversion

; MediaMonkey requires its own Add-ons UI to install the packaged MMIPs.
Source: "..\mediamonkey_archive_stream\Release\ArchiveStreamPlayer-MediaMonkey5.mmip"; DestDir: "{app}\MediaMonkey"; Components: mediamonkey; Flags: ignoreversion
Source: "..\mediamonkey_archive_stream\Release\ArchiveStreamPlayer-MediaMonkey5-Native.mmip"; DestDir: "{app}\MediaMonkey"; Components: mediamonkey; Flags: ignoreversion

; VLC uses an external native plugin via the launcher, not a file copied into Program Files\VLC.
Source: "..\vlc_archive_stream\x64\Release\libarchive_stream_plugin.dll"; DestDir: "{app}\VLC\plugins\access"; Components: vlc; Flags: ignoreversion
Source: "..\vlc_archive_stream\x64\Release\vlc_native_playlist.exe"; DestDir: "{app}\VLC"; Components: vlc; Flags: ignoreversion
Source: "..\archive_player_bridge\x64\Release\archive_player_bridge.exe"; DestDir: "{app}\VLC\x64\Release"; Components: vlc; Flags: ignoreversion
Source: "..\archive_player_bridge\launch-archive.ps1"; DestDir: "{app}\VLC"; Components: vlc; Flags: ignoreversion
Source: "..\archive_player_bridge\configure-adapters.ps1"; DestDir: "{app}\VLC"; Components: vlc; Flags: ignoreversion
Source: "Open-VLC-Archive.ps1"; DestDir: "{app}\VLC"; Components: vlc; Flags: ignoreversion

[Icons]
Name: "{group}\Configure Archive Stream Player"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\configure-adapters.ps1"""
Name: "{group}\Project homepage"; Filename: "{#HomeURL}"
Name: "{group}\Open an archive in VLC"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\VLC\Open-VLC-Archive.ps1"" -VlcExe ""{code:GetPlayerExe|vlc}"""; Components: vlc
Name: "{group}\Install MediaMonkey add-ons"; Filename: "{app}\MediaMonkey"; Components: mediamonkey

[Run]
Filename: "{app}\MediaMonkey"; Description: "Open MediaMonkey add-on packages (install both in MediaMonkey Tools > Addons > Add)"; Flags: shellexec postinstall skipifsilent; Components: mediamonkey
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
begin
  Result := False;
  if Folder = '' then Exit;
  ExePath := AddBackslash(Folder) + AppExe(Index);
  Result := FileExists(ExePath) and IsExpectedArchitecture(ExePath, Index);
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
    for I := 0 to 9 do
      WizardForm.ComponentsList.Checked[I] := PlayerFound[I] <> '';
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
