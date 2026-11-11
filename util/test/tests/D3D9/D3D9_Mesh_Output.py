import os

import renderdoc as rd
import rdtest

# the packed draw's signatures, as the demo declares them: (name, register, mask, used)
PACKED_VS_OUT = [
    ('POSITION', 0, 0xF, 0xF),
    ('TEXCOORD0', 1, 0x3, 0x3), ('TEXCOORD1', 1, 0xC, 0xC),
    ('COLOR0', 2, 0x7, 0x7), ('FOG', 2, 0x8, 0x8),
    ('TEXCOORD2', 3, 0x1, 0x1), ('TEXCOORD3', 3, 0xE, 0xE),
    ('TEXCOORD4', 4, 0x5, 0x5), ('TEXCOORD5', 4, 0xA, 0xA),
    ('TEXCOORD6', 5, 0x1, 0x1), ('TEXCOORD7', 5, 0x2, 0x2), ('COLOR1', 5, 0xC, 0xC),
]
PACKED_PS_IN = [
    ('TEXCOORD0', 0, 0x3, 0x3), ('TEXCOORD1', 0, 0xC, 0xC),
    ('TEXCOORD2', 1, 0x1, 0x1), ('TEXCOORD3', 1, 0xE, 0xE),
    ('COLOR', 2, 0x7, 0x7),
]


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

        self.check_packed_signatures()

    def check_packed_signatures(self):
        # vs_3_0 and ps_3_0 can declare two or more semantics in one register, each with its own
        # mask: each is an element of its own, with as many components as its mask has
        marker = self.find_action('vs_3_0 packed outputs')
        if marker is None:
            raise rdtest.TestFailureException("No 'vs_3_0 packed outputs' marker")
        draws = [a for a in marker.children if a.flags & rd.ActionFlags.Drawcall]
        draw = draws[0] if draws else None
        if draw is None:
            raise rdtest.TestFailureException("No draw in 'vs_3_0 packed outputs'")
        self.controller.SetFrameEvent(draw.eventId, True)
        pipe = self.controller.GetPipelineState()

        for what, sig, expected in [
                ('vertex shader output', pipe.GetShaderReflection(rd.ShaderStage.Vertex).outputSignature, PACKED_VS_OUT),
                ('pixel shader input', pipe.GetShaderReflection(rd.ShaderStage.Pixel).inputSignature, PACKED_PS_IN)]:
            got = [(s.semanticIdxName, s.regIndex, s.regChannelMask, s.channelUsedMask) for s in sig]
            if got != expected:
                raise rdtest.TestFailureException(f"The packed draw's {what} signature is {got}, expected {expected}")
            for s in sig:
                if s.compCount != bin(s.regChannelMask).count('1'):
                    raise rdtest.TestFailureException(f"{s.semanticIdxName} has {s.compCount} components, its mask "
                                                      f"{s.regChannelMask:#x}")
            rdtest.log.success(f"The packed draw's {what} signature has the {len(expected)} declared elements")
