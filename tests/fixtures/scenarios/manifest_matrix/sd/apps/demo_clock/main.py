class DemoClockApp:
    def __init__(self, sdk):
        self.sdk = sdk

    def on_start(self):
        self.sdk.set_frame(["Clock", "manifest ok"], "BACK returns")


def create_app(sdk):
    return DemoClockApp(sdk)
