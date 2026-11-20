/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

// Pixel shader debugging (step 5.7): the parts that don't need a device, so they're unit tested on
// every platform. The replay side (DebugPixel in d3d9_shaderdebug.cpp) draws the event again with
// the shaders BuildInputReadback makes, reads what they wrote, and hands the quad QuadInputs builds
// from it to a PSDebugger, which steps the pixel through the interpreter (d3d9_ps_emulate.h).

#include "d3d9_ps_emulate.h"
#include "d3d9_vs_debug.h"

namespace D3D9Emu
{
// what one render target of a readback pass holds
enum class ReadbackValue : uint8_t
{
  // the input register as the pixel got it
  Value,
  // its dsx and dsy, across the pixel's quad (ps_2_x)
  Ddx,
  Ddy,
  // vFace in x: +1 or -1, read with cmp since it can only be a condition
  Face,
  // the register in the quad's top left, top right and bottom left pixels, which dsx and dsy
  // are worked out from (ps_3_0, which picks them out with vPos)
  TopLeft,
  TopRight,
  BottomLeft,
};

struct ReadbackItem
{
  ReadbackValue what = ReadbackValue::Value;
  // Input (v#) or AddrOrTexture (t#, the texture coordinates); MiscType 1 for vFace
  D3D9BC::RegType type = D3D9BC::RegType::Input;
  uint32_t index = 0;
  // the components declared, which are written; the others are 0
  uint8_t mask = 0xF;
};

// one draw of the readback: a pixel shader writing items[n] to oC#n, or 0 to oC0 when it has none
struct ReadbackPass
{
  rdcarray<ReadbackItem> items;
  rdcarray<uint32_t> tokens;
};

// Builds the pixel shaders that read back the inputs prog (a ps_1_1 to ps_3_0 shader) gets, and
// what the other pixels of its quad got, at most maxTargets (1-4) values a pass. They declare the
// inputs as prog does, so the rasterizer interpolates them the same way:
// - for ps_3_0 a ps_3_0 shader with prog's own input declarations (semantics, masks, centroid),
//   which also reads vFace if prog declares it. It reads the quad's top left, top right and bottom
//   left pixels' inputs exactly, picking them out by vPos. vPos itself isn't read: it's the
//   pixel's position.
// - otherwise ps_2_x, the first model with dsx and dsy, with prog's t# and v# declarations, or for
//   ps_1_x the t# and v# it reads. It reads the inputs' dsx and dsy, which give the quad's other
//   pixels' inputs, when gradients is true; without them it's ps_2_0, and reads only the pixel's.
// A shader that reads no inputs gets one pass with no items. Returns false with the reason in
// error if prog isn't a pixel shader that can be read back.
bool BuildInputReadback(const D3D9BC::Program &prog, bool gradients, uint32_t maxTargets,
                        rdcarray<ReadbackPass> &passes, rdcstr &error);

// The quad of pixel (x, y) from the readback's results (results[pass][target], each pass's
// targets' values at the pixel), in QuadPixel order; the pixel itself is at QuadPixelOf(x, y).
// The pixel gets its values as read. dsx is the quad's top row's difference and dsy its left
// column's, so the top left, top right and bottom left pixels are what the interpreter's
// derivatives come from:
// - ps_3_0's are read in the pixel itself.
// - ps_2_x's are corners[0-2], the same readback drawn at those pixels (empty where it wasn't, or
//   the draw doesn't write it, or with corners NULL), for each input whose derivatives there are
//   the pixel's bit for bit: then the same primitive covered it. Otherwise they're worked out from
//   the pixel's derivatives, which gets them back exactly from the top row and left column, but
//   with perspective only nearly from the bottom right.
// Without derivatives all four get the pixel's values. vPos is each pixel's position, vFace the
// pixel's. t# items go to PSInputs::t, the texture coordinates, in every model.
void QuadInputs(const rdcarray<ReadbackPass> &passes, const rdcarray<rdcarray<Vec4>> &results,
                const rdcarray<rdcarray<Vec4>> *corners, uint32_t x, uint32_t y,
                PSInputs quad[QuadPixels]);

inline uint32_t QuadPixelOf(uint32_t x, uint32_t y)
{
  return (y & 1) * 2 + (x & 1);
}

// whether inst samples a texture: texld, texldp, texldb, texldd, texldl, and ps_1_x's texture
// instructions that look up their stage
bool PSInstructionSamples(const D3D9BC::Instruction &inst);

// Whether DebugPixel can debug prog yet, and if not why (the reflection's debugStatus): ps_2_0 to
// ps_3_0 shaders that don't sample. Sampling on the GPU (step 5.8) and ps_1_x (5.9) come later.
bool PixelShaderDebuggable(const D3D9BC::Program &prog, rdcstr &status);

class PSDebugger : public StepDebugger
{
public:
  // Starts debugging pixel (a QuadPixel) of a quad, returning its trace with debugger set to
  // this. inputs are copied. emu (initialised without an error), consts (from emu.ApplyLiterals),
  // callbacks (or NULL for a shader that doesn't sample) and stages (or NULL) must outlive the
  // debugger: owner can keep what they belong to alive. info is the shader's reflection and
  // constant table (only used here), and instLines each instruction's line in its disassembly.
  ShaderDebugTrace *Begin(const PSEmulator &emu, const D3D9BC::ShaderInfo &info,
                          const rdcarray<uint32_t> &instLines, const PSInputs inputs[QuadPixels],
                          uint32_t pixel, const VSConstants &consts, PSCallbacks *callbacks,
                          const PSStageState *stages);

  const rdcstr &GetError() const override { return Thread().GetError(); }
  // the quad, as it is after the states returned so far
  const PSQuad &Quad() const { return m_Quad; }

private:
  const PSThread &Thread() const { return m_Quad.Pixel(m_Pixel); }
  bool ThreadFinished() const override { return Thread().Finished(); }
  uint32_t ThreadNextInstruction() const override { return Thread().NextInstruction(); }
  bool ThreadStep(rdcarray<VSRegisterChange> &changes) override
  {
    return m_Quad.Step(m_Pixel, &changes);
  }
  const FlowRegisters &Flow() const override { return Thread().Registers(); }
  ShaderVariable Variable(D3D9BC::RegType type, uint32_t index, const Vec4 &f,
                          const IVec4 &i) const override;
  ShaderVariable Current(D3D9BC::RegType type, uint32_t index) const override;
  bool Samples(const D3D9BC::Instruction &inst) const override
  {
    return PSInstructionSamples(inst);
  }

  // the name of an input register: v#, t#, vPos and vFace, and ps_1_1 to ps_1_3's texture
  // coordinates as texcoord# (their t# are the registers the texture instructions write)
  rdcstr InputName(D3D9BC::RegType type, uint32_t index) const;

  PSQuad m_Quad;
  uint32_t m_Pixel = 0;
  bool m_TexRegisters = false;
};

};    // namespace D3D9Emu
