#!/usr/bin/env python3
# Checks a D3D9 capture replays (phase 3a). Run it with the renderdoc python module from a build:
#
#   set PYTHONPATH=<build>\x64\Release\pymodules
#   set PATH=<build>\x64\Release;%PATH%
#   python d3d9_replay_check.py capture.rdc [--all]
#
# It prints the event list and the textures, saves the back buffer at the first event and at the
# last one as PNGs next to the capture, and prints the depth stencils' values at the first event
# (their initial contents, written back at replay). With --all it also steps through every action,
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


def print_depth(controller, tex, name):
    fmt = tex.format
    for sample in range(max(1, tex.msSamp)):
        data = controller.GetTextureData(tex.resourceId, rd.Subresource(0, 0, sample))
        if not data:
            print('  {} sample {}: couldn\'t read it'.format(name, sample))
            continue

        counts = {}
        if fmt.type == rd.ResourceFormatType.D24S8:
            for (v,) in struct.iter_unpack('<I', data):
                # back to D3D9's layout, depth << 8 | stencil, to compare with STATUS.md
                d3d9 = ((v << 8) | (v >> 24)) & 0xFFFFFFFF
                counts[d3d9] = counts.get(d3d9, 0) + 1
        elif fmt.compByteWidth == 2:
            for (v,) in struct.iter_unpack('<H', data):
                counts[v] = counts.get(v, 0) + 1
        else:
            print('  {}: {} isn\'t decoded by this script'.format(name, fmt.Name()))
            return

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

    first = actions[0].events[0].eventId if actions[0].events else actions[0].eventId
    last = actions[-1].eventId

    print('\nAt the first event ({}):'.format(first))
    controller.SetFrameEvent(first, True)
    if backbuffer:
        save_bgra(controller, backbuffer, base + '_first.png')
    for t in textures:
        if t.creationFlags & rd.TextureCategory.DepthTarget:
            print_depth(controller, t, names.get(t.resourceId, str(t.resourceId)))

    print('\nAt the last event ({}):'.format(last))
    controller.SetFrameEvent(last, True)
    if backbuffer:
        save_bgra(controller, backbuffer, base + '_last.png')
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
