# Remove obsolete generated outputs only; retain anything still registered,
# referenced by daily configuration, or loaded in a running process.
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$references=[Collections.Generic.List[string]]::new()
foreach ($process in Get-Process) {
    try {
        if ($process.Path) { $references.Add($process.Path) }
        foreach ($module in $process.Modules) { if ($module.ModuleName -ieq 'EuphonyTsf.dll' -or $module.ModuleName -ieq 'phono_core.dll') { $references.Add($module.FileName) } }
    } catch { } finally { $process.Dispose() }
}
foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)) {
    $hive=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
    try {
        $key=$hive.OpenSubKey('Software\Classes\CLSID\{1E7BDC4A-9862-43C6-AA16-8A40D96614E1}\InprocServer32')
        try { if ($key -and $key.GetValue('')) { $references.Add([string]$key.GetValue('')) } } finally { if ($key) { $key.Dispose() } }
    } finally { $hive.Dispose() }
}
$preferences=Join-Path $env:LOCALAPPDATA 'Euphony/config.json'
if (Test-Path -LiteralPath $preferences) {
    $config=[IO.File]::ReadAllText($preferences)|ConvertFrom-Json
    foreach ($name in @('core_library','dictionary_path','language_model_path','model_directory')) { if ($config.$name) { $references.Add([string]$config.$name) } }
}
$targets=@('build-release','build-release-x86','build-x86','tools/__pycache__',
    'build/test-results/smoke-package.json','build/test-results/temporary-registration','build/test-results/temporary-smoke','build/test-results/temporary-tray',
    'dist/Euphony-0.1.0-win64.zip','dist/Euphony-0.1.1-win64.zip','dist/Euphony','dist/Euphony-0.1.1')
$build=Join-Path $root 'build'
if (Test-Path -LiteralPath $build) {
    foreach ($item in Get-ChildItem -LiteralPath $build -Force) {
        if ($item.Name -notin @('x64','x86','test-results')) { $targets += 'build/'+$item.Name }
    }
}
foreach ($relative in $targets) {
    $path=[IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $path.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe cleanup path: $path" }
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $held=@($references | Where-Object { $_ -eq $path -or $_.StartsWith($path+'\',[StringComparison]::OrdinalIgnoreCase) })
    if ($held.Count) { Write-Host "Retained in-use output: $relative"; continue }
    try { Remove-Item -LiteralPath $path -Recurse -Force; Write-Host "Removed: $relative" }
    catch { Write-Warning "Retained busy output $relative : $($_.Exception.Message)" }
}
Write-Host 'Cleanup complete. Current build/x64, build/x86, release, data, dependencies and user files were retained.'
