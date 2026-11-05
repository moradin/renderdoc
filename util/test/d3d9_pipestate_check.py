#!/usr/bin/env python3
# Prints the D3D9 pipeline state (phase 3c) at each draw of a capture. Run it with the renderdoc
# python module from a build:
#
#   set PYTHONPATH=<build>\x64\Release\pymodules
#   set PATH=<build>\x64\Release;%PATH%
#   python d3d9_pipestate_check.py capture.rdc [eventId]
#
# With an eventId it only prints that event, and also every render state. It checks that the
# capture presents itself as D3D9, that the generic PipeState agrees with the D3D9 state, and reads
# the shader constants through the constant pseudo buffers. For each constant block a shader uses,
# it checks that GetCBufferVariableContents with a length of 0 gives the same values as with the
# descriptor's size, and prints a summary of any mismatches at the end.

import struct
import sys

import renderdoc as rd


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def vec(v):
    return '({:g}, {:g}, {:g}, {:g})'.format(v.x, v.y, v.z, v.w)


def flatten_vars(variables, prefix, out):
    for v in variables:
        if len(v.members) > 0:
            flatten_vars(v.members, prefix + v.name + '.', out)
        else:
            out.append((prefix + v.name, tuple(v.value.u32v[0:v.rows * v.columns])))
    return out


# GetCBufferVariableContents with a length of 0 passes no data to the driver, which for D3D9 reads
# the device's constants itself. It should give the same values as passing the descriptor's size.
def check_length0(controller, generic, stage, used):
    args = (generic.GetGraphicsPipelineObject(), generic.GetShader(stage), stage,
            generic.GetShaderEntryPoint(stage), used.access.index, used.descriptor.resource,
            used.descriptor.byteOffset)
    full = flatten_vars(controller.GetCBufferVariableContents(*args, used.descriptor.byteSize), '', [])
    zero = flatten_vars(controller.GetCBufferVariableContents(*args, 0), '', [])
    nonzero = sum(1 for n, v in full if any(v))
    if full == zero:
        print('    constant block {} length 0: same {} variables ({} non-zero)'.format(
            used.access.index, len(full), nonzero))
        return True
    print('    constant block {} length 0: MISMATCH'.format(used.access.index))
    for (n, v), (n0, v0) in zip(full, zero):
        if (n, v) != (n0, v0):
            print('      {} = {} with byteSize {}, {} = {} with 0'.format(
                n, v, used.descriptor.byteSize, n0, v0))
    if len(full) != len(zero):
        print('      {} variables with byteSize, {} with 0'.format(len(full), len(zero)))
    return False


length0_failures = []


