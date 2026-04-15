# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A GStreamer 1.x source plugin (`norisrc`) for Nori Xvision USB cameras. It wraps the proprietary Nori SDK to expose camera capture as a standard GStreamer element with MJPEG and YUY2 output, plus Nori-specific controls (trigger modes, sensor shutter/gain, ISP parameters) as GObject properties.

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
```

There are no unit tests. Verification is done by `gst-inspect-1.0` (plugin loads, properties registered) and `gst-launch-1.0` with a real camera.

## Architecture

The plugin has three source files and one external dependency (the Nori SDK):

- **`src/gstnori.cpp`** -- Plugin entry point. `GST_PLUGIN_DEFINE` + `plugin_init` registers the single element `norisrc`.
- **`src/gstnorisrc.h`** -- Element struct `_GstNoriSrc`, class struct, GEnum types (`GstNoriTriggerMode`, `GstNoriMirrorFlip`), and `PROP_DIRTY_*` bitmask defines.
- **`src/gstnorisrc.cpp`** -- All element logic. Organized into these sections (marked by `===` comment banners):
  - **SDK global init** (`nori_sdk_ref`/`nori_sdk_unref`) -- Reference-counted init behind a static mutex. `Nori_Xvision_Init` is process-global, so multiple `norisrc` elements share one init.
  - **GEnum registrations** -- Thread-safe `g_once_init_enter` registration for property enums.
  - **Properties and pad template** -- 14 camera-control properties + static caps template (MJPEG + YUY2).
  - **`class_init` / `init`** -- GObject boilerplate, vmethod wiring, live-source configuration.
  - **`nori_apply_controls`** -- Applies only dirty-flagged properties to the SDK. Called from `set_caps` (initial application) and `set_property` (runtime changes while streaming).
  - **`set_property` / `get_property`** -- Standard GObject property handlers. `set_property` sets the dirty bit and calls `nori_apply_controls` if already streaming.
  - **`start`** -- Inits SDK, validates device index, enumerates supported formats into `self->video_infos` (used by `get_caps`).
  - **`stop`** -- Stops video, uninits video, unrefs SDK.
  - **`get_caps`** -- Returns device-specific caps after `start()`, or template caps before.
  - **`set_caps`** -- Parses negotiated caps, calls `DeviceVideoInit` + `VideoStart`, applies queued properties.
  - **`create`** -- Polls `Nori_Xvision_GetFrameBuff` with 500ms timeout in a loop, checking the `flushing` flag for clean shutdown. Copies frame into a new `GstBuffer`, frees SDK frame.
  - **`unlock` / `unlock_stop`** -- Sets/clears `flushing` flag so `create` can exit its poll loop.

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
- **`PACKAGE` macro required** -- `GST_PLUGIN_DEFINE` expands to reference `PACKAGE`. Defined via `-DPACKAGE="gst-nori"` in `cpp_args`.
- **SDK enums must stay in sync** -- `GstNoriTriggerMode` values must match `E_TRIGGER_MODE`, `GstNoriMirrorFlip` must match `E_SENSOR_MIRROR_FLIP`. They are cast directly.
- **Properties only applied when dirty** -- The `props_dirty` bitmask tracks user-set properties. Unset properties never touch the device, preserving camera defaults. When adding a new property: add a `PROP_DIRTY_*` bit, set it in `set_property`, handle it in `nori_apply_controls`.
