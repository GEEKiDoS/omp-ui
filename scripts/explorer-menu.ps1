<#
.SYNOPSIS
Adds "Open with OMP-UI" to Explorer's right-click menu on folders (or removes it with -Uninstall).

.DESCRIPTION
Per user (HKCU), no administrator rights needed. On Windows 11 the entry is in the classic menu,
under "Show more options" (Shift+F10, or Shift+right-click, opens that menu directly).

.PARAMETER Exe
omp-ui.exe to open folders with; defaults to the one beside this script.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File explorer-menu.ps1
powershell -ExecutionPolicy Bypass -File explorer-menu.ps1 -Uninstall
#>
param(
	[string]$Exe = (Join-Path $PSScriptRoot 'omp-ui.exe'),
	[switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$key = 'HKCU:\Software\Classes\Directory\shell\omp-ui'

if ($Uninstall) {
	if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse }
	Write-Host 'Removed "Open with OMP-UI".'
	return
}

$Exe = (Resolve-Path -LiteralPath $Exe).Path
New-Item -Path "$key\command" -Force | Out-Null
Set-ItemProperty -LiteralPath $key -Name '(default)' -Value 'Open with OMP-UI'
Set-ItemProperty -LiteralPath $key -Name 'Icon' -Value "`"$Exe`""
# %V is the folder that was right-clicked.
Set-ItemProperty -LiteralPath "$key\command" -Name '(default)' -Value "`"$Exe`" `"%V`""
Write-Host "Added `"Open with OMP-UI`" for folders, opening $Exe."
