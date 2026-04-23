# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A GStreamer 1.x source plugin (`norisrc`) for Nori Xvision USB cameras, plus a companion CLI (`nori-ctl`). The plugin wraps the proprietary Nori SDK to expose camera capture as a standard GStreamer element with MJPEG and YUY2 output and Nori-specific controls (trigger modes, sensor shutter/gain, mirror/flip, AE/AWB, per-camera identity tags) as GObject properties. `nori-ctl` covers out-of-band operations: device enumeration, role tagging, trigger/ESN/user-data, sensor shutter/gain, and a sectioned `state` dump.

Design notes for the AE state machine and the SDK-control concurrency fix live in [`docs/exposure-state-machine.md`](docs/exposure-state-machine.md). Release history in [`CHANGELOG.md`](CHANGELOG.md).

## Build commands

```bash
meson setup builddir          # configure (one-time)
ninja -C builddir             # build
meson setup builddir --wipe   # reconfigure after meson.build changes
```

## Testing the plugin

```bash
# Verify GStreamer discovers the element and all properties
GST_PLUGIN_PATH=builddir gst-inspect-1.0 norisrc

# Live preview (requires camera connected)
GST_PLUGIN_PATH=builddir gst-launch-1.0 norisrc ! jpegdec ! videoconvert ! autovideosink

# Debug output from the element
GST_DEBUG=norisrc:5 GST_PLUGIN_PATH=builddir gst-launch-1.0 norisrc ! fakesink

# Runtime exposure-state-machine harness (PyGObject; needs a camera)
GST_DEBUG=norisrc:5 GST_PLUGIN_PATH=builddir \
    python3 tests/test_exposure_state_machine.py 2>&1 | tee /tmp/exp_test.log
```

There are no unit tests. Verification is done by `gst-inspect-1.0` (plugin loads, properties registered), `gst-launch-1.0` with a real camera, and `tests/test_exposure_state_machine.py` for the AE state machine. The `nori-ctl state <idx>` subcommand is also useful for confirming what hit the device.

## Architecture

Two binaries built from one tree:

- **`libgstnori.so`** — the plugin. Sources in `src/`.
- **`nori-ctl`** — the CLI. Source in `tools/nori-ctl.cpp`.

Both link against `libNori_Xvision_Std.so` from `sdk/`.

### Plugin sources (`src/`)

- **`src/gstnori.cpp`** — Plugin entry point. `GST_PLUGIN_DEFINE` + `plugin_init` registers the single element `norisrc`. Version string comes from the meson-injected `VERSION` macro.
- **`src/gstnorisrc.h`** — Element struct `_GstNoriSrc`, class struct, GEnum types (`GstNoriTriggerMode`, `GstNoriMirrorFlip`), `GstNoriExposureState` enum (`UNKNOWN`/`AUTO`/`MANUAL`), `PROP_DIRTY_*` bitmask defines, and the `apply_lock` GMutex field.
- **`src/nori_tag.h`** — Shared NORICAM tag block format (encode/decode of role strings stored in user-data block 255). Used by both `norisrc` (for `role`-based selection in `start()`) and `nori-ctl tag`.
- **`src/gstnorisrc.cpp`** — All element logic, organized into `===`-banner sections:
  - **SDK global init** (`nori_sdk_ref`/`nori_sdk_unref`) -- Reference-counted init behind a static mutex. `Nori_Xvision_Init` is process-global, so multiple `norisrc` elements share one init.
  - **GEnum registrations** -- Thread-safe `g_once_init_enter` registration for property enums.
  - **Properties and pad template** -- 8 camera-control properties (`device-index`, `role`, `trigger-mode`, `mirror-flip`, `auto-exposure`, `auto-white-balance`, `sensor-shutter`, `sensor-gain`) + static caps template (MJPEG + YUY2).
  - **`class_init` / `init`** -- GObject boilerplate, vmethod wiring, live-source configuration, `apply_lock` init. `finalize` clears the mutex.
  - **`nori_set_pu` / `nori_get_pu`** -- Thin wrappers over `Nori_Xvision_{Set,Get}ProcessingUnitControl` with logging and a cached fallback.
  - **`nori_apply_exposure`** -- Drives the `AUTO`↔`MANUAL` state machine. On entry to `MANUAL`, flips `EXPOSURE_AUTO=1`, resets UVC exposure+gain registers to per-device defaults captured in `start()`, then writes `sensor-shutter`/`sensor-gain`. On entry to `AUTO`, just sets `EXPOSURE_AUTO=3`. Sensor writes attempted while in `AUTO` log a warning and do nothing.
  - **`nori_apply_controls`** -- Top-level apply: trigger, mirror-flip, AWB, then defers exposure/gain to `nori_apply_exposure`. Called from `set_caps` (initial) and `set_property` (runtime); both call sites hold `apply_lock`.
  - **`set_property` / `get_property`** -- Standard GObject property handlers. `set_property` holds `apply_lock` for the entire body (field write + dirty bit + apply). `get_property` reads live SDK state where supported (live PU, sensor shutter/gain, trigger mode, mirror/flip), falling back to cached values otherwise.
  - **`start`** -- Inits SDK, resolves `role` to a device index if set, validates index, enumerates supported formats into `self->video_infos`, and reads UVC exposure/gain defaults via `Nori_Xvision_GetProcessingUnitControl` for the MANUAL-entry baseline. Sets `exposure_applied = UNKNOWN`.
  - **`stop`** -- Stops video, uninits video, unrefs SDK, invalidates UVC defaults, resets `exposure_applied = UNKNOWN`.
  - **`get_caps`** -- Returns device-specific caps after `start()`, or template caps before.
  - **`set_caps`** -- Parses negotiated caps, calls `DeviceVideoInit`, force-applies trigger mode (the SDK latches it at `VideoStart` and the firmware persists it across reopens), then `VideoStart`. Forces `PROP_DIRTY_AUTO_EXP` and runs `nori_apply_controls` under `apply_lock` to drive the state machine's first transition into a known state.
  - **`create`** -- Polls `Nori_Xvision_GetFrameBuff` with 500 ms timeout in a loop, checking the `flushing` flag for clean shutdown. Copies frame into a new `GstBuffer`, frees SDK frame.
  - **`unlock` / `unlock_stop`** -- Sets/clears `flushing` flag so `create` can exit its poll loop.

