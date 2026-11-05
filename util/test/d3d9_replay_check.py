#!/usr/bin/env python3
# Checks a D3D9 capture replays (phase 3a). Run it with the renderdoc python module from a build:
#
#   set PYTHONPATH=<build>\x64\Release\pymodules
#   set PATH=<build>\x64\Release;%PATH%
#   python d3d9_replay_check.py capture.rdc [--all]
#
# It prints the event list and the textures, saves the back buffer at the start of the frame (event
# 0, before any call in it has replayed) and at the last event as PNGs next to the capture, saves it
# again at the last event with SaveTexture (printing the saved alpha range: 255-255 for an X8R8G8B8
# back buffer), and prints the depth stencils' values at the start of the frame (their initial contents, written back
# at replay). With --all it also steps through every action,
# which replays from the start for each one.

import os
import struct
import sys
import time
import zlib

import renderdoc as rd


def write_png(path, width, height, rows):
    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b''.join(b'\x00' + row for row in rows)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def read_png_alpha(path):
    # the alpha values of an 8-bit RGBA PNG, as SaveTexture writes them
    with open(path, 'rb') as f:
        data = f.read()
    pos = 8
    idat = b''
    width = height = 0
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height, depth, colour = struct.unpack('>IIBB', body[:10])
            if depth != 8 or colour != 6:
                return None
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    prev = bytearray(stride)
    alphas = []
    for y in range(height):
        filt = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        alphas += line[3::4]
        prev = line
    return alphas


def check_save_texture(controller, tex, path):
    # SaveTexture remaps to RGBA8 for PNG. X8R8G8B8 back buffers have to come out opaque.
    save = rd.TextureSave()
    save.resourceId = tex.resourceId
    save.destType = rd.FileType.PNG
    save.alpha = rd.AlphaMapping.Preserve
    # returns the saved alpha range, or None
    result = controller.SaveTexture(save, path)
    if not result.OK():
        print('  SaveTexture failed: ' + str(result.Message()))
        return None
    alphas = read_png_alpha(path)
    if alphas is None:
        print('  saved ' + path + ' (not 8-bit RGBA, alpha not checked)')
        return None
    print('  saved {} with SaveTexture, alpha {}-{}'.format(path, min(alphas), max(alphas)))
    return min(alphas), max(alphas)


def print_actions(actions, sdfile, depth=0):
    for a in actions:
        extra = ''
        if a.flags & rd.ActionFlags.Drawcall:
            extra = ' ({} indices, {} instances)'.format(a.numIndices, a.numInstances)
        print('{}{:5d} {}{}'.format('  ' * depth, a.eventId, a.GetName(sdfile), extra))
        print_actions(a.children, sdfile, depth + 1)


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def save_bgra(controller, tex, path):
    data = controller.GetTextureData(tex.resourceId, rd.Subresource(0, 0, 0))
    if len(data) < tex.width * tex.height * 4:
        print('  couldn\'t read {} ({} bytes)'.format(path, len(data)))
        return
    rows = []
    for y in range(tex.height):
        row = bytearray()
        line = data[y * tex.width * 4:(y + 1) * tex.width * 4]
        for x in range(tex.width):
            b, g, r = line[x * 4], line[x * 4 + 1], line[x * 4 + 2]
            row += bytes([r, g, b])
        rows.append(bytes(row))
    write_png(path, tex.width, tex.height, rows)
    print('  saved ' + path)


def depth_values(controller, tex, sample):
    # a depth stencil's texels in D3D9's layout (depth << 8 | stencil for D24S8, the 16-bit value
    # for D16), as a list in row order. None if it can't be read or decoded.
    data = controller.GetTextureData(tex.resourceId, rd.Subresource(0, 0, sample))
    if not data:
        return None
    if tex.format.type == rd.ResourceFormatType.D24S8:
        # back to D3D9's layout, depth << 8 | stencil, to compare with STATUS.md
        return [((v << 8) | (v >> 24)) & 0xFFFFFFFF for (v,) in struct.iter_unpack('<I', data)]
    if tex.format.compByteWidth == 2:
        return [v for (v,) in struct.iter_unpack('<H', data)]
    return None


def print_depth(controller, tex, name):
    fmt = tex.format
    for sample in range(max(1, tex.msSamp)):
        values = depth_values(controller, tex, sample)
        if values is None:
            print('  {} ({}, sample {}): couldn\'t read or decode it'.format(name, fmt.Name(), sample))
            continue

        counts = {}
        for v in values:
            counts[v] = counts.get(v, 0) + 1

        top = sorted(counts.items(), key=lambda kv: -kv[1])[:6]
        print('  {} ({}, sample {}): {}'.format(
            name, fmt.Name(), sample,
            ', '.join('0x{:X} x{}'.format(v, n) for v, n in top)))


def main():
    if len(sys.argv) < 2:
        print(__doc__ if __doc__ else 'usage: d3d9_replay_check.py capture.rdc [--all]')
        return 1

    path = sys.argv[1]
    step_all = '--all' in sys.argv[2:]
    base = os.path.splitext(path)[0]

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])

    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(path, '', None)
    if result != rd.ResultCode.Succeeded:
        print('Couldn\'t open the file: ' + str(result))
        return 1

    if cap.LocalReplaySupport() != rd.ReplaySupport.Supported:
        print('Capture can\'t be replayed: ' + str(cap.LocalReplaySupport()))
        return 1

    start = time.time()
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        print('Couldn\'t load the capture: ' + str(result))
        return 1
    print('Loaded in {:.2f}s'.format(time.time() - start))

    sdfile = controller.GetStructuredFile()
    names = {r.resourceId: r.name for r in controller.GetResources()}

    print('\nEvents:')
    roots = controller.GetRootActions()
    print_actions(roots, sdfile)

    print('\nTextures:')
    textures = controller.GetTextures()
    for t in textures:
        print('  {:30s} {:4d}x{:<4d} mips {:2d} slices {} samples {} {} {}'.format(
            names.get(t.resourceId, str(t.resourceId)), t.width, t.height, t.mips,
            t.arraysize, t.msSamp, t.format.Name(), str(t.creationFlags)))

    actions = flatten(roots, [])
    if not actions:
        print('No actions')
        return 1

    texById = {t.resourceId: t for t in textures}

    # the back buffer: what the last present presents
    backbuffer = None
    for a in reversed(actions):
        if a.flags & rd.ActionFlags.Present and a.copyDestination in texById:
            backbuffer = texById[a.copyDestination]
            break

    last = actions[-1].eventId

    # event 0 is the frame's initial state: nothing in the frame has replayed yet. The first event
    # can be a Clear, which would hide the initial contents.
    print('\nAt the start of the frame (event 0):')
    controller.SetFrameEvent(0, True)
    if backbuffer:
        save_bgra(controller, backbuffer, base + '_first.png')
    for t in textures:
        if t.creationFlags & rd.TextureCategory.DepthTarget:
            print_depth(controller, t, names.get(t.resourceId, str(t.resourceId)))

    print('\nAt the last event ({}):'.format(last))
    controller.SetFrameEvent(last, True)
    if backbuffer:
        save_bgra(controller, backbuffer, base + '_last.png')
        check_save_texture(controller, backbuffer, base + '_saved.png')
    else:
        print('  no presented back buffer found')

    if step_all:
        print('\nStepping through {} actions'.format(len(actions)))
        start = time.time()
        for a in actions:
            controller.SetFrameEvent(a.eventId, True)
        print('  done in {:.2f}s'.format(time.time() - start))

    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
    return 0


if __name__ == '__main__':
    sys.exit(main())
