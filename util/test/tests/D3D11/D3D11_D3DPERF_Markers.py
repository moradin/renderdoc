import renderdoc as rd
import rdtest


# The fork's D3D9 driver shares d3d9.dll's D3DPERF hooks with D3D11's, which records them as D3D11
# markers. The check is shared with util/test/d3d11_d3dperf_check.py.
class D3D11_D3DPERF_Markers(rdtest.D3D9_Test):
    demos_test_name = 'D3D11_D3DPERF_Markers'
    internal = False

    def check_d3d9_capture(self):
        self.check_api(rd.GraphicsAPI.D3D11)

        perf = self.check_script('d3d11_d3dperf_check')
        self.run_script_check('The D3DPERF markers check', perf.check_capture, self.controller)
