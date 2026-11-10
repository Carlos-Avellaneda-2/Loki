; Loki installer (Inno Setup 6, D27). Built by scripts/make-installer.ps1, which stages the files
; (loki.exe, Qt via windeployqt, FFmpeg/ADS/vcpkg DLLs, MSVC runtime, licenses) into StageDir.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #error StageDir must be defined (/DStageDir=...)
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6F3D2A61-5B0E-4C8E-9C1A-7A2B4D9E0F11}
AppName=Loki
AppVersion={#AppVersion}
AppVerName=Loki {#AppVersion}
AppPublisher=Loki
DefaultDirName={autopf}\Loki
DefaultGroupName=Loki
UninstallDisplayIcon={app}\loki.exe
OutputDir={#OutputDir}
OutputBaseFilename=Loki-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-user install by default (no admin needed); the dialog offers an all-users install.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes
WizardStyle=modern
SetupIconFile=..\src\app\resources\icons\loki.ico
LicenseFile={#StageDir}\licenses\NOTICE.txt

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "associate"; Description: "Abrir archivos .loki con Loki"; GroupDescription: "Asociaciones:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Loki"; Filename: "{app}\loki.exe"
Name: "{group}\Licencias"; Filename: "{app}\licenses"
Name: "{group}\{cm:UninstallProgram,Loki}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Loki"; Filename: "{app}\loki.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.loki"; ValueType: string; ValueName: ""; ValueData: "Loki.Project"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Loki.Project"; ValueType: string; ValueName: ""; ValueData: "Proyecto de Loki"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Loki.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\loki.exe,0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Loki.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\loki.exe"" ""%1"""; Tasks: associate

[Run]
Filename: "{app}\loki.exe"; Description: "{cm:LaunchProgram,Loki}"; Flags: nowait postinstall skipifsilent
