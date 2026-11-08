# Validation and installation status

## Completed

- Firmware 1.0.0 compiled for ESP32-S31 with the exact pinned ESP-IDF revision.
- Final System Update package built and inspected by the official Mosaico CLI.
  Package SHA-256: `07f0816bd4d4f5910dfc4755b31ab0dfe1f4f2d25a13a29216784eac014c5e2d`.
  Contains partition table, main application and UI resource image; no Vibe Mode,
  sysmeta, NVS or Core Dump image. Requires Vibe Mode >=0.2.0.
- Mac companion reads real signed-in Codex allowance, reset timestamp, active
  turn boundaries and explicit recent completion timestamps. Current desktop
  tasks were observed through their local rollouts without global config changes.
- Eight Python behavior tests passed: lifecycle vs notLoaded, completion while
  another task runs, expiry, waiting-input correlation, stale/malformed tails,
  weekly allowance, failure and manual stop semantics.
- Shared native C UI compiled and ran in GSP simulator. Actual face tap and back
  input, working/waiting/completed/failed/offline state changes, blink animation,
  first-frame title and blue-marker visibility were exercised. Images are under
  `design/simulator`; numbers/titles there are test data, not a physical device capture.

## Still pending: physical installation and acceptance

USB discovery sees `ESP-Iris Mosaico english_voice`, VID/PID 303A:4002,
`/dev/cu.usbmodem101`. New tools cannot classify its CDC control/data role and
reject it before the live handshake. This is compatible with an older interface,
but no live firmware version, Device ID or Boot ID has been established.

Official `recover --dry-run` explicitly requires manual ROM Download Mode:
power off, hold Boot (left of USB-C), power on, release Boot after entry.
The connected application is not ROM mode. There is no active write operation,
and no firmware or device data has been written by this task.

Next: detect ROM identity, apply the reviewed base through `mosaico.py recover`,
verify live Vibe Mode identity/version/ABI, install the prepared application through
`iris system-update`, then verify the same identity, new Boot ID, healthy state,
real screen capture, actual Codex data updates, touch and reconnect behavior.
Do not claim device completion from this build or the simulator alone.

Logs:
- Firmware build: vendor/esp-mosaico-vibe/projects/codex_pet/.codex-runs/idf-low-noise-build/20261101-235735-build-32117/raw.log
- Final bundle/failed device connection: vendor/esp-mosaico-vibe/.codex-runs/mosaico/20261101T160304Z-system-update/raw.log
- Recovery preflight: vendor/esp-mosaico-vibe/.codex-runs/mosaico/20261101T155429Z-recover/raw.log
