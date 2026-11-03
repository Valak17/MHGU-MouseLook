param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath,
    [bool]$ApplyKeymap = $true
)

$ErrorActionPreference = "Stop"

function Resolve-GameRoot([string]$Path) {
    $Path = $Path.Trim().Trim('"')
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
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
$directTarget = Join-Path $titleRoot "MouseLookDirect"
$payload = Join-Path $PSScriptRoot "payload"

if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Game EXE was not found in: $gameRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $gameRoot "exefs\main"))) {
    throw "This is not a supported recompiled MHGU folder: $gameRoot"
}
if (Get-Process -Name "MONSTER HUNTER GENERATIONS ULTIMATE" -ErrorAction SilentlyContinue) {
    throw "Close the game before installing MouseLookDirect."
}

$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $exePath).Hash
$acceptedHashes = @(
    "053CAE60DA1BFF123E391E5582A650C1B25E200A0F09A0DD265E321225F38FE5",
    "D7CC9B32187CA255EF9224BD3FDCF295BB3788D0474440434C11030650BCADCA"
)
if ($acceptedHashes -notcontains $actualHash) {
    throw "Unsupported game build. Found $actualHash"
}
$mainHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $gameRoot "exefs\main")).Hash
if ($mainHash -ne "B3935415499549ADAF4C0E89822831DAA175CFD456A7990791C79BE3997645EF") {
    throw "Unsupported game module. exefs\main hash is $mainHash"
}

New-Item -ItemType Directory -Force -Path $forgeTarget, $directTarget, (Join-Path $directTarget "plugins") | Out-Null
Backup-Once (Join-Path $forgeTarget "forge.ini") (Join-Path $forgeTarget "forge.ini.oneclick-backup")
Backup-Once $settingsPath "$settingsPath.oneclick-backup"
Backup-Once $sdlPath "$sdlPath.oneclick-backup"
Backup-Once (Join-Path $directTarget "mouselook_direct.ini") (Join-Path $directTarget "mouselook_direct.ini.oneclick-backup")
Backup-Once (Join-Path $gameRoot "launch.bat") (Join-Path $gameRoot "launch.bat.oneclick-backup")

# A legacy hook-based plugin must not remain in the Forge plugin scan path.
$legacy = Join-Path $titleRoot "MouseLook"
if (Test-Path -LiteralPath $legacy) {
    $legacyBackup = Join-Path $gameRoot ("MouseLook.legacy-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    Move-Item -LiteralPath $legacy -Destination $legacyBackup
    Write-Host "Legacy hook-based MouseLook moved to: $legacyBackup"
}

Copy-Item -Path (Join-Path $payload "Forge\*") -Destination $forgeTarget -Recurse -Force
Copy-Item -Path (Join-Path $payload "MouseLookDirect\*") -Destination $directTarget -Recurse -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "apply_pc_keymap.ps1") `
    -Destination (Join-Path $directTarget "apply_pc_keymap.ps1") -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "disable_touchscreen.ps1") `
    -Destination (Join-Path $directTarget "disable_touchscreen.ps1") -Force
Copy-Item -LiteralPath (Join-Path $payload "launch.bat") `
    -Destination (Join-Path $gameRoot "launch.bat") -Force

Set-IniValue $settingsPath "Mods" "loader" "Forge/forge.dll"
Set-IniValue $settingsPath "Dev" "order_probe" "false"
Set-IniValue $sdlPath "Controls" "mouse_enabled" "false" -Compact
Set-IniValue $sdlPath "Controls" "touchscreen_enabled" "false" -Compact
Set-IniValue $sdlPath "Controls" "keyboard_enabled" "true" -Compact

if ($ApplyKeymap) {
    & (Join-Path $PSScriptRoot "apply_pc_keymap.ps1") -GamePath $gameRoot
}

Write-Host ""
Write-Host "MouseLookDirect installation complete."
Write-Host "Start with: $gameRoot\launch.bat"
Write-Host "Enter a stable gameplay scene, wait 2 seconds, then press F6."
