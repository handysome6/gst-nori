# gst-nori

A GStreamer source element (`norisrc`) for **Nori Xvision USB cameras**, built
on top of the Nori Xvision SDK (V10.00.01). Works as a drop-in replacement for
`v4l2src` while exposing Nori-specific controls (trigger modes, sensor
shutter/gain, mirror/flip, AE/AWB, per-camera identity tags) as GObject
properties that can be set on the command line or changed at runtime.

The repo also ships **`nori-ctl`**, a command-line companion for enumerating
devices, tagging them with role names (so pipelines can pick a camera by
purpose instead of bus order), and reading or poking SDK state out-of-band.

See [`CHANGELOG.md`](CHANGELOG.md) for what's new and
[`docs/exposure-state-machine.md`](docs/exposure-state-machine.md) for the
auto/manual exposure design.

## Supported output formats

| GStreamer caps         | SDK format            | Notes                              |
|------------------------|-----------------------|------------------------------------|
| `image/jpeg`           | `VIDEO_MEDIA_TYPE_MJPG` | Compressed; lowest bandwidth    |
| `video/x-raw,format=YUY2` | `VIDEO_MEDIA_TYPE_YUYV` | Uncompressed YUV 4:2:2      |

The exact resolutions and frame rates depend on the connected camera model.
`norisrc` queries them from the device at start-up and advertises only the
formats the hardware actually supports.

## Supported architectures

The SDK ships pre-built shared libraries for four targets.  The Meson build
auto-detects the host CPU and selects the correct one:

| Host CPU family | SDK library directory |
|-----------------|-----------------------|
| `x86_64`        | `sdk/lib/x64`        |
| `x86`           | `sdk/lib/x86`        |
| `aarch64`       | `sdk/lib/arm64`      |
| `arm`           | `sdk/lib/arm32`      |

## Building from source

### Prerequisites

```bash
# GStreamer 1.16+ development headers
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev

# Build tools
sudo apt install meson ninja-build g++
```

### Build

```bash
cd gst-nori
meson setup builddir
ninja -C builddir
```

A successful build produces `builddir/libgstnori.so`.

### Verify

```bash
GST_PLUGIN_PATH=builddir gst-inspect-1.0 norisrc
```

You should see the element details, pad templates, and the full property list.

### Package and install

The recommended way to install is via the `.deb` package, which places both the
plugin and SDK library in the correct system directories and runs `ldconfig`
automatically.

```bash
# Build the .deb (after ninja -C builddir)
./package.sh                  # produces gst-nori_1.1.0_arm64.deb

# Install or upgrade
sudo dpkg -i gst-nori_1.1.0_arm64.deb

# Remove
sudo dpkg -r gst-nori
```

Alternatively, install manually without packaging:

```bash
sudo ninja -C builddir install
sudo cp sdk/lib/arm64/libNori_Xvision_Std.so /usr/local/lib/   # adjust arch
sudo ldconfig
```

## Using pre-built .deb packages

Download the `.deb` for your architecture from the
[Releases](../../releases) page.

```bash
# Install (also handles upgrades)
sudo dpkg -i gst-nori_1.1.0_arm64.deb

# Verify
gst-inspect-1.0 norisrc

# Remove
sudo dpkg -r gst-nori
```

The `.deb` package:
- Installs `libgstnori.so` to the system GStreamer plugin directory
- Installs `libNori_Xvision_Std.so` to `/usr/local/lib/`
- Runs `ldconfig` on install and removal
- Declares dependencies on `gstreamer1.0-tools` and `gstreamer1.0-plugins-base`

### Requirements on the target machine

- Same CPU architecture as the build (arm64 binary will not run on x64)
- A Nori Xvision USB camera connected to the host

## Usage examples

```bash
# Live preview (MJPEG decoded to display)
gst-launch-1.0 norisrc ! jpegdec ! videoconvert ! autovideosink

# Force YUY2 raw output
gst-launch-1.0 norisrc ! 'video/x-raw,format=YUY2' ! videoconvert ! autovideosink

# Select the second camera by index
gst-launch-1.0 norisrc device-index=1 ! jpegdec ! videoconvert ! autovideosink

# Select a camera by role tag (assigned with `nori-ctl tag set`, see below)
gst-launch-1.0 norisrc role=front-cam ! jpegdec ! videoconvert ! autovideosink

# Manual exposure and gain (sensor-level, microsecond precision)
gst-launch-1.0 norisrc auto-exposure=false sensor-shutter=10000 sensor-gain=4 \
    ! jpegdec ! videoconvert ! autovideosink

# Hardware trigger mode
gst-launch-1.0 norisrc trigger-mode=hardware ! jpegdec ! autovideosink

# Mirror + flip
gst-launch-1.0 norisrc mirror-flip=mirror-flip ! jpegdec ! videoconvert ! autovideosink

# Record to MP4
gst-launch-1.0 norisrc ! jpegdec ! videoconvert ! x264enc ! mp4mux ! filesink location=out.mp4

# Snapshot (single frame)
gst-launch-1.0 norisrc num-buffers=1 ! jpegdec ! pngenc ! filesink location=frame.png

# Stream over network via RTP
gst-launch-1.0 norisrc ! rtpjpegpay ! udpsink host=192.168.1.100 port=5000
```