def print_state(controller, names, eventId, verbose):
    controller.SetFrameEvent(eventId, True)

    pipe = controller.GetD3D9PipelineState()
    generic = controller.GetPipelineState()
    if pipe is None:
        print('  GetD3D9PipelineState() returned None')
        return False

    name = lambda id: names.get(id, str(id)) if id != rd.ResourceId.Null() else '-'

    ia = pipe.inputAssembly
    print('  topology {} (PipeState: {})'.format(str(ia.topology), str(generic.GetPrimitiveTopology())))
    print('  declaration {} fvf 0x{:x}'.format(name(ia.declarationId), ia.fvf))
    for e in ia.vertexElements:
        print('    stream {} +{:<3d} {}{} {} ({}) {}'.format(e.stream, e.byteOffset, e.semanticName,
                                                          e.semanticIndex, e.declType,
                                                          e.format.Name(), e.method))
    for i, s in enumerate(ia.streams):
        if s.resourceId != rd.ResourceId.Null():
            print('    stream {}: {} offset {} stride {} freq {}{}{}'.format(
                i, name(s.resourceId), s.byteOffset, s.byteStride, s.frequency,
                ' indexed' if s.indexedData else '', ' instance' if s.instanceData else ''))
    if ia.indexBuffer.resourceId != rd.ResourceId.Null():
        print('    indices: {} {} bytes'.format(name(ia.indexBuffer.resourceId), ia.indexBuffer.byteStride))
    for attr in generic.GetVertexInputs():
        print('    PipeState input {} slot {} +{} {} used {}'.format(attr.name, attr.vertexBuffer,
                                                                   attr.byteOffset, attr.format.Name(),
                                                                   attr.used))

    for sh in [pipe.vertexShader, pipe.pixelShader]:
        print('  {}: {} {} reflection {}'.format(str(sh.stage), name(sh.resourceId), sh.shaderModel,
                                                 'yes' if sh.reflection else 'no'))
        if sh.resourceId != rd.ResourceId.Null():
            data = controller.GetBufferData(sh.floatConstantsBuffer, 0, 0)
            print('    {} float constants ({} bytes read), c0 = {}'.format(
                sh.floatConstantCount, len(data),
                struct.unpack_from('4f', data, 0) if len(data) >= 16 else '-'))
            data = controller.GetBufferData(sh.intConstantsBuffer, 0, 0)
            print('    {} int constants ({} bytes read)'.format(sh.intConstantCount, len(data)))
            data = controller.GetBufferData(sh.boolConstantsBuffer, 0, 0)
            print('    {} bool constants ({} bytes read)'.format(sh.boolConstantCount, len(data)))
        for used in generic.GetReadOnlyResources(sh.stage, True):
            print('    reads {} (index {}, descriptor {})'.format(name(used.descriptor.resource),
                                                                  used.access.index,
                                                                  used.access.byteOffset))
        for used in generic.GetConstantBlocks(sh.stage, True):
            print('    constant block {} -> {}'.format(used.access.index, name(used.descriptor.resource)))
            if sh.resourceId != rd.ResourceId.Null():
                if not check_length0(controller, generic, sh.stage, used):
                    length0_failures.append((eventId, str(sh.stage), used.access.index))

    for s in pipe.pixelSamplers:
        if s.resourceId != rd.ResourceId.Null():
            print('  ps sampler {}: {} addr {}/{}/{} filter {}/{}/{} srgb {}'.format(
                s.samplerIndex, name(s.resourceId), str(s.addressU), str(s.addressV),
                str(s.addressW), str(s.minFilter), str(s.magFilter), str(s.mipFilter), s.srgbTexture))
    for s in pipe.vertexSamplers:
        if s.resourceId != rd.ResourceId.Null():
            print('  vs sampler {}: {}'.format(s.samplerIndex, name(s.resourceId)))

    ff = pipe.fixedFunction
    if pipe.pixelShader.resourceId == rd.ResourceId.Null():
        for ts in ff.textureStages:
            if ts.colorOp == 'DISABLE':
                break
            print('  stage {}: color {}({}, {}) alpha {}({}, {})'.format(
                ts.stage, ts.colorOp, ts.colorArg1, ts.colorArg2, ts.alphaOp, ts.alphaArg1, ts.alphaArg2))
    if pipe.vertexShader.resourceId == rd.ResourceId.Null():
        print('  lighting {} lights {} transforms {}'.format(ff.lighting, len(ff.lights),
                                                            [t.name for t in ff.transforms]))
    print('  fog {} {} {}'.format(ff.fog.enabled, ff.fog.tableMode, ff.fog.vertexMode))

    rs = pipe.rasterizer
    vp = rs.viewport
    print('  raster fill {} cull {} viewport {}x{} at {},{} depth {}-{} scissor {}'.format(
        str(rs.fillMode), str(rs.cullMode), vp.width, vp.height, vp.x, vp.y, vp.minDepth,
        vp.maxDepth, rs.scissor.enabled))

    om = pipe.outputMerger
    ds = om.depthStencilState
    print('  depth {} write {} func {} stencil {} two-sided {}'.format(
        ds.depthEnable, ds.depthWrites, str(ds.depthFunction), ds.stencilEnable, ds.twoSidedStencil))
    b = om.blendState.blends[0]
    print('  blend {} {} {} {} separate alpha {} mask 0x{:x} alpha test {} {} {} srgb {}'.format(
        b.enabled, str(b.colorBlend.source), str(b.colorBlend.destination),
        str(b.colorBlend.operation), om.blendState.separateAlphaBlend, b.writeMask,
        om.alphaTest.enabled, str(om.alphaTest.function), om.alphaTest.reference, om.srgbWrite))
    for i, rt in enumerate(om.renderTargets):
        if rt.resource != rd.ResourceId.Null():
            print('  rt{}: {} mip {} slice {} {}'.format(i, name(rt.resource), rt.firstMip,
                                                        rt.firstSlice, rt.format.Name()))
    if om.depthTarget.resource != rd.ResourceId.Null():
        print('  depth: {}'.format(name(om.depthTarget.resource)))
    print('  PipeState output targets: {}'.format(
        [name(d.resource) for d in generic.GetOutputTargets()]))

    if verbose:
        for r in pipe.renderStates:
            print('  {} = 0x{:x}'.format(r.name, r.value))

    return True


def main():
    if len(sys.argv) < 2:
        print('usage: d3d9_pipestate_check.py capture.rdc [eventId]')
        return 1

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])

    cap = rd.OpenCaptureFile()
    if cap.OpenFile(sys.argv[1], '', None) != rd.ResultCode.Succeeded:
        print('Couldn\'t open the file')
        return 1

    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        print('Couldn\'t load the capture: ' + str(result))
        return 1

    props = controller.GetAPIProperties()
    print('pipelineType {} localRenderer {}'.format(str(props.pipelineType), str(props.localRenderer)))
    if props.pipelineType != rd.GraphicsAPI.D3D9:
        print('Not a D3D9 capture')

    names = {r.resourceId: r.name for r in controller.GetResources()}

    if len(sys.argv) > 2:
        eventIds = [int(sys.argv[2])]
    else:
        eventIds = [a.eventId for a in flatten(controller.GetRootActions(), [])
                    if a.flags & rd.ActionFlags.Drawcall]

    for eid in eventIds:
        print('\nEvent {}:'.format(eid))
        if not print_state(controller, names, eid, len(sys.argv) > 2):
            break

    if length0_failures:
        print('\nGetCBufferVariableContents length 0 MISMATCH at (event, stage, block): {}'.format(
            length0_failures))
    else:
        print('\nGetCBufferVariableContents length 0 matched the descriptor size everywhere')

    controller.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()
    return 0


if __name__ == '__main__':
    sys.exit(main())
