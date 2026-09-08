; BlitzForge installer: a standard Windows setup wizard (Inno Setup 6) around the
; public-preview bundle. The wizard finds the game through Steam, checks the
; client build, downloads what is missing (Visual C++ runtime x86; Python for
; developers when that component is chosen), then runs the bundle's tested
; install.ps1 to put the loader, Lua host, tools and keys into the game.
; Uninstall (Windows "Apps") runs uninstall.ps1, which restores the originals.
;
; Built by tools/build_public_preview.ps1:
;   ISCC.exe /DAppVersion=... /DBuildNumber=N /DBundleDir=... /DBundleZip=...
;            /DClientVersion=... /DClientHash=... /DOutputDir=... /DOutputBaseName=... setup.iss

#ifndef AppVersion
  #error AppVersion is required (0.1.0-preview.N)
#endif
#ifndef BundleDir
  #error BundleDir is required (the assembled bundle folder)
#endif
#ifndef BundleZip
  #error BundleZip is required (the bundle .zip next to the folder)
#endif
#ifndef ClientVersion
  #define ClientVersion "11.20.0.887"
#endif
#ifndef ClientHash
  #error ClientHash is required (SHA-256 of the supported wotblitz.exe)
#endif
#ifndef BuildNumber
  #define BuildNumber 0
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef OutputBaseName
  #define OutputBaseName "BlitzForge-Setup-" + AppVersion
#endif
#define BundleZipName ExtractFileName(BundleZip)
#define BundleFolderName ExtractFileName(BundleDir)
#define PythonUrl "https://www.python.org/ftp/python/3.13.7/python-3.13.7-amd64.exe"
#define PythonSha256 "b12e2e82461ac8e51fc43289050bc8eb937a32d84ce4d242e2c88258c37cf2bb"
#define VcRedistUrl "https://aka.ms/vs/17/release/vc_redist.x86.exe"

