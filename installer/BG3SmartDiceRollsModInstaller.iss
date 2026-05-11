
; BG3SmartDiceRollsModInstaller.iss
; Installer script for Baldur's Gate 3 — Smart Dice Rolls Mod

#define ModName "Baldur's Gate 3 — Smart Dice Rolls Mod"
#define TrayExeName "PixelsTray.exe"
#define ModINIFile "smart-dice-rolls-mod.ini"
#define PixelsINIFile "pixels.ini"
#define ModDLL "smart-dice-rolls.dll"
#define ProxyDLL "xinput1_4.dll"
#define AppId "BG3.SmartDiceRolls.Mod"

[Setup]
AppId={#AppId}
AppName={#ModName}
AppVersion=1.1
VersionInfoVersion=1.1.0.0
VersionInfoTextVersion=1.1.0.0
VersionInfoProductVersion=1.1.0.0
DefaultDirName={autopf}\SmartDiceRolls\{#ModName}
DefaultGroupName={#ModName}
OutputDir=.
OutputBaseFilename=BG3-SmartDiceRolls-Mod_Setup
Compression=lzma
SolidCompression=yes
SetupIconFile=assets\installer.ico
WizardSmallImageFile=assets\d20_small.bmp
WizardImageFile=assets\installer_bg_240x459_blackfill.bmp
UninstallDisplayIcon={app}\uninstaller.ico
UninstallDisplayName={#ModName}
DisableProgramGroupPage=yes

[CustomMessages]
InstallInfoLine1=Install the Smart Dice Rolls Mod for Baldur's Gate 3.
InstallInfoLine2=You can select Steam, GOG, Epic Games, or a custom installation path. Leave unchecked any
InstallInfoLine3=version you don't want to mod.
InstallInfoLine4=To continue, click Next. If you would like to select a different directory, click Browse.

[Files]
Source: "files\{#ProxyDLL}"; DestDir: "{code:GetInstallPath}"; Flags: ignoreversion uninsrestartdelete
Source: "files\{#ModDLL}"; DestDir: "{code:GetInstallPath}\mods"; Flags: ignoreversion uninsrestartdelete
Source: "files\{#ModINIFile}"; DestDir: "{code:GetInstallPath}\mods"; Flags: ignoreversion uninsrestartdelete
Source: "files\{#TrayExeName}"; DestDir: "{code:GetInstallPath}\mods\PixelsDiceTray"; Flags: ignoreversion uninsrestartdelete
Source: "files\{#PixelsINIFile}"; DestDir: "{code:GetInstallPath}\mods\PixelsDiceTray"; Flags: ignoreversion uninsrestartdelete
Source: "assets\uninstaller.ico"; DestDir: "{app}"; DestName: "uninstaller.ico"; Flags: ignoreversion
Source: "assets\smart_dice_rolls_clean_transparent.png"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Uninstall {#ModName}"; Filename: "{uninstallexe}"

[Registry]
Root: HKCU; Subkey: "Software\SmartDiceRolls\{#AppId}"; ValueType: string; ValueName: "GameInstallPath"; ValueData: "{code:GetInstallPath}"; Flags: uninsdeletekey

[UninstallDelete]
Type: dirifempty; Name: "{app}"

[Code]
var
  DisclaimerCheckBox: TNewCheckBox;
  DisclaimerAccepted: Boolean;
  DisclaimerPage: TWizardPage;
  SteamCheckbox, GOGCheckbox, EpicCheckbox, ManualCheckbox: TCheckBox;
  ManualPathEdit: TEdit;
  ManualBrowseButton: TButton;
  GOGInstallPath: string;
  EpicInstallPath: string;
  SelectedInstallPath: string;
  MyPage: TWizardPage;
  FinishExtrasCreated: Boolean;
  SteamInstallPath: string;


procedure SafeSetParent(Control: TControl; ParentCtrl: TWinControl);
begin
  Control.Parent := ParentCtrl;
end;

function IsWindows11OrNewer: Boolean;
var
  s: string;
  build: Integer;
begin
  s := '';
  if not RegQueryStringValue(HKLM,
    'SOFTWARE\Microsoft\Windows NT\CurrentVersion', 'CurrentBuildNumber', s) then
  begin
    RegQueryStringValue(HKLM,
      'SOFTWARE\Microsoft\Windows NT\CurrentVersion', 'CurrentBuild', s);
  end;
  build := StrToIntDef(s, 0);
  Result := (build >= 22000);
end;

procedure CurPageChangedCheck(Sender: TObject);
begin
  DisclaimerAccepted := TNewCheckBox(Sender).Checked;
  if not WizardSilent and Assigned(WizardForm.NextButton) then
    WizardForm.NextButton.Enabled := DisclaimerAccepted;
end;

procedure CreateDisclaimerPage();
var
  Memo: TMemo;
begin
  DisclaimerAccepted := False;
  DisclaimerPage := CreateCustomPage(
    wpWelcome,
    'Disclaimer',
    'Please read and accept the following disclaimer before continuing.'
  );

  Memo := TMemo.Create(WizardForm);
  Memo.Parent := DisclaimerPage.Surface;
  Memo.Left := ScaleX(0);
  Memo.Top := ScaleY(0);
  Memo.Width := DisclaimerPage.Surface.Width;
  Memo.Height := ScaleY(150);
  Memo.ReadOnly := True;
  Memo.ScrollBars := ssVertical;
  Memo.WordWrap := True;
  Memo.Text :=
'This mod is provided "as is" with no warranty or guarantee of performance.' + #13#10 +
'By continuing, you acknowledge that you are installing third-party software' + #13#10 +
'which may modify or interact with the game in ways not intended by its' + #13#10 +
'original developers.' + #13#10 +
'' + #13#10 +
'Use at your own risk. The authors and platforms are not responsible for any' + #13#10 +
'damage, data loss, or other issues caused by this software.' + #13#10 +
'' + #13#10 +
'This is a non-commercial fan-made project. All rights to the game' + #13#10 +
'"Baldur''s Gate 3" and its characters belong to Larian Studios.' + #13#10 +
'' + #13#10 +
'Created by Thanos Petsas - https://thanasispetsas.com';

  DisclaimerCheckBox := TNewCheckBox.Create(WizardForm);
  if Assigned(DisclaimerCheckBox) then
  begin
    DisclaimerCheckBox.Parent := DisclaimerPage.Surface;
    DisclaimerCheckBox.Top := Memo.Top + Memo.Height + ScaleY(8);
    DisclaimerCheckBox.Left := ScaleX(0);
    DisclaimerCheckBox.Width := DisclaimerPage.Surface.Width;
    DisclaimerCheckBox.Height := ScaleY(20);
    DisclaimerCheckBox.Caption := 'I have read and accept the disclaimer above.';
    DisclaimerCheckBox.OnClick := @CurPageChangedCheck;
  end;

  if not WizardSilent and Assigned(WizardForm.NextButton) then
    WizardForm.NextButton.Enabled := False;
end;


function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if Assigned(DisclaimerPage) and (PageID = DisclaimerPage.ID) then
    Result := DisclaimerAccepted;
end;

procedure OnVisitWebsiteClick(Sender: TObject);
var
  ErrCode: Integer;
begin
  ShellExec('open', 'https://thanasispetsas.com/', '', '', SW_SHOW, ewNoWait, ErrCode);
end;

procedure CurPageChanged(CurPageID: Integer);
var
  ThankYouLabel, WebsiteLabel: TNewStaticText;
  FS: TFontStyles;
begin
  if not WizardSilent and Assigned(WizardForm.NextButton) and Assigned(DisclaimerPage) and
     (CurPageID = DisclaimerPage.ID) then
    WizardForm.NextButton.Enabled := DisclaimerAccepted;

  if (CurPageID = wpFinished) and (not FinishExtrasCreated) then
  begin
    ThankYouLabel := TNewStaticText.Create(WizardForm);
    ThankYouLabel.Parent := WizardForm.FinishedPage;
    ThankYouLabel.Caption := #13#10 +
      'Thank you for installing the Smart Dice Rolls Mod for Baldur''s Gate 3!' + #13#10 +
      'For news and updates, please visit:';
    ThankYouLabel.Top := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(16);
    ThankYouLabel.Left := WizardForm.FinishedLabel.Left;
    ThankYouLabel.AutoSize := True;

    WebsiteLabel := TNewStaticText.Create(WizardForm);
    WebsiteLabel.Parent := WizardForm.FinishedPage;
    WebsiteLabel.Caption := 'https://thanasispetsas.com/';
    WebsiteLabel.Font.Color := clBlue;
    FS := WebsiteLabel.Font.Style;
    Include(FS, fsUnderline);
    WebsiteLabel.Font.Style := FS;
    WebsiteLabel.Cursor := crHand;
    WebsiteLabel.OnClick := @OnVisitWebsiteClick;
    WebsiteLabel.Top := ThankYouLabel.Top + ThankYouLabel.Height + ScaleY(8);
    WebsiteLabel.Left := ThankYouLabel.Left;
    WebsiteLabel.AutoSize := True;

    FinishExtrasCreated := True;
  end;
end;

procedure BrowseManualPath(Sender: TObject);
var Dir: string;
begin
  Dir := ManualPathEdit.Text;
  if BrowseForFolder('Select game''s bin folder...', Dir, false) then
    ManualPathEdit.Text := Dir;
end;

procedure ManualCheckboxClick(Sender: TObject);
var
  Enabled: Boolean;
begin
  Enabled := ManualCheckbox.Checked;
  ManualPathEdit.Enabled := Enabled;
  ManualBrowseButton.Enabled := Enabled;
end;

function GetInstallPath(Default: string): string;
begin
if SelectedInstallPath <> '' then
    Result := SelectedInstallPath
else if Assigned(ManualPathEdit) and (ManualPathEdit.Text <> '') then
    Result := ManualPathEdit.Text
else
    Result := ExpandConstant('{autopf}\SmartDiceRolls\Baldur''s Gate 3 — Smart Dice Rolls Mod');
end;


function NormalizeLibraryPath(P: string): string;
var
  i: Integer;
begin
  if (Length(P) >= 2) and (P[1] = '"') and (P[Length(P)] = '"') then
    P := Copy(P, 2, Length(P) - 2);
  if (Length(P) > 0) and (P[Length(P)] = '"') then
    Delete(P, Length(P), 1);

  i := 1;
  while i <= Length(P) do
  begin
    if (P[i] = '\') and (i < Length(P)) and (P[i + 1] = '\') then
      Delete(P, i, 1)
    else
      Inc(i);
  end;

  for i := 1 to Length(P) do
    if P[i] = '/' then P[i] := '\';

  Result := Trim(P);
end;

function FindNextQuote(const S: string; StartAt: Integer): Integer;
var
  i, L: Integer;
begin
  L := Length(S);
  if (L = 0) or (StartAt < 1) or (StartAt > L) then
  begin
    Result := 0;
    Exit;
  end;

  for i := StartAt to L do
    if S[i] = '"' then
    begin
      Result := i;
      Exit;
    end;

  Result := 0;
end;

function ExtractJsonValue(Line: string): string;
var
  i: Integer;
begin
  Result := '';
  i := Pos(':', Line);
  if i > 0 then
  begin
    Result := Trim(Copy(Line, i + 1, MaxInt));
    if (Length(Result) > 0) and (Result[1] = '"') then
    begin
      Delete(Result, 1, 1);
      i := Pos('"', Result);
      if i > 0 then
        Result := Copy(Result, 1, i - 1);
    end;
    if (Length(Result) > 0) and (Result[Length(Result)] = ',') then
      Delete(Result, Length(Result), 1);
  end;
end;


function ExtractVdfPathValue(const Line: string): string;
var
  p, q1, q2: Integer;
  val: string;
begin
  Result := '';
  p := Pos('"path"', LowerCase(Line));
  if p = 0 then Exit;

  q1 := FindNextQuote(Line, p + 6);
  if q1 = 0 then Exit;
  q2 := FindNextQuote(Line, q1 + 1);
  if q2 = 0 then Exit;

  val := Copy(Line, q1 + 1, q2 - q1 - 1);
  Result := NormalizeLibraryPath(val);
end;


{ ── Steam Detection ── }

function CheckSteamRoot(const steamappsRoot: string; var OutDir: string): Boolean;
var
  commonDir, gameDir: string;
begin
  Result := False;

  commonDir := AddBackslash(steamappsRoot) + 'common';

  gameDir := AddBackslash(commonDir) + 'Baldurs Gate 3';
  if DirExists(gameDir) then
  begin
    OutDir := AddBackslash(gameDir) + 'bin';
    Result := True;
    Exit;
  end;

  if FileExists(AddBackslash(steamappsRoot) + 'appmanifest_1086940.acf') then
  begin
    gameDir := AddBackslash(commonDir) + 'Baldurs Gate 3';
    OutDir := AddBackslash(gameDir) + 'bin';
    Result := True;
    Exit;
  end;
end;

function ProbeSteamRoot(const steamappsRoot: string; var OutDir: string): Boolean;
begin
  Log('Steam: probing steamapps root ' + steamappsRoot);
  Result := DirExists(steamappsRoot) and CheckSteamRoot(steamappsRoot, OutDir);
end;

function TryFindBG3ByHeuristic(var OutDir: string): Boolean;
var
  d: Integer;
  root: string;
begin
  Result := False;
  OutDir := '';

  root := ExpandConstant('{pf32}') + '\Steam\steamapps';
  if ProbeSteamRoot(root, OutDir) then begin Result := True; Exit; end;

  root := ExpandConstant('{pf}') + '\Steam\steamapps';
  if ProbeSteamRoot(root, OutDir) then begin Result := True; Exit; end;

  for d := Ord('C') to Ord('Z') do
  begin
    root := Chr(d) + ':\Steam\steamapps';
    if ProbeSteamRoot(root, OutDir) then begin Result := True; Exit; end;

    root := Chr(d) + ':\SteamLibrary\steamapps';
    if ProbeSteamRoot(root, OutDir) then begin Result := True; Exit; end;

    root := Chr(d) + ':\Games\Steam\steamapps';
    if ProbeSteamRoot(root, OutDir) then begin Result := True; Exit; end;
  end;
end;

function FileExistsInSteam(): Boolean;
var
  SteamPath, VdfPath1, VdfPath2: string;
  Lines: TArrayOfString;
  i, j, n: Integer;
  LibRoots: array of string;
  Root, GameDir: string;
  ExistsAlready: Boolean;
begin
  Result := False;
  SteamInstallPath := '';

  try
    if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamPath) then
    begin
      SetArrayLength(LibRoots, 1);
      LibRoots[0] := SteamPath;

      VdfPath1 := AddBackslash(SteamPath) + 'steamapps\libraryfolders.vdf';
      VdfPath2 := AddBackslash(SteamPath) + 'config\libraryfolders.vdf';

      if LoadStringsFromFile(VdfPath1, Lines) and (GetArrayLength(Lines) > 0) then
        for i := 0 to GetArrayLength(Lines) - 1 do
          try
              if Pos('"path"', Lines[i]) > 0 then
              begin
                Root := ExtractVdfPathValue(Lines[i]);
                if Root <> '' then
                begin
                  ExistsAlready := False;
                  n := GetArrayLength(LibRoots);
                  for j := 0 to n - 1 do
                    if CompareText(LibRoots[j], Root) = 0 then begin ExistsAlready := True; Break; end;
                  if not ExistsAlready then
                  begin
                    SetArrayLength(LibRoots, n + 1);
                    LibRoots[n] := Root;
                    Log('Steam: library from VDF = ' + Root);
                  end;
                end;
                end;
            except
                Log('Steam exception while parsing line: ' + lines[i]);
          end;

      if LoadStringsFromFile(VdfPath2, Lines) and (GetArrayLength(Lines) > 0) then
        for i := 0 to GetArrayLength(Lines) - 1 do
          try
              if Pos('"path"', Lines[i]) > 0 then
              begin
                Root := ExtractVdfPathValue(Lines[i]);
                if Root <> '' then
                begin
                  ExistsAlready := False;
                  n := GetArrayLength(LibRoots);
                  for j := 0 to n - 1 do
                    if CompareText(LibRoots[j], Root) = 0 then begin ExistsAlready := True; Break; end;
                  if not ExistsAlready then
                  begin
                    SetArrayLength(LibRoots, n + 1);
                    LibRoots[n] := Root;
                    Log('Steam: library from VDF = ' + Root);
                  end;
                end;
                end;
            except
                Log('Steam exception while parsing line: ' + lines[i]);
          end;

      for i := 0 to GetArrayLength(LibRoots) - 1 do
      begin
        Log('Steam: probing library "' + LibRoots[i] + '"');

        GameDir := AddBackslash(LibRoots[i]) + 'steamapps\common\Baldurs Gate 3';
        if DirExists(GameDir) then
        begin
          SteamInstallPath := AddBackslash(GameDir) + 'bin';
          Result := True;
          Log('Steam: found BG3 at ' + SteamInstallPath);
          Exit;
        end;

        if FileExists(AddBackslash(LibRoots[i]) + 'steamapps\appmanifest_1086940.acf') then
        begin
          GameDir := AddBackslash(LibRoots[i]) + 'steamapps\common\Baldurs Gate 3';
          SteamInstallPath := AddBackslash(GameDir) + 'bin';
          Result := True;
          Log('Steam: found BG3 via appmanifest at ' + SteamInstallPath);
          Exit;
        end;
      end;
    end
    else
      Log('Steam: SteamPath not found in registry.');
  except
    Log('Steam: EXCEPTION while parsing VDF; falling back to drive scan.');
  end;

  if not Result then
  begin
    Log('Steam: starting drive-scan fallback.');
    if TryFindBG3ByHeuristic(SteamInstallPath) then
    begin
      Result := True;
      Log('Steam: heuristic found BG3 at ' + SteamInstallPath);
    end
    else
    begin
      Log('Steam: heuristic did not find BG3.');
    end;
  end;
end;


{ ── GOG Detection ── }

function FileExistsInGOG(): Boolean;
var
  GOGPath: string;
  BinDir: string;
begin
  Result := False;
  GOGInstallPath := '';

  try
    if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\GOG.com\Games\1456460669', 'path', GOGPath) then
    begin
      BinDir := AddBackslash(GOGPath) + 'bin';
      if DirExists(BinDir) then
      begin
        GOGInstallPath := BinDir;
        Result := True;
        Log('GOG: found BG3 at ' + GOGInstallPath);
        Exit;
      end;
    end;

    if RegQueryStringValue(HKLM, 'SOFTWARE\GOG.com\Games\1456460669', 'path', GOGPath) then
    begin
      BinDir := AddBackslash(GOGPath) + 'bin';
      if DirExists(BinDir) then
      begin
        GOGInstallPath := BinDir;
        Result := True;
        Log('GOG: found BG3 at ' + GOGInstallPath);
        Exit;
      end;
    end;
  except
    Log('GOG: EXCEPTION during detection; treating as not installed.');
  end;
end;


{ ── Epic Games Detection ── }

function FileExistsInEpic(): Boolean;
var
  FindRec: TFindRec;
  ManifestDir, FilePath, InstallLoc: string;
  Content: AnsiString;
  startedFind: Boolean;
begin
  Result := False;
  EpicInstallPath := '';
  startedFind := False;

  try
    ManifestDir := ExpandConstant('{commonappdata}') +
                   '\Epic\EpicGamesLauncher\Data\Manifests';
    Log('Checking if the game is installed via Epic');

    if not DirExists(ManifestDir) then
      Exit;

    if FindFirst(ManifestDir + '\*.item', FindRec) then
    begin
      startedFind := True;
      repeat
        FilePath := ManifestDir + '\' + FindRec.Name;
        try
          if LoadStringFromFile(FilePath, Content) then
          begin
            if (Pos('"DisplayName"', Content) > 0) and
               (Pos('Baldur', Content) > 0) and
               (Pos('Gate 3', Content) > 0) and
               (Pos('"InstallLocation":', Content) > 0) then
            begin
              InstallLoc := Copy(Content, Pos('"InstallLocation":', Content) + 19, MaxInt);
              if Pos('"', InstallLoc) > 0 then
              begin
                InstallLoc := Copy(InstallLoc, Pos('"', InstallLoc) + 1, MaxInt);
                if Pos('"', InstallLoc) > 0 then
                  InstallLoc := Copy(InstallLoc, 1, Pos('"', InstallLoc) - 1);
              end;
              EpicInstallPath := AddBackslash(Trim(InstallLoc)) + 'bin';
              Result := EpicInstallPath <> 'bin';
              if Result then
              begin
                Log('Found Epic path: ' + EpicInstallPath);
                Exit;
              end;
            end;
          end;
        except
          Log('Warning: failed to read/parse "' + FilePath + '"; skipping.');
        end;
      until not FindNext(FindRec);
    end;
  except
    Log('Warning: exception in FileExistsInEpic(); treating as not installed.');
    Result := False;
  end;

  if startedFind then
  begin
    try
      FindClose(FindRec);
    except
      { ignore }
    end;
  end;
end;


{ ── Next button / page navigation ── }

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;

  if CurPageID = MyPage.ID then
  begin
    if not DisclaimerAccepted then
    begin
      MsgBox('You must accept the disclaimer to continue.', mbError, MB_OK);
      Result := False;
    end;

    if (SteamCheckbox <> nil) and SteamCheckbox.Checked then
    begin
      if FileExistsInSteam() and (SteamInstallPath <> '') then
        SelectedInstallPath := SteamInstallPath
      else
        SelectedInstallPath := '';
      Log('Using Steam path: ' + SelectedInstallPath);
    end;

    if (GOGCheckbox <> nil) and GOGCheckbox.Checked then
    begin
      SelectedInstallPath := GOGInstallPath;
      Log('Using GOG path: ' + SelectedInstallPath);
    end;

    if (EpicCheckbox <> nil) and EpicCheckbox.Checked then
    begin
      SelectedInstallPath := EpicInstallPath;
      Log('Using Epic path: ' + SelectedInstallPath);
    end;

    if ManualCheckbox.Checked then
    begin
      SelectedInstallPath := ManualPathEdit.Text;
      Log('Using manual path: ' + SelectedInstallPath);
    end;

    if not DirExists(SelectedInstallPath) then
    begin
      if not CreateDir(SelectedInstallPath) then
      begin
        MsgBox('Failed to create folder: ' + SelectedInstallPath, mbError, MB_OK);
        Result := False;
        exit;
       end;
    end;

    Log('SelectedInstallPath: ' + SelectedInstallPath);

  end;
end;


{ ── Log file cleanup ── }

var
  DeleteLogsCheckbox: TNewCheckBox;
  LogPaths: TStringList;

procedure CheckAndAddPath(BasePath: string);
var
  ModsDir: string;
begin
  ModsDir := BasePath + '\mods';
  if FileExists(ModsDir + '\smart-dice-rolls.log') then
    LogPaths.Add(ModsDir);
  if FileExists(ModsDir + '\PixelsDiceTray\pixels_log.txt') then
    LogPaths.Add(ModsDir + '\PixelsDiceTray');
end;

procedure DetectLogFiles();
begin
  if (SteamInstallPath <> '') then
    CheckAndAddPath(SteamInstallPath);

  if GOGInstallPath <> '' then
    CheckAndAddPath(GOGInstallPath);

  if EpicInstallPath <> '' then
    CheckAndAddPath(EpicInstallPath);

  CheckAndAddPath(ExpandConstant('{app}'));
end;

procedure InitializeUninstallProgressForm();
begin
  LogPaths := TStringList.Create;
  DetectLogFiles();
  if LogPaths.Count > 0 then
  begin
    if MsgBox(
         'Log files from Smart Dice Rolls Mod were found.' + #13#10#13#10 +
         'Do you want to delete these log files?',
         mbConfirmation, MB_YESNO) = IDYES
    then
    begin
      DeleteLogsCheckbox := TNewCheckBox.Create(nil);
      DeleteLogsCheckbox.Checked := True;
    end;
  end;
end;


{ ── Install / Uninstall steps ── }

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    if (SteamCheckbox <> nil) and SteamCheckbox.Checked then
    begin
      if FileExistsInSteam() and (SteamInstallPath <> '') then
        SelectedInstallPath := SteamInstallPath;
      Log('CurStepChanged: Using Steam path: ' + SelectedInstallPath);
    end
    else if (GOGCheckbox <> nil) and GOGCheckbox.Checked then
    begin
      SelectedInstallPath := GOGInstallPath;
      Log('CurStepChanged: Using GOG path: ' + SelectedInstallPath);
    end
    else if (EpicCheckbox <> nil) and EpicCheckbox.Checked then
    begin
      SelectedInstallPath := EpicInstallPath;
      Log('CurStepChanged: Using Epic path: ' + SelectedInstallPath);
    end
    else if (ManualCheckbox <> nil) and ManualCheckbox.Checked then
    begin
      SelectedInstallPath := ManualPathEdit.Text;
      Log('CurStepChanged: Using manual path: ' + SelectedInstallPath);
    end;

    Log('CurStepChanged: Final SelectedInstallPath = ' + SelectedInstallPath);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  i: Integer;
  GamePath: string;
begin
  if CurUninstallStep = usUninstall then
  begin
    if RegQueryStringValue(HKCU, 'Software\SmartDiceRolls\{#AppId}', 'GameInstallPath', GamePath) then
    begin
      Log('Uninstall: Found game path in registry: ' + GamePath);

      if FileExists(GamePath + '\{#ProxyDLL}') then
      begin
        DeleteFile(GamePath + '\{#ProxyDLL}');
        Log('Uninstall: Deleted ' + GamePath + '\{#ProxyDLL}');
      end;

      if FileExists(GamePath + '\mods\{#ModDLL}') then
      begin
        DeleteFile(GamePath + '\mods\{#ModDLL}');
        Log('Uninstall: Deleted ' + GamePath + '\mods\{#ModDLL}');
      end;

      if FileExists(GamePath + '\mods\{#ModINIFile}') then
      begin
        DeleteFile(GamePath + '\mods\{#ModINIFile}');
        Log('Uninstall: Deleted ' + GamePath + '\mods\{#ModINIFile}');
      end;

      if FileExists(GamePath + '\mods\PixelsDiceTray\{#TrayExeName}') then
      begin
        DeleteFile(GamePath + '\mods\PixelsDiceTray\{#TrayExeName}');
        Log('Uninstall: Deleted ' + GamePath + '\mods\PixelsDiceTray\{#TrayExeName}');
      end;

      if FileExists(GamePath + '\mods\PixelsDiceTray\{#PixelsINIFile}') then
      begin
        DeleteFile(GamePath + '\mods\PixelsDiceTray\{#PixelsINIFile}');
        Log('Uninstall: Deleted ' + GamePath + '\mods\PixelsDiceTray\{#PixelsINIFile}');
      end;

      if FileExists(GamePath + '\mods\PixelsDiceTray\pixels.cfg') then
      begin
        DeleteFile(GamePath + '\mods\PixelsDiceTray\pixels.cfg');
        Log('Uninstall: Deleted ' + GamePath + '\mods\PixelsDiceTray\pixels.cfg');
      end;

      if FileExists(GamePath + '\mods\smart-dice-rolls.log') then
      begin
        DeleteFile(GamePath + '\mods\smart-dice-rolls.log');
        Log('Uninstall: Deleted ' + GamePath + '\mods\smart-dice-rolls.log');
      end;

      DelTree(GamePath + '\mods\PixelsDiceTray', True, True, False);
      RemoveDir(GamePath + '\mods\PixelsDiceTray');
      DelTree(GamePath + '\mods', True, True, False);
      RemoveDir(GamePath + '\mods');
      Log('Uninstall: Cleaned up mod directories');
    end
    else
      Log('Uninstall: Could not find game path in registry');
  end;

  if (CurUninstallStep = usPostUninstall) and
     (LogPaths <> nil) and
     (DeleteLogsCheckbox <> nil) and
     DeleteLogsCheckbox.Checked then
  begin
    for i := 0 to LogPaths.Count - 1 do
    begin
      Log('Deleting: ' + LogPaths[i]);
      DelTree(LogPaths[i], True, True, True);
    end;
  end;
end;


{ ── Wizard initialization ── }

procedure InitializeWizard;
var
  InfoLabel1, InfoLabel2, InfoLabel3, InfoLabel4: TLabel;
  IsSteamInstalled, IsGOGInstalled, IsEpicInstalled: Boolean;
  CurrentTop: Integer;
begin
  Log('IW: start');
  CreateDisclaimerPage();
  Log('IW: disclaimer page created');

  MyPage := CreateCustomPage(
    wpSelectDir,
    'Choose Game Version',
    'Select which game installation to install the mod for.'
  );
  Log('IW: custom page created');

  try
    IsSteamInstalled := FileExistsInSteam();
  except
    Log('IW: EXCEPTION in FileExistsInSteam; treating as not installed.');
    IsSteamInstalled := False;
  end;
  Log('IW: steam detection done');

  try
    IsGOGInstalled := FileExistsInGOG();
  except
    Log('IW: GOG detect raised, treating as not installed');
    IsGOGInstalled := False;
  end;
  Log('IW: GOG detection done');

  try
    IsEpicInstalled := FileExistsInEpic();
  except
    Log('IW: Epic detect raised, treating as not installed');
    IsEpicInstalled := False;
  end;
  Log('IW: Epic detection done');

  InfoLabel1 := TLabel.Create(WizardForm);
  InfoLabel1.Parent := MyPage.Surface;
  InfoLabel1.Top := ScaleY(0);
  InfoLabel1.Left := ScaleX(0);
  InfoLabel1.Font.Style := [fsBold];
  InfoLabel1.Caption := CustomMessage('InstallInfoLine1');

  InfoLabel2 := TLabel.Create(WizardForm);
  InfoLabel2.Parent := MyPage.Surface;
  InfoLabel2.Top := InfoLabel1.Top + ScaleY(20);
  InfoLabel2.Left := ScaleX(0);
  InfoLabel2.Caption := CustomMessage('InstallInfoLine2');

  InfoLabel3 := TLabel.Create(WizardForm);
  InfoLabel3.Parent := MyPage.Surface;
  InfoLabel3.Top := InfoLabel2.Top + ScaleY(20);
  InfoLabel3.Left := ScaleX(0);
  InfoLabel3.Caption := CustomMessage('InstallInfoLine3');

  InfoLabel4 := TLabel.Create(WizardForm);
  InfoLabel4.Parent := MyPage.Surface;
  InfoLabel4.Top := InfoLabel3.Top + ScaleY(30);
  InfoLabel4.Left := ScaleX(0);
  InfoLabel4.Caption := CustomMessage('InstallInfoLine4');

  CurrentTop := InfoLabel4.Top + ScaleY(24);

  { Manual path (always available) }
  try
    Log('IW: creating Manual checkbox');
    ManualCheckbox := TCheckBox.Create(WizardForm);
    ManualCheckbox.Parent := MyPage.Surface;
    ManualCheckbox.Top := CurrentTop;
    ManualCheckbox.Left := ScaleX(0);
    ManualCheckbox.Width := ScaleX(300);
    ManualCheckbox.Height := ScaleY(20);
    ManualCheckbox.Caption := 'Install to custom path (bin folder):';
    ManualCheckbox.OnClick := @ManualCheckboxClick;
    ManualCheckbox.Checked := False;
    CurrentTop := CurrentTop + ScaleY(24);

    Log('IW: creating Manual path edit + Browse');
    ManualPathEdit := TEdit.Create(WizardForm);
    ManualPathEdit.Parent := MyPage.Surface;
    ManualPathEdit.Top := CurrentTop;
    ManualPathEdit.Left := ScaleX(0);
    ManualPathEdit.Width := ScaleX(300);
    ManualPathEdit.Height := ScaleY(25);
    ManualPathEdit.Text := 'C:\Games\Baldurs Gate 3\bin';

    ManualBrowseButton := TButton.Create(WizardForm);
    ManualBrowseButton.Parent := MyPage.Surface;
    ManualBrowseButton.Top := CurrentTop;
    ManualBrowseButton.Left := ManualPathEdit.Left + ManualPathEdit.Width + ScaleX(8);
    ManualBrowseButton.Width := ScaleX(75);
    ManualBrowseButton.Height := ScaleY(25);
    ManualBrowseButton.Caption := 'Browse...';
    ManualBrowseButton.OnClick := @BrowseManualPath;

    Log('IW: manual controls created');
  except
    Log('IW: ERROR creating manual controls; minimalizing.');
    if ManualCheckbox = nil then
    begin
      ManualCheckbox := TCheckBox.Create(WizardForm);
      ManualCheckbox.Parent := MyPage.Surface;
      ManualCheckbox.Top := CurrentTop;
      ManualCheckbox.Left := ScaleX(0);
      ManualCheckbox.Caption := 'Install to custom path:';
      ManualCheckbox.Checked := True;
    end;
  end;

  { Steam checkbox }
  if IsSteamInstalled then
  begin
    try
      if Assigned(ManualBrowseButton) then
        CurrentTop := ManualBrowseButton.Top + ManualBrowseButton.Height + ScaleY(8)
      else if Assigned(ManualCheckbox) then
        CurrentTop := ManualCheckbox.Top + ScaleY(28)
      else
        CurrentTop := InfoLabel4.Top + ScaleY(24);

      Log('IW: creating Steam checkbox (path=' + SteamInstallPath + ')');
      SteamCheckbox := TCheckBox.Create(WizardForm);
      SteamCheckbox.Parent := MyPage.Surface;
      SteamCheckbox.Top := CurrentTop;
      SteamCheckbox.Left := ScaleX(0);
      SteamCheckbox.Width := ScaleX(300);
      SteamCheckbox.Height := ScaleY(20);
      SteamCheckbox.Caption := 'Install for Steam';
      SteamCheckbox.Checked := True;

      if Assigned(ManualCheckbox) then
        ManualCheckbox.Checked := False;

      CurrentTop := SteamCheckbox.Top + SteamCheckbox.Height + ScaleY(8);
      Log('IW: steam checkbox created');
    except
      Log('IW: WARNING Steam checkbox creation failed; continuing without Steam option.');
    end;
  end;

  { GOG checkbox }
  if IsGOGInstalled and (GOGInstallPath <> '') then
  begin
    try
      if Assigned(SteamCheckbox) then
        CurrentTop := SteamCheckbox.Top + SteamCheckbox.Height + ScaleY(8)
      else if Assigned(ManualBrowseButton) then
        CurrentTop := ManualBrowseButton.Top + ManualBrowseButton.Height + ScaleY(8)
      else if Assigned(ManualCheckbox) then
        CurrentTop := ManualCheckbox.Top + ScaleY(28)
      else
        CurrentTop := InfoLabel4.Top + ScaleY(24);

      Log('IW: creating GOG checkbox (path=' + GOGInstallPath + ')');
      GOGCheckbox := TCheckBox.Create(WizardForm);
      GOGCheckbox.Parent := MyPage.Surface;
      GOGCheckbox.Top := CurrentTop;
      GOGCheckbox.Left := ScaleX(0);
      GOGCheckbox.Width := ScaleX(300);
      GOGCheckbox.Height := ScaleY(20);
      GOGCheckbox.Caption := 'Install for GOG';
      GOGCheckbox.Checked := not IsSteamInstalled;

      if GOGCheckbox.Checked and Assigned(ManualCheckbox) then
        ManualCheckbox.Checked := False;

      CurrentTop := GOGCheckbox.Top + GOGCheckbox.Height + ScaleY(8);
      Log('IW: GOG checkbox created');
    except
      Log('IW: WARNING GOG checkbox creation failed; continuing without GOG option.');
    end;
  end;

  { Epic checkbox }
  if IsEpicInstalled and (EpicInstallPath <> '') then
  begin
    try
      if Assigned(GOGCheckbox) then
        CurrentTop := GOGCheckbox.Top + GOGCheckbox.Height + ScaleY(8)
      else if Assigned(SteamCheckbox) then
        CurrentTop := SteamCheckbox.Top + SteamCheckbox.Height + ScaleY(8)
      else if Assigned(ManualBrowseButton) then
        CurrentTop := ManualBrowseButton.Top + ManualBrowseButton.Height + ScaleY(8)
      else if Assigned(ManualCheckbox) then
        CurrentTop := ManualCheckbox.Top + ScaleY(28)
      else
        CurrentTop := InfoLabel4.Top + ScaleY(24);

      Log('IW: creating Epic checkbox (path=' + EpicInstallPath + ')');
      EpicCheckbox := TCheckBox.Create(WizardForm);
      EpicCheckbox.Parent := MyPage.Surface;
      EpicCheckbox.Top := CurrentTop;
      EpicCheckbox.Left := ScaleX(0);
      EpicCheckbox.Width := ScaleX(300);
      EpicCheckbox.Height := ScaleY(20);
      EpicCheckbox.Caption := 'Install for Epic Games';
      EpicCheckbox.Checked := not IsSteamInstalled and not IsGOGInstalled;

      if EpicCheckbox.Checked and Assigned(ManualCheckbox) then
        ManualCheckbox.Checked := False;

      CurrentTop := EpicCheckbox.Top + EpicCheckbox.Height + ScaleY(8);
      Log('IW: Epic checkbox created');
    except
      Log('IW: WARNING Epic checkbox creation failed; continuing without Epic option.');
    end;
  end;

  if not IsSteamInstalled and not IsGOGInstalled and not IsEpicInstalled then
  begin
    Log('IW: no game installations detected; leaving Manual default');
    if Assigned(ManualCheckbox) then
      ManualCheckbox.Checked := True;
  end;

  ManualCheckboxClick(nil);
  Log('IW: checkboxes wired');
end;