### GStreamer debug output

```bash
# Show norisrc debug messages
GST_DEBUG=norisrc:5 gst-launch-1.0 norisrc ! fakesink
```

## Element properties

All properties can be set on the `gst-launch-1.0` command line or via
`g_object_set()` in C code. Camera-control properties (everything below
`role`) can be changed while the pipeline is running.

| Property             | Type    | Default | Description                                                                                                                                                              |
|----------------------|---------|---------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `device-index`       | uint    | 0       | Camera index (0 = first detected device). Ignored when `role` is set.                                                                                                    |
| `role`               | string  | (none)  | Select the camera whose NORICAM tag matches this string. Takes precedence over `device-index`. Assign with `nori-ctl tag set`.                                            |
| `trigger-mode`       | enum    | none    | `none`, `software`, `hardware`, or `command`.                                                                                                                            |
| `mirror-flip`        | enum    | normal  | `normal`, `mirror`, `flip`, or `mirror-flip`.                                                                                                                            |
| `auto-exposure`      | boolean | true    | When `true`, firmware AE drives both shutter and gain. When `false`, the UVC exposure/gain registers are reset to device defaults and `sensor-shutter`/`sensor-gain` take over. |
| `auto-white-balance` | boolean | true    | Enable/disable UVC auto white balance.                                                                                                                                   |
| `sensor-shutter`     | uint    | 0       | Sensor shutter / exposure time in microseconds. Only applied when `auto-exposure=false`; a warning is logged otherwise.                                                  |
| `sensor-gain`        | uint    | 0       | Sensor analog-gain multiplier. Only applied when `auto-exposure=false`; a warning is logged otherwise.                                                                   |

**Manual exposure semantics.** `auto-exposure=false` flips the camera into
manual mode, resets the UVC exposure and gain registers to *each device's
own defaults* (so every camera starts from an identical baseline), and then
applies `sensor-shutter` / `sensor-gain`. Writing those properties while
`auto-exposure=true` is logged as ignored — the firmware AE loop would
overwrite them every frame anyway. Full design notes:
[`docs/exposure-state-machine.md`](docs/exposure-state-machine.md).

**Dirty-bit policy.** Only properties explicitly set by the user are sent
to the device. Unset properties leave the camera's own defaults untouched.

## `nori-ctl`

A command-line companion installed by the same `.deb` to
`/usr/local/bin/nori-ctl`. Useful for one-off operations that don't fit
into a streaming pipeline: enumerating cameras, assigning role tags,
reading or writing trigger mode and ESN, dumping the full SDK state, and
poking sensor shutter/gain.

```
Usage: nori-ctl <command> [args]

  list                                          List connected cameras (incl. tags)
  state   <idx>                                 Sectioned dump of all SDK state

  trigger get <idx>
  trigger set <idx> <none|software|hardware|command>

  tag     get   <idx> [--block <n>]             Get role tag
  tag     set   <idx> <role> [--block <n>] [--force]
  tag     clear <idx> [--block <n>]

  shutter get <idx>                             Sensor shutter (µs)
  shutter set <idx> <microseconds>              (auto-switches AE to manual)

  gain    get <idx>                             Sensor analog gain (cur/min/max/step)
  gain    set <idx> <value>                     (auto-switches AE to manual)

  esn     get <idx> [--hex]                     Camera serial (Gen60 ESN)
  esn     set <idx> <string>                    (Gen60 cannot write ESN)

  udata   get <idx> <piece> [--out <path>]      Raw 4096-byte user-data block
  udata   set <idx> <piece>  --in <path>
```

### Examples

```bash
# See what's plugged in (and any existing role tags)
nori-ctl list

# Tag two cameras so pipelines can pick them by purpose
nori-ctl tag set 0 front-cam
nori-ctl tag set 1 rear-cam

# Then refer to them in pipelines by role:
gst-launch-1.0 norisrc role=front-cam ! jpegdec ! autovideosink

# Full state dump for a camera (sensor, image, trigger, WB, UVC PU controls)
nori-ctl state 0

# Switch a camera into hardware-trigger mode for an external sync source
nori-ctl trigger set 0 hardware

# Set sensor shutter directly (also flips AE to manual under the hood)
nori-ctl shutter set 0 5000     # 5 ms exposure
nori-ctl gain    set 0 8        # 8x analog gain
```

The role tag is written to user-data block 255 by default and survives
power cycles. `tag set` refuses to overwrite unrecognised data without
`--force`.

## Design decisions

### Why a custom plugin instead of v4l2src?

`v4l2src` works with Nori cameras at the V4L2 level, but it cannot access
Nori-specific features exposed only through the SDK:

- **Trigger modes** (software, hardware, command) for industrial capture
- **Sensor-level shutter and gain** controls distinct from UVC PU controls
- **Mirror/flip** at the sensor level
- **Device enumeration** by SDK index instead of `/dev/videoN` paths
- **GPIO / PWM** control (extensible in future versions)

