# Developer notes

MouseLookDirect creates no hook at startup.

After F8 finds a stable `uQuestCamera`, pre-hooks are installed at both
`main+0x1C33C` and `main+0x1C358`, matching the proven legacy implementation.
Their callbacks run before the camera functions and write the same target
vectors idempotently. `hook_enable` is never called.

The default vector mode uses:

```text
+0x40 eye
+0x50 up
+0x60 target
```

The optional angle mode uses `0xAE/0xEA/0xD2` for yaw and `0x196` for pitch
with 65536 units per turn.

The cursor is hidden with a temporary `WM_SETCURSOR` subclass and transparent
class cursor only while F8 capture is active. The original window procedure and
class cursor are restored on focus loss, toggle-off, and unload.
