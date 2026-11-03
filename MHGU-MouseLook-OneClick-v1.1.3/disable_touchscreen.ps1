param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath
)

$ErrorActionPreference = "Stop"
$GamePath = $GamePath.Trim('"')
$configPath = Join-Path $GamePath "user\config\sdl2-config.ini"
$backupPath = "$configPath.mouselook-pointer-backup"

if (-not (Test-Path -LiteralPath $configPath)) {
    throw "SDL config not found: $configPath"
}
if (-not (Test-Path -LiteralPath $backupPath)) {
    Copy-Item -LiteralPath $configPath -Destination $backupPath
}

$lines = [IO.File]::ReadAllLines($configPath)
$changed = 0
for ($i = 0; $i -lt $lines.Count; ++$i) {
    if ($lines[$i] -match '^\s*touchscreen_enabled\s*=') {
        $lines[$i] = "touchscreen_enabled=false"
        $changed++
    } elseif ($lines[$i] -match '^\s*mouse_enabled\s*=') {
        $lines[$i] = "mouse_enabled=false"
        $changed++
    }
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllLines($configPath, $lines, $utf8)
Write-Host "Disabled emulator touchscreen/mouse pointer ($changed entries)."
