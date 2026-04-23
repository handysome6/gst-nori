# Exposure / gain control in `norisrc`

This document explains why `norisrc` exposes only a handful of camera-control
properties, how they map onto the Nori SDK, and the design pattern used to
keep auto-exposure and manual sensor control coherent — including the
concurrency bug the design uncovered and how it is solved.

## TL;DR

- `norisrc` exposes one mode knob (`auto-exposure`) and two manual values
  (`sensor-shutter`, `sensor-gain`). The UVC `exposure`/`gain` Processing
  Unit controls are deliberately **not** exposed.
- A two-state machine (`AUTO` ↔ `MANUAL`) drives all writes. Entry into
  `MANUAL` resets the UVC exposure and gain registers to **the camera's own
  defaults** so every device starts from an identical baseline before
  `sensor-shutter` / `sensor-gain` take over.
- All control writes are serialised by `apply_lock`. The Nori SDK's PU
  control writes can block for >1 s, which is long enough for the
  state-change thread (`set_caps`) and the property-setter thread to race
  through the state machine and double-apply transitions.

## Background: two control surfaces, one register

The Nori SDK exposes the same physical sensor-integration counter through
two layers:

| Layer | Path | Unit | Driven by |
|---|---|---|---|
| UVC Processing Unit | `Nori_Xvision_SetProcessingUnitControl(V4L2_CID_EXPOSURE_ABSOLUTE)` and `…(V4L2_CID_GAIN)` | 100 µs / normalised | UVC firmware (and its AE loop) |
| Direct sensor | `Nori_Xvision_SetSensorShutter()` / `…SetSensorGain()` | 1 µs / multiplier | Sensor register |

Both layers ultimately program the same sensor register, but the UVC layer
is mediated by firmware and is what the camera's auto-exposure loop drives
when `V4L2_CID_EXPOSURE_AUTO` is on. There is no separate "auto gain" knob
in the Nori SDK; the AE loop owns both integration time and gain together.

## Property surface

`norisrc` deliberately keeps only the knobs needed for the two real use
cases — "let the camera figure it out" and "give me deterministic, per-µs
control for triggered/sync work":

| Property | Type | Effect |
|---|---|---|
| `auto-exposure` | `bool` | Mode switch. `true`: firmware AE owns shutter+gain. `false`: enter MANUAL state machine. |
| `auto-white-balance` | `bool` | Independent UVC AWB toggle. |
| `sensor-shutter` | `uint` (µs) | Manual shutter. Applied only in MANUAL mode; logs a warning otherwise. |
| `sensor-gain` | `uint` (×) | Manual analog-gain multiplier. Same MANUAL-only rule. |
| `trigger-mode` | enum | Free-run / software / hardware / command trigger. |
| `mirror-flip` | enum | Sensor mirror / flip. |
| `device-index`, `role` | identification | Pick which camera. |

Brightness, contrast, saturation, sharpness, hue, gamma, plus the
UVC-layer `exposure`/`gain` were intentionally removed. They duplicate the
sensor-layer controls (for exposure/gain) or are not relevant to the
target use cases. `nori-ctl` still covers them out-of-band when needed.

## The exposure state machine

The coupled trio (`auto-exposure`, `sensor-shutter`, `sensor-gain`) is
modelled as a two-state machine. Transitions are owned by one helper
(`nori_apply_exposure`), and the hardware-applied state is tracked in
`exposure_applied` independently of what the user has asked for, so
transitions are idempotent and safe across pipeline restarts.

### States

| State | UVC `EXPOSURE_AUTO` | UVC exposure register | UVC gain register | Sensor shutter | Sensor gain |
|---|---|---|---|---|---|
| `AUTO`    | `3` (aperture-priority) | driven by firmware AE | driven by firmware AE | driven by firmware AE | driven by firmware AE |
| `MANUAL`  | `1` (manual) | **device default** | **device default** | `sensor-shutter` | `sensor-gain` |
| `UNKNOWN` | (unspecified) | (unspecified) | (unspecified) | (unspecified) | (unspecified) |

`UNKNOWN` is the initial state and the post-`stop()` state. Because any
desired state differs from `UNKNOWN`, the first apply after a fresh
`start()` always runs a full transition, regardless of whether the user
has touched `auto-exposure`. This is what makes default pipelines (no
properties set) end up in a known state.

### Why reset UVC to device defaults on AUTO → MANUAL?

When `EXPOSURE_AUTO` flips from auto to manual, the camera firmware
"latches" whatever the UVC `EXPOSURE_ABSOLUTE` and `GAIN` registers
currently hold. Different cameras (and the same camera after different
prior runs) hold different latched values, producing inconsistent visual
behaviour at the exact moment manual mode is entered — even before the
sensor-level writes land.

Resetting both UVC registers to the camera's own factory default (read via
`Nori_Xvision_GetProcessingUnitControl(...)` once in `start()`) gives every
camera an identical, reproducible baseline before `sensor-shutter` and
`sensor-gain` take over. We use the camera's own reported default because:

- `0` is unsafe — different firmware clamps to min/max or rejects it.
- A hard-coded value would not be portable across camera models.
- The reported default is the only value guaranteed to be valid.

