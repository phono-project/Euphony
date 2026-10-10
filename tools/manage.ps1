[CmdletBinding()]
param(
    [Parameter(Position=0)][ValidateSet('Install','Uninstall','Status','Start','Stop','Settings')][string]$Action='Status',
    [string]$InstallDirectory
)
$ErrorActionPreference='Stop'
if ($env:EUPHONY_INSTANCE -or $env:EUPHONY_USER_DIR -or $env:EUPHONY_BROKER_PATH) {
    throw 'Clear EUPHONY_INSTANCE, EUPHONY_USER_DIR and EUPHONY_BROKER_PATH before managing the daily installation.'
}
$base=Split-Path $PSScriptRoot -Parent
$packaged=Test-Path -LiteralPath (Join-Path $base 'EuphonyBroker.exe')
$record=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Euphony')
$installed=if ($record) { $record.GetValue('InstallDir') }
if ($record) { $record.Dispose() }
if (-not $InstallDirectory) {
    if ($packaged) { $InstallDirectory=$base }
    elseif ($Action -eq 'Install') { $InstallDirectory=Join-Path $base 'dist/Euphony-0.2.0' }
    elseif ($installed) { $InstallDirectory=$installed }
    else { $InstallDirectory=Join-Path $base 'dist/Euphony-0.2.0' }
}
$InstallDirectory=[IO.Path]::GetFullPath($InstallDirectory)
function Test-Release {
    foreach ($name in @('EuphonyBroker.exe','EuphonySettings.exe','EuphonyTsf.dll','x86/EuphonyTsf.dll')) {
        $file=Join-Path $InstallDirectory $name
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Incomplete package: $file" }
        $version=[Diagnostics.FileVersionInfo]::GetVersionInfo($file).FileVersion
        if ($version -ne '0.2.0') { throw "Mixed or outdated package: $name version '$version'; this manager requires 0.2.0. Use the complete current release." }
    }
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class EuphonyPipeProcess {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint processId);
}
'@
function Read-Bytes($Pipe,[int]$Count) {
    $buffer=New-Object byte[] $Count
    $offset=0
    while ($offset -lt $Count) {
        $read=$Pipe.ReadAsync($buffer,$offset,$Count-$offset)
        if (-not $read.Wait(5000)) { throw 'Broker response timed out.' }
        $size=$read.Result
        if ($size -eq 0) { throw 'Broker disconnected.' }
        $offset += $size
    }
    return ,$buffer
}
function Invoke-Broker([string]$Operation) {
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $session=[Diagnostics.Process]::GetCurrentProcess().SessionId
    $pipe=[IO.Pipes.NamedPipeClientStream]::new('.',"Euphony-v1-$sid-$session",[IO.Pipes.PipeDirection]::InOut,[IO.Pipes.PipeOptions]::Asynchronous)
    try {
        try { $pipe.Connect(700) } catch [TimeoutException] { return $null }
        [uint32]$server=0
        if (-not [EuphonyPipeProcess]::GetNamedPipeServerProcessId($pipe.SafePipeHandle.DangerousGetHandle(),[ref]$server)) { throw 'Cannot identify broker process.' }
        $bytes=[Text.Encoding]::UTF8.GetBytes((@{version=1;op=$Operation}|ConvertTo-Json -Compress))
        $header=[BitConverter]::GetBytes([int]$bytes.Length)
        $pipe.Write($header,0,4); $pipe.Write($bytes,0,$bytes.Length)
        $size=[BitConverter]::ToInt32((Read-Bytes $pipe 4),0)
        if ($size -le 0 -or $size -gt 1048576) { throw 'Invalid broker response.' }
        $response=[Text.Encoding]::UTF8.GetString((Read-Bytes $pipe $size))|ConvertFrom-Json
        $ack=[Text.Encoding]::UTF8.GetBytes('{"ack":true}')
        $header=[BitConverter]::GetBytes([int]$ack.Length)
        $pipe.Write($header,0,4); $pipe.Write($ack,0,$ack.Length)
        if (-not $response.ok) { throw "Broker: $($response.error)" }
        return @{pid=$server;value=$response}
    } finally { $pipe.Dispose() }
}
function Stop-Broker {
    $reply=Invoke-Broker 'shutdown'
    if ($reply) {
        $process=Get-Process -Id $reply.pid -ErrorAction SilentlyContinue
        if ($process) { try { if (-not $process.WaitForExit(20000)) { throw 'Broker did not stop. No process was forcibly terminated.' } } finally { $process.Dispose() } }
        Write-Host 'Broker stopped through IPC.'
    }
}
function Update-ResourcePaths {
    $preferences=Join-Path $env:LOCALAPPDATA 'Euphony/config.json'
    if (-not (Test-Path -LiteralPath $preferences)) { return }
    $current=[IO.File]::ReadAllText($preferences)|ConvertFrom-Json
    $changed=$false
    foreach ($entry in @(@('core_library','phono_core.dll'),@('dictionary_path','data/libime.tsv'),@('language_model_path','data/zh_CN.lm'))) {
        $key=$entry[0]; $prior=[string]$current.$key
        if (-not $prior) { continue }
        $owner=Split-Path $prior -Parent
        if ($key -ne 'core_library') { $owner=Split-Path $owner -Parent }
        $managed=Test-Path -LiteralPath (Join-Path $owner 'EuphonyBroker.exe') -PathType Leaf
        if ($managed -and ([IO.Path]::GetFileName($prior) -eq [IO.Path]::GetFileName($entry[1]))) {
            $next=Join-Path $InstallDirectory $entry[1]
            if (-not (Test-Path -LiteralPath $next -PathType Leaf)) { throw "Packaged resource missing: $next" }
            if ($next -ne $prior) { $current.$key=$next; $changed=$true }
        }
    }
    if ($changed) {
        Copy-Item -LiteralPath $preferences -Destination ($preferences+'.before-update') -Force
        $temporary=$preferences+'.update'
        [IO.File]::WriteAllText($temporary,($current|ConvertTo-Json -Depth 30),[Text.UTF8Encoding]::new($false))
        [IO.File]::Replace($temporary,$preferences,$null)
        Write-Host 'Updated packaged core, dictionary and KenLM paths. External resource paths and model selection were preserved.'
    }
}
function Start-CurrentBroker {
    Test-Release
    Stop-Broker
    $file=Join-Path $InstallDirectory 'EuphonyBroker.exe'
    if (-not (Test-Path -LiteralPath $file)) { throw "Package missing: $file" }
    Update-ResourcePaths
    $process=Start-Process -FilePath $file -WorkingDirectory $InstallDirectory -WindowStyle Hidden -PassThru
    try {
        $deadline=[DateTime]::UtcNow.AddSeconds(25)
        while ($true) {
            if ($process.HasExited) { throw "Broker exited during startup (exit $($process.ExitCode)). Run Status to inspect the active installation." }
            $reply=Invoke-Broker 'get_config'
            if ($reply) {
                if ($reply.pid -ne $process.Id) { throw 'A different broker won startup. Run Status and retry Start after closing old input applications.' }
                Write-Host "Ready: $file (version $($reply.value.status.app_version), PID $($reply.pid))"
                break
            }
            if ([DateTime]::UtcNow -ge $deadline) { throw 'Broker startup timed out. Run Status to inspect the actual process and resource paths.' }
            Start-Sleep -Milliseconds 100
        }
    } finally { $process.Dispose() }
}
function Show-Status {
    Write-Host "Selected package: $InstallDirectory"
    foreach ($name in @('EuphonyBroker.exe','EuphonySettings.exe','EuphonyTsf.dll','x86/EuphonyTsf.dll')) {
        $file=Join-Path $InstallDirectory $name
        if (Test-Path -LiteralPath $file -PathType Leaf) { Write-Host "$name version: $([Diagnostics.FileVersionInfo]::GetVersionInfo($file).FileVersion)" }
    }
    Write-Host "Registered installation: $installed"
    $clsid='{1E7BDC4A-9862-43C6-AA16-8A40D96614E1}'
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)) {
        $hive=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
        try {
            $key=$hive.OpenSubKey("Software\Classes\CLSID\$clsid\InprocServer32")
            try { if ($key) { Write-Host "$view TSF: $($key.GetValue(''))" } else { Write-Host "$view TSF: not registered" } }
            finally { if ($key) { $key.Dispose() } }
        } finally { $hive.Dispose() }
    }
    $reply=Invoke-Broker 'get_config'
    if ($reply) {
        $running=Get-Process -Id $reply.pid -ErrorAction Stop
        try { Write-Host "Broker PID $($reply.pid): $($running.Path)" } finally { $running.Dispose() }
        Write-Host "Broker version: $($reply.value.status.app_version)"
        foreach ($key in @('core_library','dictionary_path','language_model_path','model_directory')) { Write-Host "$key : $($reply.value.config.$key)" }
    } else { Write-Host 'Broker: stopped' }
    Write-Host 'Applications with loaded Euphony TSF modules (reopen these after updating):'
    foreach ($process in Get-Process) {
        try {
            foreach ($module in $process.Modules) {
                if ($module.ModuleName -ieq 'EuphonyTsf.dll') {
                    $stale=-not $module.FileName.StartsWith($InstallDirectory+'\',[StringComparison]::OrdinalIgnoreCase)
                    Write-Host "  $($process.ProcessName) PID $($process.Id): $($module.FileName)  old-path=$stale"
                }
            }
        } catch { } finally { $process.Dispose() }
    }
    Write-Host 'Enumeration may omit protected or inaccessible processes. Sign out and in for a complete reload; unregistering alone does not unload DLLs.'
}
switch ($Action) {
    'Status' { Show-Status }
    'Stop' { Stop-Broker }
    'Install' {
        Test-Release
        # The registration helper checks elevation before any registration mutation.
        $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
        try { if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run Install in an administrator PowerShell under the same Windows account.' } } finally { $identity.Dispose() }
        Stop-Broker
        & (Join-Path $PSScriptRoot 'register.ps1') -InstallDirectory $InstallDirectory -X86DllPath (Join-Path $InstallDirectory 'x86/EuphonyTsf.dll')
        Update-ResourcePaths
        Write-Host 'Installation complete. Sign out and in now, then choose Euphony with Win+Space. Do not delete the old package until its DLLs unload.'
        Show-Status
    }
    'Uninstall' {
        $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
        try { if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run Uninstall in an administrator PowerShell under the same Windows account.' } } finally { $identity.Dispose() }
        Stop-Broker
        if ($installed) { & (Join-Path $PSScriptRoot 'unregister.ps1') }
        else { & (Join-Path $PSScriptRoot 'unregister.ps1') -InstallDirectory $InstallDirectory -X86DllPath (Join-Path $InstallDirectory 'x86/EuphonyTsf.dll') }
        Write-Host 'Sign out and in before deleting the old package. Configuration and learned vocabulary were retained.'
    }
    'Start' { Start-CurrentBroker }
    'Settings' {
        Test-Release
        $reply=Invoke-Broker 'get_config'
        $brokerFile=Join-Path $InstallDirectory 'EuphonyBroker.exe'
        $runningFile=if ($reply) { (Get-Process -Id $reply.pid -ErrorAction Stop).Path }
        if (-not $reply -or $reply.value.status.app_version -ne '0.2.0' -or $runningFile -ne $brokerFile) { Start-CurrentBroker }
        $file=Join-Path $InstallDirectory 'EuphonySettings.exe'
        if (-not (Test-Path -LiteralPath $file)) { throw "Package missing: $file" }
        Start-Process -FilePath $file -WorkingDirectory $InstallDirectory
    }
}