[Setup]
AppId={{8D2C4E7A-5B1F-4C39-9E6B-2F7A1D3C5E80}
AppName=BlitzForge
AppVersion={#AppVersion}
AppVerName=BlitzForge {#AppVersion}
AppPublisher=BlitzForge
AppPublisherURL=https://blitz-forge.org
AppSupportURL=https://blitz-forge.org/docs
AppUpdatesURL=https://blitz-forge.org/download
VersionInfoVersion=0.1.0.{#BuildNumber}
VersionInfoDescription=BlitzForge installer for World of Tanks Blitz {#ClientVersion}
DefaultDirName={code:DefaultGameDir}
DirExistsWarning=no
DisableWelcomePage=no
DisableDirPage=no
AppendDefaultDirName=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
DisableReadyMemo=no
WizardStyle=classic
WizardSizePercent=100
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x86compatible
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2/max
SolidCompression=yes
UninstallFilesDir={app}\wotbmod\setup
UninstallDisplayName=BlitzForge {#AppVersion} (World of Tanks Blitz)
Uninstallable=yes
SetupLogging=yes
ShowLanguageDialog=auto

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
ru.WelcomeText=Мастер поставит в World of Tanks Blitz загрузчик модов BlitzForge, Lua-хост, команду wotbmod и кнопку «Установить в игру» для сайта blitz-forge.org.%n%nНабор {#AppVersion} рассчитан на клиент {#ClientVersion}. Закройте игру перед установкой.%n%nНедостающие зависимости (Visual C++ Runtime x86; Python, если выбран) мастер скачает сам.
en.WelcomeText=This wizard installs the BlitzForge mod loader, the Lua host, the wotbmod command and the "Install to game" browser button for blitz-forge.org into World of Tanks Blitz.%n%nBundle {#AppVersion} supports client {#ClientVersion}. Close the game before installing.%n%nMissing prerequisites (Visual C++ Runtime x86; Python when selected) are downloaded automatically.
ru.DirLabel=Папка игры (в ней лежит wotblitz.exe)
en.DirLabel=Game folder (the one with wotblitz.exe)
ru.DirText=Мастер нашёл эту папку через Steam. Если игра стоит в другом месте, укажите её папку.
en.DirText=Setup found this folder through Steam. Pick another one if the game lives elsewhere.
ru.NoExe=В папке %1 нет wotblitz.exe. Укажите папку игры, обычно ...\steamapps\common\World of Tanks Blitz.
en.NoExe=%1 has no wotblitz.exe. Choose the game folder, usually ...\steamapps\common\World of Tanks Blitz.
ru.WrongBuild=Клиент в папке %1 не той сборки. Этот набор рассчитан на {#ClientVersion}; после обновления игры скачайте новый набор с blitz-forge.org.
en.WrongBuild=The client in %1 is another build. This bundle supports {#ClientVersion}; after a game update download a new bundle from blitz-forge.org.
ru.GameRunning=Игра сейчас запущена. Закройте World of Tanks Blitz и нажмите «Далее» снова.
en.GameRunning=The game is running. Close World of Tanks Blitz and click Next again.
ru.CompCore=Загрузчик модов и Lua-хост
en.CompCore=Mod loader and Lua host
ru.CompTools=Команда wotbmod в терминале (папка wotbmod в PATH пользователя)
en.CompTools=The wotbmod command in any terminal (wotbmod folder on the user PATH)
ru.CompLauncher=Кнопка «Установить в игру» на сайте (ссылки wotbmod://)
en.CompLauncher=The "Install to game" button on the site (wotbmod:// links)
ru.CompPython=Python 3.13 для разработчиков модов (скачивается, ~27 МБ)
en.CompPython=Python 3.13 for mod developers (downloaded, ~27 MB)
ru.TypeFull=Полная (с Python для разработчиков)
en.TypeFull=Full (with Python for developers)
ru.TypePlayer=Для игрока
en.TypePlayer=Player
ru.TypeCustom=Выборочная
en.TypeCustom=Custom
ru.Installing=Копирую файлы в игру и проверяю их хеши...
en.Installing=Copying files into the game and checking their hashes...
ru.InstallFailed=Установка в папку игры не удалась. Ничего не изменено: установщик вернул всё, что успел тронуть.%n%n%1
en.InstallFailed=Installing into the game folder failed. Nothing was changed: the installer put back everything it had touched.%n%n%1
ru.VcFailed=Не удалось установить Visual C++ Runtime x86 (код %1). Установите его вручную с сайта Microsoft и запустите мастер снова.
en.VcFailed=Visual C++ Runtime x86 could not be installed (code %1). Install it from Microsoft and run this setup again.
ru.PyFailed=Не удалось установить Python (код %1). Загрузчик и моды работают без него; Python нужен только для разработки.
en.PyFailed=Python could not be installed (code %1). The loader and mods work without it; Python is only needed for development.
ru.OpenPortal=Открыть каталог модов blitz-forge.org
en.OpenPortal=Open the mod catalogue at blitz-forge.org
ru.UninstallFailed=Возврат исходных файлов игры не удался:%n%n%1%n%nПроверьте файлы игры в Steam (Свойства → Установленные файлы → Проверить целостность).
en.UninstallFailed=Restoring the game's original files failed:%n%n%1%n%nVerify the game files in Steam (Properties → Installed files → Verify integrity).
ru.ReadyDeps=Зависимости
en.ReadyDeps=Prerequisites
ru.DepVc=Visual C++ Runtime x86: будет скачан и установлен (нужны права администратора)
en.DepVc=Visual C++ Runtime x86: will be downloaded and installed (administrator rights required)
ru.DepVcOk=Visual C++ Runtime x86: уже установлен
en.DepVcOk=Visual C++ Runtime x86: already installed
ru.DepPy=Python 3.13: будет скачан и установлен для текущего пользователя
en.DepPy=Python 3.13: will be downloaded and installed for the current user
ru.DepPyOk=Python 3.13: уже установлен
en.DepPyOk=Python 3.13: already installed

[Types]
Name: "player"; Description: "{cm:TypePlayer}"
Name: "full"; Description: "{cm:TypeFull}"
Name: "custom"; Description: "{cm:TypeCustom}"; Flags: iscustom

[Components]
Name: "core"; Description: "{cm:CompCore}"; Types: player full custom; Flags: fixed
Name: "tools"; Description: "{cm:CompTools}"; Types: player full custom
Name: "launcher"; Description: "{cm:CompLauncher}"; Types: player full custom
Name: "python"; Description: "{cm:CompPython}"; Types: full

[Files]
; The whole bundle travels as its zip and is unpacked into {tmp} right before
; install.ps1 runs (PrepareToInstall), so a failure aborts before Setup
; registers anything.
Source: "{#BundleZip}"; DestDir: "{tmp}"; Flags: dontcopy
; What the uninstaller needs stays next to it in the game folder.
Source: "{#BundleDir}\common.ps1"; DestDir: "{app}\wotbmod\setup"; Flags: ignoreversion
Source: "{#BundleDir}\install.ps1"; DestDir: "{app}\wotbmod\setup"; Flags: ignoreversion
Source: "{#BundleDir}\uninstall.ps1"; DestDir: "{app}\wotbmod\setup"; Flags: ignoreversion
Source: "{#BundleDir}\verify.ps1"; DestDir: "{app}\wotbmod\setup"; Flags: ignoreversion
Source: "{#BundleDir}\release-manifest.json"; DestDir: "{app}\wotbmod\setup"; Flags: ignoreversion
Source: "{#BundleDir}\README_RU.md"; DestDir: "{app}\wotbmod\setup"; Flags: ignoreversion

[Run]
Filename: "https://blitz-forge.org"; Description: "{cm:OpenPortal}"; Flags: shellexec postinstall nowait skipifsilent unchecked

[Code]
var
  DownloadPage: TDownloadWizardPage;
  NeedVcRedist: Boolean;
  NeedPython: Boolean;
  VcRedistFile: String;
  PythonFile: String;

// --- finding the game --------------------------------------------------------------

function GameExe(const Dir: String): String;
begin
  Result := AddBackslash(Dir) + 'wotblitz.exe';
end;

function SteamGameDir(const SteamRoot: String): String;
var
  Vdf, Line, Value: String;
  Lines: TArrayOfString;
  I, P: Integer;
  Candidate: String;
begin
  Result := '';
  if SteamRoot = '' then exit;
  Candidate := AddBackslash(SteamRoot) + 'steamapps\common\World of Tanks Blitz';
  if FileExists(GameExe(Candidate)) then begin Result := Candidate; exit; end;
  Vdf := AddBackslash(SteamRoot) + 'steamapps\libraryfolders.vdf';
  if not LoadStringsFromFile(Vdf, Lines) then exit;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    Line := Trim(Lines[I]);
    if Pos('"path"', Line) = 1 then begin
      Value := Trim(Copy(Line, 7, Length(Line)));
      StringChangeEx(Value, '"', '', True);
      StringChangeEx(Value, '\\', '\', True);
      Candidate := AddBackslash(Trim(Value)) + 'steamapps\common\World of Tanks Blitz';
      if FileExists(GameExe(Candidate)) then begin Result := Candidate; exit; end;
    end;
  end;
  P := 0;
end;

function DefaultGameDir(Param: String): String;
var
  SteamRoot: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamRoot) then begin
    StringChangeEx(SteamRoot, '/', '\', True);
    Result := SteamGameDir(SteamRoot);
  end;
  if (Result = '') and RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath', SteamRoot) then
    Result := SteamGameDir(SteamRoot);
  if (Result = '') and RegQueryStringValue(HKLM, 'SOFTWARE\Valve\Steam', 'InstallPath', SteamRoot) then
    Result := SteamGameDir(SteamRoot);
  if Result = '' then
    Result := ExpandConstant('{commonpf32}') + '\Steam\steamapps\common\World of Tanks Blitz';
end;

function GameIsRunning: Boolean;
var
  ResultCode: Integer;
begin
  // tasklist answers 0 either way; the filter output names the process only when it runs
  Result := False;
  if Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq wotblitz.exe" | find /I "wotblitz.exe" >nul', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := ResultCode = 0;
end;

// --- prerequisites ------------------------------------------------------------------

function VcRedistInstalled: Boolean;
var
  Installed, Major: Cardinal;
begin
  Result := False;
  if RegQueryDWordValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x86', 'Installed', Installed) then
    Result := Installed = 1
  else if RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x86', 'Installed', Installed) then
    Result := Installed = 1;
  if Result and RegQueryDWordValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x86', 'Major', Major) then
    Result := Major >= 14;
end;

function PythonInstalled: Boolean;
var
  Path: String;
begin
  Result := RegQueryStringValue(HKCU, 'Software\Python\PythonCore\3.13\InstallPath', '', Path)
    or RegQueryStringValue(HKLM, 'SOFTWARE\Python\PythonCore\3.13\InstallPath', '', Path)
    or RegQueryStringValue(HKCU, 'Software\Python\PythonCore\3.12\InstallPath', '', Path)
    or RegQueryStringValue(HKLM, 'SOFTWARE\Python\PythonCore\3.12\InstallPath', '', Path);
end;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  if Progress = ProgressMax then Log(Format('Downloaded %s', [FileName]));
  Result := True;
end;

procedure InitializeWizard;
begin
  WizardForm.WelcomeLabel2.Caption := CustomMessage('WelcomeText');
  WizardForm.SelectDirLabel.Caption := CustomMessage('DirText');
  WizardForm.SelectDirBrowseLabel.Caption := CustomMessage('DirLabel');
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc), @OnDownloadProgress);
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Deps: String;
begin
  if VcRedistInstalled then Deps := Space + CustomMessage('DepVcOk') else Deps := Space + CustomMessage('DepVc');
  if WizardIsComponentSelected('python') then begin
    if PythonInstalled then Deps := Deps + NewLine + Space + CustomMessage('DepPyOk')
    else Deps := Deps + NewLine + Space + CustomMessage('DepPy');
  end;
  Result := MemoDirInfo + NewLine + NewLine + MemoTypeInfo + NewLine + NewLine + MemoComponentsInfo + NewLine + NewLine
    + CustomMessage('ReadyDeps') + ':' + NewLine + Deps;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir: String;
  Hash: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    Dir := RemoveBackslashUnlessRoot(WizardDirValue);
    if not FileExists(GameExe(Dir)) then begin
      MsgBox(FmtMessage(CustomMessage('NoExe'), [Dir]), mbError, MB_OK);
      Result := False; exit;
    end;
    Hash := Lowercase(GetSHA256OfFile(GameExe(Dir)));
    if Hash <> Lowercase('{#ClientHash}') then begin
      MsgBox(FmtMessage(CustomMessage('WrongBuild'), [Dir]), mbError, MB_OK);
      Result := False; exit;
    end;
    if GameIsRunning then begin
      MsgBox(CustomMessage('GameRunning'), mbError, MB_OK);
      Result := False; exit;
    end;
  end;
  if CurPageID = wpReady then begin
    NeedVcRedist := not VcRedistInstalled;
    NeedPython := WizardIsComponentSelected('python') and not PythonInstalled;
    if NeedVcRedist or NeedPython then begin
      DownloadPage.Clear;
      if NeedVcRedist then DownloadPage.Add('{#VcRedistUrl}', 'vc_redist.x86.exe', '');
      if NeedPython then DownloadPage.Add('{#PythonUrl}', 'python-installer.exe', '{#PythonSha256}');
      DownloadPage.Show;
      try
        try
          DownloadPage.Download;
          VcRedistFile := ExpandConstant('{tmp}\vc_redist.x86.exe');
          PythonFile := ExpandConstant('{tmp}\python-installer.exe');
        except
          if DownloadPage.AbortedByUser then Log('Download aborted by user.')
          else SuppressibleMsgBox(AddPeriod(GetExceptionMessage), mbCriticalError, MB_OK, IDOK);
          Result := False;
        end;
      finally
        DownloadPage.Hide;
      end;
    end;
  end;
end;

function TailOfFile(const FileName: String; MaxLines: Integer): String;
var
  Lines: TArrayOfString;
  I, Start: Integer;
begin
  Result := '';
  if not LoadStringsFromFile(FileName, Lines) then exit;
  Start := GetArrayLength(Lines) - MaxLines;
  if Start < 0 then Start := 0;
  for I := Start to GetArrayLength(Lines) - 1 do
    Result := Result + Lines[I] + #13#10;
end;

function InstallSwitches: String;
begin
  Result := '';
  if WizardIsComponentSelected('launcher') then Result := Result + ' -RegisterLauncher';
  if WizardIsComponentSelected('tools') then Result := Result + ' -AddToPath';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  Tmp, LogFile, Command: String;
begin
  Result := '';
  // 1. prerequisites
  if NeedVcRedist and FileExists(VcRedistFile) then begin
    WizardForm.StatusLabel.Caption := 'Visual C++ Runtime x86';
    if not ShellExec('runas', VcRedistFile, '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then ResultCode := -1;
    if (ResultCode <> 0) and (ResultCode <> 1638) and (ResultCode <> 3010) then begin
      Result := FmtMessage(CustomMessage('VcFailed'), [IntToStr(ResultCode)]);
      exit;
    end;
    if ResultCode = 3010 then NeedsRestart := True;
  end;
  if NeedPython and FileExists(PythonFile) then begin
    WizardForm.StatusLabel.Caption := 'Python 3.13';
    if not Exec(PythonFile, '/quiet InstallAllUsers=0 PrependPath=1 Include_test=0 Include_launcher=1', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then ResultCode := -1;
    if ResultCode <> 0 then
      SuppressibleMsgBox(FmtMessage(CustomMessage('PyFailed'), [IntToStr(ResultCode)]), mbInformation, MB_OK, IDOK);
  end;
  // 2. the bundle's own installer, from the unpacked zip
  WizardForm.StatusLabel.Caption := CustomMessage('Installing');
  ExtractTemporaryFile('{#BundleZipName}');
  Tmp := ExpandConstant('{tmp}');
  LogFile := Tmp + '\wotbmod-install.log';
  // Every stream of the script lands in a UTF-8 log; the last line says how it went.
  Command := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "'
    + '$out = & { try { '
    + 'Expand-Archive -LiteralPath ''' + Tmp + '\{#BundleZipName}'' -DestinationPath ''' + Tmp + '\bundle'' -Force; '
    + '& ''' + Tmp + '\bundle\{#BundleFolderName}\install.ps1'' -GameRoot ''' + ExpandConstant('{app}') + '''' + InstallSwitches + '; '
    + '''WOTBMOD-OK'' } catch { $_.ToString(); ''WOTBMOD-FAILED'' } } *>&1; '
    + '$out | Out-String -Stream | Out-File -LiteralPath ''' + LogFile + ''' -Encoding utf8; '
    + 'if (@($out)[-1] -eq ''WOTBMOD-OK'') { exit 0 } else { exit 1 }"';
  Log('install: powershell ' + Command);
  if not Exec('powershell.exe', Command, Tmp, SW_HIDE, ewWaitUntilTerminated, ResultCode) then ResultCode := -1;
  Log('install.ps1 exit code ' + IntToStr(ResultCode));
  if ResultCode <> 0 then
    Result := FmtMessage(CustomMessage('InstallFailed'), [TailOfFile(LogFile, 12)]);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  Setup, LogFile, Command: String;
begin
  if CurUninstallStep <> usUninstall then exit;
  Setup := ExpandConstant('{app}\wotbmod\setup');
  LogFile := ExpandConstant('{tmp}\wotbmod-uninstall.log');
  Command := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "'
    + '$out = & { try { & ''' + Setup + '\uninstall.ps1'' -GameRoot ''' + ExpandConstant('{app}') + '''; ''WOTBMOD-OK'' } '
    + 'catch { $_.ToString(); ''WOTBMOD-FAILED'' } } *>&1; '
    + '$out | Out-String -Stream | Out-File -LiteralPath ''' + LogFile + ''' -Encoding utf8; '
    + 'if (@($out)[-1] -eq ''WOTBMOD-OK'') { exit 0 } else { exit 1 }"';
  if not Exec('powershell.exe', Command, Setup, SW_HIDE, ewWaitUntilTerminated, ResultCode) then ResultCode := -1;
  if ResultCode <> 0 then
    SuppressibleMsgBox(FmtMessage(CustomMessage('UninstallFailed'), [TailOfFile(LogFile, 12)]), mbError, MB_OK, IDOK);
end;
