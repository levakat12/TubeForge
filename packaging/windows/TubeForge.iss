#define AppName "TubeForge"
#define AppVersion "0.10.0"
#ifndef StageDir
  #error StageDir must point to a staged TubeForge release.
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{7A47EA93-7305-4FD2-BCC4-10C598AA10EF}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=TubeForge Audio
DefaultDirName={autopf}\TubeForge
DefaultGroupName=TubeForge
OutputDir={#OutputDir}
OutputBaseFilename=TubeForge-{#AppVersion}-Windows-x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=admin
UninstallDisplayIcon={app}\TubeForge.exe
WizardStyle=modern
ChangesAssociations=no

[Types]
Name: "full"; Description: "Standalone and VST3"
Name: "compact"; Description: "VST3 only"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "standalone"; Description: "Standalone application"; Types: full
Name: "vst3"; Description: "VST3 plug-in"; Types: full compact
Name: "models"; Description: "Optional factory model packs"; Types: full; Flags: checkablealone
Name: "mlseparator"; Description: "Local Demucs ML separator setup (downloads runtime and weights)"; Types: full; Flags: checkablealone
Name: "docs"; Description: "Documentation and diagnostic collector"; Types: full compact

[Files]
Source: "{#StageDir}\Standalone\TubeForge.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#StageDir}\VST3\TubeForge.vst3\*"; DestDir: "{commoncf64}\VST3\TubeForge.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\Models\*"; DestDir: "{commonappdata}\TubeForge\Models"; Components: models; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#StageDir}\Documentation\*"; DestDir: "{app}\Documentation"; Components: docs; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\collect-diagnostics.ps1"; DestDir: "{app}\Tools"; Components: docs; Flags: ignoreversion
Source: "{#StageDir}\Tools\setup-ml-separator.ps1"; DestDir: "{app}\Tools"; Components: mlseparator; Flags: ignoreversion
Source: "{#StageDir}\Tools\demucs_separator_worker.py"; DestDir: "{app}\Tools"; Components: mlseparator; Flags: ignoreversion

[Icons]
Name: "{group}\TubeForge"; Filename: "{app}\TubeForge.exe"; Components: standalone
Name: "{group}\Collect diagnostics"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\Tools\collect-diagnostics.ps1"""; Components: docs
Name: "{group}\Install or repair ML separator"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\Tools\setup-ml-separator.ps1"""; Components: mlseparator
Name: "{group}\Uninstall TubeForge"; Filename: "{uninstallexe}"

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\Tools\setup-ml-separator.ps1"""; Description: "Install local ML separator (downloads dependencies)"; Flags: postinstall skipifsilent runasoriginaluser; Components: mlseparator
Filename: "{app}\TubeForge.exe"; Description: "Launch TubeForge"; Flags: nowait postinstall skipifsilent; Components: standalone
