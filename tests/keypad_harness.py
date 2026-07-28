"""Drive the real jpp_keypad_core state machine on the host.

jpp_keypad_poll() is hardware-agnostic — one ADC sample in, debounced events
out, and its only notion of time is sample_index * poll_interval_ms — so the
firmware source can be compiled natively and stepped a poll at a time. That
makes the CENTER gesture timing (long press, the double-click window, the
deferred short click) testable without hardware.
"""

import ctypes
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
KEYPAD_SRC = REPO_ROOT / "components" / "jpp_core" / "src" / "jpp_keypad_core.c"

# Mirrors the KEYPAD_BANDS table in main/app_main.c.
CENTER_UV = 900000
UP_UV = 120000
IDLE_UV = 2700000  # far from every band, so nothing matches

# Mirrors the config built in run_main_loop().
POLL_MS = 20
LONG_PRESS_MS = 700
DOUBLE_CLICK_MS = 300

KIND_NO_EVENT, KIND_PRESS, KIND_RELEASE, KIND_REPEAT, \
    KIND_CENTER_SHORT, KIND_CENTER_LONG, KIND_CENTER_DOUBLE = range(7)

KIND_NAMES = {
    KIND_NO_EVENT: "NO_EVENT",
    KIND_PRESS: "PRESS",
    KIND_RELEASE: "RELEASE",
    KIND_REPEAT: "REPEAT",
    KIND_CENTER_SHORT: "CENTER_SHORT",
    KIND_CENTER_LONG: "CENTER_LONG",
    KIND_CENTER_DOUBLE: "CENTER_DOUBLE",
}


class Band(ctypes.Structure):
    _fields_ = [
        ("key", ctypes.c_char_p),
        ("center_uv", ctypes.c_int),
        ("tolerance_uv", ctypes.c_int),
        ("repeatable", ctypes.c_bool),
    ]


class Config(ctypes.Structure):
    _fields_ = [
        ("enabled", ctypes.c_bool),
        ("calibration_uv", ctypes.c_int),
        ("hysteresis_uv", ctypes.c_int),
        ("debounce_samples", ctypes.c_int),
        ("long_press_ms", ctypes.c_int),
        ("poll_interval_ms", ctypes.c_int),
        ("repeat_enabled", ctypes.c_bool),
        ("repeat_delay_ms", ctypes.c_int),
        ("repeat_interval_ms", ctypes.c_int),
        ("detect_double_click", ctypes.c_bool),
        ("double_click_ms", ctypes.c_int),
        ("bands", ctypes.POINTER(Band)),
        ("band_count", ctypes.c_size_t),
    ]


class Event(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_int),
        ("key", ctypes.c_char_p),
        ("mapped", ctypes.c_char_p),
        ("duration_ms", ctypes.c_int),
    ]


class State(ctypes.Structure):
    _fields_ = [
        ("ready", ctypes.c_bool),
        ("enabled", ctypes.c_bool),
        ("calibration_uv", ctypes.c_int),
        ("hysteresis_uv", ctypes.c_int),
        ("debounce_samples", ctypes.c_int),
        ("long_press_ms", ctypes.c_int),
        ("poll_interval_ms", ctypes.c_int),
        ("repeat_enabled", ctypes.c_bool),
        ("repeat_delay_ms", ctypes.c_int),
        ("repeat_interval_ms", ctypes.c_int),
        ("stable_key", ctypes.c_char_p),
        ("candidate_key", ctypes.c_char_p),
        ("candidate_count", ctypes.c_int),
        ("press_started_ms", ctypes.c_int),
        ("last_repeat_ms", ctypes.c_int),
        ("center_long_emitted", ctypes.c_bool),
        ("sample_index", ctypes.c_size_t),
        ("short_pending", ctypes.c_bool),
        ("pending_release_ms", ctypes.c_int),
    ]


_BANDS = (Band * 5)(
    Band(b"UP", UP_UV, 40000, True),
    Band(b"DOWN", 300000, 40000, True),
    Band(b"LEFT", 500000, 40000, True),
    Band(b"RIGHT", 700000, 40000, True),
    Band(b"CENTER", CENTER_UV, 40000, False),
)


def build_library(tmp_path):
    """Compile jpp_keypad_core.c into a shared library and load it."""
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available to build the keypad harness")
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    lib_path = tmp_path / f"libjpp_keypad{suffix}"
    subprocess.run(
        [cc, "-std=c11", "-shared", "-fPIC", "-O1",
         str(KEYPAD_SRC), "-o", str(lib_path)],
        check=True, capture_output=True,
    )
    lib = ctypes.CDLL(str(lib_path))
    lib.jpp_keypad_state_init.argtypes = [ctypes.POINTER(State), ctypes.POINTER(Config)]
    lib.jpp_keypad_state_init.restype = None
    lib.jpp_keypad_poll.argtypes = [
        ctypes.POINTER(State), ctypes.POINTER(Config), ctypes.c_int, ctypes.c_bool,
        ctypes.POINTER(Event), ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.jpp_keypad_poll.restype = ctypes.c_int
    return lib


class Keypad:
    """A keypad you can press, release and let idle, one 20 ms poll at a time."""

    def __init__(self, lib, detect_double_click=False):
        self.lib = lib
        self.cfg = Config(
            enabled=True,
            calibration_uv=0,
            hysteresis_uv=20000,
            debounce_samples=2,
            long_press_ms=LONG_PRESS_MS,
            poll_interval_ms=POLL_MS,
            repeat_enabled=True,
            repeat_delay_ms=500,
            repeat_interval_ms=500,
            detect_double_click=detect_double_click,
            double_click_ms=DOUBLE_CLICK_MS,
            bands=_BANDS,
            band_count=len(_BANDS),
        )
        self.state = State()
        self.lib.jpp_keypad_state_init(ctypes.byref(self.state), ctypes.byref(self.cfg))

    def _poll(self, sample_uv):
        events = (Event * 8)()
        count = ctypes.c_size_t(0)
        rc = self.lib.jpp_keypad_poll(
            ctypes.byref(self.state), ctypes.byref(self.cfg),
            sample_uv, True, events, 8, ctypes.byref(count),
        )
        assert rc == 0, "jpp_keypad_poll reported an error"
        return [
            (events[i].kind,
             events[i].key.decode() if events[i].key else None,
             events[i].mapped.decode() if events[i].mapped else None)
            for i in range(count.value)
        ]

    def run(self, sample_uv, duration_ms):
        """Feed one sample for duration_ms, returning every event produced."""
        out = []
        for _ in range(max(1, duration_ms // POLL_MS)):
            out += self._poll(sample_uv)
        return out

    def press(self, duration_ms, key_uv=CENTER_UV):
        return self.run(key_uv, duration_ms)

    def idle(self, duration_ms):
        return self.run(IDLE_UV, duration_ms)

    def tap(self, key_uv=CENTER_UV):
        """A press just long enough to register, well under the long-press."""
        return self.press(100, key_uv)


def kinds(events):
    return [KIND_NAMES[k] for k, _key, _mapped in events]
