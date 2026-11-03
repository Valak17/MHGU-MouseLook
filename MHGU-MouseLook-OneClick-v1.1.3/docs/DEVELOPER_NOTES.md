# Developer notes

The installer payload is layout-complete:

```text
mods\0100770008DD8000\Forge\forge.dll
mods\0100770008DD8000\Forge\forge.ini
mods\0100770008DD8000\Forge\fonts\
mods\0100770008DD8000\Forge\licenses\
mods\0100770008DD8000\MouseLook\plugins\mouselook.dll
mods\0100770008DD8000\MouseLook\mouselook.ini
```

The clean game must use the exact executable hash listed in README.md.
`install.ps1` enables `[Mods] loader = Forge/forge.dll`, disables
`mouse_enabled` and `touchscreen_enabled`, and applies the optional PC keymap.

MouseLook uses the renderer camera pre-hook, edits only the real
`uQuestCamera`, and is isolated from menus and save selection.

Pointer handling uses a transparent cursor plus an active-only
`SetWindowLongPtrW` window procedure for `WM_SETCURSOR`, the approach verified
in v1.0.0. The original procedure is restored on menu, focus loss, F8 off and
unload.
