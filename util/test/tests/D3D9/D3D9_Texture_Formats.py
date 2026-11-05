import os

import rdtest


class D3D9_Texture_Formats(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Texture_Formats'
    internal = False

    def get_capture(self):
        # the native run writes what the GPU samples from each format, to compare RenderDoc with
        self.run_native()
        return super().get_capture()

    def check_d3d9_capture(self):
        self.check_api()

        native = self.demo_tmp_path('D3D9_Texture_Formats_reference.txt')
        under_rdoc = self.demo_tmp_path('D3D9_Texture_Formats_reference_rdoc.txt')
        for path in (native, under_rdoc):
            if not os.path.exists(path):
                raise rdtest.TestFailureException(f"The demo didn't write {path}")

        # the GPU samples every format the same under RenderDoc as natively
        with open(native, 'rb') as f:
            native_data = f.read()
        with open(under_rdoc, 'rb') as f:
            rdoc_data = f.read()
        if native_data != rdoc_data:
            raise rdtest.TestFailureException("The reference written under RenderDoc differs from the native one")
        rdtest.log.success(f"The reference written under RenderDoc is identical to the native one ({len(native_data)} bytes)")

        formats = self.check_script('d3d9_formats_check')
        self.run_script_check('The formats check', formats.check_capture, self.controller, native)
