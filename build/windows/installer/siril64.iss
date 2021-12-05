; Script generated by the Inno Setup Script Wizard.
; SEE THE DOCUMENTATION FOR DETAILS ON CREATING INNO SETUP SCRIPT FILES!

#include "version.isi"

#define MyAppName "SiriL"
#define MyAppExeName "siril.exe"
#define RootDir "C:\GitLab-Runner\builds\free-astro\siril"

[Setup]
; NOTE: The value of AppId uniquely identifies this application.
; Do not use the same AppId value in installers for other applications.
; (To generate a new GUID, click Tools | Generate GUID inside the IDE.)
AppId={{ADA3C347-68C3-4EAA-92B3-C1BDBD836EDB}
AppName=SiriL
AppVersion={#MAJOR}.{#MINOR}.{#MICRO}
AppPublisher=Free-Astro
AppPublisherURL=https://www.siril.org/
AppSupportURL=https://www.siril.org/
AppUpdatesURL=https://www.siril.org/
DefaultDirName={commonpf}\SiriL
DefaultGroupName=SiriL
OutputDir=_Output
OutputBaseFilename=siril-{#MAJOR}.{#MINOR}.{#MICRO}-setup
Compression=lzma
SolidCompression=yes
ChangesAssociations=yes
ArchitecturesInstallIn64BitMode=x64

WizardSmallImageFile=siril.bmp

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"; InfoBeforeFile: "texts\About-EN.rtf"; InfoAfterFile: "texts\Scripts-EN.rtf"
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"; InfoBeforeFile: "texts\About-FR.rtf"; InfoAfterFile: "texts\Scripts-FR.rtf"

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#RootDir}\siril\bin\siril.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
; Source: "C:\Users\Cyril\Pictures\SiriL_Travail\*"; DestDir: "{%USERPROFILE}\Pictures\"; Languages: fr; Flags: ignoreversion recursesubdirs createallsubdirs
; Source: "C:\Users\Cyril\Pictures\SiriL_Work\*"; DestDir: "{%USERPROFILE}\Pictures\"; Languages: en; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RootDir}\siril\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RootDir}\siril\lib\*"; DestDir: "{app}\lib"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RootDir}\siril\share\*"; DestDir: "{app}\share"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RootDir}\3rdparty\scripts\fr\*.ssf"; DestDir: "{app}\scripts"; Languages: fr; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RootDir}\scripts\*.ssf"; DestDir: "{app}\scripts"; Languages: en; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RootDir}\3rdparty\scripts\en\*.ssf"; DestDir: "{app}\scripts"; Languages:  en; Flags: ignoreversion recursesubdirs createallsubdirs

; NOTE: Don't use "Flags: ignoreversion" on any shared system files

[Registry]
Root: HKCR; Subkey: ".seq";                             ValueData: "{#MyAppName}";          Flags: uninsdeletevalue; ValueType: string;  ValueName: ""
Root: HKCR; Subkey: "{#MyAppName}";                     ValueData: "Program {#MyAppName}";  Flags: uninsdeletekey;   ValueType: string;  ValueName: ""
Root: HKCR; Subkey: "{#MyAppName}\DefaultIcon";         ValueData: "{app}\bin\{#MyAppExeName},1";               ValueType: string;  ValueName: ""
Root: HKCR; Subkey: "{#MyAppName}\shell\open\command";  ValueData: """{app}\bin\{#MyAppExeName}"" ""%1""";  ValueType: string;  ValueName: ""

[Icons]
Name: "{group}\SiriL"; Filename: "{app}\bin\{#MyAppExeName}";
Name: "{group}\{cm:UninstallProgram,SiriL}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\SiriL"; Filename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon; 
