import renderdoc as rd
import rdtest


class D3D9_Simple_Triangle(rdtest.D3D9_Test):
    demos_test_name = 'D3D9_Simple_Triangle'
    internal = False

    def check_d3d9_capture(self):
        self.check_api()

        replay = self.check_script('d3d9_replay_check')

        last_action = self.get_last_action()
        if not (last_action.flags & rd.ActionFlags.Present):
            raise rdtest.TestFailureException(f"The last action, {self.action_name(last_action)}, isn't a Present")
        backbuffer = last_action.copyDestination

        self.set_event(last_action.eventId, True)

        # the shader triangle: green in the middle, on the dark grey clear
        self.check_triangle(out=backbuffer)

        tex = self.get_texture(backbuffer)
        w, h = tex.width, tex.height

        # the fixed function DrawPrimitiveUP triangle, in the bottom left
        self.check_pixel_value(backbuffer, int(w * 0.15), int(h * 0.9), [1.0, 0.0, 1.0, 1.0])

        # the pixel shader constants triangle, in the bottom right: (0.5, 2, 1, 1) * 4 steps of
        # (0.4, 0.2, 0, 0) is 96, 191, 0. 191, 96, 0 would mean b3 was read as FALSE, and black that
        # the loop over i0 didn't run.
        self.check_pixel_value(backbuffer, int(w * 0.85), int(h * 0.88), [96 / 255.0, 191 / 255.0, 0.0, 1.0])

        # SaveTexture writes an X8R8G8B8 back buffer opaque
        alpha = replay.check_save_texture(self.controller, tex, rdtest.get_tmp_path('saved.png'))
        if alpha != (255, 255):
            raise rdtest.TestFailureException(f"SaveTexture's PNG has alpha {alpha}, expected 255-255")
        rdtest.log.success("SaveTexture writes the back buffer opaque")

        # the pipeline state at every draw, and constants read with a length of 0
        pipestate = self.check_script('d3d9_pipestate_check')
        self.run_script_check('The pipeline state check', pipestate.check_capture, self.controller)

        draw = self.find_marker_draw('Pixel shader constants')
        self.set_event(draw.eventId, True)
        pipe = self.controller.GetD3D9PipelineState()
        if pipe.pixelShader.shaderModel != 'ps_3_0':
            raise rdtest.TestFailureException(f"The pixel shader is {pipe.pixelShader.shaderModel}, expected ps_3_0")
        refl = pipe.pixelShader.reflection
        blocks = [b.name for b in refl.constantBlocks]
        expected = ['Float constants (c#)', 'Integer constants (i#)', 'Boolean constants (b#)']
        if blocks != expected:
            raise rdtest.TestFailureException(f"The pixel shader's constant blocks are {blocks}, expected {expected}")
        rdtest.log.success("The pixel shader has its float, int and bool constant blocks")

        # the fixed function draw has no shaders and its declaration comes from the FVF
        draw = self.find_marker_draw('Fixed function triangle')
        self.set_event(draw.eventId, True)
        pipe = self.controller.GetD3D9PipelineState()
        if pipe.vertexShader.resourceId != rd.ResourceId.Null() or pipe.pixelShader.resourceId != rd.ResourceId.Null():
            raise rdtest.TestFailureException("The fixed function draw has a shader bound")
        if pipe.inputAssembly.fvf == 0:
            raise rdtest.TestFailureException("The fixed function draw has no FVF")
        rdtest.log.success(f"The fixed function draw has no shaders and FVF 0x{pipe.inputAssembly.fvf:x}")

        self.step_all_actions()

        self.check_export(self.capture_filename)
