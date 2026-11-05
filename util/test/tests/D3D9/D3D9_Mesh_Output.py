import os

import rdtest


class D3D9_Mesh_Output(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Mesh_Output'
    internal = False

    def get_capture(self):
        # the native run writes the post-transform vertices from ProcessVertices, C++ and the GPU
        self.run_native()
        return super().get_capture()

    def check_d3d9_capture(self):
        self.check_api()

        native = self.demo_tmp_path('D3D9_Mesh_Output_reference.txt')
        under_rdoc = self.demo_tmp_path('D3D9_Mesh_Output_reference_rdoc.txt')
        for path in (native, under_rdoc):
            if not os.path.exists(path):
                raise rdtest.TestFailureException(f"The demo didn't write {path}")

        # ProcessVertices and the GPU's readback give the same under RenderDoc as natively
        with open(native, 'rb') as f:
            native_data = f.read()
        with open(under_rdoc, 'rb') as f:
            rdoc_data = f.read()
        if native_data != rdoc_data:
            raise rdtest.TestFailureException("The reference written under RenderDoc differs from the native one")
        rdtest.log.success(f"The reference written under RenderDoc is identical to the native one ({len(native_data)} bytes)")

        mesh = self.check_script('d3d9_mesh_check')
        self.run_script_check('The mesh output check', mesh.check_capture, self.controller, native)
