; Instalador de Zyntra (Inno Setup 6). Un solo instalador para Windows de 64 y 32 bits:
; en un Windows de 64 bits instala bin\x64, en uno de 32 bits instala bin\x86.
; Se compila con:  iscc installer\zyntra.iss   (lo hace .github\workflows\build.yml)

#define AppName "Zyntra"
#define AppVersion GetEnv("ZYNTRA_VERSION")
#if AppVersion == ""
  #define AppVersion "1.0.0"
#endif
#define AppPublisher "Zargch"
#define AppUrl "https://github.com/Zargch/zyntra"
#define AppExe "Zyntra.exe"
#define AppUserModelId "Zargch.Zyntra"

[Setup]
AppId={{6F2B8C1E-5A7D-4E39-9B1C-2D8E4F7A9C30}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
AppCopyright=Copyright (c) 2026 Zargch
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription=Instalador de {#AppName}
VersionInfoProductName={#AppName}
; por usuario y sin pedir permisos de administrador (se puede elegir "para todos" en el asistente)
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x86compatible x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
LicenseFile=..\docs\TERMINOS.txt
SetupIconFile=..\assets\zyntra.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=Zyntra-Setup-{#AppVersion}
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\bin\x64\Zyntra.exe"; DestDir: "{app}"; Check: Is64BitInstallMode; Flags: ignoreversion
Source: "..\bin\x86\Zyntra.exe"; DestDir: "{app}"; Check: not Is64BitInstallMode; Flags: ignoreversion
Source: "..\assets\intro.mov"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\docs\TERMINOS.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\PRIVACIDAD.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; AppUserModelID: "{#AppUserModelId}"; Comment: "Tus juegos de Steam, Epic, Xbox y Riot en un solo lugar"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; AppUserModelID: "{#AppUserModelId}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; datos que crea Zyntra: configuracion y cache de caratulas y videos
Type: filesandordirs; Name: "{localappdata}\Zyntra"
