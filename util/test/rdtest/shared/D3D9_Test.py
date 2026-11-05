import contextlib
import importlib
import io
import os
import re
import struct
import subprocess
import sys
import time

import renderdoc as rd
import rdtest

# util/test, where the standalone d3d9_*_check.py scripts the D3D9 tests share their checks with are
_util_test_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

_demo_log_problem = re.compile(r'\b(Error|Fatal Error):')


# Not a direct test, re-used by the D3D9 tests (and D3D11_D3DPERF_Markers, which belongs to them).
#
# Adds what those tests need on top of TestCase:
# - the demo's log is checked for TEST_ERRORs. The D3D9 demos check a lot themselves (state read
#   back after a capture read it, the wrapping checks...), and with --log those only show in the
#   log. check_capture() checks it before calling check_d3d9_capture().
# - running the demo natively, for demos that write a reference of what the GPU does
#   (D3D9_Texture_Formats, D3D9_Mesh_Output). They write it to the demo's tmp folder, which is the
#   test's tmp folder since the demo is named like the test.
# - running a check from the standalone util/test/d3d9_*_check.py scripts on the open capture, with
#   its output in the test log.
class D3D9_Test(rdtest.TestCase):
    internal = True

    # how many frames the native run of run_native() draws
    native_frames = 3

    # how long the demo keeps running after the last capture, so that the checks it makes in the
    # frames after a captured one get to its log
    linger_seconds = 2.0

    def get_capture(self):
        return self.capture_demo(self.demos_frame_cap)[0].path

    def capture_demo(self, frame, captures_expected: int = 1):
        """
        Runs the demo under RenderDoc like TestCase.get_capture(), capturing frame (or nothing, for
        demos that capture themselves with the in-app API when frame is None), and lets it run for
        linger_seconds after the last capture. Returns the renderdoc.NewCaptureData of each capture.
        """
        logfile = self.demo_log_path()
        os.makedirs(os.path.dirname(logfile), exist_ok=True)
        cmdline = f"{self.demos_test_name} --log {logfile} --tmp {rdtest.get_tmp_dir()}"
        timeout = self.demos_timeout if self.demos_timeout is not None else rdtest.get_demos_timeout()

        ident = rdtest.run_executable(rdtest.get_demos_binary(), cmdline, cappath=rdtest.get_tmp_path('capture'),
                                      opts=self.get_capture_options())
        control = rdtest.TargetControl(ident, timeout=timeout)

        if frame is not None:
            rdtest.log.print(f"Queuing capture of frame {frame}")
            control.queue_capture(frame)

        last_capture = []

        def keep_running(c: rdtest.TargetControl):
            if len(c.captures()) < captures_expected:
                return True
            if not last_capture:
                last_capture.append(time.monotonic())
            return time.monotonic() - last_capture[0] < self.linger_seconds

        control.run(keep_running)

        captures = control.captures()
        rdtest.log.print(f"Retrieved {len(captures)} captures, of frames {[c.frameNumber for c in captures]}")

        if os.path.exists(logfile):
            rdtest.log.inline_file('Process output', logfile, with_stdout=True)

        if len(captures) != captures_expected:
            raise rdtest.TestFailureException(f"Expected {captures_expected} captures, got {len(captures)}")

        return captures

    def check_capture(self):
        self.check_demo_log(self.demo_log_path())
        self.check_d3d9_capture()

    def check_d3d9_capture(self):
        raise NotImplementedError("D3D9 tests implement check_d3d9_capture()")

    def demo_tmp_path(self, name: str):
        # the demo puts its files in <--tmp>/<demo name>, and get_capture() passes the test's tmp dir
        return os.path.join(rdtest.get_tmp_dir(), self.demos_test_name, name)

    def demo_log_path(self):
        return os.path.join(rdtest.get_tmp_dir(), rdtest.get_current_test_name(), 'demos.log')

    def check_demo_log(self, path: str, what: str = 'The demo'):
        if not os.path.exists(path):
            raise rdtest.TestFailureException(f"{what} wrote no log at {path}")
        with open(path, errors='replace') as f:
            lines = [l.rstrip() for l in f]
        problems = [l for l in lines if _demo_log_problem.search(l)]
        warnings = [l for l in lines if ' Warning: ' in l]
        for w in warnings:
            rdtest.log.print(f"{what} warned: {w}")
        if problems:
            for p in problems:
                rdtest.log.print(f"{what} logged: {p}")
            raise rdtest.TestFailureException(f"{what} logged {len(problems)} errors, the first: {problems[0]}")
        rdtest.log.success(f"{what}'s log has no errors ({len(warnings)} warnings)")

    def run_native(self, extra_args=None):
        """
        Runs the demo without RenderDoc for native_frames frames, with its log at native.log in the
        test's tmp folder, and checks that log for errors.
        """
        logfile = rdtest.get_tmp_path('native.log')
        args = [rdtest.get_demos_binary(), self.demos_test_name, '--frames', str(self.native_frames),
                '--log', logfile, '--tmp', rdtest.get_tmp_dir()] + (extra_args or [])
        rdtest.log.print(f"Running {self.demos_test_name} natively for {self.native_frames} frames")
        timeout = self.demos_timeout if self.demos_timeout is not None else 120
        try:
            proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise rdtest.TestFailureException(f"The native run didn't finish in {timeout} seconds")
        if os.path.exists(logfile):
            rdtest.log.inline_file('Native run output', logfile)
        if proc.returncode != 0:
            raise rdtest.TestFailureException(f"The native run exited with {proc.returncode}")
        self.check_demo_log(logfile, 'The native run')

    def check_script(self, module_name: str):
        """Imports one of the util/test/d3d9_*_check.py scripts."""
        if _util_test_dir not in sys.path:
            sys.path.insert(0, _util_test_dir)
        return importlib.import_module(module_name)

    def run_script_check(self, what: str, func, *args):
        """
        Calls one of the check scripts' functions, with its printed output going to the test log,
        and fails the test if it doesn't return a true value. Returns what it returned.
        """
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            result = func(*args)
        for line in out.getvalue().splitlines():
            rdtest.log.print(line)
        if not result:
            raise rdtest.TestFailureException(f"{what} failed, see the lines above")
        rdtest.log.success(f"{what} passed")
        return result

    def check_api(self, api=rd.GraphicsAPI.D3D9):
        props = self.controller.GetAPIProperties()
        if props.pipelineType != api:
            raise rdtest.TestFailureException(f"The capture's API is {props.pipelineType}, expected {api}")

    def find_marker_draw(self, name: str):
        """The first draw inside the marker called name."""
        marker = self.find_action(name)
        if marker is None:
            raise rdtest.TestFailureException(f"No marker '{name}'")

        def first_draw(list_):
            for a in list_:
                if a.flags & rd.ActionFlags.Drawcall:
                    return a
                d = first_draw(a.children)
                if d is not None:
                    return d
            return None

        draw = first_draw(marker.children)
        if draw is None:
            raise rdtest.TestFailureException(f"No draw in the marker '{name}'")
        return draw

    def texture_by_name(self, name: str):
        res = self.get_resource_by_name(name)
        if res is None:
            raise rdtest.TestFailureException(f"No resource named '{name}'")
        tex = self.get_texture(res.resourceId)
        if tex is None:
            raise rdtest.TestFailureException(f"'{name}' isn't a texture")
        return tex

    def read_dwords(self, tex: rd.TextureDescription, sub: rd.Subresource):
        """
        A 32-bit texture's subresource as rows of DWORDs (0xAARRGGBB for B8G8R8A8). For a 3D texture
        sub.slice is the depth slice: GetTextureData returns the whole mip, as on D3D11.
        """
        w = max(1, tex.width >> sub.mip)
        h = max(1, tex.height >> sub.mip)
        offset = 0
        if tex.type == rd.TextureType.Texture3D:
            offset = sub.slice * w * h * 4
            sub = rd.Subresource(sub.mip, 0, sub.sample)
        data = self.controller.GetTextureData(tex.resourceId, sub)
        if len(data) < offset + w * h * 4:
            raise rdtest.TestFailureException(f"Read {len(data)} bytes of a {w}x{h} subresource of {tex.resourceId}")
        vals = struct.unpack_from(f'<{w * h}I', data, offset)
        return [list(vals[y * w:(y + 1) * w]) for y in range(h)]

    def check_texels(self, what: str, rows, expected, mask=0xFFFFFFFF):
        """
        Checks every texel. expected(x, y) gives the value, or a tuple of the values allowed there.
        Returns the set of values found where several were allowed.
        """
        bad = []
        chosen = set()
        for y, row in enumerate(rows):
            for x, v in enumerate(row):
                want = expected(x, y)
                allowed = want if isinstance(want, tuple) else (want,)
                if (v & mask) not in [a & mask for a in allowed]:
                    bad.append((x, y, v, want))
                elif len(allowed) > 1:
                    chosen.add(v & mask)
        if bad:
            x, y, v, want = bad[0]
            want_str = ' or '.join(f'0x{a:08X}' for a in (want if isinstance(want, tuple) else (want,)))
            raise rdtest.TestFailureException(
                f"{what}: {len(bad)} texels are wrong, the first at {x},{y} is 0x{v:08X}, expected {want_str}")
        rdtest.log.success(f"{what} is as expected")
        return chosen

    def step_all_actions(self):
        """Replays to every action, as clicking through the event browser does."""
        actions = []

        def walk(list_):
            for a in list_:
                actions.append(a)
                walk(a.children)

        walk(self.controller.GetRootActions())
        for a in actions:
            self.controller.SetFrameEvent(a.eventId, True)
        rdtest.log.success(f"Replayed to each of the {len(actions)} actions")
