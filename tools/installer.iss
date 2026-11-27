; installer.iss — USBMonitor Windows setup (Inno Setup 6)
;
; Built on hosted runners (ISCC) with /DAPP_VERSION=x.y.z; the output
; installer is published as a release asset next to the portable zip:
;   usbmon-<version>-windows-setup.exe
;
; Installs usbmon.exe (GUI subsystem, tray daemon), optional autostart,
; Start Menu entry and a proper uninstaller.

#define AppName "USBMonitor"
#define AppExeName "usbmon.exe"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

[Setup]
AppId={{B7E4C1F2-9A3D-4E5B-8C6F-2D1A0B9E7C31}
AppName={#AppName}
AppVersion={#APP_VERSION}
AppVerName={#AppName} {#APP_VERSION}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
; The icon ships with the repo and is embedded by windres; reuse it for
; the installer so the setup exe matches the installed app.
SetupIconFile=..\res\usbmon.ico
UninstallDisplayIcon={app}\{#AppExeName}
OutputDir=dist
OutputBaseFilename=usbmon-{#APP_VERSION}-windows-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
PrivilegesRequired=admin
Uninstallable=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; \
    GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "开机自动启动 USB 设备监控"; \
    GroupDescription: "后台行为:"

[Files]
; Paths are relative to this script (tools\), so the build output at the
; repo root is addressed with ..\.
Source: "..\winbin\usbmon.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; \
    Tasks: desktopicon

[Registry]
; HKCU — no admin needed at runtime; autostart launches the tray daemon.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "{#AppName}"; \
    ValueData: """{app}\{#AppExeName}"""; \
    Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop a running daemon before removal (best effort; fails silently).
Filename: "{cmd}"; Parameters: "/C taskkill /IM {#AppExeName} /F >nul 2>&1"; \
    Flags: runhidden; RunOnceId: "KillDaemon"
