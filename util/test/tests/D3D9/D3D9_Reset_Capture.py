import renderdoc as rd
import rdtest

RED = [0.6, 0.1, 0.1, 1.0]
BLUE = [0.1, 0.1, 0.6, 1.0]


# The demo resets the device half way through two captured frames: frame 30, captured with the
# in-app API, which should keep only what came after the reset, and frame 60, triggered on frame 59,
# which should be discarded and frame 61 captured instead. See STATUS.md, "Checking
# D3D9_Reset_Capture".
class D3D9_Reset_Capture(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Reset_Capture'
    internal = False

    def get_capture(self):
        # the demo makes both captures itself
        captures = self.capture_demo(None, captures_expected=2)
        self.second_capture = captures[1].path
        return captures[0].path

    def markers(self, controller: rd.ReplayController):
        names = []

        def walk(actions):
            for a in actions:
                if a.flags & rd.ActionFlags.PushMarker:
                    names.append(a.customName)
                walk(a.children)

        walk(controller.GetRootActions())
        return names

    def check_frame(self, what: str, expected_markers, clear):
        markers = self.markers(self.controller)
        if markers != expected_markers:
            raise rdtest.TestFailureException(f"{what} has the markers {markers}, expected {expected_markers}")
        rdtest.log.success(f"{what} has the markers {markers}")

        # the in-app capture ends at EndFrameCapture, before the Present, so its last action is the
        # draw
        last_action = self.get_last_action()
        self.set_event(last_action.eventId, True)
        if last_action.flags & rd.ActionFlags.Present:
            backbuffer = last_action.copyDestination
        else:
            backbuffer = self.controller.GetPipelineState().GetOutputTargets()[0].resource
        self.check_pixel_value(backbuffer, 10, 10, clear)
        self.check_triangle(out=backbuffer, back=clear)
        self.step_all_actions()

    def check_d3d9_capture(self):
        self.check_api()

        # frame 30: only what came after the reset, so a blue clear
        self.check_frame('The in-app capture of frame 30', ['After the reset'], BLUE)

        # frame 61, since the triggered capture of frame 60 was reset half way through: no reset,
        # and a red clear
        self.controller.Shutdown()
        self.cap.Shutdown()
        self.cap, self.controller = rdtest.open_capture(self.second_capture, opts=self.get_replay_options())
        self.sdfile = self.controller.GetStructuredFile()
        self.check_api()
        self.check_frame('The triggered capture', ['Frame'], RED)
