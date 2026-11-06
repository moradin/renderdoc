import os

import renderdoc as rd
import rdtest

D3DERR_INVALIDCALL = 0x8876086C


class D3D9_Failed_Calls(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Failed_Calls'
    internal = False

    def get_capture(self):
        # the native run says which calls fail
        self.run_native()
        return super().get_capture()

    def read_reference(self, path: str):
        if not os.path.exists(path):
            raise rdtest.TestFailureException(f"The demo didn't write {path}")
        ret = []
        with open(path) as f:
            for line in f:
                marker, call, hr = line.rstrip('\n').split('\t')
                ret.append((marker, call, int(hr, 16)))
        return ret

    def find_marker(self, name: str):
        """The marker called exactly name."""
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

    def marker_events(self, name: str):
        """The events inside the marker called name: from the marker to its pop."""
        marker = self.find_marker(name)
        first = marker.eventId
        last = marker.children[-1].eventId if marker.children else marker.eventId
        return [e for e in self.all_events if first < e.eventId < last]

    def check_d3d9_capture(self):
        self.check_api()

        native = self.read_reference(self.demo_tmp_path('D3D9_Failed_Calls_reference.txt'))
        under_rdoc = self.read_reference(self.demo_tmp_path('D3D9_Failed_Calls_reference_rdoc.txt'))
        if native != under_rdoc:
            for n, r in zip(native, under_rdoc):
                if n != r:
                    rdtest.log.error(f"{n[0]}: {n[1]} returned {n[2]:08X} natively, {r[2]:08X} under RenderDoc")
            raise rdtest.TestFailureException("RenderDoc changed what the calls returned")
        rdtest.log.success(f"All {len(native)} calls returned the same natively and under RenderDoc")

        failed = [r for r in native if r[2] & 0x80000000]
        succeeded = [r for r in native if not (r[2] & 0x80000000)]
        rdtest.log.print(f"Natively {len(failed)} calls failed: " + ', '.join(f'{m} ({hr:08X})' for m, c, hr in failed))
        rdtest.log.print(f"and {len(succeeded)} succeeded: " + ', '.join(m for m, c, hr in succeeded))
        if not failed:
            raise rdtest.TestFailureException("No call failed natively, there's nothing to check")

        self.all_events = []

        def walk(actions):
            for a in actions:
                self.all_events.extend(a.events)
                walk(a.children)

        walk(self.controller.GetRootActions())

        sdfile = self.controller.GetStructuredFile()
        messages = self.controller.GetDebugMessages()
        rdtest.log.print(f"{len(messages)} debug messages:")
        for m in messages:
            rdtest.log.print(f"  event {m.eventId}: {m.description} ({m.source}, {m.severity}, {m.category}, "
                             f"ID {m.messageID:08X})")

        problems = []
        used = set()

        for marker, call, hr in native:
            events = self.marker_events(marker)
            event_ids = {e.eventId for e in events}
            found = [m for m in messages if m.eventId in event_ids]
            used.update((m.eventId, m.description) for m in found)

            if not (hr & 0x80000000):
                if found:
                    problems.append(f"{marker}: succeeded natively but has messages {[m.description for m in found]}")
                failed_chunks = [sdfile.chunks[e.chunkIndex].name for e in events
                                 if sdfile.chunks[e.chunkIndex].name.endswith(' (failed)')]
                if failed_chunks:
                    problems.append(f"{marker}: succeeded natively but has failed calls {failed_chunks}")
                continue

            if len(found) != 1:
                problems.append(f"{marker}: expected one message, found {[m.description for m in found]}")
                continue
            msg = found[0]

            chunk = sdfile.chunks[[e for e in events if e.eventId == msg.eventId][0].chunkIndex]
            if not chunk.name.endswith(f'::{call} (failed)'):
                problems.append(f"{marker}: the message's event is {chunk.name}, not the failed {call}")
                continue

            function = chunk.FindChild('Function').AsString()
            result = chunk.FindChild('Result')
            if (msg.messageID != hr or msg.source != rd.MessageSource.API or
                    msg.severity != rd.MessageSeverity.High or function not in msg.description or
                    'D3DERR_INVALIDCALL' not in msg.description):
                problems.append(f"{marker}: unexpected message {msg.description} ({msg.source}, {msg.severity}, "
                                f"ID {msg.messageID:08X})")
                continue
            if result.AsInt() != hr or result.AsString() != 'D3DERR_INVALIDCALL':
                problems.append(f"{marker}: the chunk's result is {result.AsString()} ({result.AsInt():08X})")
                continue

            params = [chunk.GetChild(i).name for i in range(chunk.NumChildren())]
            rdtest.log.success(f"{marker}: event {msg.eventId} {chunk.name} with {', '.join(params)}: "
                               f"{msg.description}")

        # the parameters of a few, read from the failed call's chunk
        def chunk_in(marker):
            for e in self.marker_events(marker):
                c = sdfile.chunks[e.chunkIndex]
                if c.name.endswith(' (failed)'):
                    return c
            return None

        def check_param(marker, path, expected):
            c = chunk_in(marker)
            if c is None:
                return
            obj = c
            for p in path:
                obj = obj.FindChild(p) if obj is not None else None
            value = obj.AsInt() if obj is not None else None
            if value != expected:
                problems.append(f"{marker}: {'.'.join(path)} is {value}, expected {expected}")
            else:
                rdtest.log.success(f"{marker}: {'.'.join(path)} is {value}")

        failed_markers = {m for m, c, hr in failed}
        param_checks = [
            ('SetRenderTarget index 4', ['RenderTargetIndex'], 4),
            ('SetVertexShaderConstantF past the end', ['StartRegister'], 300),
            ('SetVertexShaderConstantF past the end', ['Vector4fCount'], 1),
            ('SetPixelShaderConstantF past the end', ['StartRegister'], 224),
            ('StretchRect outside the source', ['pSourceRect', 'right'], 128),
            ('CreateTexture with zero size', ['Width'], 0),
            ('CreateTexture with zero size', ['Levels'], 1),
        ]
        for marker, path, expected in param_checks:
            if marker in failed_markers:
                check_param(marker, path, expected)

        # a call on an object records the object
        if 'LockRect on a missing level' in failed_markers:
            c = chunk_in('LockRect on a missing level')
            obj = c.FindChild('Object') if c else None
            if obj is None or obj.AsResourceId() == rd.ResourceId.Null():
                problems.append("LockRect on a missing level: the failed call doesn't record the texture")
            else:
                # the object is added to the capture, so it shows by name
                tex = [t for t in self.controller.GetTextures() if t.resourceId == obj.AsResourceId()]
                if not tex or (tex[0].width, tex[0].height, tex[0].mips) != (8, 8, 1):
                    problems.append(f"LockRect on a missing level: the failed call records {obj.AsResourceId()}, "
                                    f"which isn't the 8x8 texture in the capture")
                else:
                    rdtest.log.success(f"LockRect on a missing level: the failed call records the 8x8 texture "
                                       f"{self.get_resource(obj.AsResourceId()).name}")

        # the arrays sized by the call's counts are left out
        c = chunk_in('SetVertexShaderConstantF past the end')
        if c is not None:
            data = c.FindChild('pConstantData')
            if data is None or data.NumChildren() != 0:
                problems.append("SetVertexShaderConstantF past the end: pConstantData was read")
            else:
                rdtest.log.success("SetVertexShaderConstantF past the end: pConstantData isn't read")

        extra = [m for m in messages if (m.eventId, m.description) not in used]
        if extra:
            problems.append(f"Messages outside the failed calls: {[(m.eventId, m.description) for m in extra]}")

        # the messages are only returned once, as on D3D11
        again = self.controller.GetDebugMessages()
        if again:
            problems.append(f"GetDebugMessages returned {len(again)} messages a second time")

        # the draw after the failed calls still replays
        triangle = self.find_marker('Triangle').children[0]
        self.controller.SetFrameEvent(triangle.eventId, True)
        pipe = self.controller.GetPipelineState()
        rt = pipe.GetOutputTargets()[0].resource
        self.check_pixel_value(rt, 0.5, 0.5, [0.0, 1.0, 0.0, 1.0], eps=1.0 / 255.0)
        rdtest.log.success("The triangle after the failed calls replays")

        for p in problems:
            rdtest.log.error(p)
        if problems:
            raise rdtest.TestFailureException(f"{len(problems)} problems with the failed calls")

        self.check_export(self.capture_filename)
