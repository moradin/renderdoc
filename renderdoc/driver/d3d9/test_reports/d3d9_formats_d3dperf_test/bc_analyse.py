# bc_analyse.py capture.rdc reference.txt
# For the block formats of a D3D9_Texture_Formats capture: checks the captured texture bytes against
# the demo's generator, decodes them per the D3D spec, and compares the spec decode with the native
# GPU reference, RenderDoc's PickPixel and the replay render target, channel by channel.
import sys, struct, re
import renderdoc as rd

sys.path.insert(0, r'E:\Projects\renderdoc\util\test')
INDEX = {'DXT1': 35, 'DXT2': 36, 'DXT3': 37, 'DXT4': 38, 'DXT5': 39, 'ATI1': 40, 'ATI2': 41}
BLOCK = {'DXT1': 8, 'ATI1': 8}
M = 0xffffffff


def gen(name):
    s = (0x12345678 + INDEX[name] * 0x9E3779B9) & M
    out = bytearray()
    for _ in range(16 * BLOCK.get(name, 16)):
        s ^= (s << 13) & M
        s ^= s >> 17
        s ^= (s << 5) & M
        out.append(s & 0xff)
    return bytes(out)


def c565(v):
    return ((v >> 11) & 31) / 31.0, ((v >> 5) & 63) / 63.0, (v & 31) / 31.0


def colour_block(b, four):
    c0, c1, bits = struct.unpack('<HHI', b)
    a, bb = c565(c0), c565(c1)
    if four or c0 > c1:
        pal = [a, bb, tuple((2 * x + y) / 3 for x, y in zip(a, bb)), tuple((x + 2 * y) / 3 for x, y in zip(a, bb))]
        alpha = [1, 1, 1, 1]
    else:
        pal = [a, bb, tuple((x + y) / 2 for x, y in zip(a, bb)), (0.0, 0.0, 0.0)]
        alpha = [1, 1, 1, 0]
    return [(pal[(bits >> (2 * i)) & 3], alpha[(bits >> (2 * i)) & 3]) for i in range(16)]


def bc4_block(b):
    r0, r1 = b[0], b[1]
    bits = int.from_bytes(b[2:8], 'little')
    if r0 > r1:
        pal = [r0, r1] + [((7 - k) * r0 + k * r1) / 7 for k in range(1, 7)]
    else:
        pal = [r0, r1] + [((5 - k) * r0 + k * r1) / 5 for k in range(1, 5)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] / 255.0 for i in range(16)]


def bc2_alpha(b):
    bits = int.from_bytes(b, 'little')
    return [((bits >> (4 * i)) & 15) / 15.0 for i in range(16)]


def decode(name, data, mode):
    # returns 16x16 rows of (r,g,b,a). mode: for colour formats 'four' or 'bc1'; ATI2 'xy' or 'yx'
    bs = BLOCK.get(name, 16)
    img = [[None] * 16 for _ in range(16)]
    for by in range(4):
        for bx in range(4):
            b = data[(by * 4 + bx) * bs:(by * 4 + bx + 1) * bs]
            if name == 'ATI1':
                r = bc4_block(b)
                tex = [(r[i], 0.0, 0.0, 1.0) for i in range(16)]
            elif name == 'ATI2':
                x, y = bc4_block(b[:8]), bc4_block(b[8:])
                if mode == 'yx':
                    x, y = y, x
                tex = [(x[i], y[i], 1.0, 1.0) for i in range(16)]
            else:
                if name == 'DXT1':
                    col = colour_block(b, mode == 'four')
                    tex = [c + (al,) for c, al in col]
                else:
                    col = colour_block(b[8:], mode == 'four')
                    al = bc2_alpha(b[:8]) if name in ('DXT2', 'DXT3') else bc4_block(b[:8])
                    tex = [col[i][0] + (al[i],) for i in range(16)]
            for i in range(16):
                img[by * 4 + i // 4][bx * 4 + i % 4] = tex[i]
    return img


def cmp(a, b):
    # max diff per channel
    return [max(abs(a[y][x][c] - b[y][x][c]) for y in range(16) for x in range(16)) for c in range(4)]


def fmt(v):
    return '(' + ', '.join('{:.4f}'.format(x) for x in v) + ')'


def main():
    from d3d9_formats_check import parse_reference, find_formats
    refs = parse_reference(sys.argv[2])
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(sys.argv[1], '', None)
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    found = find_formats(ctl.GetRootActions(), ctl.GetStructuredFile(), [], [])
    texs = {t.resourceId: t for t in ctl.GetTextures()}

    last = found[-1][3]
    ctl.SetFrameEvent(last.eventId, True)
    rt = ctl.GetPipelineState().GetOutputTargets()[0].resource
    rtdesc = texs[rt]
    rtdata = bytes(ctl.GetTextureData(rt, rd.Subresource(0, 0, 0)))
    print('replay RT {} {}x{} at event {}'.format(rtdesc.format.Name(), rtdesc.width, rtdesc.height, last.eventId))

    for name, x0, y0, draw in found:
        if name not in INDEX:
            continue
        ref = refs[name]
        gpu = ref['rows']
        ctl.SetFrameEvent(draw.eventId, True)
        tex = ctl.GetPipelineState().GetReadOnlyResources(rd.ShaderStage.Pixel, True)[0].descriptor.resource
        data = bytes(ctl.GetTextureData(tex, rd.Subresource(0, 0, 0)))
        expect = gen(name)
        print('\n== {} event {} ({}), {} bytes from GetTextureData, generator {} bytes: {}'.format(
            name, draw.eventId, texs[tex].format.Name(), len(data), len(expect),
            'identical' if data == expect else 'DIFFERENT'))
        if data != expect:
            for i in range(0, min(len(data), len(expect)), 16):
                if data[i:i + 16] != expect[i:i + 16]:
                    print('   first diff at byte {}: got {} want {}'.format(i, data[i:i + 16].hex(), expect[i:i + 16].hex()))
                    break
        pick = [[list(ctl.PickPixel(tex, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless).floatValue)
                 for x in range(16)] for y in range(16)]
        rep = [[struct.unpack_from('4f', rtdata, ((y0 + y) * rtdesc.width + x0 + x) * 16) for x in range(16)] for y in range(16)]
        modes = ['xy', 'yx'] if name == 'ATI2' else ['-'] if name == 'ATI1' else ['four', 'bc1']
        for m in modes:
            spec = decode(name, expect, m)
            print('   spec decode [{}] vs native GPU  max diff per channel {}'.format(m, fmt(cmp(spec, gpu))))
            print('   spec decode [{}] vs PickPixel   max diff per channel {}'.format(m, fmt(cmp(spec, pick))))
            print('   spec decode [{}] vs replay RT   max diff per channel {}'.format(m, fmt(cmp(spec, rep))))
        print('   replay RT vs native GPU          max diff per channel {}'.format(fmt(cmp(rep, gpu))))
        for (x, y) in [(0, 0), (8, 0), (5, 9)]:
            print('   texel {},{}: native {} replay {} pick {}'.format(x, y, fmt(gpu[y][x]), fmt(rep[y][x]), fmt(pick[y][x])))

    ctl.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
    return 0


sys.exit(main())
