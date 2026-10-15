; Compile through tools/workflow.py installer, using the portable package as input.
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef PackageDir
  #error PackageDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif

[Setup]
AppId=Euphony
AppName=Euphony
AppVersion={#AppVersion}
AppPublisher=phono-project
AppPublisherURL=https://github.com/phono-project/Euphony
DefaultDirName={autopf}\Euphony
DefaultGroupName=Euphony
DisableProgramGroupPage=yes
; Keep upgrades at the same path, including the paths in existing user settings.
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.18362
OutputDir={#OutputDir}
OutputBaseFilename=Euphony-{#AppVersion}-windows-x64-setup
SetupIconFile=..\img\euphony.ico
UninstallDisplayIcon={app}\EuphonySettings.exe
LicenseFile=..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
; Do not ask Restart Manager to close every application hosting a TSF DLL.
CloseApplicationsFilter=*.exe
RestartApplications=no
UninstallRestartComputer=yes
UninstallLogging=yes

[Files]
; Inno calls the native DLL entry points for each architecture, including rollback
; and uninstallation. No PowerShell or script host runs on the user's machine.
Source: "{#PackageDir}\EuphonyTsf.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit restartreplace uninsrestartdelete
Source: "{#PackageDir}\x86\EuphonyTsf.dll"; DestDir: "{app}\x86"; Flags: ignoreversion regserver 32bit restartreplace uninsrestartdelete
Source: "{#PackageDir}\*"; DestDir: "{app}"; Excludes: "EuphonyTsf.dll,x86\EuphonyTsf.dll"; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace uninsrestartdelete

[Registry]
; Match the existing portable registration and x86 broker-discovery convention.
Root: HKCU64; Subkey: "Software\Euphony"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKCU64; Subkey: "Software\Euphony"; ValueType: string; ValueName: "X86DllPath"; ValueData: "{app}\x86\EuphonyTsf.dll"; Flags: uninsdeletevalue uninsdeletekeyifempty

[Icons]
Name: "{group}\Euphony Settings"; Filename: "{app}\EuphonySettings.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall Euphony"; Filename: "{uninstallexe}"

[Messages]
FinishedLabel=Euphony is installed. Restart Windows if prompted; otherwise sign out and back in before using Win+Space to select Euphony. Open Euphony Settings from Start to select your separately downloaded model. Settings require Microsoft WebView2 Evergreen Runtime.
ConfirmUninstall=Save your work and exit Euphony from its tray menu before continuing.%n%nDo you want to remove Euphony? Preferences, learned vocabulary and separately downloaded models will be kept.
