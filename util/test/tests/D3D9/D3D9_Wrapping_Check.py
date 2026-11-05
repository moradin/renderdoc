import rdtest


# The demo checks object identity and lifetime itself and logs errors, so this checks its log (in
# check_capture) and that the capture loads and replays.
class D3D9_Wrapping_Check(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Wrapping_Check'
    internal = False

    def check_d3d9_capture(self):
        self.check_api()
        self.step_all_actions()

