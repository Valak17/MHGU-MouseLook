# Changelog

## 1.1.3 - 2026-10-24

- Swap the two physical mouse side buttons.
- Mouse4/X1 now activates gamepad L3 (Left Shift).
- Mouse5/X2 now activates gamepad R (keyboard C intermediary).

## 1.1.2 - 2026-10-24

- Remap gamepad A to mouse RMB and wheel-down.
- Remap gamepad Y to keyboard Q and mouse wheel-up.
- Remap gamepad R to a mouse side button through the keyboard C intermediary.
- Remap gamepad L3 to Left Shift and the other mouse side button.
- Remap gamepad ZL to keyboard R.
- Remap gamepad L to Left Ctrl (also mouse middle).
- Hold synthesized mouse-button keys for at least 100 ms so one-frame clicks
  are not missed by the game's polling loop.
- Queue wheel input and hold each wheel key for 100 ms.
- Replace `launch.bat` with a wrapper that disables the SDL touchscreen layer
  before every game start. The original launcher is backed up and restored by
  uninstall.

## 1.1.1 - 2026-10-23

- Restore the cursor-hiding method proven in v1.0.0 (`SetWindowLongPtrW` +
  `WM_SETCURSOR`).
- Install the cursor window hook only while MouseLook owns the gameplay camera.
- Remove it immediately on F8 off, menu/focus loss, and unload.
- Keep Forge one-click installation and touchscreen-pointer disable support.

## 1.1.0 - 2026-10-23

- Added a self-contained one-click installer for clean MHGU exports.
- Bundles Forge PC 0.7.0 with its licenses and menu font.
- Validates the supported executable SHA-256 before installation.
- Enables Forge in `game_settings.ini` automatically.
- Installs MouseLook and disables the emulator touchscreen pointer.
- Hides the Windows system cursor with a transparent cursor and a temporary
  `WM_SETCURSOR` guard active only during gameplay.
- Applies the recommended PC keyboard mapping by default.
- Adds backup and uninstall support.
