; installer.iss — Inno Setup script for XR_APILAYER_MLEDOUR_fov_crop
;
; Builds a single-file Setup.exe that:
;   1. Copies the DLL + JSON manifest to Program Files (correct ACLs for
;      sandboxed identities like WebXR in Chrome, inherited by default).
;   2. Registers the JSON manifest in HKLM for the OpenXR loader.
;   3. Creates an Add/Remove Programs entry with uninstaller.
;
; Compile from CI or locally:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DMyAppVersion=0.0.1 installer.iss
;
; The /DMyAppVersion flag is mandatory for tagged builds; for local dev
; builds without it, the fallback "0.0.0-dev" is used.

#define MyAppName "XR_APILAYER_MLEDOUR_fov_crop"

; Accept version from the ISCC command line (/DMyAppVersion=x.y.z).
; Fall back to a dev placeholder when compiling interactively.
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif

[Setup]
; AppId is a fixed GUID that identifies this product across upgrades.
; Do NOT change it between releases — Inno Setup uses it to detect an
; existing installation and offer an upgrade instead of a side-by-side.
AppId={{60C13550-5D5E-446A-BD00-E85112A7D6A2}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=mledour
AppPublisherURL=https://github.com/mledour/OpenXR-Layer-crop-fov
AppSupportURL=https://github.com/mledour/OpenXR-Layer-crop-fov/issues
DefaultDirName={autopf}\OpenXR-Layer-fov-crop
; No Start Menu group — this layer has no user-facing executable.
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\bin\installer
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-x64-Setup
Compression=lzma2
SolidCompression=yes
; x64 only. The layer DLL is 64-bit; 32-bit is not currently supported.
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
; Admin elevation required: writing to HKLM + Program Files.
PrivilegesRequired=admin
; Show the license page during install.
; Uninstall info visible in Add/Remove Programs.
UninstallDisplayName={#MyAppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; The DLL and the processed JSON manifest from the Release x64 build.
; Paths are relative to this .iss file.
Source: "..\bin\x64\Release\{#MyAppName}.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\bin\x64\Release\{#MyAppName}.json"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Register the layer as an implicit API layer for the OpenXR 1.x loader.
; The value name is the full path to the JSON manifest; the DWORD value 0
; means "enabled" (the loader spec treats non-zero as "disabled").
; Flags: uninsdeletevalue removes this entry automatically on uninstall.
Root: HKLM; Subkey: "Software\Khronos\OpenXR\1\ApiLayers\Implicit"; \
  ValueName: "{app}\{#MyAppName}.json"; ValueType: dword; ValueData: 0; \
  Flags: uninsdeletevalue
