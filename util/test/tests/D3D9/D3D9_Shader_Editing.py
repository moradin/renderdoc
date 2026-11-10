import os
import struct

import renderdoc as rd
import rdtest

# the demo's draws, in order (d3d9_shader_editing.cpp), and the cells their quads are in
MARKERS = ['Frame Start', 'HLSL 3_0', 'HLSL 2_0', 'SM1', 'ps_1_4', 'State Block', 'Shared', 'FFP Pixel',
           'In Frame']

ENCODINGS = {
    'HLSL': rd.ShaderEncoding.HLSL,
    'D3D9Asm': rd.ShaderEncoding.D3D9Asm,
    'D3D9Bytecode': rd.ShaderEncoding.D3D9Bytecode,
}

STAGES = {'vs': rd.ShaderStage.Vertex, 'ps': rd.ShaderStage.Pixel}

# the scene is A8R8G8B8, the reference is 8 bit values as floats
EPS = 1.5 / 255.0


def instructions(raw: bytes):
    """A shader's tokens without its comment blocks (the constant table, the compiler's name)."""
    tokens = struct.unpack(f'<{len(raw) // 4}I', raw)
    ret = [tokens[0]]
    i = 1
    while i < len(tokens):
        if tokens[i] & 0xFFFF == 0xFFFE:
            i += 1 + ((tokens[i] >> 16) & 0x7FFF)
            continue
        ret.append(tokens[i])
        i += 1
    return ret


