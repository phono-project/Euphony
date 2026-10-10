[CmdletBinding()]
param(
    [string]$InstallDirectory,
    [string]$X86DllPath
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'native_registration.ps1')
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try {
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'TSF registration requires an elevated PowerShell window under the same Windows account. Run this script there; it does not request elevation automatically.'
    }
} finally { $identity.Dispose() }
if (-not [Environment]::Is64BitOperatingSystem) { throw 'This installation requires 64-bit Windows.' }
if (-not $InstallDirectory) {
    $InstallDirectory = Split-Path $PSScriptRoot -Parent
    if (-not (Test-Path -LiteralPath (Join-Path $InstallDirectory 'EuphonyTsf.dll'))) {
        $InstallDirectory = Join-Path $InstallDirectory 'dist/Euphony-0.2.0'
    }
}
$InstallDirectory = (Resolve-Path -LiteralPath $InstallDirectory).Path
$nativeDll = Join-Path $InstallDirectory 'EuphonyTsf.dll'
if (-not (Test-Path -LiteralPath $nativeDll -PathType Leaf)) { throw "EuphonyTsf.dll not found in $InstallDirectory" }
if (-not (Test-Path -LiteralPath (Join-Path $InstallDirectory 'EuphonyBroker.exe') -PathType Leaf)) { throw 'InstallDirectory must contain EuphonyBroker.exe and the native TSF DLL.' }
function Get-PeMachine([string]$DllPath) {
    $stream = [System.IO.File]::OpenRead($DllPath)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw 'Invalid DLL executable header.' }
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        if ($offset -lt 0 -or $offset -gt $stream.Length - 6) { throw 'Invalid DLL executable offset.' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw 'Invalid DLL PE header.' }
        return $reader.ReadUInt16()
    } finally { $reader.Dispose(); $stream.Dispose() }
}
if ((Get-PeMachine $nativeDll) -ne 0x8664) { throw 'The main installation requires the x64 EuphonyTsf.dll.' }
if (-not $X86DllPath) {
    $packaged = Join-Path $InstallDirectory 'x86/EuphonyTsf.dll'
    if (Test-Path -LiteralPath $packaged) { $X86DllPath = $packaged }
}
if ($X86DllPath) {
    $X86DllPath = (Resolve-Path -LiteralPath $X86DllPath).Path
    if ((Get-PeMachine $X86DllPath) -ne 0x014c) { throw 'X86DllPath must point to the x86 EuphonyTsf.dll.' }
}
$nativeSystem = if ([Environment]::Is64BitProcess) { 'System32' } else { 'Sysnative' }
$nativeRegsvr = Join-Path $env:WINDIR "$nativeSystem/regsvr32.exe"
# The TSF profile and COM DLLs are machine registrations; enablement and broker
# discovery belong to this account. Run elevated as the intended input-method user.
$recovery = "& '" + (Join-Path $PSScriptRoot 'unregister.ps1').Replace("'", "''") + "' -InstallDirectory '" + $InstallDirectory.Replace("'", "''") + "'"
if ($X86DllPath) { $recovery += " -X86DllPath '" + $X86DllPath.Replace("'", "''") + "'" }
$nativeExit = Invoke-EuphonyRegistrationProcess -ExecutablePath $nativeRegsvr -DllPath $nativeDll
if ($nativeExit -ne 0) { throw "Native TSF registration failed (exit $nativeExit)." }
try {
    # Keep recovery paths even when a subsequent x86 registration fails.
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Euphony')
    try {
        $key.SetValue('InstallDir', $InstallDirectory, [Microsoft.Win32.RegistryValueKind]::String)
        if ($X86DllPath) { $key.SetValue('X86DllPath', $X86DllPath, [Microsoft.Win32.RegistryValueKind]::String) }
    } finally { $key.Dispose() }
} catch {
    throw "The native TSF DLL was registered, but its recovery paths could not be saved: $($_.Exception.Message). Run the following command from the same elevated account to undo this partial installation: $recovery"
}
if ($X86DllPath) {
    $x86Exit = Invoke-EuphonyRegistrationProcess -ExecutablePath (Join-Path $env:WINDIR 'SysWOW64/regsvr32.exe') -DllPath $X86DllPath
    if ($x86Exit -ne 0) { throw "32-bit TSF registration failed (exit $x86Exit) after native registration succeeded. Recovery paths have been retained. Run the following command from the same elevated account to undo this partial installation: $recovery" }
}
Write-Host 'Euphony has been registered on this computer and enabled for this Windows account. Select Euphony from the Windows input language menu; reopen target applications if needed.'
if (-not $X86DllPath) { Write-Host 'The x86 TSF DLL is absent; 32-bit applications require a separate x86 build.' }
