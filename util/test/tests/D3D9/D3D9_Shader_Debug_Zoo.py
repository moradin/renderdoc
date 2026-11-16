import math
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
