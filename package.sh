#!/bin/bash
#
# Build a .deb package for gst-nori.
#
# Usage:
#   ./package.sh              # uses builddir/ by default
#   ./package.sh /path/to/builddir
#
# Prerequisites: the plugin must already be compiled (ninja -C builddir).

set -euo pipefail

BUILDDIR="${1:-builddir}"
PLUGIN="$BUILDDIR/libgstnori.so"
NORICTL="$BUILDDIR/nori-ctl"

if [ ! -f "$PLUGIN" ]; then
    echo "Error: $PLUGIN not found. Build the plugin first:" >&2
    echo "  meson setup builddir && ninja -C builddir" >&2
    exit 1
fi

if [ ! -f "$NORICTL" ]; then
    echo "Error: $NORICTL not found. Build it first:" >&2
    echo "  ninja -C builddir" >&2
    exit 1
fi

# ---- Read version from meson.build ----
VERSION=$(grep -oP "version\s*:\s*'\K[^']+" meson.build | head -1)
if [ -z "$VERSION" ]; then
    echo "Error: could not parse version from meson.build" >&2
    exit 1
fi

# ---- Detect architecture ----
DEB_ARCH=$(dpkg --print-architecture 2>/dev/null || echo "arm64")

# ---- Detect GStreamer plugin directory ----
GST_PLUGIN_DIR=$(pkg-config --variable=pluginsdir gstreamer-1.0 2>/dev/null \
    || echo "/usr/lib/$DEB_ARCH-linux-gnu/gstreamer-1.0")
# Strip leading / for the staging tree
GST_PLUGIN_REL="${GST_PLUGIN_DIR#/}"

# ---- Select matching SDK library ----
case "$DEB_ARCH" in
    amd64)  SDK_ARCH="x64"   ;;
    i386)   SDK_ARCH="x86"   ;;
    arm64)  SDK_ARCH="arm64" ;;
    armhf)  SDK_ARCH="arm32" ;;
    *)      echo "Error: unsupported arch $DEB_ARCH" >&2; exit 1 ;;
esac

SDK_LIB="sdk/lib/$SDK_ARCH/libNori_Xvision_Std.so"
if [ ! -f "$SDK_LIB" ]; then
    echo "Error: SDK library not found at $SDK_LIB" >&2
    exit 1
fi

PKG_NAME="gst-nori"
DEB_NAME="${PKG_NAME}_${VERSION}_${DEB_ARCH}"
STAGE="/tmp/${DEB_NAME}"

echo "Packaging $PKG_NAME $VERSION ($DEB_ARCH) ..."

# ---- Clean and create staging tree ----
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN"
mkdir -p "$STAGE/$GST_PLUGIN_REL"
mkdir -p "$STAGE/usr/local/lib"
mkdir -p "$STAGE/usr/local/bin"

# ---- Copy files ----
cp "$PLUGIN"  "$STAGE/$GST_PLUGIN_REL/libgstnori.so"
cp "$SDK_LIB" "$STAGE/usr/local/lib/libNori_Xvision_Std.so"
cp "$NORICTL" "$STAGE/usr/local/bin/nori-ctl"

# Strip debug symbols for a smaller package
strip --strip-unneeded "$STAGE/$GST_PLUGIN_REL/libgstnori.so" 2>/dev/null || true
strip --strip-unneeded "$STAGE/usr/local/bin/nori-ctl"        2>/dev/null || true

# ---- Compute installed size (KiB) ----
INSTALLED_SIZE=$(du -sk "$STAGE" | cut -f1)

# ---- Write control file ----
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG_NAME
Version: $VERSION
Architecture: $DEB_ARCH
Maintainer: gst-nori maintainers
Depends: gstreamer1.0-tools, gstreamer1.0-plugins-base
Description: GStreamer source plugin for Nori Xvision USB cameras
 A GstPushSrc-based element (norisrc) that captures video from Nori
 Xvision USB cameras via the Nori SDK. Supports MJPEG and YUY2 output
 with runtime camera controls (trigger, exposure, gain, mirror/flip)
 exposed as GStreamer properties. Ships with a nori-ctl CLI utility
 for enumerating devices and reading/writing trigger mode, ESN, and
 user-data blocks.
Installed-Size: $INSTALLED_SIZE
Section: libs
Priority: optional
EOF

# ---- Post-install: run ldconfig so libNori_Xvision_Std.so is found ----
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
ldconfig
EOF
chmod 755 "$STAGE/DEBIAN/postinst"

# ---- Post-remove: re-run ldconfig to clean up ----
cat > "$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
ldconfig
EOF
chmod 755 "$STAGE/DEBIAN/postrm"

# ---- Build the .deb ----
dpkg-deb --build --root-owner-group "$STAGE" "${DEB_NAME}.deb"

# ---- Cleanup ----
rm -rf "$STAGE"

echo ""
echo "Created: ${DEB_NAME}.deb"
echo ""
echo "  Install:   sudo dpkg -i ${DEB_NAME}.deb"
echo "  Remove:    sudo dpkg -r $PKG_NAME"
echo "  Verify:    gst-inspect-1.0 norisrc"
