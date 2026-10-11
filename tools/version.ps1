# Installed packages carry VERSION; source checkouts use the pixi manifest.
$EuphonyRoot=Split-Path $PSScriptRoot -Parent
$versionFile=Join-Path $EuphonyRoot 'VERSION'
if (Test-Path -LiteralPath $versionFile) {
    $EuphonyVersion=[IO.File]::ReadAllText($versionFile).Trim()
} else {
    $manifest=[IO.File]::ReadAllText((Join-Path $EuphonyRoot 'pixi.toml'))
    $match=[regex]::Match($manifest,'(?m)^version\s*=\s*"(\d+\.\d+\.\d+)"')
    if (-not $match.Success) { throw 'Cannot read Euphony workspace version.' }
    $EuphonyVersion=$match.Groups[1].Value
}
$EuphonyPackageDirectory=Join-Path $EuphonyRoot "dist/Euphony-$EuphonyVersion-windows-x64"
