import rdtest


class D3D9_Overlay_Test(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Overlay_Test'
    internal = False

    def check_d3d9_capture(self):
        self.check_api()

        # saves each overlay as overlay_<marker>_<overlay>.png in the test's tmp folder
        overlay = self.check_script('d3d9_overlay_check')
        self.run_script_check('The overlay check', overlay.check_capture, self.controller,
                              rdtest.get_tmp_path('overlay'))
