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

// Pixel shader debugging (steps 5.7 and 5.8): the parts that don't need a device, so they're unit
// tested on every platform. The replay side (DebugPixel in d3d9_shaderdebug.cpp) draws the event
// again with the shaders BuildInputReadback makes, reads what they wrote, and hands the quad
// QuadInputs builds from it to a PSDebugger, which steps the pixel through the interpreter
// (d3d9_ps_emulate.h). Its lookups and transcendentals come from a PSLookupCache, filled on the GPU
// with the shaders BuildSampleShader and BuildTranscendentalShader make.

#include <string.h>
#include <map>
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
// ps_3_0 shaders. ps_1_x comes in step 5.9.
bool PixelShaderDebuggable(const D3D9BC::Program &prog, rdcstr &status);

// The ps_3_0 shader that looks up a sample on the GPU (5.8), drawn to a 2x2 A32B32G32R32F target
// whose pixels are the quad's, from sampler as the event set it up, writing oC0. For texld, texldp
// and texldb each pixel looks up its own coordinate, c0-c3 in QuadPixel order (picked by vPos), so
// the GPU works out the derivatives, the level and the anisotropy as it did for the event. texldd
// takes the coordinate and derivatives from c0-c2, and texldl the coordinate from c0, the same in
// every pixel. type is the sampler's declaration; Unknown (ps_1_x) or 1D sample as 2D.
rdcarray<uint32_t> BuildSampleShader(SampleKind kind, uint32_t sampler,
                                     D3D9BC::SamplerTextureType type);
// the constants BuildSampleShader's shader takes for sample, returning how many
uint32_t SampleShaderConstants(const PSSample &sample, Vec4 consts[QuadPixels]);
// The ps_3_0 shader that runs rcp, rsq, exp, log, pow or sincos on the GPU, drawn like
// BuildSampleShader's: each pixel of the 2x2 target runs it on x (and y, pow's exponent) of c0-c3 in
// QuadPixel order, picked by vPos, writing the result as CPUTranscendental returns it to oC0:
// sincos's (cos, sin, 0, 0), the others' in x and 0 in y, z and w.
rdcarray<uint32_t> BuildTranscendentalShader(D3D9BC::Opcode op);

// Where PSLookupCache gets what it hasn't seen yet: the GPU, at replay
class PSLookupSource
{
public:
  virtual ~PSLookupSource() = default;
  // texld, texldp and texldb (and ps_1_x's lookups): the result in each pixel of sample.quad;
  // texldd and texldl: the result in results[0]. Returns false with the reason in error.
  virtual bool SampleQuad(const PSSample &sample, Vec4 results[QuadPixels], rdcstr &error) = 0;
  virtual bool Transcendental(D3D9BC::Opcode op, float a, float b, Vec4 &result, rdcstr &error) = 0;
};

// The callbacks for a debugged pixel's quad: every lookup and transcendental, kept by what was
// asked. DebugPixel runs the quad once with the GPU as the source while the replay is at the
// event, then takes the source away, and the debugger steps the same quad from the cache alone,
// whenever the UI asks, since by then the replay may be anywhere. A quad's four texld, texldp or
// texldb lookups are one GPU draw, in whichever pixel asks first.
class PSLookupCache : public PSCallbacks
{
public:
  void SetSource(PSLookupSource *source) { m_Source = source; }
  Vec4 Sample(const PSSample &sample) override;
  Vec4 Transcendental(D3D9BC::Opcode op, float a, float b) override;

  // how many lookups and transcendentals there were: what was looked up and run
  size_t NumSamples() const { return m_Samples.size(); }
  size_t NumTranscendentals() const { return m_Transcendentals.size(); }
  // lookups that failed, or weren't cached once the source was gone: their results are 0, and
  // transcendentals' the C library's. The first one's reason is in GetError().
  uint32_t Failures() const { return m_Failures; }
  const rdcstr &GetError() const { return m_Error; }

private:
  struct Key
  {
    uint32_t words[20] = {};
    bool operator<(const Key &o) const { return memcmp(words, o.words, sizeof(words)) < 0; }
  };
  struct Results
  {
    Vec4 v[QuadPixels];
  };
  static Key SampleKey(const PSSample &s);
  void Fail(const rdcstr &why);

  PSLookupSource *m_Source = NULL;
  std::map<Key, Results> m_Samples;
  std::map<Key, Vec4> m_Transcendentals;
  uint32_t m_Failures = 0;
  rdcstr m_Error;
};

class PSDebugger : public StepDebugger
{
public:
  // Starts debugging pixel (a QuadPixel) of a quad, returning its trace with debugger set to
  // this. inputs are copied. emu (initialised without an error), consts (from emu.ApplyLiterals),
  // callbacks (a PSLookupCache at replay, or NULL for a shader without lookups, whose
  // transcendentals then come from the C library) and stages (or NULL) must outlive the
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
