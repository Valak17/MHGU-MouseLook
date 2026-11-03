param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath
)

$ErrorActionPreference = "Stop"
$GamePath = $GamePath.Trim().Trim('"')
if (Test-Path -LiteralPath $GamePath -PathType Leaf) {
    $GamePath = Split-Path -Parent $GamePath
}
$GamePath = (Resolve-Path -LiteralPath $GamePath).Path.TrimEnd('\')
$titleRoot = Join-Path $GamePath "mods\0100770008DD8000"
$mouseTarget = Join-Path $titleRoot "MouseLook"
$settingsPath = Join-Path $GamePath "game_settings.ini"
$sdlPath = Join-Path $GamePath "user\config\sdl2-config.ini"

if (Get-Process -Name "MONSTER HUNTER GENERATIONS ULTIMATE" -ErrorAction SilentlyContinue) {
    throw "Close the game before uninstalling MouseLook."
}

if (Test-Path -LiteralPath $mouseTarget) {
    $disabled = "$mouseTarget.disabled-" + (Get-Date -Format "yyyyMMdd-HHmmss")
    Move-Item -LiteralPath $mouseTarget -Destination $disabled
    Write-Host "MouseLook moved to: $disabled"
}
if (Test-Path -LiteralPath "$settingsPath.oneclick-backup") {
    Copy-Item -LiteralPath "$settingsPath.oneclick-backup" -Destination $settingsPath -Force
}
if (Test-Path -LiteralPath "$sdlPath.oneclick-backup") {
    Copy-Item -LiteralPath "$sdlPath.oneclick-backup" -Destination $sdlPath -Force
}
if (Test-Path -LiteralPath (Join-Path $GamePath "launch.bat.oneclick-backup")) {
    Copy-Item -LiteralPath (Join-Path $GamePath "launch.bat.oneclick-backup") `
        -Destination (Join-Path $GamePath "launch.bat") -Force
}
Write-Host "Original settings restored. Forge is left installed for other mods."
