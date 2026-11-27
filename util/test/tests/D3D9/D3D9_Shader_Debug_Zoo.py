import math
import struct
import renderdoc as rd
import rdtest

# each debugged draw: its marker, the shader model and how many instances it draws
DRAWS = [
    ('vs_1_1', 'vs_1_1', 1),
    ('vs_2_0', 'vs_2_0', 1),
    ('vs_2_x', 'vs_2_x', 1),
    ('vs_3_0', 'vs_3_0', 1),
    ('vs_3_0 instanced', 'vs_3_0', 2),
    ('vs_2_x calls', 'vs_2_x', 1),
    ('vs_3_0 names', 'vs_3_0', 1),
]

# the call stacks each draw's first vertex goes through
CALLSTACKS = {
    'vs_2_x': [['main'], ['main', 'l0']],
    'vs_2_x calls': [['main'], ['main', 'l1'], ['main', 'l1', 'l3'], ['main', 'l2']],
}

# the constant table's names in the names draw: rows, columns, type, and the registers and components
# the first and last of their values are in (as the variable "c[N]" and a component)
NAMES = {
    'wvp': (4, 4, rd.VarType.Float, ('c[0]', 0), ('c[3]', 3)),
    # row_major: a register per row
    'world': (4, 3, rd.VarType.Float, ('c[4]', 0), ('c[7]', 2)),
    'lights[0].colour': (1, 4, rd.VarType.Float, ('c[8]', 0), ('c[8]', 3)),
    'lights[0].dir': (1, 3, rd.VarType.Float, ('c[9]', 0), ('c[9]', 2)),
    'lights[1].range': (1, 1, rd.VarType.Float, ('c[13]', 0), ('c[13]', 0)),
    # column_major, but the compiler only allocates the two columns the shader reads
    'texMat': (4, 2, rd.VarType.Float, ('c[14]', 0), ('c[15]', 3)),
    'zero': (1, 1, rd.VarType.Float, ('c[18]', 0), ('c[18]', 0)),
    'flags[1]': (1, 1, rd.VarType.Bool, ('b[1]', 0), ('b[1]', 0)),
}

# values the application set, by name and component
NAME_VALUES = {
    ('world', 9): 0.05,
    ('lights[0].dir', 1): 0.8,
    ('lights[1].range', 0): 1.5,
    ('texMat', 7): 0.2,
}

# registers each draw's first vertex's trace has, with their type and size
REGISTERS = {
    'vs_1_1': {'r0': (rd.VarType.Float, 4), 'a0': (rd.VarType.SInt, 4), 'oPos': (rd.VarType.Float, 4),
               'oFog': (rd.VarType.Float, 1), 'oPts': (rd.VarType.Float, 1), 'oD1': (rd.VarType.Float, 4),
               'oT3': (rd.VarType.Float, 4)},
    'vs_2_x': {'aL': (rd.VarType.SInt, 1), 'p0': (rd.VarType.Bool, 4), 'oD0': (rd.VarType.Float, 4)},
    'vs_3_0': {'o0': (rd.VarType.Float, 4)},
}

