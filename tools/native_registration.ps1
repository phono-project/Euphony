# regsvr32 is a GUI executable. Do not use a direct invocation followed by
# $LASTEXITCODE: some PowerShell hosts return before it exits and leave that
# variable null (or containing a previous command's result).
function Invoke-EuphonyRegistrationProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ExecutablePath,
        [Parameter(Mandatory = $true)][string]$DllPath,
        [switch]$Unregister
    )
    $nativeArguments = @('/s')
    if ($Unregister) { $nativeArguments += '/u' }
    # Start-Process joins ArgumentList into a command line; quote the DLL path
    # explicitly so spaces remain part of a single argument.
    $nativeArguments += '"' + $DllPath + '"'
    $process = Start-Process -FilePath $ExecutablePath -ArgumentList $nativeArguments -WindowStyle Hidden -Wait -PassThru -ErrorAction Stop
    try {
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        if ($null -eq $exitCode) {
            throw 'The registration process exited, but its exit code could not be obtained.'
        }
        return [int]$exitCode
    } finally { $process.Dispose() }
}