class D3D9_Shader_Editing(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Shader_Editing'
    internal = False

    def get_capture(self):
        # the native run writes the reference and the edits
        self.run_native()
        return super().get_capture()

    def build(self, stage, encoding, source: bytes, profile=None):
        flags = rd.ShaderCompileFlags()
        if profile:
            flag = rd.ShaderCompileFlag()
            flag.name = '@cmdline'
            flag.value = f'/T {profile}'
            flags.flags = [flag]
        return self.controller.BuildTargetShader('main', encoding, source, flags, stage)

    def picks(self):
        self.controller.SetFrameEvent(self.last.eventId, True)
        return {p: list(self.controller.PickPixel(self.scene, p[0], p[1], rd.Subresource(0, 0, 0),
                                                  rd.CompType.Typeless).floatValue)
                for p in self.probes}

    def compare(self, name: str, problems: list):
        got = self.picks()
        bad = [f"{p}: {got[p]}, natively {self.reference[name][p]}" for p in self.probes
               if not rdtest.value_compare(got[p], self.reference[name][p], eps=EPS)]
        for b in bad[:4]:
            problems.append(f"{name}: {b}")
        if len(bad) > 4:
            problems.append(f"{name}: and {len(bad) - 4} more texels differ")
        return len(bad) == 0

    def shader_at(self, marker: str, stage: str):
        self.controller.SetFrameEvent(self.draws[marker].eventId, True)
        pipe = self.controller.GetD3D9PipelineState()
        sh = pipe.vertexShader if stage == 'vs' else pipe.pixelShader
        return sh.resourceId, sh.reflection

    def vs_out_x(self, marker: str):
        draw = self.draws[marker]
        self.controller.SetFrameEvent(draw.eventId, True)
        data = self.get_postvs(draw, rd.MeshDataStage.VSOut)
        key = [k for k in data[0].keys() if k.upper().startswith('POSITION')]
        if not key:
            raise rdtest.TestFailureException(f"No position in {marker}'s VS Out: {list(data[0].keys())}")
        return [v[key[0]][0] for v in data]

    def check_d3d9_capture(self):
        self.check_api()

        encodings = self.controller.GetTargetShaderEncodings()
        for e in ENCODINGS.values():
            if e not in encodings:
                raise rdtest.TestFailureException(f"Target shader encodings are {encodings}, {e} is missing")
        rdtest.log.success(f"Shaders can be built from {[rd.ShaderEncoding(e).name for e in encodings]}")

        self.draws = {m: self.find_marker_draw(m) for m in MARKERS}
        self.last = self.draws[MARKERS[-1]]

        scenes = [t for t in self.controller.GetTextures() if (t.width, t.height) == (288, 32)]
        if len(scenes) != 1:
            raise rdtest.TestFailureException(f"Expected one 288x32 scene target, found {len(scenes)}")
        self.scene = scenes[0].resourceId

        # the native reference: what the GPU drew as captured, and with each edit
        self.reference = {}
        with open(self.demo_tmp_path('D3D9_Shader_Editing_reference.txt')) as f:
            for line in f:
                name, x, y, r, g, b, a = line.rstrip('\n').split('\t')
                self.reference.setdefault(name, {})[(int(x), int(y))] = [float(r), float(g), float(b), float(a)]
        self.probes = list(self.reference['None'].keys())

        edits = []
        with open(self.demo_tmp_path('D3D9_Shader_Editing_edits.txt')) as f:
            for line in f:
                edits.append(line.rstrip('\n').split('\t'))

        problems = []

        if self.compare('None', problems):
            rdtest.log.success(f"Unedited, the {len(self.probes)} texels match the GPU's native rendering")

        # every shader's disassembly assembles back to the same instructions
        originals = {}
        for m in MARKERS:
            for stage in ['vs', 'ps']:
                id, refl = self.shader_at(m, stage)
                if id != rd.ResourceId.Null() and id not in originals:
                    originals[id] = (f"{m}'s {stage}", refl)
        for id, (label, refl) in originals.items():
            disasm = self.controller.DisassembleShader(rd.ResourceId.Null(), refl, '')
            model = [l.strip() for l in disasm.split('\n') if l.strip() and not l.strip().startswith('//')][0]
            label = f"{label} ({model})"
            new, errors = self.build(refl.stage, rd.ShaderEncoding.D3D9Asm, disasm.encode('utf-8'))
            if new == rd.ResourceId.Null():
                problems.append(f"{label}: its disassembly doesn't assemble: {errors}")
                continue
            rebuilt = self.controller.GetShader(rd.ResourceId.Null(), new,
                                                rd.ShaderEntryPoint('main', refl.stage))
            if instructions(bytes(rebuilt.rawBytes)) != instructions(bytes(refl.rawBytes)):
                problems.append(f"{label}: its disassembly assembles to different instructions")
            else:
                rdtest.log.success(f"{label}: its disassembly assembles back to the same instructions")
            self.controller.FreeTargetResource(new)

        # each edit, built and replacing the original, against the GPU drawing with the edit natively
        for name, marker, stage, encoding, profile, file in edits:
            mode = 'rb' if encoding == 'D3D9Bytecode' else 'r'
            with open(self.demo_tmp_path(file), mode) as f:
                source = f.read()
            if mode == 'r':
                source = source.encode('utf-8')

            original, refl = self.shader_at(marker, stage)
            before_x = self.vs_out_x(marker) if stage == 'vs' else None

            new, errors = self.build(STAGES[stage], ENCODINGS[encoding], source,
                                     profile if profile != '-' else None)
            if new == rd.ResourceId.Null():
                problems.append(f"{name}: didn't build: {errors}")
                continue

            self.controller.ReplaceResource(original, new)

            if self.compare(name, problems):
                rdtest.log.success(f"{name}: the {len(self.probes)} texels match the GPU's native rendering with the edit")

            # the pipeline state has the original's ID, and what's bound's reflection
            id, bound = self.shader_at(marker, stage)
            if id != original or bound is None or bound.resourceId != new:
                problems.append(f"{name}: the pipeline state has {id} with {bound.resourceId if bound else None}'s "
                                f"reflection, expected {original} with {new}'s")

            # mesh output runs the edited vertex shader, which moves the quad a quarter to the right
            if stage == 'vs' and marker in ['HLSL 3_0', 'SM1']:
                after_x = self.vs_out_x(marker)
                quarter = 0.25 * (max(before_x) - min(before_x))
                expected = [x + quarter for x in before_x]
                if not rdtest.value_compare(after_x, expected, eps=1e-5):
                    problems.append(f"{name}: VS Out x is {after_x}, expected {expected}")
                else:
                    rdtest.log.success(f"{name}: VS Out positions are the edited shader's")

            self.controller.RemoveReplacement(original)
            if not self.compare('None', problems):
                problems.append(f"{name}: removing the replacement didn't restore the original")
            self.controller.FreeTargetResource(new)

        rdtest.log.success("Removing each replacement restores the original rendering")

        # what isn't built or replaced
        ps_id, ps_refl = self.shader_at('HLSL 2_0', 'ps')
        vs_id, vs_refl = self.shader_at('HLSL 2_0', 'vs')
        failures = [
            ("an HLSL error", rd.ShaderStage.Pixel, rd.ShaderEncoding.HLSL,
             b'float4 main() : COLOR0 { return undefined; }', None, "error X3004: undeclared identifier"),
            ("an assembly error", rd.ShaderStage.Pixel, rd.ShaderEncoding.D3D9Asm,
             b'ps_2_0\nmov oC0, r99\n', None, 'error X'),
            ("HLSL for ps_1_1", rd.ShaderStage.Pixel, rd.ShaderEncoding.HLSL,
             b'float4 main() : COLOR0 { return 1; }', 'ps_1_1', 'ps_1_x is no longer supported'),
            ("a vertex profile for a pixel shader", rd.ShaderStage.Pixel, rd.ShaderEncoding.HLSL,
             b'float4 main() : COLOR0 { return 1; }', 'vs_3_0', "isn't a pixel shader profile"),
            ("vertex shader bytecode for a pixel shader", rd.ShaderStage.Pixel, rd.ShaderEncoding.D3D9Bytecode,
             bytes(vs_refl.rawBytes), None, 'not a pixel shader'),
            ("bytecode with no end token", rd.ShaderStage.Pixel, rd.ShaderEncoding.D3D9Bytecode,
             bytes(ps_refl.rawBytes)[:-4], None, 'no end token'),
            ("a geometry shader", rd.ShaderStage.Geometry, rd.ShaderEncoding.HLSL,
             b'float4 main() : COLOR0 { return 1; }', None, 'only has vertex and pixel shaders'),
        ]
        for what, stage, encoding, source, profile, expect in failures:
            new, errors = self.build(stage, encoding, source, profile)
            if new != rd.ResourceId.Null() or expect not in errors:
                problems.append(f"{what} built as {new}, errors '{errors}'")
            else:
                line = [l for l in errors.splitlines() if expect in l][0]
                rdtest.log.success(f"{what} doesn't build: {line[line.find('error X'):] if 'error X' in line else line}")

        # HLSL with no profile is shader model 3
        new, errors = self.build(rd.ShaderStage.Pixel, rd.ShaderEncoding.HLSL,
                                 b'float4 main() : COLOR0 { return 1; }')
        refl = self.controller.GetShader(rd.ResourceId.Null(), new, rd.ShaderEntryPoint('main', rd.ShaderStage.Pixel))
        if new == rd.ResourceId.Null() or bytes(refl.rawBytes)[:4] != bytes([0, 3, 0xFF, 0xFF]):
            problems.append(f"HLSL with no profile built as {new} ({errors})")
        else:
            rdtest.log.success("HLSL with no profile is compiled as ps_3_0")

        # a pixel shader can't replace a vertex shader, the replacement is ignored
        self.controller.ReplaceResource(vs_id, new)
        if self.compare('None', problems):
            rdtest.log.success("A pixel shader doesn't replace a vertex shader")
        self.controller.RemoveReplacement(vs_id)
        self.controller.FreeTargetResource(new)

        # freeing a replacement removes it
        with open(self.demo_tmp_path(edits[2][5]), 'rb') as f:
            new, errors = self.build(rd.ShaderStage.Pixel, rd.ShaderEncoding.D3D9Bytecode, f.read())
        self.controller.ReplaceResource(ps_id, new)
        self.controller.FreeTargetResource(new)
        if self.compare('None', problems):
            rdtest.log.success("Freeing a replacement shader removes the replacement")

        for p in problems:
            rdtest.log.error(p)
        if problems:
            raise rdtest.TestFailureException(f"{len(problems)} problems with shader editing")