# the pixel draws that can be debugged: their marker, the outputs they write, and how far each output
# may be from the GPU's (relative to its magnitude, at least 1), with the reason. The driver fuses a
# mul and the add after it, which the bytecode has apart (ps_2_0's mul r0, r0, r1.x and add r0, r0,
# r1), and a derivative of the result magnifies the difference. Lookups and transcendentals are made
# on the GPU, so the draws that only look up, or work out transcendentals of inputs, are bit for bit.
PIXEL_DRAWS = {
    'ps_2_0': (['oC0'], 1e-6, 'the driver fuses a mul and an add'),
    'ps_2_x': (['oC0', 'oC1'], 1e-5, 'the driver fuses a mul and an add, then ddx and ddy'),
    'ps_3_0': (['oC0', 'oC1', 'oDepth'], 1e-5, 'the driver fuses a mul and an add'),
    'ps_3_0 texture': (['oC0'], 0.0, ''),
    # fxc reduces sincos's argument to [-pi, pi] with a mad, frc and mad. The driver works out the
    # arguments its own way: at 2 of 7 pixels they're an ulp or two from the bytecode's, and sin of
    # 0.1 is then 50 ulps off. Of the inputs as they are (the asm draw), the GPU's results are exact.
    'ps_3_0 transcendentals': (['oC0', 'oC1'], 1e-6, 'the driver works out their arguments its own way'),
    'ps_2_0 lookups': (['oC0', 'oC1'], 0.0, ''),
    'ps_2_x gradients': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 anisotropic': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 cube': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 volume': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 border': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 sRGB': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 SetLOD': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 shadow': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 unbound': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 transcendentals asm': (['oC0', 'oC1'], 0.0, ''),
    # which fragment: overlapping layers in one draw, where the one written is decided by depth
    # writes and texkill, texkill, the alpha test, stencil writes, and the alpha test on an 8-bit
    # target (at whose precision it compares). The layer's id is in oC0.x and oC1.z.
    'ps_3_0 depth': (['oC0', 'oC1'], 0.0, ''),
    'ps_2_0 texkill': (['oC0', 'oC1'], 0.0, ''),
    'ps_2_x alpha test': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 stencil': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 alpha test 8-bit': (['oC0', 'oC1'], 0.0, ''),
    'ps_2_x point sprites': (['oC0', 'oC1'], 0.0, ''),
    'ps_3_0 centroid': (['oC0', 'oC1'], 0.0, ''),
    # depth layers on a 4x multisampled target, drawn alone (D3D9 has no multiple render targets
    # with multisampling): the fragment covering the pixel centre, shaded there. The probes are
    # inside the primitives, so the GPU's resolve is every sample's value.
    'ps_3_0 multisampled': (['oC0'], 0.0, ''),
}

# outputs drawn to an A8R8G8B8 target: the debugger's value is compared as the target stores it,
# rounded to 8 bits, and the native reference is c / 255
UNORM8_OUTPUTS = {
    'ps_3_0 alpha test 8-bit': ['oC0'],
}

# the lookups each sampling draw's pixel makes, by instruction
LOOKUPS = {
    'ps_2_0 lookups': ['texld', 'texldp', 'texldb'],
    'ps_2_x gradients': ['texldd', 'texldd'],
    'ps_3_0 cube': ['texld', 'texldb'],
    'ps_3_0 volume': ['texld', 'texldl'],
    'ps_3_0 shadow': ['texldp', 'texldb'],
}

# the D24S8 depth target's step
DEPTH_EPS = 1.5 / (1 << 24)