### `nori-ctl` (`tools/nori-ctl.cpp`)

Single-file C++ CLI. RAII wrapper (`SdkGuard`) over `Nori_Xvision_Init`/`UnInit`. Subcommands map 1:1 to SDK calls plus the shared `nori_tag` codec for `tag` operations. The `state <idx>` subcommand is the most useful single command for verifying camera state — it dumps sensor (shutter/gain/black-level/mirror-flip), image (offset/lens correction), trigger, white balance, one-shot auto status, and all UVC PU controls in sectioned tables. The `shutter set` and `gain set` subcommands auto-flip the camera into manual exposure mode first (mirroring what `nori_apply_exposure` does), since the SDK requires it.

### Element lifecycle (GStreamer state transitions)

```
NULL -> READY:       (nothing)
READY -> PAUSED:     start() -> get_caps() -> fixate() -> set_caps()
PAUSED -> PLAYING:   create() called in a loop from basesrc thread
PLAYING -> PAUSED:   unlock() sets flushing, create() returns FLUSHING
PAUSED -> READY:     stop()
```

`start()` only inits the SDK and enumerates formats. Actual video streaming begins in `set_caps()` after caps negotiation. This split is intentional -- `get_caps()` needs format info from the device before the stream starts.

### SDK library

The Nori SDK is a closed-source shared library (`libNori_Xvision_Std.so`) in `sdk/lib/{arm32,arm64,x64,x86}/`. The meson build auto-selects the correct arch via `host_machine.cpu_family()`. Headers are in `sdk/include/Nori_Xvision_API/`.

## Key constraints

- **Must compile as C++** -- `Nori_Public.h` includes `<chrono>` and `<ctime>`, so all `.cpp` files use GStreamer's C API from C++.
- **No `volatile` on `gsize` for `g_once_init_enter`** -- GCC 11+ in C++ mode rejects `volatile gsize*` with `__atomic_load`. Use plain `static gsize`.
- **`PACKAGE` and `VERSION` macros required** -- `GST_PLUGIN_DEFINE` references `PACKAGE`, and the plugin's reported version uses `VERSION`. Both are defined in `meson.build` via `cpp_args`. To bump the version, edit only `meson.build` (`project(version: ...)` and the `-DVERSION=...` cpp_arg).
- **SDK enums must stay in sync** -- `GstNoriTriggerMode` values must match `E_TRIGGER_MODE`, `GstNoriMirrorFlip` must match `E_SENSOR_MIRROR_FLIP`. They are cast directly.
- **Properties only applied when dirty** -- The `props_dirty` bitmask tracks user-set properties. Unset properties never touch the device, preserving camera defaults. When adding a new property: add a `PROP_DIRTY_*` bit, set it in `set_property`, handle it in `nori_apply_controls` (or, if it interacts with AE, in `nori_apply_exposure`).
- **Hold `apply_lock` around all apply paths** -- The SDK's UVC control writes can block ~1 s. Both `set_caps` (state-change thread) and `set_property` (caller's thread) call `nori_apply_controls`; without serialisation they race through the AE state machine. Any new code that writes camera state from outside `nori_apply_controls` must take this lock.
- **Tag block format is shared with `nori-ctl`** -- The NORICAM tag codec lives in `src/nori_tag.h`. Don't duplicate; both binaries include the same header.

## Release / packaging

- Version lives in `meson.build` (two places: `project(version: ...)` and the `-DVERSION=...` cpp_arg).
- `CHANGELOG.md` documents each release. Add a new section at the top before tagging.
- `package.sh` reads the version from `meson.build` and produces `gst-nori_<ver>_<arch>.deb` containing the plugin (to the GStreamer plugin dir), `nori-ctl` (to `/usr/local/bin`), and `libNori_Xvision_Std.so` (to `/usr/local/lib`).
- After committing the version bump and CHANGELOG update, tag `vX.Y.Z` and push, then `gh release create vX.Y.Z gst-nori_<ver>_<arch>.deb --notes ...`. Past releases (v1.0.1, v1.1.0) follow this pattern.
