param(
    [string]$GamePath,
    [switch]$Revert
)

$ErrorActionPreference = "Stop"

if (-not $GamePath) {
    $GamePath = Read-Host "Enter the MHGU game folder containing the game EXE"
}
$GamePath = $GamePath.Trim('"')
if (Test-Path -LiteralPath $GamePath -PathType Leaf) {
    $GamePath = Split-Path -Parent $GamePath
}

$configPath = Join-Path $GamePath "user\config\sdl2-config.ini"
$backupPath = "$configPath.mouselook-backup"

if (-not (Test-Path -LiteralPath $configPath)) {
    throw "SDL config not found: $configPath"
}

if ($Revert) {
    if (-not (Test-Path -LiteralPath $backupPath)) {
        throw "No MouseLook keymap backup exists: $backupPath"
    }
    Copy-Item -LiteralPath $backupPath -Destination $configPath -Force
    Write-Host "Restored original controller/keyboard mapping."
    exit 0
}

if (-not (Test-Path -LiteralPath $backupPath)) {
    Copy-Item -LiteralPath $configPath -Destination $backupPath
}

# SDL scancodes:
# WASD 26/4/22/7, E 8, Space 44, Z 29, X 27, F 9, Q 20, R 21,
# Escape 41, M 16, digits 1..4 30..33, LShift 225.
$replacements = [ordered]@{
    "player_0_button_a"      = '"engine:keyboard,code:8,toggle:0"'
    "player_0_button_b"      = '"engine:keyboard,code:44,toggle:0"'
    "player_0_button_x"      = '"engine:keyboard,code:29,toggle:0"'
    "player_0_button_y"      = '"engine:keyboard,code:20,toggle:0"'
    "player_0_button_lstick" = '"engine:keyboard,code:225,toggle:0"'
    "player_0_button_rstick" = '"engine:keyboard,code:10,toggle:0"'
    "player_0_button_l"      = '"engine:keyboard,code:224,toggle:0"'
    "player_0_button_r"      = '"engine:keyboard,code:6,toggle:0"'
    "player_0_button_zl"     = '"engine:keyboard,code:21,toggle:0"'
    "player_0_button_zr"     = '"engine:keyboard,code:21,toggle:0"'
    "player_0_button_plus"   = '"engine:keyboard,code:41,toggle:0"'
    "player_0_button_minus"  = '"engine:keyboard,code:16,toggle:0"'
    "player_0_button_dleft"  = '"engine:keyboard,code:32,toggle:0"'
    "player_0_button_dup"    = '"engine:keyboard,code:30,toggle:0"'
    "player_0_button_dright" = '"engine:keyboard,code:33,toggle:0"'
    "player_0_button_ddown"  = '"engine:keyboard,code:31,toggle:0"'
    "touchscreen_enabled"    = 'false'
    "mouse_enabled"          = 'false'
    "keyboard_enabled"       = 'true'
    "controls_set_by"        = 'player'
    "player_0_lstick"        = '"engine:analog_from_button,up:engine$0keyboard$1code$026$1toggle$00,down:engine$0keyboard$1code$022$1toggle$00,left:engine$0keyboard$1code$004$1toggle$00,right:engine$0keyboard$1code$007$1toggle$00,modifier:engine$0keyboard$1code$00$1toggle$00,modifier_scale:0.500000"'
    "player_0_rstick"        = '"engine:analog_from_button,up:engine$0keyboard$1code$012$1toggle$00,down:engine$0keyboard$1code$014$1toggle$00,left:engine$0keyboard$1code$013$1toggle$00,right:engine$0keyboard$1code$015$1toggle$00,modifier:engine$0keyboard$1code$00$1toggle$00,modifier_scale:0.500000"'
}

$lines = [IO.File]::ReadAllLines($configPath)
$changed = 0
for ($i = 0; $i -lt $lines.Count; ++$i) {
    $line = $lines[$i]
    foreach ($key in $replacements.Keys) {
        $pattern = '^\s*' + [Regex]::Escape($key) + '\s*='
        if ($line -match $pattern) {
            $lines[$i] = "$key=$($replacements[$key])"
            $changed++
            break
        }
    }
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllLines($configPath, $lines, $utf8)
Write-Host "Applied MouseLook PC key preset to player 0 ($changed entries)."
Write-Host "Backup: $backupPath"
Write-Host "Revert with: powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Revert"
