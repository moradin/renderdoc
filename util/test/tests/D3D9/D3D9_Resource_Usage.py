import os

import renderdoc as rd
import rdtest

U = rd.ResourceUsage
# ResourceUsage values print as numbers
_usage_names = {getattr(U, n): n for n in dir(U) if not n.startswith('_') and isinstance(getattr(U, n), type(U.Clear))}


def usage_name(u):
    return _usage_names.get(u, str(u))


class D3D9_Resource_Usage(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Resource_Usage'
    internal = False

    def get_capture(self):
        # the native run checks what a colour Clear with two render targets bound clears
        self.run_native()
        return super().get_capture()

    def read_reference(self, path: str):
        if not os.path.exists(path):
            raise rdtest.TestFailureException(f"The demo didn't write {path}")
        ret = {}
        with open(path) as f:
            for line in f:
                name, value = line.rstrip('\n').split('\t')
                ret[name] = int(value)
        return ret

    def marker_actions(self, name: str):
        """The actions inside the marker called name, in order."""
        marker = self.find_marker(name)
        return [a for a in marker.children if not (a.flags & rd.ActionFlags.PopMarker)]

    def find_marker(self, name: str):
        """The marker called exactly name (find_action matches any action whose name contains it)."""
        found = []

        def walk(actions):
            for a in actions:
                if (a.flags & rd.ActionFlags.PushMarker) and a.customName == name:
                    found.append(a)
                walk(a.children)

        walk(self.controller.GetRootActions())
        if len(found) != 1:
            raise rdtest.TestFailureException(f"Expected one marker '{name}', found {len(found)}")
        return found[0]

    def marker_event(self, name: str, index: int = 0):
        actions = self.marker_actions(name)
        if len(actions) <= index:
            raise rdtest.TestFailureException(f"The marker '{name}' has {len(actions)} actions")
        return actions[index].eventId

    def unlock_event(self, marker: str):
        """The Unlock call inside the marker. It isn't an action, so it's an event of the next one."""
        marker_action = self.find_marker(marker)
        found = []

        def walk(actions):
            for a in actions:
                for e in a.events:
                    name = self.sdfile.chunks[e.chunkIndex].name
                    if 'Unlock' in name and e.eventId > marker_action.eventId:
                        found.append(e.eventId)
                walk(a.children)

        walk(self.controller.GetRootActions())
        if not found:
            raise rdtest.TestFailureException(f"No Unlock after the marker '{marker}'")
        return min(found)

    def check_d3d9_capture(self):
        self.check_api()

        native = self.read_reference(self.demo_tmp_path('D3D9_Resource_Usage_reference.txt'))
        under_rdoc = self.read_reference(self.demo_tmp_path('D3D9_Resource_Usage_reference_rdoc.txt'))
        if native != under_rdoc:
            raise rdtest.TestFailureException(f"Under RenderDoc the demo found {under_rdoc}, natively {native}")
        rdtest.log.success(f"The demo found the same natively and under RenderDoc: {native}")
        if native['Clear clears render target 1'] != 1:
            raise rdtest.TestFailureException("Natively a colour Clear didn't clear the second render target, "
                                              "but RenderDoc records it as cleared")
        rdtest.log.success("Natively a colour Clear clears both bound render targets, as RenderDoc records")

        textures = self.controller.GetTextures()
        buffers = self.controller.GetBuffers()

        def texture(w, h, fmt=None, ms=1, flags=None):
            found = [t for t in textures if t.width == w and t.height == h and t.msSamp == ms and
                     (fmt is None or t.format.Name() == fmt) and
                     (flags is None or (t.creationFlags & flags) == flags)]
            return [t.resourceId for t in found]

        def one(ids, what):
            if len(ids) != 1:
                raise rdtest.TestFailureException(f"Expected one {what}, found {len(ids)}")
            return ids[0]

        def buffer(length):
            return one([b.resourceId for b in buffers if b.length == length], f"{length} byte buffer")

        def named(name):
            res = self.get_resource_by_name(name)
            if res is None:
                raise rdtest.TestFailureException(f"No resource named '{name}'")
            return res.resourceId

        clear = self.marker_event('Clear two targets')
        shader = self.marker_event('Shader draw')
        nonindexed = self.marker_event('Non-indexed draw')
        ffp = self.marker_event('Fixed function draw')
        stretch = self.marker_event('StretchRect')
        getrt = self.marker_event('GetRenderTargetData')
        colorfill = self.marker_event('ColorFill')
        update = self.marker_event('UpdateTexture')
        vb_unlock = self.unlock_event('Lock vertex buffer')
        tex_unlock = self.unlock_event('Lock texture')
        rdtest.log.print(f"Events: clear {clear}, shader draw {shader}, non-indexed {nonindexed}, fixed function "
                         f"{ffp}, StretchRect {stretch}, GetRenderTargetData {getrt}, ColorFill {colorfill}, "
                         f"UpdateTexture {update}, Unlocks {vb_unlock} and {tex_unlock}")

        # the two render targets are the same size, so they're taken from the draw's outputs
        shader_action = self.marker_actions('Shader draw')[0]
        if sorted(texture(64, 32)) != sorted(shader_action.outputs[0:2]):
            raise rdtest.TestFailureException(f"The shader draw's outputs {shader_action.outputs[0:2]} aren't the "
                                              f"64x32 textures {texture(64, 32)}")

        last_clear = [a for a in self.controller.GetRootActions() if a.flags & rd.ActionFlags.Clear][-1].eventId

        draws = [shader, nonindexed]
        expected = {
            'sampled pixel texture (8x8)': (one(texture(8, 8), '8x8 texture'),
                                            {(e, U.PS_Resource) for e in draws}),
            'unsampled pixel texture (12x12)': (one(texture(12, 12), '12x12 texture'), set()),
            'fixed function texture (20x20)': (one(texture(20, 20), '20x20 texture'), {(ffp, U.PS_Resource)}),
            'texture after a disabled stage (28x28)': (one(texture(28, 28), '28x28 texture'), set()),
            'locked texture (44x44)': (one(texture(44, 44), '44x44 texture'), {(tex_unlock, U.CPUWrite)}),
            'stream 0 (1024 bytes)': (buffer(1024), {(e, U.VertexBuffer) for e in draws}),
            'stream 1 (1152 bytes)': (buffer(1152), {(e, U.VertexBuffer) for e in draws} | {(vb_unlock, U.CPUWrite)}),
            'unused stream 2 (1280 bytes)': (buffer(1280), set()),
            'index buffer (640 bytes)': (buffer(640), {(shader, U.IndexBuffer)}),
            'render target 0 (64x32)': (shader_action.outputs[0],
                                             {(clear, U.Clear), (shader, U.ColorTarget), (nonindexed, U.ColorTarget),
                                              (ffp, U.ColorTarget), (stretch, U.CopySrc)}),
            'render target 1 (64x32)': (shader_action.outputs[1],
                                             {(clear, U.Clear), (shader, U.ColorTarget), (nonindexed, U.ColorTarget)}),
            'depth stencil (72x40)': (one(texture(72, 40), '72x40 depth stencil'),
                                      {(clear, U.Clear), (shader, U.DepthStencilTarget)}),
            'StretchRect destination (64x44)': (one(texture(64, 44, flags=rd.TextureCategory.ColorTarget),
                                                    '64x44 render target'),
                                                {(stretch, U.CopyDst), (getrt, U.CopySrc), (colorfill, U.Clear)}),
            'VS float constants': (named('VS float constants (c#)'), {(e, U.VS_Constants) for e in draws}),
            'PS float constants': (named('PS float constants (c#)'), {(e, U.PS_Constants) for e in draws}),
            'VS integer constants': (named('VS integer constants (i#)'), set()),
            'VS boolean constants': (named('VS boolean constants (b#)'), set()),
            'PS integer constants': (named('PS integer constants (i#)'), set()),
            'PS boolean constants': (named('PS boolean constants (b#)'), set()),
        }

        # the system memory copy of the 64x44 target is the other 64x44 texture
        sys_surf = [t for t in texture(64, 44) if t != expected['StretchRect destination (64x44)'][0]]
        expected['GetRenderTargetData destination (64x44)'] = (one(sys_surf, 'other 64x44 surface'),
                                                               {(getrt, U.CopyDst)})

        back = [t for t in textures if t.width == 400 and t.height == 300]
        for t in back:
            expected[f'{t.format.Name()} back buffer or depth (400x300)'] = (t.resourceId, {(last_clear, U.Clear)})

        if native['Vertex texture fetch']:
            expected['vertex texture (4x4)'] = (one(texture(4, 4), '4x4 texture'),
                                                {(e, U.VS_Resource) for e in draws})
        else:
            rdtest.log.print("No vertex texture fetch, the vertex texture isn't checked")

        if native['Multisampling']:
            msaa_draw = self.marker_event('Multisampled draw')
            resolve = self.marker_event('Resolve')
            expected['texture of the fixed function draws (20x20)'] = (
                expected.pop('fixed function texture (20x20)')[0], {(ffp, U.PS_Resource), (msaa_draw, U.PS_Resource)})
            expected['multisampled target (40x40)'] = (one(texture(40, 40, ms=4), '40x40 4x target'),
                                                       {(msaa_draw, U.ColorTarget), (resolve, U.ResolveSrc)})
            expected['resolve destination (40x40)'] = (one(texture(40, 40), '40x40 texture'),
                                                       {(resolve, U.ResolveDst)})
        else:
            rdtest.log.print("No 4x multisampling, the resolve isn't checked")

        if native['Autogen mipmaps']:
            fill = self.marker_event('Generate mips', 0)
            genmips = self.marker_event('Generate mips', 1)
            expected['autogen mip texture (128x128)'] = (one(texture(128, 128), '128x128 texture'),
                                                         {(fill, U.Clear), (genmips, U.GenMips)})
        else:
            rdtest.log.print("No autogen mipmaps, GenerateMipSubLevels isn't checked")

        # UpdateTexture between the two 36x36 textures: one is the source, the other the destination
        update_textures = texture(36, 36)
        if len(update_textures) != 2:
            raise rdtest.TestFailureException(f"Expected two 36x36 textures, found {len(update_textures)}")
        update_uses = sorted([sorted((u.eventId, u.usage) for u in self.controller.GetUsage(t))
                              for t in update_textures], key=lambda l: [str(x) for x in l])
        want = sorted([[(update, U.CopyDst)], [(update, U.CopySrc)]], key=lambda l: [str(x) for x in l])
        if update_uses != want:
            raise rdtest.TestFailureException(f"The 36x36 textures are used as {update_uses}, expected {want}")
        rdtest.log.success("UpdateTexture's source and destination are CopySrc and CopyDst")

        failed = []
        for what, (rid, want) in expected.items():
            got = {(u.eventId, u.usage) for u in self.controller.GetUsage(rid)}
            got_list = self.controller.GetUsage(rid)
            if len(got_list) != len(got):
                failed.append(f"{what} has duplicate usages: {[(u.eventId, usage_name(u.usage)) for u in got_list]}")
            if got != want:
                missing = sorted(want - got)
                extra = sorted(got - want)
                failed.append(f"{what}: missing {[(e, usage_name(u)) for e, u in missing]}, "
                              f"unexpected {[(e, usage_name(u)) for e, u in extra]}")
            else:
                uses = ', '.join(f'{e} {usage_name(u)}' for e, u in sorted(got))
                rdtest.log.success(f"{what}: {uses if got else 'unused'}")

        for f in failed:
            rdtest.log.error(f)
        if failed:
            raise rdtest.TestFailureException(f"{len(failed)} resources have the wrong usage")

        self.check_export(self.capture_filename)
