import os

import renderdoc as rd
import rdtest


class D3D9_Counters(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Counters'
    internal = False

    # the draws the demo counts with occlusion queries of its own, in order
    draw_names = ['Scissored quad', 'Hidden quad', 'Half hidden quad', 'Heavy quad', 'Single pixel quad',
                  'Multisampled quad']

    def get_capture(self):
        # the native run writes what the GPU's occlusion queries counted for each draw
        self.run_native()
        return super().get_capture()

    def read_reference(self, path: str, conv=int):
        if not os.path.exists(path):
            raise rdtest.TestFailureException(f"The demo didn't write {path}")
        counts = {}
        with open(path) as f:
            for line in f:
                name, samples = line.rstrip('\n').split('\t')
                counts[name] = conv(samples)
        return counts

    def check_d3d9_capture(self):
        self.check_api()

        # what the GPU counted natively, and under RenderDoc while capturing. The demo checks both
        # against the counts its scissor rects and depths give, and logs an error if they differ.
        native = self.read_reference(self.demo_tmp_path('D3D9_Counters_reference.txt'))
        under_rdoc = self.read_reference(self.demo_tmp_path('D3D9_Counters_reference_rdoc.txt'))
        if native != under_rdoc:
            raise rdtest.TestFailureException(f"The counts under RenderDoc {under_rdoc} differ from native {native}")
        rdtest.log.success(f"The GPU's counts are the same natively and under RenderDoc: {native}")

        counters = self.controller.EnumerateCounters()
        for c in (rd.GPUCounter.EventGPUDuration, rd.GPUCounter.SamplesPassed):
            if c not in counters:
                raise rdtest.TestFailureException(f"{c} isn't in the enumerated counters {counters}")
        rdtest.log.success(f"The counters enumerated are {[self.controller.DescribeCounter(c).name for c in counters]}")

        desc = self.controller.DescribeCounter(rd.GPUCounter.EventGPUDuration)
        if desc.unit != rd.CounterUnit.Seconds or desc.resultType != rd.CompType.Float or desc.resultByteWidth != 8:
            raise rdtest.TestFailureException(f"GPU Duration is described as {desc.unit} {desc.resultType} "
                                              f"{desc.resultByteWidth} bytes")
        desc = self.controller.DescribeCounter(rd.GPUCounter.SamplesPassed)
        if desc.unit != rd.CounterUnit.Absolute or desc.resultType != rd.CompType.UInt or desc.resultByteWidth != 8:
            raise rdtest.TestFailureException(f"Samples Passed is described as {desc.unit} {desc.resultType} "
                                              f"{desc.resultByteWidth} bytes")
        rdtest.log.success("The counters are described with their units and types")

        # FetchCounters leaves the replay where it was: at the first draw, the back buffer has only
        # its red quad, which the heavy quad later covers
        first = self.find_marker_draw('Scissored quad')
        self.set_event(first.eventId, True)
        backbuffer = first.outputs[0]

        results = self.controller.FetchCounters([rd.GPUCounter.EventGPUDuration, rd.GPUCounter.SamplesPassed])

        self.check_pixel_value(backbuffer, 20, 20, [1.0, 0.0, 0.0, 1.0])
        rdtest.log.success("After fetching the counters the back buffer is as it was at the first draw")

        durations = {}
        samples = {}
        for r in results:
            if r.counter == rd.GPUCounter.EventGPUDuration:
                durations[r.eventId] = r.value.d
            elif r.counter == rd.GPUCounter.SamplesPassed:
                samples[r.eventId] = r.value.u64

        # every action that isn't a marker has both
        actions = []

        def walk(list_):
            for a in list_:
                walk(a.children)
                if not (a.flags & (rd.ActionFlags.PushMarker | rd.ActionFlags.PopMarker | rd.ActionFlags.SetMarker)):
                    actions.append(a)

        walk(self.controller.GetRootActions())
        for a in actions:
            if a.eventId not in durations or a.eventId not in samples:
                raise rdtest.TestFailureException(f"{self.action_name(a)} at {a.eventId} has no counter results")
            if durations[a.eventId] < 0.0:
                raise rdtest.TestFailureException(f"{self.action_name(a)} at {a.eventId} has no duration")
            rdtest.log.print(f"{a.eventId} {self.action_name(a)}: {durations[a.eventId] * 1e6:.2f} us, "
                             f"{samples[a.eventId]} samples")
        if len(durations) != len(actions) or len(samples) != len(actions):
            raise rdtest.TestFailureException(f"{len(durations)} durations and {len(samples)} sample counts "
                                              f"for {len(actions)} actions")
        rdtest.log.success(f"Each of the {len(actions)} actions has a duration and a sample count")

        # each draw passes the samples it did natively
        for name in self.draw_names:
            if name not in native:
                rdtest.log.print(f"The demo didn't count '{name}', skipping it")
                continue
            draw = self.find_marker_draw(name)
            if samples[draw.eventId] != native[name]:
                raise rdtest.TestFailureException(
                    f"'{name}' passed {samples[draw.eventId]} samples at replay, {native[name]} natively")
            rdtest.log.success(f"'{name}' passed {samples[draw.eventId]} samples, as natively")

        # the full screen draw with about 4000 loop iterations per pixel takes far longer than the
        # same shader on one pixel
        heavy = durations[self.find_marker_draw('Heavy quad').eventId]
        single = durations[self.find_marker_draw('Single pixel quad').eventId]
        if heavy <= 0.0 or heavy < 10.0 * single:
            raise rdtest.TestFailureException(
                f"The heavy quad took {heavy * 1e6:.2f} us and the single pixel one {single * 1e6:.2f} us")
        rdtest.log.success(f"The heavy quad took {heavy * 1e6:.2f} us, the single pixel one {single * 1e6:.2f} us")

        # and about as long as the demo's own timestamps measured natively. The GPU's clocks differ
        # between runs, so this only catches durations in the wrong units or from the wrong
        # frequency, not small differences.
        timing = self.read_reference(self.demo_tmp_path('D3D9_Counters_timing.txt'), float)
        native_heavy = timing['Heavy quad']
        rdtest.log.print(f"Natively the heavy quad took {native_heavy * 1e6:.2f} us at its fastest, with a "
                         f"timestamp frequency of {timing['Timestamp frequency']:.0f} Hz")
        ratio = heavy / native_heavy if native_heavy > 0.0 else 0.0
        if not 0.25 <= ratio <= 4.0:
            raise rdtest.TestFailureException(
                f"The heavy quad took {heavy * 1e6:.2f} us at replay, {native_heavy * 1e6:.2f} us natively")
        rdtest.log.success(f"The heavy quad's duration at replay is {ratio:.2f} times the native one")

        # a second fetch gives the same sample counts
        again = self.controller.FetchCounters([rd.GPUCounter.SamplesPassed])
        again = {r.eventId: r.value.u64 for r in again}
        if again != samples:
            raise rdtest.TestFailureException("A second fetch of the samples passed gave different counts")
        rdtest.log.success("A second fetch gives the same sample counts")

        self.check_export(self.capture_filename)
