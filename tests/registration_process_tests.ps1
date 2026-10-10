param([string]$ToolsDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $ToolsDirectory) { $ToolsDirectory = Join-Path $root 'tools' }
. (Join-Path $ToolsDirectory 'native_registration.ps1')
$unicodeName = ([char]0x6a21).ToString() + ([char]0x578b).ToString()
$output = Join-Path $root ('build/test-results/temporary-registration/GUI process ' + $unicodeName + '/' + [Guid]::NewGuid().ToString('N'))
[void][System.IO.Directory]::CreateDirectory($output)
$executable = Join-Path $output 'test registrar.exe'
# Compile a real GUI-subsystem process under Windows PowerShell 5.1. It waits
# before exiting and records its arguments, without loading or registering DLLs.
Add-Type -OutputAssembly $executable -OutputType WindowsApplication -TypeDefinition @'
using System;
using System.IO;
using System.Threading;
public static class RegistrationFixture {
    public static int Main(string[] args) {
        string path = args[args.Length - 1];
        Thread.Sleep(250);
        File.WriteAllLines(path + ".args", args);
        return Path.GetFileNameWithoutExtension(path) == "failure" ? 5 : 0;
    }
}
'@
foreach ($name in @('success', 'failure')) {
    $dll = Join-Path $output ($name + '.dll')
    [System.IO.File]::WriteAllText($dll, '')
    $global:LASTEXITCODE = $null
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $result = Invoke-EuphonyRegistrationProcess -ExecutablePath $executable -DllPath $dll -Unregister
    $timer.Stop()
    $expected = if ($name -eq 'success') { 0 } else { 5 }
    if ($result -ne $expected) { throw "Expected GUI exit $expected; obtained $result." }
    if ($timer.ElapsedMilliseconds -lt 200) { throw 'Registration helper did not wait for the GUI process.' }
    $arguments = [System.IO.File]::ReadAllLines($dll + '.args')
    if ($arguments.Length -ne 3 -or $arguments[0] -ne '/s' -or $arguments[1] -ne '/u' -or $arguments[2] -ne $dll) {
        throw 'Registration arguments were lost or split at spaces / Unicode characters.'
    }
}
foreach ($name in @('manage.ps1', 'register.ps1', 'unregister.ps1', 'native_registration.ps1')) {
    $tokens = $null; $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $ToolsDirectory $name), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "PowerShell parser errors in $name : $parseErrors" }
}
Write-Host 'GUI registration process: waited correctly, exit 0/5 captured, spaced Unicode paths preserved; scripts parse successfully.'
$cleanupRoot=[IO.Path]::GetFullPath((Join-Path $root 'build/test-results/temporary-registration'))
$cleanupPath=[IO.Path]::GetFullPath($output)
if (-not $cleanupPath.StartsWith($cleanupRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid test cleanup path.' }
Remove-Item -LiteralPath $cleanupPath -Recurse -Force
