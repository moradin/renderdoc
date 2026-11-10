import os

import renderdoc as rd
import rdtest

# the demo's textures (d3d9_custom_shaders.cpp): width, height, mips, depth or array size, and what
# RD_TextureType() returns for them on D3D9
TEXTURES = {
    'Default 2D': (64, 32, 3, 1, 2),
    'Managed 2D': (48, 24, 2, 1, 2),
    'L8': (16, 8, 1, 1, 2),
    'Cube': (16, 16, 2, 6, 8),
    'Volume': (16, 8, 2, 4, 3),
    'Render Target': (40, 20, 1, 1, 2),
    'Depth': (40, 20, 1, 1, 5),
}

# what the texture viewer's "new custom shader" button writes for HLSL
UI_TEMPLATE = '''float4 main(float4 pos : SV_Position, float4 uv : TEXCOORD0) : SV_Target0
{
    return float4(0,0,0,1);
}
'''

# the prefix's values at pixels 0-4 of the row
CONSTANTS_SHADER = '''
float4 userValue;

float4 main(float4 pos : SV_Position, float4 uv : TEXCOORD0) : SV_Target0
{
  if(pos.x < 0.5f)
    return float4(RD_TexDim());
  if(pos.x < 1.5f)
    return float4(RD_SelectedMip(), RD_SelectedSliceFace(), RD_SelectedSample(), RD_TextureType());
  if(pos.x < 2.5f)
    return float4(RD_SelectedRange(), 0, 0);
  if(pos.x < 3.5f)
    return float4(RD_YUVDownsampleRate()) + float4(RD_YUVAChannels());
  return userValue;
}
'''

# HLSL's texture and sampler objects, which fxc puts in a sampler register of its own choosing
OBJECTS_SHADER = '''
Texture2D tex : register(t0);
SamplerState pointSampler : register(s0);

float4 main(float4 pos : SV_Position, float4 uv : TEXCOORD0) : SV_Target0
{
  return tex.SampleLevel(pointSampler, uv.xy, RD_SelectedMip());
}
'''

# the output is A16B16G16R16F
EPS = 1.0 / 1024.0