The plugin wraps the full SDK lifecycle so these controls are first-class
GStreamer properties.

### GstPushSrc base class

The element subclasses `GstPushSrc` (via `GstBaseSrc`).  This is the standard
GStreamer pattern for live sources that produce data at their own pace.
`GstBaseSrc` manages threading, timestamping, state changes, and flushing; the
plugin only needs to implement `create()`, `start()`, `stop()`, and caps
negotiation.

### Frame acquisition: polling, not callback

The SDK offers two modes: callback (`Nori_Xvision_VideoCallBack`) and polling
(`Nori_Xvision_GetFrameBuff` / `Nori_Xvision_FreeFrameBuff`).

The plugin uses **polling** because:

1. `GstPushSrc::create()` is called from a dedicated streaming thread managed by
   `GstBaseSrc`.  The polling model maps directly to this: block in `create()`,
   return when a frame arrives.
2. The callback model would require an intermediate queue between the SDK
   callback thread and GStreamer's streaming thread, adding complexity with no
   performance benefit (the USB transfer is the bottleneck, not the handoff).
3. Polling respects `GstBaseSrc::unlock()` naturally by using a short timeout
   (500 ms) and checking a flushing flag, which makes pipeline shutdown clean.

### Caps negotiation from device capabilities

On `start()`, the element queries all supported format/resolution/framerate
combinations from the SDK (`Nori_Xvision_GetDeviceVideoInfoSize` +
`Nori_Xvision_GetDeviceVideoInfo`) and advertises only those in `get_caps()`.
Before the device is opened, the element returns wide template caps so that
pipeline construction can proceed.

The `fixate()` implementation prefers 1920x1080 @ 30 fps when no specific
format is requested, but downstream caps filters override this normally.

### SDK init is reference-counted

`Nori_Xvision_Init` / `Nori_Xvision_UnInit` are global operations.  If
multiple `norisrc` elements are used in one process (e.g., two cameras), the
plugin reference-counts them behind a mutex so the SDK is initialised exactly
once and torn down only after the last element stops.

### Dirty-property bitmask

Camera-control properties (trigger-mode, sensor-shutter, etc.) are only sent
to the device when explicitly set by the user. A bitmask tracks which
properties have been touched. This avoids overwriting the camera's factory
defaults on start-up when the user hasn't requested specific values.
Properties set before streaming are queued and applied in `set_caps()` once
the stream is live; properties set while running are applied immediately.

### AE state machine and `apply_lock`

`auto-exposure` plus `sensor-shutter`/`sensor-gain` are coupled — they all
end up programming the same physical sensor register via different SDK
paths. A two-state machine (`AUTO` ↔ `MANUAL`) owns the transitions, and
entry into `MANUAL` resets the UVC exposure/gain registers to each device's
own defaults so cross-camera behaviour is deterministic. A per-element
`GMutex apply_lock` serialises the apply path because the SDK's UVC
control writes can block for ~1 s, long enough for the state-change thread
and an external `g_object_set` to race through the transition. Full
write-up: [`docs/exposure-state-machine.md`](docs/exposure-state-machine.md).

### C++ compilation

The Nori SDK's public header (`Nori_Public.h`) includes `<chrono>` and
`<ctime>`, so any translation unit that includes it must be compiled as C++.
The plugin is therefore written as `.cpp` files using GStreamer's C API, which
works correctly in C++.  The `volatile gsize` qualifier was removed from
`g_once_init_enter` calls to avoid a GCC 11+ error with `__atomic_load` in C++
mode; this is compatible with GLib >= 2.32.

## Project structure

```
gst-nori/
├── meson.build                      # Build system (auto-selects SDK arch)
├── package.sh                       # Builds a .deb from compiled artifacts
├── README.md
├── CHANGELOG.md
├── docs/
│   └── exposure-state-machine.md    # AE design + apply_lock concurrency
├── sdk/
│   ├── include/Nori_Xvision_API/    # SDK headers (3 files)
│   └── lib/
│       ├── arm32/libNori_Xvision_Std.so
│       ├── arm64/libNori_Xvision_Std.so
│       ├── x64/libNori_Xvision_Std.so
│       └── x86/libNori_Xvision_Std.so
├── src/
│   ├── gstnori.cpp                  # Plugin entry: GST_PLUGIN_DEFINE
│   ├── gstnorisrc.cpp               # Element implementation
│   ├── gstnorisrc.h                 # Type definitions, enums, struct layout
│   └── nori_tag.h                   # Shared tag (NORICAM) encode/decode
├── tools/
│   └── nori-ctl.cpp                 # nori-ctl CLI source
└── tests/
    └── test_exposure_state_machine.py   # PyGObject runtime harness
```

## License

The GStreamer plugin code is licensed under LGPL-2.1-or-later, consistent with
GStreamer's own licensing.  The Nori Xvision SDK (`sdk/` directory) is
proprietary and subject to its own license terms from Norigine Technology.
