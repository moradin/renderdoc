import struct

import renderdoc as rd
import rdtest


class D3D9_Frame_Start_State(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Frame_Start_State'
    internal = False

    # register: value, as the demo sets them once before the first frame
    vs_floats = {10: (1.0, 2.0, 3.0, 4.0), 200: (13.0, 14.0, 15.0, 16.0), 138: (0.0, 0.0, 0.0, 0.0)}
    ps_floats = {20: (9.0, 10.0, 11.0, 12.0), 180: (17.0, 18.0, 19.0, 20.0), 148: (0.0, 0.0, 0.0, 0.0)}

    def check_floats(self, stage: str, buffer: rd.ResourceId, expected):
        data = self.controller.GetBufferData(buffer, 0, 0)
        for reg, want in expected.items():
            if len(data) < (reg + 1) * 16:
                raise rdtest.TestFailureException(f"{stage} float constants are {len(data)} bytes, too few for c{reg}")
            got = struct.unpack_from('4f', data, reg * 16)
            if not rdtest.value_compare(got, want):
                raise rdtest.TestFailureException(f"{stage} c{reg} is {got}, expected {want}")
        rdtest.log.success(f"{stage} float constants {sorted(expected.keys())} are as the demo set them")

    def check_d3d9_capture(self):
        self.check_api()

        last_action = self.get_last_action()
        self.set_event(last_action.eventId, True)
        self.check_triangle(out=last_action.copyDestination)

        # the state set before the first frame is still there at the shader draw
        draw = self.find_action('DrawPrimitive')
        self.set_event(draw.eventId, True)
        pipe = self.controller.GetD3D9PipelineState()

        states = {r.name: r.value for r in pipe.renderStates}
        for name, want in [('D3DRS_AMBIENT', 0x00102030), ('D3DRS_STENCILREF', 0x5A)]:
            got = states.get(name, states.get(name.replace('D3DRS_', '')))
            if got != want:
                raise rdtest.TestFailureException(f"{name} is {got}, expected 0x{want:x} (have {list(states.keys())[:8]}...)")
        rdtest.log.success("D3DRS_AMBIENT and D3DRS_STENCILREF are as the demo set them")

        self.check_floats('VS', pipe.vertexShader.floatConstantsBuffer, self.vs_floats)
        self.check_floats('PS', pipe.pixelShader.floatConstantsBuffer, self.ps_floats)

        pipestate = self.check_script('d3d9_pipestate_check')
        self.run_script_check('The pipeline state check', pipestate.check_capture, self.controller)

        self.check_export(self.capture_filename)
