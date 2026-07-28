"""CENTER gesture behaviour of jpp_keypad_core, exercised on the host.

The detector reports gestures only — whether a hold or a double-click means
"Back" is decided by keypad_task() in main/app_main.c from the user's
Settings > Controls preference and the foreground app's claim. What is
pinned here is the part with the subtle timing: when a short click is
released to the caller, and when a second click turns the pair into
CENTER_DOUBLE instead.
"""

import pytest

from keypad_harness import (
    DOUBLE_CLICK_MS,
    Keypad,
    build_library,
    kinds,
)


@pytest.fixture(scope="session")
def keypad_lib(tmp_path_factory):
    return build_library(tmp_path_factory.mktemp("keypad"))


# --- detect_double_click off: the short click is never withheld ------------- #

def test_short_click_is_immediate_without_double_click_detection(keypad_lib):
    kp = Keypad(keypad_lib, detect_double_click=False)
    kp.tap()
    # Only the release debounce stands between the button coming up and the
    # event — nothing like the DOUBLE_CLICK_MS wait the deferred path pays.
    events = kp.idle(60)
    assert kinds(events) == ["CENTER_SHORT"]
    assert events[0][2] == "OK"
    assert 60 < DOUBLE_CLICK_MS


def test_double_click_never_fires_without_detection(keypad_lib):
    kp = Keypad(keypad_lib, detect_double_click=False)
    kp.tap()
    first = kp.idle(60)
    kp.tap()
    second = kp.idle(60)
    # Two independent OKs, no CENTER_DOUBLE anywhere.
    assert kinds(first) == ["CENTER_SHORT"]
    assert kinds(second) == ["CENTER_SHORT"]


# --- detect_double_click on: the short click waits for the window ----------- #

def test_short_click_is_deferred_until_the_window_closes(keypad_lib):
    kp = Keypad(keypad_lib, detect_double_click=True)
    kp.tap()
    # Nothing may be reported while a second click is still possible.
    during = kp.idle(DOUBLE_CLICK_MS - 60)
    assert kinds(during) == []
    after = kp.idle(120)
    assert kinds(after) == ["CENTER_SHORT"]


def test_second_click_inside_the_window_becomes_center_double(keypad_lib):
    kp = Keypad(keypad_lib, detect_double_click=True)
    kp.tap()
    gap = kp.idle(60)
    second = kp.tap()
    tail = kp.idle(DOUBLE_CLICK_MS + 100)
    # The pair reports CENTER_DOUBLE and neither click leaks an OK.
    assert kinds(gap) == []
    assert kinds(second + tail) == ["CENTER_DOUBLE"]


def test_second_click_after_the_window_is_two_separate_clicks(keypad_lib):
    kp = Keypad(keypad_lib, detect_double_click=True)
    kp.tap()
    first = kp.idle(DOUBLE_CLICK_MS + 100)
    kp.tap()
    second = kp.idle(DOUBLE_CLICK_MS + 100)
    assert kinds(first) == ["CENTER_SHORT"]
    assert kinds(second) == ["CENTER_SHORT"]


# --- a hold is detected in both modes -------------------------------------- #

@pytest.mark.parametrize("detect_double_click", [False, True])
def test_hold_always_reports_center_long(keypad_lib, detect_double_click):
    kp = Keypad(keypad_lib, detect_double_click=detect_double_click)
    events = kp.press(900)
    assert "CENTER_LONG" in kinds(events)
    long_event = next(e for e in events if e[2] == "HOLD")
    assert long_event[1] == "CENTER"


def test_hold_reports_center_long_exactly_once(keypad_lib):
    kp = Keypad(keypad_lib, detect_double_click=False)
    events = kp.press(900) + kp.idle(100)
    assert kinds(events).count("CENTER_LONG") == 1


def test_hold_does_not_report_a_short_click(keypad_lib):
    kp = Keypad(keypad_lib, detect_double_click=True)
    events = kp.press(900) + kp.idle(DOUBLE_CLICK_MS + 100)
    assert "CENTER_SHORT" not in kinds(events)


# --- switching modes under a click already in flight ----------------------- #

def test_pending_click_is_flushed_when_detection_is_switched_off(keypad_lib):
    """Settings > Controls can flip between polls; a click already withheld
    must neither be dropped nor replayed once the window no longer applies."""
    kp = Keypad(keypad_lib, detect_double_click=True)
    kp.tap()
    assert kinds(kp.idle(60)) == []
    kp.cfg.detect_double_click = False
    flushed = kp.idle(20)
    assert kinds(flushed) == ["CENTER_SHORT"]
    # And it is not delivered a second time when the old window would expire.
    assert kinds(kp.idle(DOUBLE_CLICK_MS + 100)) == []


# --- directions are untouched by any of this ------------------------------- #

@pytest.mark.parametrize("detect_double_click", [False, True])
def test_direction_keys_are_unaffected(keypad_lib, detect_double_click):
    from keypad_harness import UP_UV

    kp = Keypad(keypad_lib, detect_double_click=detect_double_click)
    events = kp.press(100, key_uv=UP_UV) + kp.idle(60)
    assert kinds(events) == ["PRESS", "RELEASE"]
    assert all(e[1] == "UP" for e in events)
