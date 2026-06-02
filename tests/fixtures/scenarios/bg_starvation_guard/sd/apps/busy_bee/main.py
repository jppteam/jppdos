import time


class BusyBeeApp:
    def __init__(self, sdk):
        self.sdk = sdk

    def on_start(self):
        self.sdk.add_background_task("spin", self._spin)
        self.sdk.set_frame(["Busy Bee", "watchdog demo"], "BACK launcher")

    def on_foreground(self):
        self.sdk.set_frame(["Busy Bee", "foreground"], "BACK launcher")

    def on_background(self):
        self.sdk.set_frame(["Busy Bee", "background"], "guard active")

    def _spin(self):
        deadline = time.monotonic() + 0.08
        while time.monotonic() < deadline:
            pass


def create_app(sdk):
    return BusyBeeApp(sdk)