### Transition table

| From → To | SDK calls (in order) | When |
|---|---|---|
| `* → AUTO`    | `EXPOSURE_AUTO=3` | `auto-exposure=true` written; first apply after `set_caps` if default is `true` |
| `* → MANUAL`  | `EXPOSURE_AUTO=1` → `EXPOSURE_ABSOLUTE=default` → `GAIN=default` → `SetSensorShutter(value)` → `SetSensorGain(value)` | `auto-exposure=false` written; first apply after `set_caps` if user set it `false` before play |
| `MANUAL → MANUAL` (sensor value change) | only the dirty sensor write(s) | `sensor-shutter` or `sensor-gain` written while already in MANUAL |
| `AUTO → AUTO` (sensor value change) | nothing; logs a warning | `sensor-shutter`/`sensor-gain` written while in AUTO |

The "warn but don't apply" path is the visible footgun: a user who
forgets to disable AE before writing `sensor-shutter` will see exactly
what is happening instead of the firmware silently overriding their
value frame-to-frame.

## Concurrency: `apply_lock`

`nori_apply_controls` and `nori_apply_exposure` are reachable from two
threads:

- the **state-change thread**, when `set_caps` calls
  `nori_apply_controls(self)` after `VideoStart`;
- the **arbitrary thread** that called `g_object_set` (`set_property`),
  which also calls `nori_apply_controls(self)` while streaming.

The Nori SDK's `Nori_Xvision_SetProcessingUnitControl` is **blocking and
slow** — empirically ~1.5 s per call on the test hardware. A typical
AUTO → MANUAL transition issues three UVC writes plus two sensor writes,
so the state-change thread can be ~5 s deep inside the apply when an
external `g_object_set` arrives.

Without serialisation, both threads enter `nori_apply_exposure`
concurrently, both read `exposure_applied = UNKNOWN`, both decide it is a
mode change, both run the full MANUAL entry sequence — producing duplicate
`EXPOSURE_AUTO`, `EXPOSURE_ABSOLUTE`, and `GAIN` writes. This was visible
in the test harness as paired log lines on different thread IDs.

The fix is a per-element `GMutex apply_lock` held over:

- the entire `set_property` body (field write, dirty bit, and apply call
  are atomic w.r.t. other threads);
- the first-apply at the tail of `set_caps` (forced
  `PROP_DIRTY_AUTO_EXP` plus `nori_apply_controls`).

The lock is initialised in `gst_nori_src_init` and cleared in
`gst_nori_src_finalize`. It does not nest with any GstObject/baseSrc lock
that we are aware of, so adding it does not introduce a deadlock risk.

## Lifecycle and where state is set / cleared

| Stage | What happens |
|---|---|
| `init` | Defaults set, `exposure_applied = UNKNOWN`, `apply_lock` initialised. |
| `start` | SDK opened, device chosen, formats enumerated, **UVC defaults read** for this device, `exposure_applied = UNKNOWN`. |
| `set_caps` (first time) | `VideoStart` → `PROP_DIRTY_AUTO_EXP` is forced → `nori_apply_controls` runs the first transition under `apply_lock`. |
| `set_property` (while streaming) | Field updated, dirty bit set, `nori_apply_controls` invoked under `apply_lock`. |
| `stop` | Video stopped, SDK released, `uvc_defaults_valid = FALSE`, `exposure_applied = UNKNOWN`. A subsequent `start` (possibly on a different camera) re-reads defaults and runs a fresh transition. |
| `finalize` | `apply_lock` cleared. |

## What this design intentionally does *not* try to handle

- **One-shot auto AE/AWB/AF**: the SDK exposes
  `Nori_Xvision_SetSingleAuto*` triggers, but they are actions, not state.
  `nori-ctl` covers the CLI side; if a downstream app needs them inside
  the pipeline, GObject action signals (not properties) are the right
  shape.
- **Manual UVC exposure/gain**: deliberately removed. If you want
  microsecond-precise manual control you use `sensor-shutter` /
  `sensor-gain`; if you want auto, you use `auto-exposure=true`.
- **Exposing the UVC defaults as readable properties**: they are an
  implementation detail of the MANUAL entry sequence. They are logged at
  `INFO` level when read in `start()`; that is enough to diagnose
  cross-camera differences.

## Verifying changes

Use the throwaway test harness at `/tmp/test_norisrc_exposure.py` (created
during development) for a quick re-verification of all eight scenarios. It
exercises:

1. default AUTO start
2. default MANUAL start with sensor values
3. runtime AUTO → MANUAL
4. runtime MANUAL → AUTO
5. stay MANUAL, change sensor values mid-stream
6. sensor-write while AUTO (warn path)
7. stop / restart re-reads UVC defaults
8. AE toggle on → off → on → off

Run it with:

```bash
GST_DEBUG=norisrc:5 GST_PLUGIN_PATH=builddir \
    python3 /tmp/test_norisrc_exposure.py 2>&1 | tee /tmp/exp_test.log
```

For each scenario the log should show one — and only one —
`Set auto-exposure=…` line per real mode transition, plus the UVC default
resets only on `MANUAL` entry.
