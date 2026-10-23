; Inno Setup script for RefAB (Windows, VST3).
; Build:  iscc /DAppVersion=1.1.0 packaging\windows\RefAB.iss
; Expects the plugin at build\RefAB_artefacts\Release\VST3\RefAB.vst3

#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif

#define Root "..\.."

[Setup]
AppId={{4A3329DB-7207-4180-80EF-AD720FC0AB8D}
AppName=RefAB
AppVersion={#AppVersion}
AppVerName=RefAB {#AppVersion}
AppPublisher=RefAB
DefaultDirName={commoncf64}\VST3\RefAB.vst3
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
UninstallFilesDir={autopf}\RefAB
UninstallDisplayName=RefAB {#AppVersion} (VST3)
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#Root}\dist
OutputBaseFilename=RefAB-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Messages]
en.WelcomeLabel2=This will install RefAB {#AppVersion} (VST3) into%n%nC:\Program Files\Common Files\VST3%n%nClose your DAW before continuing.
ru.WelcomeLabel2=Будет установлен RefAB {#AppVersion} (VST3) в папку%n%nC:\Program Files\Common Files\VST3%n%nПеред продолжением закройте DAW.
en.FinishedLabel=RefAB is installed. Start your DAW and rescan plugins (REAPER: Preferences > Plug-ins > VST > Re-scan).
ru.FinishedLabel=RefAB установлен. Запустите DAW и пересканируйте плагины (REAPER: Preferences > Plug-ins > VST > Re-scan).

[InstallDelete]
; remove files of an older version inside the bundle
Type: filesandordirs; Name: "{commoncf64}\VST3\RefAB.vst3"

[Files]
Source: "{#Root}\build\RefAB_artefacts\Release\VST3\RefAB.vst3\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Root}\README.md"; DestDir: "{autopf}\RefAB"; DestName: "README.txt"; Flags: ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\RefAB.vst3"
