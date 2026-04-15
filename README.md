# gst-nori

A GStreamer source element (`norisrc`) for **Nori Xvision USB cameras**, built on
top of the Nori Xvision SDK (V10.00.01).  It works as a drop-in replacement for
`v4l2src` while exposing Nori-specific camera controls (trigger modes, sensor
shutter/gain, ISP parameters) as GObject properties that can be read and changed
at runtime.

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
./package.sh                  # produces gst-nori_1.0.0_arm64.deb

# Install or upgrade
sudo dpkg -i gst-nori_1.0.0_arm64.deb

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
sudo dpkg -i gst-nori_1.0.0_arm64.deb

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

# Select second camera
gst-launch-1.0 norisrc device-index=1 ! jpegdec ! videoconvert ! autovideosink

# Set exposure and gain manually
gst-launch-1.0 norisrc auto-exposure=false exposure=200 gain=64 ! jpegdec ! autovideosink

# Hardware trigger mode
gst-launch-1.0 norisrc trigger-mode=hardware ! jpegdec ! autovideosink

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
`g_object_set()` in C code.  Camera-control properties (everything below
`device-index`) can be changed while the pipeline is running.

| Property               | Type    | Default | Description                                         |
|------------------------|---------|---------|-----------------------------------------------------|
| `device-index`         | uint    | 0       | Camera index (0 = first detected device)            |
| `trigger-mode`         | enum    | none    | `none`, `software`, `hardware`, or `command`        |
| `mirror-flip`          | enum    | normal  | `normal`, `mirror`, `flip`, or `mirror-flip`        |
| `brightness`           | int     | 0       | UVC brightness (`V4L2_CID_BRIGHTNESS`)              |
| `contrast`             | int     | 0       | UVC contrast (`V4L2_CID_CONTRAST`)                  |
| `saturation`           | int     | 0       | UVC saturation (`V4L2_CID_SATURATION`)              |
| `sharpness`            | int     | 0       | UVC sharpness (`V4L2_CID_SHARPNESS`)                |
| `hue`                  | int     | 0       | UVC hue (`V4L2_CID_HUE`)                            |
| `gamma`                | int     | 0       | UVC gamma (`V4L2_CID_GAMMA`)                        |
| `gain`                 | int     | 0       | UVC gain (`V4L2_CID_GAIN`)                          |
| `exposure`             | int     | 0       | Manual exposure value (`V4L2_CID_EXPOSURE_ABSOLUTE`) |
| `auto-exposure`        | boolean | true    | Enable/disable auto exposure                        |
| `auto-white-balance`   | boolean | true    | Enable/disable auto white balance                   |
| `sensor-shutter`       | uint    | 0       | Sensor shutter time in microseconds                 |
| `sensor-gain`          | uint    | 0       | Sensor analogue gain multiplier                     |

**Note:** The valid ranges for UVC controls (brightness, contrast, etc.) are
device-specific.  The element accepts the full int range and forwards values to
the SDK, which will clamp or reject out-of-range values.  Only properties
explicitly set by the user are sent to the device; unset properties leave the
camera's own defaults untouched.

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

Camera-control properties (brightness, trigger-mode, etc.) are only sent to the
device when explicitly set by the user.  A bitmask tracks which properties have
been touched.  This avoids overwriting the camera's factory defaults on start-up
when the user hasn't requested specific values.  Properties set before streaming
are queued and applied in `set_caps()` once the stream is live; properties set
while running are applied immediately.

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
├── sdk/
│   ├── include/Nori_Xvision_API/    # SDK headers (3 files)
│   └── lib/
│       ├── arm32/libNori_Xvision_Std.so
│       ├── arm64/libNori_Xvision_Std.so
│       ├── x64/libNori_Xvision_Std.so
│       └── x86/libNori_Xvision_Std.so
└── src/
    ├── gstnori.cpp                  # Plugin entry: GST_PLUGIN_DEFINE
    ├── gstnorisrc.cpp               # Element implementation (~560 lines)
    └── gstnorisrc.h                 # Type definitions, enums, struct layout
```

## License

The GStreamer plugin code is licensed under LGPL-2.1-or-later, consistent with
GStreamer's own licensing.  The Nori Xvision SDK (`sdk/` directory) is
proprietary and subject to its own license terms from Norigine Technology.
