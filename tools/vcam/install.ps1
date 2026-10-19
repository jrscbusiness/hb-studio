# Installs the HB Studio virtual camera media source (run as Administrator).
# The DLL must live in a location the Frame Server (LocalService) can read,
# so it is copied to Program Files and registered under HKLM.
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$src    = Join-Path $PSScriptRoot 'hb_vcam_source.dll'
$target = Join-Path $env:ProgramFiles 'HB Studio\VirtualCamera'
$dll    = Join-Path $target 'hb_vcam_source.dll'
$logDir = Join-Path $env:ProgramData 'HBStudio'

if (-not (Test-Path $src)) { throw "hb_vcam_source.dll not found next to this script." }
if ([Environment]::OSVersion.Version.Build -lt 22000) { throw "Windows 11 (build 22000+) is required." }

# An older copy may be loaded by the Frame Server; stop it so the file can be replaced.
Stop-Service -Name FrameServer -Force -ErrorAction SilentlyContinue

New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item $src $dll -Force

# Frame Server runs as LocalService: needs read+execute on the DLL.
icacls $target /grant "NT AUTHORITY\LOCAL SERVICE:(OI)(CI)RX" | Out-Null

# Diagnostics log, writable by the service.
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
New-Item -ItemType File -Force -Path (Join-Path $logDir 'vcam.log') | Out-Null
icacls $logDir /grant "*S-1-1-0:(OI)(CI)M" | Out-Null   # Everyone: modify

# regsvr32 calls DllRegisterServer (writes HKLM\Software\Classes\CLSID\{...}).
$p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($p.ExitCode)" }

$key = 'HKLM:\Software\Classes\CLSID\{6F0F5B0E-3A52-4D7C-9B2B-5E0C7A1D4C31}\InprocServer32'
if (-not (Test-Path $key)) { throw "Registration key was not created." }

Write-Host "Installed: $dll"
Write-Host "Registered: $key = $((Get-ItemProperty $key).'(default)')"
Write-Host "Log file:   $(Join-Path $logDir 'vcam.log')"
Write-Host "Next: run hb_vcam_poc.exe (normal user, NOT elevated)."
