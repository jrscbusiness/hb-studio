# Removes the HB Studio virtual camera media source (run as Administrator).
#Requires -RunAsAdministrator
$ErrorActionPreference = 'Continue'
$target = Join-Path $env:ProgramFiles 'HB Studio\VirtualCamera'
$dll    = Join-Path $target 'hb_vcam_source.dll'

Stop-Service -Name FrameServer -Force -ErrorAction SilentlyContinue
if (Test-Path $dll) { Start-Process regsvr32.exe -ArgumentList '/u', '/s', "`"$dll`"" -Wait }
Remove-Item -Recurse -Force $target -ErrorAction SilentlyContinue
Write-Host "Uninstalled. (Log folder $env:ProgramData\HBStudio kept for diagnostics.)"