class D3D9_Custom_Shaders(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Custom_Shaders'
    internal = False

    def get_capture(self):
        # the native run writes the reference and the shaders
        self.run_native()
        return super().get_capture()

    def build(self, source: str, flags=None):
        if flags is None:
            flags = rd.ShaderCompileFlags()
        return self.controller.BuildCustomShader('main', rd.ShaderEncoding.HLSL,
                                                 (self.prefix + source).encode('utf-8'), flags,
                                                 rd.ShaderStage.Pixel)

    def build_ok(self, name: str, source: str):
        shader, errors = self.build(source)
        if shader == rd.ResourceId.Null():
            raise rdtest.TestFailureException(f"The {name} shader didn't build: {errors}")
        rdtest.log.success(f"The {name} shader builds")
        return shader

    def apply(self, shader, tex, mip: int, slice: int, rangeMin=0.0, rangeMax=1.0):
        disp = rd.TextureDisplay()
        disp.resourceId = tex
        disp.subresource = rd.Subresource(mip, slice, 0)
        disp.customShaderId = shader
        disp.rangeMin = rangeMin
        disp.rangeMax = rangeMax
        self.out.SetTextureDisplay(disp)
        self.out.Display()
        custom = self.out.GetCustomShaderTexID()
        if custom == rd.ResourceId.Null():
            raise rdtest.TestFailureException(f"No custom shader output for {tex} mip {mip} slice {slice}")
        return custom

    def pick(self, custom, mip: int, x: int, y: int):
        return list(self.controller.PickPixel(custom, x, y, rd.Subresource(mip, 0, 0),
                                              rd.CompType.Typeless).floatValue)

    def check_d3d9_capture(self):
        self.check_api()

        encodings = self.controller.GetCustomShaderEncodings()
        if rd.ShaderEncoding.HLSL not in encodings or rd.ShaderEncoding.D3D9Bytecode not in encodings:
            raise rdtest.TestFailureException(f"Custom shader encodings are {encodings}")
        prefixes = [p for p in self.controller.GetCustomShaderSourcePrefixes()
                    if p.encoding == rd.ShaderEncoding.HLSL]
        if len(prefixes) != 1:
            raise rdtest.TestFailureException("No HLSL prefix for custom shaders")
        self.prefix = prefixes[0].prefix
        rdtest.log.success(f"HLSL and D3D9 bytecode are accepted, with an HLSL prefix")

        # the textures, by their sizes
        textures = {}
        for name, (w, h, mips, depth, ttype) in TEXTURES.items():
            found = [t for t in self.controller.GetTextures()
                     if (t.width, t.height, t.mips) == (w, h, mips) and
                     (name != 'Depth') == (t.format.compType != rd.CompType.Depth) and
                     (name != 'Cube' or t.cubemap) and
                     (name != 'Volume' or t.type == rd.TextureType.Texture3D)]
            if len(found) != 1:
                raise rdtest.TestFailureException(f"Expected one {name} texture, found {len(found)}")
            textures[name] = found[0]

        # after the targets are filled
        last = self.get_last_action()
        self.controller.SetFrameEvent(last.eventId, True)

        self.out = self.controller.CreateOutput(rd.CreateHeadlessWindowingData(100, 100),
                                                rd.ReplayOutputType.Texture)

        problems = []

        # building
        self.build_ok('UI template', UI_TEMPLATE)

        shader, errors = self.build('float4 main() : SV_Target0 { return undefined; }')
        # the prefix ends with #line 1, so the error is on the shader's own first line
        if shader != rd.ResourceId.Null() or 'error X' not in errors or '(1,' not in errors:
            problems.append(f"A shader with an error built as {shader}, errors '{errors}'")
        else:
            rdtest.log.success(f"A shader with an error doesn't build: {errors.strip().splitlines()[0]}")

        flags = rd.ShaderCompileFlags()
        flag = rd.ShaderCompileFlag()
        flag.name = '@cmdline'
        flag.value = '/T ps_2_0'
        flags.flags = [flag]
        shader, errors = self.build(UI_TEMPLATE, flags)
        if shader != rd.ResourceId.Null() or 'ps_3_0' not in errors:
            problems.append(f"A shader for ps_2_0 built as {shader}, errors '{errors}'")
        else:
            rdtest.log.success(f"A shader for another profile doesn't build: {errors}")

        # the native reference
        ref_path = self.demo_tmp_path('D3D9_Custom_Shaders_reference.txt')
        if not os.path.exists(ref_path):
            raise rdtest.TestFailureException(f"The demo didn't write {ref_path}")
        reference = []
        with open(ref_path) as f:
            for line in f:
                name, mip, slice, x, y, r, g, b, a = line.rstrip('\n').split('\t')
                reference.append((name, int(mip), int(slice), int(x), int(y),
                                  [float(r), float(g), float(b), float(a)]))

        def read_body(name):
            with open(self.demo_tmp_path(name)) as f:
                return f.read()

        sample = self.build_ok('sample', read_body('D3D9_Custom_Shaders_sample.hlsl'))
        position = self.build_ok('position', read_body('D3D9_Custom_Shaders_position.hlsl'))
        constants = self.build_ok('constants', CONSTANTS_SHADER)
        objects = self.build_ok('Texture2D and SamplerState', OBJECTS_SHADER)

        # each subresource against what the GPU sampled natively
        checked = {}
        groups = {}
        for name, mip, slice, x, y, expected in reference:
            groups.setdefault((name, mip, slice), []).append((x, y, expected))

        for (name, mip, slice), probes in groups.items():
            if name == 'Position':
                tex, shader = textures['L8'], position
            else:
                tex, shader = textures[name], sample
            custom = self.apply(shader, tex.resourceId, mip, slice)
            for x, y, expected in probes:
                got = self.pick(custom, mip, x, y)
                if not rdtest.value_compare(got, expected, eps=EPS):
                    problems.append(f"{name} mip {mip} slice {slice} at {x},{y}: {got}, natively {expected}")
                else:
                    checked[name] = checked.get(name, 0) + 1

        for name, count in checked.items():
            rdtest.log.success(f"{name}: {count} texels match the GPU's native sampling")
        missing = [n for n in list(TEXTURES.keys()) + ['Position'] if n not in checked]
        if missing:
            problems.append(f"Nothing matched for {missing}")

        # the prefix's values, at the last mip and slice that exist
        for name, (w, h, mips, depth, ttype) in TEXTURES.items():
            tex = textures[name]
            mip = mips - 1
            slice = 3 if name == 'Cube' else (1 if name == 'Volume' else 0)
            custom = self.apply(constants, tex.resourceId, mip, slice, 0.125, 0.75)
            got = [self.pick(custom, mip, x, 0) for x in range(5)]
            expected = [
                [w, h, depth, mips],
                [mip, slice, 0, ttype],
                [0.125, 0.75, 0, 0],
                [0, 0, 0, 0],
                [0, 0, 0, 0],
            ]
            labels = ['RD_TexDim', 'mip, slice, sample, type', 'RD_SelectedRange', 'YUV', 'userValue']
            for label, g, e in zip(labels, got, expected):
                if not rdtest.value_compare(g, e, eps=EPS):
                    problems.append(f"{name} mip {mip} slice {slice}: {label} is {g}, expected {e}")
            rdtest.log.success(f"{name} mip {mip} slice {slice}: TexDim {got[0]}, mip/slice/sample/type "
                               f"{got[1]}, range {got[2][:2]}, a global the prefix doesn't fill is 0")

        # HLSL's objects sample the same as sampler2D
        for mip in range(3):
            a = self.apply(sample, textures['Default 2D'].resourceId, mip, 0)
            a_values = [self.pick(a, mip, x, y) for x, y in [(0, 0), (5, 3), (7, 1)]]
            b = self.apply(objects, textures['Default 2D'].resourceId, mip, 0)
            b_values = [self.pick(b, mip, x, y) for x, y in [(0, 0), (5, 3), (7, 1)]]
            if a_values != b_values:
                problems.append(f"Default 2D mip {mip}: Texture2D and SamplerState give {b_values}, "
                                f"sampler2D gives {a_values}")
        rdtest.log.success("Texture2D and SamplerState sample the same as sampler2D")

        for s in [sample, position, constants, objects]:
            self.controller.FreeCustomShader(s)

        self.out.Shutdown()

        for p in problems:
            rdtest.log.error(p)
        if problems:
            raise rdtest.TestFailureException(f"{len(problems)} problems with custom shaders")
