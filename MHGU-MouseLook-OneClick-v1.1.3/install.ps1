param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath,
    [bool]$ApplyKeymap = $true
)

$ErrorActionPreference = "Stop"

function Resolve-GameRoot([string]$Path) {
    $Path = $Path.Trim().Trim('"')
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        if ([IO.Path]::GetExtension($Path) -ine ".exe") {
            throw "Select the game folder or MONSTER HUNTER GENERATIONS ULTIMATE.exe."
        }
        return Split-Path -Parent $Path
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Game path does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path.TrimEnd('\')
}

function Backup-Once([string]$Path, [string]$Backup) {
    if ((Test-Path -LiteralPath $Path) -and -not (Test-Path -LiteralPath $Backup)) {
        Copy-Item -LiteralPath $Path -Destination $Backup
    }
}

function Set-IniValue(
    [string]$Path,
    [string]$Section,
    [string]$Key,
    [string]$Value,
    [switch]$Compact
) {
    $lines = [Collections.Generic.List[string]]([IO.File]::ReadAllLines($Path))
    $sectionIndex = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i].Trim() -ieq "[$Section]") {
            $sectionIndex = $i
            break
        }
    }
    if ($sectionIndex -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne "") {
            $lines.Add("")
        }
        $lines.Add("[$Section]")
        $lines.Add($(if ($Compact) { "$Key=$Value" } else { "$Key = $Value" }))
    } else {
        $sectionEnd = $lines.Count
        for ($i = $sectionIndex + 1; $i -lt $lines.Count; ++$i) {
            if ($lines[$i].Trim().StartsWith("[")) {
                $sectionEnd = $i
                break
            }
        }
        $pattern = '^\s*' + [Regex]::Escape($Key) + '\s*='
        $found = $false
        for ($i = $sectionIndex + 1; $i -lt $sectionEnd; ++$i) {
            if ($lines[$i] -match $pattern) {
                $lines[$i] = $(if ($Compact) { "$Key=$Value" } else { "$Key = $Value" })
                $found = $true
                break
            }
        }
        if (-not $found) {
            $lines.Insert($sectionEnd,
                $(if ($Compact) { "$Key=$Value" } else { "$Key = $Value" }))
        }
    }
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllLines($Path, $lines, $utf8)
}

$gameRoot = Resolve-GameRoot $GamePath
$exePath = Join-Path $gameRoot "MONSTER HUNTER GENERATIONS ULTIMATE.exe"
$settingsPath = Join-Path $gameRoot "game_settings.ini"
$sdlPath = Join-Path $gameRoot "user\config\sdl2-config.ini"
$titleRoot = Join-Path $gameRoot "mods\0100770008DD8000"
$forgeTarget = Join-Path $titleRoot "Forge"
$mouseTarget = Join-Path $titleRoot "MouseLook"
$payload = Join-Path $PSScriptRoot "payload"

if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Game EXE was not found in: $gameRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $gameRoot "exefs\main"))) {
    throw "This is not a supported recompiled MHGU folder: $gameRoot"
}

$running = Get-Process -Name "MONSTER HUNTER GENERATIONS ULTIMATE" -ErrorAction SilentlyContinue
if ($running) {
    throw "Close the game before installing MouseLook."
}

$expectedHash = "053CAE60DA1BFF123E391E5582A650C1B25E200A0F09A0DD265E321225F38FE5"
$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $exePath).Hash
if ($actualHash -ne $expectedHash) {
    throw "Unsupported game build. Expected $expectedHash but found $actualHash"
}

New-Item -ItemType Directory -Force -Path $forgeTarget, $mouseTarget, (Join-Path $mouseTarget "plugins") | Out-Null

Backup-Once (Join-Path $forgeTarget "forge.ini") (Join-Path $forgeTarget "forge.ini.oneclick-backup")
Backup-Once $settingsPath "$settingsPath.oneclick-backup"
Backup-Once $sdlPath "$sdlPath.oneclick-backup"
Backup-Once (Join-Path $mouseTarget "mouselook.ini") (Join-Path $mouseTarget "mouselook.ini.oneclick-backup")
Backup-Once (Join-Path $gameRoot "launch.bat") (Join-Path $gameRoot "launch.bat.oneclick-backup")

Copy-Item -Path (Join-Path $payload "Forge\*") -Destination $forgeTarget -Recurse -Force
Copy-Item -Path (Join-Path $payload "MouseLook\*") -Destination $mouseTarget -Recurse -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "disable_touchscreen.ps1") `
    -Destination (Join-Path $mouseTarget "disable_touchscreen.ps1") -Force
Copy-Item -LiteralPath (Join-Path $payload "launch.bat") `
    -Destination (Join-Path $gameRoot "launch.bat") -Force

Set-IniValue $settingsPath "Mods" "loader" "Forge/forge.dll"
Set-IniValue $sdlPath "Controls" "mouse_enabled" "false" -Compact
Set-IniValue $sdlPath "Controls" "touchscreen_enabled" "false" -Compact

if ($ApplyKeymap) {
    & (Join-Path $PSScriptRoot "apply_pc_keymap.ps1") -GamePath $gameRoot
}

Write-Host ""
Write-Host "Installation complete."
Write-Host "Game: $gameRoot"
Write-Host "Forge: $forgeTarget"
Write-Host "MouseLook: $mouseTarget"
Write-Host "Forge loader enabled. Touchscreen pointer disabled."
if ($ApplyKeymap) {
    Write-Host "Recommended PC keyboard mapping applied."
} else {
    Write-Host "Existing keyboard mapping preserved."
}
Write-Host "Start the game and enter an actual gameplay scene. Press F8 if needed."