def bits(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def unorm8(v):
    """v as an 8-bit UNORM target stores it, read back as a float32"""
    c = math.floor(min(max(v, 0.0), 1.0) * 255.0 + 0.5)
    return struct.unpack('<f', struct.pack('<f', c / 255.0))[0]


def float_of(hexbits):
    return struct.unpack('<f', struct.pack('<I', int(hexbits, 16)))[0]


class D3D9_Shader_Debug_Zoo(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Shader_Debug_Zoo'
    internal = False

    def get_capture(self):
        # the native run writes what the GPU drew at the pixel draws' probes
        self.run_native()
        return super().get_capture()

    def check_d3d9_capture(self):
        self.check_api()

        if not self.controller.GetAPIProperties().shaderDebugging:
            raise rdtest.TestFailureException("D3D9 doesn't say it can debug shaders")

        for marker, model, instances in DRAWS:
            self.check_draw(marker, model, instances)

        self.read_pixel_reference()
        for marker in PIXEL_DRAWS:
            if marker not in self.pixel_reference:
                # the demo leaves out what the GPU can't do, and says so
                rdtest.log.print(f"{marker}: not drawn natively, the GPU can't")
                continue
            self.check_pixel_draw(marker)
        self.check_pixel_trace()
        for marker in LOOKUPS:
            if marker in self.pixel_reference:
                self.check_lookup_trace(marker)

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
        self.check_flow(marker, draw)
        if marker == 'vs_3_0 names':
            self.check_names(draw)

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
            # m4x4 oPos, v0, c0 reads c0-c3, each listed by register (or under Registers, when the
            # constant table names them)
            constants = [v.name.replace('Registers.', '') for v in debug.trace.sourceVars
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

    def all_states(self, debugger):
        ret = []
        states = self.controller.ContinueDebug(debugger)
        while len(states) > 0:
            ret += states
            states = self.controller.ContinueDebug(debugger)
        return ret

    def check_flow(self, marker, draw):
        """Each state's call stack ends with the subroutine (label) its next instruction is in, and
        only texldl samples and only an instruction writing a NaN or infinity says it did."""
        postvs = self.get_postvs(draw, rd.MeshDataStage.VSOut)
        refl = self.controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Vertex)
        target = self.controller.GetDisassemblyTargets(True)[0]
        disasm = self.controller.DisassembleShader(rd.ResourceId.Null(), refl, target).split('\n')

        # the subroutine each line is in: main until the first label
        section = []
        current = 'main'
        for line in disasm:
            text = line.strip()
            if text.startswith('label '):
                current = text.split()[1]
            section.append(current)

        with self.debug_vertex(0, 0, postvs[0]['idx'], 0) as debug:
            trace = debug.trace
            lines = {i.instruction: i.lineInfo.disassemblyLine for i in trace.instInfo}
            states = self.all_states(trace.debugger)

            stacks = []
            samples = 0
            infinities = 0
            for n, s in enumerate(states):
                stack = list(s.callstack)
                if len(stack) == 0 or stack[0] != 'main':
                    raise rdtest.TestFailureException(f"{marker}: step {s.stepIndex}'s call stack is {stack}")
                expected = section[lines[s.nextInstruction] - 1]
                if stack[-1] != expected:
                    raise rdtest.TestFailureException(f"{marker}: step {s.stepIndex} is in {expected}, but its call "
                                                      f"stack is {stack}")
                if stack not in stacks:
                    stacks.append(stack)

                # what ran to get to this state
                ran = disasm[lines[states[n - 1].nextInstruction] - 1].strip() if n > 0 else ''
                sampled = bool(s.flags & rd.ShaderEvents.SampleLoadGather)
                if sampled != ran.startswith('texldl'):
                    raise rdtest.TestFailureException(f"{marker}: step {s.stepIndex} ran '{ran}', and its sample "
                                                      f"flag is {sampled}")
                samples += 1 if sampled else 0

                # a NaN or infinity in a component the instruction wrote, as its disassembly says
                text = ran
                if text.startswith('('):
                    text = text[text.index(')') + 1:].strip()
                parts = text.split(None, 1)
                dst = parts[1].split(',')[0].strip() if len(parts) > 1 else ''
                dstname, _, mask = dst.partition('.')
                comps = ['xyzw'.index(m) for m in mask] if mask else [0, 1, 2, 3]
                bad = False
                for c in s.changes:
                    if n == 0 or c.after.type != rd.VarType.Float or c.after.name != dstname:
                        continue
                    for comp in comps:
                        if comp < c.after.columns:
                            f = c.after.value.f32v[comp]
                            bad = bad or math.isnan(f) or math.isinf(f)
                naninf = bool(s.flags & rd.ShaderEvents.GeneratedNanOrInf)
                if naninf != bad:
                    raise rdtest.TestFailureException(f"{marker}: step {s.stepIndex} ran '{ran}', and its NaN/Inf "
                                                      f"flag is {naninf}")
                infinities += 1 if naninf else 0

            if list(states[-1].callstack) != ['main']:
                raise rdtest.TestFailureException(f"{marker}: the last state's call stack is {states[-1].callstack}")

            if marker in CALLSTACKS and sorted(stacks) != sorted(CALLSTACKS[marker]):
                raise rdtest.TestFailureException(f"{marker}: the call stacks are {stacks}, expected "
                                                  f"{CALLSTACKS[marker]}")
            expected_samples = 1 if marker == 'vs_3_0' else 0
            if samples != expected_samples:
                raise rdtest.TestFailureException(f"{marker}: {samples} steps sampled, expected {expected_samples}")
            # rcp r0.z, c18.x (zero), then mul r0.z, r0.z, c16.x (1) before the min
            expected_infinities = 2 if marker == 'vs_3_0 names' else 0
            if infinities != expected_infinities:
                raise rdtest.TestFailureException(f"{marker}: {infinities} steps made a NaN or infinity, expected "
                                                  f"{expected_infinities}")

            rdtest.log.success(f"{marker}: call stacks {stacks}, {samples} sample(s), {infinities} NaN or infinity")

    def check_names(self, draw):
        """The constant table's names, on the registers the compiler allocated them, the literals
        marked, and the table's registers again by register."""
        postvs = self.get_postvs(draw, rd.MeshDataStage.VSOut)
        with self.debug_vertex(0, 0, postvs[0]['idx'], 0) as debug:
            trace = debug.trace
            sourcevars = {v.name: v for v in trace.sourceVars}
            constants = [v.name for v in trace.sourceVars
                         if len(v.variables) > 0 and v.variables[0].type == rd.DebugVariableType.Constant]
            rdtest.log.print(f"The names draw's constants: {constants}")

            registers = {}
            for cb in trace.constantBlocks:
                for m in cb.members:
                    registers[cb.name + m.name] = m

            for name, (rows, columns, vartype, first, last) in NAMES.items():
                if name not in sourcevars:
                    raise rdtest.TestFailureException(f"There's no constant {name}")
                v = sourcevars[name]
                refs = [(r.name, r.component) for r in v.variables]
                if (v.rows, v.columns, v.type) != (rows, columns, vartype) or refs[0] != first or refs[-1] != last:
                    raise rdtest.TestFailureException(f"{name} is {v.rows}x{v.columns} {v.type} in {refs}, expected "
                                                      f"{rows}x{columns} {vartype} from {first} to {last}")
                for r, comp in refs:
                    if r not in registers:
                        raise rdtest.TestFailureException(f"{name} is in {r}, which the trace doesn't have")
                    alias = f"Registers.{r[0]}{r[2:-1]}"
                    if alias not in sourcevars:
                        raise rdtest.TestFailureException(f"{name}'s register {r} isn't listed as {alias}")

            for (name, index), expected in NAME_VALUES.items():
                r, comp = [(r.name, r.component) for r in sourcevars[name].variables][index]
                value = registers[r].value.f32v[comp]
                if not rdtest.value_compare(value, expected):
                    raise rdtest.TestFailureException(f"{name}'s component {index} is {value}, expected {expected}")

            # partly allocated and unallocated members aren't shown: texMat has two columns, and
            # nothing is lights[2]
            for name in ['lights[2].colour', 'lights', 'Light', 'flags']:
                if name in sourcevars:
                    raise rdtest.TestFailureException(f"There's a constant called {name}")

            # the compiler's literals (the 4 in min(1 / zero, 4), and others) are marked, and aren't
            # listed by register too
            literals = [n for n in constants if n.startswith('Literals.')]
            if len(literals) == 0:
                raise rdtest.TestFailureException(f"No literals are marked in {constants}")
            for n in literals:
                if n.replace('Literals.', '') in sourcevars:
                    raise rdtest.TestFailureException(f"{n} is also listed by register")

            rdtest.log.success(f"The names draw's constants are named: {sorted(NAMES.keys())}, literals {literals}")

    def read_pixel_reference(self):
        """What the GPU wrote natively at each pixel draw's probes: both targets' values."""
        self.pixel_reference = {}
        with open(self.demo_tmp_path('D3D9_Shader_Debug_Zoo_pixels.txt')) as f:
            for line in f:
                parts = line.rstrip('\n').split('\t')
                if len(parts) != 11:
                    continue
                values = [float_of(h) for h in parts[3:]]
                self.pixel_reference.setdefault(parts[0], []).append(
                    ((int(parts[1]), int(parts[2])), values[0:4], values[4:8]))
        if len(self.pixel_reference) == 0:
            raise rdtest.TestFailureException("The native run wrote no pixel reference")

    def debug_outputs(self, trace, variables):
        """The debugged pixel's outputs by register: oC# and oDepth, through the output signature."""
        refl = self.controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Pixel)
        ret = {}
        for i, sig in enumerate(refl.outputSignature):
            source = self.find_source_var(trace.sourceVars, i, rd.DebugVariableType.Variable)
            if source is None:
                raise rdtest.TestFailureException(f"The trace has no source variable for {sig.semanticIdxName}")
            value = self.evaluate_source_var(source, variables).value.f32v
            if sig.systemValue == rd.ShaderBuiltin.DepthOutput:
                ret['oDepth'] = [value[0]]
            else:
                ret[f'oC{sig.regIndex}'] = list(value[0:4])
        return ret

    def check_pixel_draw(self, marker):
        """Each probe of the draw: the debugged pixel's outputs against the replay's targets and the
        GPU's native ones, and its changes step forwards and backwards consistently."""
        outputs, tolerance, why = PIXEL_DRAWS[marker]
        draw = self.find_marker_draw(marker)
        self.set_event(draw.eventId, True)

        pipe = self.controller.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
        if not refl.debugInfo.debuggable:
            raise rdtest.TestFailureException(f"The {marker} pixel shader can't be debugged: "
                                              f"{refl.debugInfo.debugStatus}")
        targets = [t.resource for t in pipe.GetOutputTargets()]
        depth = pipe.GetDepthTarget().resource

        worst = 0.0
        exact = 0
        compared = 0
        inexact = []
        for (x, y), native0, native1 in self.pixel_reference[marker]:
            with self.debug_pixel(x, y, rd.DebugPixelInputs()) as debug:
                if debug.trace.debugger is None:
                    raise rdtest.TestFailureException(f"{marker}: pixel ({x}, {y}) couldn't be debugged")
                if debug.trace.stage != rd.ShaderStage.Pixel:
                    raise rdtest.TestFailureException(f"{marker}: the trace's stage is {debug.trace.stage}")
                cycles, variables = self.process_trace(debug.trace)
                got = self.debug_outputs(debug.trace, variables)

            for name in outputs:
                if name not in got:
                    raise rdtest.TestFailureException(f"{marker}: pixel ({x}, {y}) has no {name}, only "
                                                      f"{sorted(got.keys())}")

                if name == 'oDepth':
                    # the depth target holds 24 bits; natively oC1 doesn't have it, the replay's depth does
                    picked = self.controller.PickPixel(depth, x, y, rd.Subresource(0, 0, 0),
                                                       rd.CompType.Typeless).floatValue[0]
                    if abs(picked - got[name][0]) > DEPTH_EPS:
                        raise rdtest.TestFailureException(f"{marker}: pixel ({x}, {y})'s oDepth is {got[name][0]}, "
                                                          f"the depth target has {picked}")
                    continue

                if name in UNORM8_OUTPUTS.get(marker, []):
                    got[name] = [unorm8(v) for v in got[name]]

                index = int(name[2:])
                replay = list(self.controller.PickPixel(targets[index], x, y, rd.Subresource(0, 0, 0),
                                                        rd.CompType.Typeless).floatValue)
                native = native0 if index == 0 else native1

                # the replay draws what the GPU drew natively, bit for bit
                if [bits(v) for v in replay] != [bits(v) for v in native]:
                    raise rdtest.TestFailureException(f"{marker}: pixel ({x}, {y})'s {name} at replay is {replay}, "
                                                      f"natively {native}")

                for c in range(4):
                    compared += 1
                    if bits(got[name][c]) == bits(native[c]):
                        exact += 1
                        continue
                    diff = abs(got[name][c] - native[c]) / max(1.0, abs(native[c]))
                    worst = max(worst, diff)
                    inexact.append(f"({x}, {y}) {name}.{'xyzw'[c]} {got[name][c]:.9g} vs {native[c]:.9g} "
                                   f"({abs(bits(got[name][c]) - bits(native[c]))} ulps)")
                    if diff > tolerance:
                        raise rdtest.TestFailureException(
                            f"{marker}: pixel ({x}, {y})'s {name} is {got[name]}, the GPU's is {native} "
                            f"(component {c} is {diff} off, more than {tolerance})")

        for line in inexact:
            rdtest.log.print(f"{marker}: {line}")
        probes = len(self.pixel_reference[marker])
        rdtest.log.success(f"{marker}: {probes} pixels debugged, {exact} of {compared} output components "
                           f"bit for bit the GPU's" +
                           (f", the others at most {worst:.3g} off ({why})" if exact < compared else ""))

    def check_pixel_trace(self):
        """The ps_3_0 draw's trace: its inputs (vPos the pixel, vFace +1 or -1 by the triangle's
        winding), the registers it changes, and steps on its instructions."""
        draw = self.find_marker_draw('ps_3_0')
        self.set_event(draw.eventId, True)
        refl = self.controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Pixel)
        target = self.controller.GetDisassemblyTargets(True)[0]
        disasm = self.controller.DisassembleShader(rd.ResourceId.Null(), refl, target).split('\n')

        # (223, 37) is in the clockwise triangle, (270, 100) in the other
        for (x, y), face in [((223, 37), 1.0), ((270, 100), -1.0)]:
            with self.debug_pixel(x, y, rd.DebugPixelInputs()) as debug:
                trace = debug.trace
                inputs = {v.name: v for v in trace.inputs}
                for name in ['vPos', 'vFace', 'v0']:
                    if name not in inputs:
                        raise rdtest.TestFailureException(f"The ps_3_0 trace has no input {name}: {sorted(inputs.keys())}")
                # the inputs by register too, as the disassembly names them, for watches and hovering
                aliases = [v.name for v in trace.sourceVars
                           if len(v.variables) > 0 and v.variables[0].type == rd.DebugVariableType.Input]
                for name in ['Registers.vPos', 'Registers.vFace', 'Registers.v0']:
                    if name not in aliases:
                        raise rdtest.TestFailureException(f"The ps_3_0 trace has no {name}: {aliases}")
                pos = inputs['vPos']
                if pos.columns != 2 or list(pos.value.f32v[0:2]) != [float(x), float(y)]:
                    raise rdtest.TestFailureException(f"Pixel ({x}, {y})'s vPos is {list(pos.value.f32v[0:pos.columns])}")
                if inputs['vFace'].columns != 1 or inputs['vFace'].value.f32v[0] != face:
                    raise rdtest.TestFailureException(f"Pixel ({x}, {y})'s vFace is {inputs['vFace'].value.f32v[0]}, "
                                                      f"expected {face}")

                lines = {i.instruction: i.lineInfo.disassemblyLine for i in trace.instInfo}
                states = self.all_states(trace.debugger)
                initial = {c.after.name: c.after for c in states[0].changes}
                for name, columns in [('oC0', 4), ('oC1', 4), ('oDepth', 1), ('r0', 4)]:
                    if name not in initial or initial[name].columns != columns:
                        raise rdtest.TestFailureException(f"The ps_3_0 trace's first state has no {name} of {columns} "
                                                          f"columns: {sorted(initial.keys())}")
                for st in states:
                    line = lines.get(st.nextInstruction, 0)
                    text = disasm[line - 1].strip() if 0 < line <= len(disasm) else ''
                    if text == '' or text.startswith('//') or text.startswith('dcl') or text.startswith('def'):
                        raise rdtest.TestFailureException(f"ps_3_0: step {st.stepIndex} is on line {line}, '{text}'")
                    if st.flags & rd.ShaderEvents.SampleLoadGather:
                        raise rdtest.TestFailureException(f"ps_3_0: step {st.stepIndex} says it sampled")
                if len(states) < 10:
                    raise rdtest.TestFailureException(f"The ps_3_0 pixel took only {len(states)} steps")

        rdtest.log.success("ps_3_0: vPos and vFace are the pixels', the inputs are listed by register too, the "
                           "outputs are listed, and every step is on an instruction")

    def check_lookup_trace(self, marker):
        """A sampling draw's trace: the steps that ran a lookup, and only those, say they sampled. It's
        stepped with the replay at another event, as the UI may: its lookups were made when the pixel
        was debugged, so its outputs are still the GPU's."""
        draw = self.find_marker_draw(marker)
        self.set_event(draw.eventId, True)
        refl = self.controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Pixel)
        target = self.controller.GetDisassemblyTargets(True)[0]
        disasm = self.controller.DisassembleShader(rd.ResourceId.Null(), refl, target).split('\n')

        (x, y), native0, native1 = self.pixel_reference[marker][0]
        with self.debug_pixel(x, y, rd.DebugPixelInputs()) as debug:
            lines = {i.instruction: i.lineInfo.disassemblyLine for i in debug.trace.instInfo}
            # the textures there are what later draws made of them, and the samplers aren't the draw's
            self.set_event(self.find_marker_draw('Fixed function').eventId, True)
            states = self.all_states(debug.trace.debugger)

            outputs = {}
            for st in states:
                for c in st.changes:
                    if c.after.name in ('oC0', 'oC1'):
                        outputs[c.after.name] = list(c.after.value.f32v[0:4])
            for name, native in [('oC0', native0), ('oC1', native1)]:
                if [bits(v) for v in outputs.get(name, [])] != [bits(v) for v in native]:
                    raise rdtest.TestFailureException(f"{marker}: stepped at another event, pixel ({x}, {y})'s "
                                                      f"{name} is {outputs.get(name)}, the GPU's is {native}")

            sampled = []
            for n, st in enumerate(states):
                ran = disasm[lines[states[n - 1].nextInstruction] - 1].strip() if n > 0 else ''
                op = ran.split()[0] if ran else ''
                flagged = bool(st.flags & rd.ShaderEvents.SampleLoadGather)
                if flagged != op.startswith('tex'):
                    raise rdtest.TestFailureException(f"{marker}: step {st.stepIndex} ran '{ran}', and its sample "
                                                      f"flag is {flagged}")
                if flagged:
                    sampled.append(op)
            if sorted(sampled) != sorted(LOOKUPS[marker]):
                raise rdtest.TestFailureException(f"{marker}: the steps that sampled ran {sampled}, expected "
                                                  f"{LOOKUPS[marker]}")
        rdtest.log.success(f"{marker}: the steps that sampled are its lookups, {sampled}, and stepped at "
                           f"another event its outputs are the GPU's")

    def check_not_debuggable(self):
        # ps_1_x pixel shaders can't be debugged yet, and say so
        for marker, words, (x, y) in [('ps_1_1', 'ps_1_x', (40, 210))]:
            draw = self.find_marker_draw(marker)
            self.set_event(draw.eventId, True)
            ps = self.controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Pixel)
            if ps.debugInfo.debuggable or words not in ps.debugInfo.debugStatus:
                raise rdtest.TestFailureException(f"The {marker} pixel shader's debug status is "
                                                  f"{ps.debugInfo.debuggable}, '{ps.debugInfo.debugStatus}'")
            trace = self.controller.DebugPixel(x, y, rd.DebugPixelInputs())
            if trace.debugger is not None:
                raise rdtest.TestFailureException(f"A pixel of the {marker} draw was debugged")
            self.controller.FreeTrace(trace)
            rdtest.log.success(f"The {marker} pixel shader can't be debugged: '{ps.debugInfo.debugStatus}'")

        # a pixel the draw doesn't cover has nothing to debug
        draw = self.find_marker_draw('ps_2_0')
        self.set_event(draw.eventId, True)
        trace = self.controller.DebugPixel(5, 5, rd.DebugPixelInputs())
        if trace.debugger is not None:
            raise rdtest.TestFailureException("A pixel outside the ps_2_0 draw was debugged")
        self.controller.FreeTrace(trace)
        rdtest.log.success("A pixel outside the draw isn't debugged")

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
        # nor is fixed function pixel processing, at a pixel the triangle covers
        trace = self.controller.DebugPixel(350, 270, rd.DebugPixelInputs())
        if trace.debugger is not None:
            raise rdtest.TestFailureException("A fixed function pixel was debugged")
        self.controller.FreeTrace(trace)
        rdtest.log.success(f"The fixed function draw can't be debugged: '{vs.debugInfo.debugStatus}'")
