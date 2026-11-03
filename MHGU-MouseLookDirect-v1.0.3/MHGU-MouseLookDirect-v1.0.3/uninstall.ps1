param(
    [string]$GamePath
)

$ErrorActionPreference = "Stop"
if (-not $GamePath) {
    $GamePath = Read-Host "Enter the MHGU game folder"
}
$GamePath = $GamePath.Trim().Trim('"')
if (Test-Path -LiteralPath $GamePath -PathType Leaf) {
    $GamePath = Split-Path -Parent $GamePath
}
$GamePath = (Resolve-Path -LiteralPath $GamePath).Path.TrimEnd('\')

if (Get-Process -Name "MONSTER HUNTER GENERATIONS ULTIMATE" -ErrorAction SilentlyContinue) {
    throw "Close the game before uninstalling MouseLookDirect."
}

$settingsPath = Join-Path $GamePath "game_settings.ini"
$sdlPath = Join-Path $GamePath "user\config\sdl2-config.ini"
$launchPath = Join-Path $GamePath "launch.bat"
$directTarget = Join-Path $GamePath "mods\0100770008DD8000\MouseLookDirect"
$removed = Join-Path $GamePath ("MouseLookDirect.removed-" + (Get-Date -Format "yyyyMMdd-HHmmss"))

if (Test-Path -LiteralPath $directTarget) {
    Move-Item -LiteralPath $directTarget -Destination $removed
}
if (Test-Path -LiteralPath "$sdlPath.oneclick-backup") {
    Copy-Item -LiteralPath "$sdlPath.oneclick-backup" -Destination $sdlPath -Force
}
if (Test-Path -LiteralPath "$launchPath.oneclick-backup") {
    Copy-Item -LiteralPath "$launchPath.oneclick-backup" -Destination $launchPath -Force
}
if (Test-Path -LiteralPath $settingsPath) {
    $lines = [Collections.Generic.List[string]]([IO.File]::ReadAllLines($settingsPath))
    $replace = [Regex]::Replace(
        ($lines -join "`r`n"),
        '(?m)^\s*loader\s*=.*$',
        'loader = none')
    [IO.File]::WriteAllText($settingsPath, $replace,
        (New-Object System.Text.UTF8Encoding($false)))
}

Write-Host "MouseLookDirect removed. Recoverable copy: $removed"
