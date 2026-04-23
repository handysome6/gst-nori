# Changelog

## v1.1.0 — 2026-04-23

Big release: a new CLI companion (`nori-ctl`), per-camera identity tagging,
a redesigned exposure-control surface backed by a state machine, and a
concurrency fix for the SDK control path.

### Breaking changes

- **`norisrc` property surface trimmed.** The UVC Processing Unit
  properties `brightness`, `contrast`, `saturation`, `sharpness`, `hue`,
  `gamma`, `gain`, and `exposure` have been removed. They duplicated the
  sensor-level controls and made the auto/manual story incoherent.
  Pipelines that set any of these will fail to launch with an
  "unknown property" error.
- **Manual exposure is now sensor-only.** Use `auto-exposure=false` plus
  `sensor-shutter` (microseconds) and `sensor-gain` (multiplier) for
  manual control. See `docs/exposure-state-machine.md`.

### New

- **`nori-ctl` command-line tool** for enumerating devices and operating
  on them out-of-band (`list`, `trigger get|set`, `esn get|set`,
  `udata get|set`, `tag get|set|clear`, `shutter get|set`,
  `gain get|set`, `state`).
- **Per-camera identity tags** persisted in user-data block 255. Use
  `nori-ctl tag set <idx> <role>` to assign, then select the camera in a
  pipeline with `norisrc role=<role>`. `role` takes precedence over
  `device-index` and survives power cycles.
- **Exposure state machine** (`auto-exposure` ↔ sensor manual). Entry
  into manual mode resets the UVC exposure and gain registers to each
  device's own defaults so every camera starts from an identical
  baseline before `sensor-shutter` / `sensor-gain` take over. Sensor
  writes while `auto-exposure=true` are explicitly logged as ignored
  rather than silently overridden by the firmware AE loop.
- **Live `get_property` reads.** Reading any property now returns the
  current SDK state (where supported) instead of the in-element cached
  value, so `gst-inspect`-style probes see the actual camera, not stale
  zeros.
- **`docs/exposure-state-machine.md`** documenting the design, the
  AUTO/MANUAL transition table, the UVC-default-reset rationale, and
  the concurrency story.
- **`tests/test_exposure_state_machine.py`** — PyGObject harness
  exercising eight runtime scenarios (defaults, transitions in both
  directions, sensor changes mid-stream, warn path, stop/restart, AE
  toggle).

### Fixes

- **Race in the SDK control path.** `Nori_Xvision_SetProcessingUnitControl`
  blocks for ~1.5 s; that left the state-change thread (`set_caps`) mid
  apply when an external `g_object_set` arrived from another thread,
  causing both threads to enter the AUTO→MANUAL transition with
  `exposure_applied = UNKNOWN` and double-apply the full sequence. A new
  `apply_lock` GMutex serialises `nori_apply_controls`.

### Misc

- Per-format enumeration log dropped from `start()` (DEBUG-only and
  noisy on cameras with many modes).

### Upgrade notes

- Audit pipelines for the removed UVC properties listed above.
- If you previously relied on `gain`/`exposure` for manual control,
  switch to `auto-exposure=false sensor-shutter=<µs> sensor-gain=<×>`.
- Re-tag any cameras that had an existing role string from a pre-1.1
  build of `nori-ctl`; the on-device format is stable but
  re-verification is cheap.

---

## v1.0.1 — 2026-04-20

- Apply trigger mode before `VideoStart` to prevent stale-mode lockup.
  Previously a pipeline reopened with `trigger-mode=none` after a prior
  hardware-trigger session would block forever in `GetFrameBuff` because
  the camera firmware persists trigger mode across close/reopen and the
  SDK latches it at `VideoStart`.

## v1.0.0 — Initial release

- GStreamer source plugin (`norisrc`) for Nori Xvision USB cameras with
  MJPEG and YUY2 output.
- `.deb` packaging via `package.sh`.
