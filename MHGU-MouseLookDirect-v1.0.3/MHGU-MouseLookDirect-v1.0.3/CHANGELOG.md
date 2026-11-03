# Changelog

## 1.0.3 - 2026-10-27

- Restore the proven legacy mouse action mappings: LMB=Z, RMB/wheel-down=E,
  wheel-up=Q, MMB=Left Ctrl, Mouse4=Left Shift, Mouse5=C.
- Hold synthesized action keys and wheel keys for at least 100 ms so game
  polling does not miss short clicks.
- Use F8 as the MouseLook toggle and force the emulator `order_probe` off.

## 1.0.2 - 2026-10-27

- Restore both camera call hooks used by the proven legacy implementation:
  `main+0x1C33C` and `main+0x1C358`.
- Keep both hooks deferred until F6 finds a stable `uQuestCamera`, so title and
  main-menu startup still perform zero camera work and create no hooks.

## 1.0.1 - 2026-10-27

- Keep startup completely hook-free, then install one camera write hook only
  after F6 finds a stable `uQuestCamera`.
- Use the verified pre-camera vector writer by default so mouse input affects
  the rendered camera instead of feeding back into character orientation.
- Temporarily subclass `WM_SETCURSOR` and replace the class cursor while active,
  restoring both immediately on F6 off, focus loss, or unload.

## 1.0.0 - 2026-10-27

- First independent MouseLookDirect implementation.
- Locate `uQuestCamera` through the `sFestaCamera` singleton.
- Bundle Forge PC 0.7.0 and a clean one-click installer.
