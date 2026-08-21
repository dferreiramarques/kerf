; Inno Setup script for Kerf Lite (Beta) VST3.
; Build the plugin first (Release config), then compile this with:
;   "C:\Users\david\AppData\Local\Programs\Inno Setup 6\ISCC.exe" installer\KerfLite.iss
; Output lands in ..\..\downloads (the same folder the Tauri desktop-app installers use).

#define MyAppName "Kerf Lite (Beta)"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "Monco.io"
#define MyAppURL "https://kerf.monco.io"
#define MyVst3Name "Kerf Lite (Beta).vst3"
#define MyBuiltVst3Dir "..\build\Kerf_artefacts\Release\VST3\Kerf Lite (Beta).vst3"

[Setup]
AppId={{6E2C9A6E-2F0B-4B7E-9C7B-A1C4D9F3E2B1}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={commoncf64}\VST3\{#MyVst3Name}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableWelcomePage=no
UsePreviousAppDir=no
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=..\..\downloads
OutputBaseFilename=KerfLite_{#MyAppVersion}_Beta_VST3_x64-setup
WizardStyle=modern
UninstallDisplayIcon={commoncf64}\VST3\{#MyVst3Name}\Contents\Resources\moduleinfo.json

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#MyBuiltVst3Dir}\*"; DestDir: "{commoncf64}\VST3\{#MyVst3Name}"; Flags: recursesubdirs ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\{#MyVst3Name}"

[Messages]
WelcomeLabel2=This installs the beta VST3 build of %n%nKerf Lite%n%ninto your system's VST3 folder, so any VST3 host can load it as an instrument.%n%nThis is a beta - please report anything that sounds wrong or behaves unexpectedly.
FinishedLabelNoIcons=Setup has finished installing {#MyAppName}. Rescan plugins in your DAW to see it.
