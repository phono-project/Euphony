[CmdletBinding()]
param([string]$InstallDirectory, [string]$X86DllPath)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
. (Join-Path $PSScriptRoot 'native_registration.ps1')
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try {
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'TSF unregistration requires an elevated PowerShell window under the same Windows account used for registration. Run this script there; it does not request elevation automatically.'
    }
} finally { $identity.Dispose() }
if (-not [Environment]::Is64BitOperatingSystem) { throw 'This installation requires 64-bit Windows.' }
$key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Euphony', $true)
try {
    if (-not $InstallDirectory -and $key) { $InstallDirectory = $key.GetValue('InstallDir') }
    if (-not $X86DllPath -and $key) { $X86DllPath = $key.GetValue('X86DllPath') }
    if (-not $InstallDirectory) {
        $InstallDirectory = Split-Path $PSScriptRoot -Parent
        if (-not (Test-Path -LiteralPath (Join-Path $InstallDirectory 'EuphonyTsf.dll'))) { $InstallDirectory = $EuphonyPackageDirectory }
    }
    $InstallDirectory = (Resolve-Path -LiteralPath $InstallDirectory).Path
    $nativeDll = Join-Path $InstallDirectory 'EuphonyTsf.dll'
    if (-not (Test-Path -LiteralPath $nativeDll -PathType Leaf)) { throw 'Keep the installed DLL available until unregistration completes.' }
    if ($X86DllPath) {
        if (-not (Test-Path -LiteralPath $X86DllPath -PathType Leaf)) {
            throw "The recorded x86 TSF DLL is missing: $X86DllPath. Restore it before unregistration; recovery paths have been retained."
        }
        $X86DllPath = (Resolve-Path -LiteralPath $X86DllPath).Path
        $x86Exit = Invoke-EuphonyRegistrationProcess -ExecutablePath (Join-Path $env:WINDIR 'SysWOW64/regsvr32.exe') -DllPath $X86DllPath -Unregister
        if ($x86Exit -ne 0) { throw "32-bit TSF unregistration failed (exit $x86Exit)." }
    }
    $nativeSystem = if ([Environment]::Is64BitProcess) { 'System32' } else { 'Sysnative' }
    $nativeExit = Invoke-EuphonyRegistrationProcess -ExecutablePath (Join-Path $env:WINDIR "$nativeSystem/regsvr32.exe") -DllPath $nativeDll -Unregister
    if ($nativeExit -ne 0) { throw "Native TSF unregistration failed (exit $nativeExit)." }
    if ($key -and $key.GetValue('InstallDir') -eq $InstallDirectory) { $key.DeleteValue('InstallDir', $false); $key.DeleteValue('X86DllPath', $false) }
} finally { if ($key) { $key.Dispose() } }
Write-Host 'Euphony has been unregistered from this computer. Your Euphony configuration and learned vocabulary remain available.'
