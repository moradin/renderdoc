import renderdoc as rd
import rdtest

# each debugged draw: its marker, the shader model and how many instances it draws
DRAWS = [
    ('vs_1_1', 'vs_1_1', 1),
    ('vs_2_0', 'vs_2_0', 1),
    ('vs_2_x', 'vs_2_x', 1),
    ('vs_3_0', 'vs_3_0', 1),
    ('vs_3_0 instanced', 'vs_3_0', 2),
]

# registers each draw's first vertex's trace has, with their type and size
REGISTERS = {
    'vs_1_1': {'r0': (rd.VarType.Float, 4), 'a0': (rd.VarType.SInt, 4), 'oPos': (rd.VarType.Float, 4),
               'oFog': (rd.VarType.Float, 1), 'oPts': (rd.VarType.Float, 1), 'oD1': (rd.VarType.Float, 4),
               'oT3': (rd.VarType.Float, 4)},
    'vs_2_x': {'aL': (rd.VarType.SInt, 1), 'p0': (rd.VarType.Bool, 4), 'oD0': (rd.VarType.Float, 4)},
    'vs_3_0': {'o0': (rd.VarType.Float, 4)},
}


class D3D9_Shader_Debug_Zoo(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Shader_Debug_Zoo'
    internal = False

    def check_d3d9_capture(self):
        self.check_api()

        if not self.controller.GetAPIProperties().shaderDebugging:
            raise rdtest.TestFailureException("D3D9 doesn't say it can debug shaders")

        for marker, model, instances in DRAWS:
            self.check_draw(marker, model, instances)

        self.check_not_debuggable()

    def check_draw(self, marker, model, instances):
        draw = self.find_marker_draw(marker)
        self.set_event(draw.eventId, True)

        d3d9pipe = self.controller.GetD3D9PipelineState()
        if d3d9pipe.vertexShader.shaderModel != model:
            raise rdtest.TestFailureException(f"The {marker} draw's vertex shader is "
                                              f"{d3d9pipe.vertexShader.shaderModel}, expected {model}")

        pipe = self.controller.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        if not refl.debugInfo.debuggable:
            raise rdtest.TestFailureException(f"The {marker} vertex shader can't be debugged: "
                                              f"{refl.debugInfo.debugStatus}")

        # every vertex of every instance: the debugger's outputs equal VS Out exactly, and its
        # changes step forwards and backwards consistently (process_trace)
        vertices = 0
        for inst in range(instances):
            postvs = self.get_postvs(draw, rd.MeshDataStage.VSOut, instance=inst)
            if len(postvs) == 0:
                raise rdtest.TestFailureException(f"The {marker} draw has no VS Out data")
            for row in range(len(postvs)):
                self.check_vertex_debug(row, postvs[row]['idx'], inst, postvs)
                vertices += 1

        self.check_trace(marker, refl, draw)

        rdtest.log.success(f"{marker}: {vertices} vertices debugged, each ending with VS Out's values")

    def check_trace(self, marker, refl, draw):
        postvs = self.get_postvs(draw, rd.MeshDataStage.VSOut)

        # the disassembly the shader viewer shows, which instInfo's lines point into
        target = self.controller.GetDisassemblyTargets(True)[0]
        disasm = self.controller.DisassembleShader(rd.ResourceId.Null(), refl, target).split('\n')

        with self.debug_vertex(0, 0, postvs[0]['idx'], 0) as debug:
            trace = debug.trace
            if trace.stage != rd.ShaderStage.Vertex:
                raise rdtest.TestFailureException(f"{marker}: the trace's stage is {trace.stage}")

            states = self.controller.ContinueDebug(trace.debugger)
            if len(states) == 0:
                raise rdtest.TestFailureException(f"{marker}: no states")

            # every step's instruction is on a line of the disassembly, and declarations aren't
            # steps
            lines = {i.instruction: i.lineInfo.disassemblyLine for i in trace.instInfo}
            seen = []
            while len(states) > 0:
                for s in states:
                    line = lines.get(s.nextInstruction, 0)
                    if line < 1 or line > len(disasm):
                        raise rdtest.TestFailureException(f"{marker}: instruction {s.nextInstruction} is on line "
                                                          f"{line}, the disassembly has {len(disasm)}")
                    text = disasm[line - 1].strip()
                    if text == '' or text.startswith('//') or text.startswith('dcl') or text.startswith('def'):
                        raise rdtest.TestFailureException(f"{marker}: step {s.stepIndex} is on line {line}, "
                                                          f"'{text}'")
                    seen.append(text)
                states = self.controller.ContinueDebug(trace.debugger)

            rdtest.log.print(f"{marker}: the first steps are on {seen[0:3]}")

            # the first state lists every register the shader can change
            first = self.controller.ContinueDebug(trace.debugger)
            if len(first) != 0:
                raise rdtest.TestFailureException(f"{marker}: states after the end")

        expected = REGISTERS.get(marker)
        if expected is None:
            return

        with self.debug_vertex(0, 0, postvs[0]['idx'], 0) as debug:
            # m4x4 oPos, v0, c0 reads c0-c3, each listed by register
            constants = [v.name for v in debug.trace.sourceVars
                         if len(v.variables) > 0 and v.variables[0].type == rd.DebugVariableType.Constant]
            for c in ['c0', 'c1', 'c2', 'c3']:
                if c not in constants:
                    raise rdtest.TestFailureException(f"{marker}: the trace has no constant {c}, it has {constants}")

            states = self.controller.ContinueDebug(debug.trace.debugger)
            initial = {c.after.name: c.after for c in states[0].changes}
            for name, (vartype, columns) in expected.items():
                if name not in initial:
                    raise rdtest.TestFailureException(f"{marker}: the first state has no {name}, it has "
                                                      f"{sorted(initial.keys())}")
                var = initial[name]
                if var.type != vartype or var.columns != columns:
                    raise rdtest.TestFailureException(f"{marker}: {name} is {var.type} with {var.columns} columns, "
                                                      f"expected {vartype} with {columns}")
            rdtest.log.success(f"{marker}: the registers are named and sized by register: {sorted(expected.keys())}")

    def check_not_debuggable(self):
        # pixel shaders can't be debugged yet, and say so
        draw = self.find_marker_draw('vs_3_0')
        self.set_event(draw.eventId, True)
        ps = self.controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Pixel)
        if ps.debugInfo.debuggable or 'pixel' not in ps.debugInfo.debugStatus:
            raise rdtest.TestFailureException(f"The pixel shader's debug status is {ps.debugInfo.debuggable}, "
                                              f"'{ps.debugInfo.debugStatus}'")
        rdtest.log.success(f"The pixel shader can't be debugged: '{ps.debugInfo.debugStatus}'")

        # fixed function vertex processing isn't a shader
        draw = self.find_marker_draw('Fixed function')
        self.set_event(draw.eventId, True)
        vs = self.controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Vertex)
        if vs is None or vs.debugInfo.debuggable or 'Fixed function' not in vs.debugInfo.debugStatus:
            raise rdtest.TestFailureException("The fixed function draw's vertex processing doesn't say it can't "
                                              "be debugged")
        trace = self.controller.DebugVertex(0, 0, 0, 0)
        if trace.debugger is not None:
            raise rdtest.TestFailureException("A fixed function vertex was debugged")
        self.controller.FreeTrace(trace)
        rdtest.log.success(f"The fixed function draw can't be debugged: '{vs.debugInfo.debugStatus}'")
