#!/usr/bin/env python3
"""
Runtime verification harness for the norisrc exposure state machine.

See docs/exposure-state-machine.md for the design this exercises.

Run from the repo root with the plugin built in builddir/:

    GST_DEBUG=norisrc:5 GST_PLUGIN_PATH=builddir \
        python3 tests/test_exposure_state_machine.py 2>&1 | tee /tmp/exp_test.log

Pass one or more scenario numbers (1-8) to run a subset:

    python3 tests/test_exposure_state_machine.py 3 5

Each scenario prints a banner like '===TEST X===' so you can grep the log
afterwards. Expected nori_set_pu / nori_apply_exposure lines for each test
are documented inline.

Requires PyGObject (python3-gi, gir1.2-gst-1.0) and a connected camera.
"""
import sys
import time

import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

Gst.init(None)


def banner(msg: str) -> None:
    print(f"\n===TEST {msg}===", flush=True)
    # Sleep to make timestamps in the GStreamer log line up cleanly.
    time.sleep(0.05)


def make() -> Gst.Pipeline:
    return Gst.parse_launch("norisrc name=src ! fakesink sync=false")


def play(pipe: Gst.Pipeline, secs: float) -> None:
    pipe.set_state(Gst.State.PLAYING)
    time.sleep(secs)


def teardown(pipe: Gst.Pipeline) -> None:
    pipe.set_state(Gst.State.NULL)
    # Give SDK + GStreamer a beat to settle before the next scenario.
    time.sleep(0.5)


# ---------------------------------------------------------------------------
# Scenario 1: default AUTO start — exactly one mode write, no sensor writes
# Expect: 'auto-exposure=on = 3 OK' (once)
# ---------------------------------------------------------------------------
def test_default_auto():
    banner("1 default AUTO start")
    pipe = make()
    play(pipe, 1.5)
    teardown(pipe)


# ---------------------------------------------------------------------------
# Scenario 2: default MANUAL start with sensor values
# Expect: AE=1, uvc-exposure=default, uvc-gain=default, then sensor writes.
# ---------------------------------------------------------------------------
def test_default_manual():
    banner("2 default MANUAL start with sensor-shutter+gain")
    pipe = make()
    src = pipe.get_by_name("src")
    src.set_property("auto-exposure", False)
    src.set_property("sensor-shutter", 10000)
    src.set_property("sensor-gain", 4)
    play(pipe, 1.5)
    teardown(pipe)


# ---------------------------------------------------------------------------
# Scenario 3: runtime AUTO -> MANUAL transition
# Expect: AE=3 at start; later AE=1, UVC defaults, sensor writes.
# ---------------------------------------------------------------------------
def test_auto_to_manual():
    banner("3 runtime AUTO -> MANUAL")
    pipe = make()
    src = pipe.get_by_name("src")
    play(pipe, 1.5)
    print("---transition---", flush=True)
    src.set_property("auto-exposure", False)
    src.set_property("sensor-shutter", 20000)
    src.set_property("sensor-gain", 8)
    time.sleep(1.5)
    teardown(pipe)


# ---------------------------------------------------------------------------
# Scenario 4: runtime MANUAL -> AUTO transition
# Expect: at start AE=1 + UVC defaults + sensor writes; later AE=3 only.
# ---------------------------------------------------------------------------
def test_manual_to_auto():
    banner("4 runtime MANUAL -> AUTO")
    pipe = make()
    src = pipe.get_by_name("src")
    src.set_property("auto-exposure", False)
    src.set_property("sensor-shutter", 15000)
    play(pipe, 1.5)
    print("---transition---", flush=True)
    src.set_property("auto-exposure", True)
    time.sleep(1.5)
    teardown(pipe)


# ---------------------------------------------------------------------------
# Scenario 5: stay in MANUAL, change sensor values mid-stream
# Expect: only SetSensorShutter / SetSensorGain on second change — NO repeat
# of the UVC default reset (we're already in MANUAL).
# ---------------------------------------------------------------------------
def test_manual_stay():
    banner("5 stay MANUAL, change sensor values")
    pipe = make()
    src = pipe.get_by_name("src")
    src.set_property("auto-exposure", False)
    src.set_property("sensor-shutter", 5000)
    play(pipe, 1.5)
    print("---change shutter only---", flush=True)
    src.set_property("sensor-shutter", 25000)
    time.sleep(1.0)
    print("---change gain only---", flush=True)
    src.set_property("sensor-gain", 16)
    time.sleep(1.0)
    teardown(pipe)


# ---------------------------------------------------------------------------
# Scenario 6: sensor write while AUTO -> warning, no SDK call
# Expect: 'sensor-shutter/sensor-gain set while auto-exposure=true; ignored'
# ---------------------------------------------------------------------------
def test_sensor_write_while_auto():
    banner("6 sensor-shutter while AUTO (warn path)")
    pipe = make()
    src = pipe.get_by_name("src")
    play(pipe, 1.0)
    print("---attempt sensor write---", flush=True)
    src.set_property("sensor-shutter", 8000)
    time.sleep(0.5)
    teardown(pipe)


# ---------------------------------------------------------------------------
# Scenario 7: stop / restart re-reads UVC defaults
# Expect: two 'UVC defaults: exposure=N gain=M' INFO lines.
# ---------------------------------------------------------------------------
def test_stop_restart():
    banner("7 stop/restart re-reads UVC defaults")
    pipe = make()
    play(pipe, 1.0)
    teardown(pipe)
    print("---restart---", flush=True)
    pipe2 = make()
    play(pipe2, 1.0)
    teardown(pipe2)


# ---------------------------------------------------------------------------
# Scenario 8: AE toggle back-and-forth, exercising transition idempotence
# Expect: AE writes only on actual mode change; redundant set_property to
# the same value should NOT trigger a second AE=3 (because props_dirty is
# set, but exposure_applied already matches desired -> no write).
# ---------------------------------------------------------------------------
def test_ae_toggle():
    banner("8 AE toggle on->off->on->off")
    pipe = make()
    src = pipe.get_by_name("src")
    play(pipe, 1.0)
    print("---off---", flush=True)
    src.set_property("auto-exposure", False)
    src.set_property("sensor-shutter", 12000)
    time.sleep(0.8)
    print("---on---", flush=True)
    src.set_property("auto-exposure", True)
    time.sleep(0.8)
    print("---off again---", flush=True)
    src.set_property("auto-exposure", False)
    src.set_property("sensor-shutter", 18000)
    time.sleep(0.8)
    teardown(pipe)


SCENARIOS = {
    "1": test_default_auto,
    "2": test_default_manual,
    "3": test_auto_to_manual,
    "4": test_manual_to_auto,
    "5": test_manual_stay,
    "6": test_sensor_write_while_auto,
    "7": test_stop_restart,
    "8": test_ae_toggle,
}


def main():
    if len(sys.argv) > 1 and sys.argv[1] != "all":
        for k in sys.argv[1:]:
            if k not in SCENARIOS:
                print(f"unknown scenario {k!r}; available: {list(SCENARIOS)}",
                      file=sys.stderr)
                return 1
            SCENARIOS[k]()
    else:
        for fn in SCENARIOS.values():
            fn()
    print("\n===DONE===", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
