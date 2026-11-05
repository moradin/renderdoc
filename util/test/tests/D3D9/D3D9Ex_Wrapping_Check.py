import rdtest


# D3D9_Wrapping_Check with a D3D9Ex device.
class D3D9Ex_Wrapping_Check(rdtest.D3D9_Test):
    demos_test_name = 'D3D9Ex_Wrapping_Check'
    internal = False

    def check_d3d9_capture(self):
        self.check_api()
        self.step_all_actions()
