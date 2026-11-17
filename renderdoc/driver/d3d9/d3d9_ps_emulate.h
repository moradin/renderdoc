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

// The pixel shader interpreter for ps_2_0, ps_2_x and ps_3_0 (step 5.5). It runs a 2x2 quad of
// pixels in lockstep, so dsx, dsy and texld's derivatives come from the neighbouring pixels as they
// do on the GPU. Sampling goes through a callback, and so do the transcendentals, since the GPU's
// differ from the C library in the last bits (5.8 runs both on the GPU). The arithmetic follows
// what the GPU was measured doing (STATUS.md, "What the GPU does with pixel shaders"). Like the
// vertex interpreter, whose flow control and constants it shares, it doesn't need any Windows
// headers, so it's unit tested on every platform.

#include "d3d9_vs_emulate.h"

namespace D3D9Emu
{
enum
{
  MaxPSTemps = 32,
  // ps_3_0's v0-v9; ps_2_0 and ps_2_x have v0 and v1, the colours
  MaxPSInputs = 10,
  // ps_2_0 and ps_2_x's t0-t7
  MaxPSTexCoords = 8,
  MaxPSColorOutputs = 4,
  NumPixelSamplers = 16,
};

// the pixels of a quad, in the order PSQuad keeps them
enum QuadPixel : uint32_t
{
  QuadTopLeft = 0,
  QuadTopRight = 1,
  QuadBottomLeft = 2,
  QuadBottomRight = 3,
  QuadPixels = 4,
};

// one pixel's inputs, as the rasterizer gives them
struct PSInputs
{
  Vec4 v[MaxPSInputs];
  Vec4 t[MaxPSTexCoords];
  // ps_3_0's vPos (the pixel's position in the render target, in x and y) and vFace (+1 for a
  // triangle clockwise on screen, -1 otherwise)
  Vec4 pos;
  float face = 1.0f;
};

// bits in PSOutputs::written
enum : uint32_t
{
  // oC0 to oC3 are bits 0 to 3
  PSWroteDepth = 1U << 4,
};

struct PSOutputs
{
  Vec4 color[MaxPSColorOutputs];
  // oDepth, in x
  Vec4 depth;
  // which of them the shader wrote
  uint32_t written = 0;
};

// the registers of one pixel's run, as they are between two instructions
struct PSRegisters : FlowRegisters
{
  Vec4 r[MaxPSTemps];
  PSOutputs out;
  // texkill discarded the pixel. It still runs to the end, as the GPU runs it for its neighbours'
  // derivatives.
  bool killed = false;
};

// a register an instruction changed, and what it held before: r#, p0, aL, oC# (ColorOut) or
// oDepth (DepthOut)
typedef VSRegisterChange PSRegisterChange;

enum class SampleKind : uint8_t
{
  // texld
  Implicit,
  // texldp
  Projected,
  // texldb
  Bias,
  // texldd
  Gradient,
  // texldl
  Level,
};

// a texture lookup, for PSCallbacks::Sample
struct PSSample
{
  SampleKind kind = SampleKind::Implicit;
  uint32_t sampler = 0;
  // as the shader declares the sampler (dcl_2d, dcl_cube, dcl_volume)
  D3D9BC::SamplerTextureType type = D3D9BC::SamplerTextureType::Unknown;
  // the coordinate the shader gave, with texldp's not divided by w yet
  Vec4 coord;
  // the derivatives of the coordinate the lookup uses (after texldp's divide), across the quad
  // like dsx and dsy, or texldd's. Zero for texldl.
  Vec4 ddx, ddy;
  // texldb's bias or texldl's level, both coord.w. Zero otherwise.
  float lod = 0.0f;
};

// The C library's rcp, rsq, exp, log, pow and sincos, with the GPU's special cases. a and b are the
// instruction's scalar sources (b only for pow), with denormals already flushed. sincos returns
// (cos, sin, 0, 0), the others their result in x.
Vec4 CPUTranscendental(D3D9BC::Opcode op, float a, float b);

class PSCallbacks
{
public:
  virtual ~PSCallbacks() = default;
  // a texture lookup's result, before the sampler register's swizzle
  virtual Vec4 Sample(const PSSample &sample) = 0;
  // rcp, rsq, exp, log, pow or sincos, as CPUTranscendental
  virtual Vec4 Transcendental(D3D9BC::Opcode op, float a, float b)
  {
    return CPUTranscendental(op, a, b);
  }
};

class PSEmulator;
class PSQuad;

// One pixel's run of a shader. A PSQuad runs four together.
class PSThread : private PSRegisters
{
public:
  bool Finished() const { return m_Finished; }
  bool Failed() const { return !m_Error.empty(); }
  const rdcstr &GetError() const { return m_Error; }
  // the instruction Step runs next
  uint32_t NextInstruction() const { return m_IP; }
  uint32_t InstructionsExecuted() const { return m_Executed; }
  const PSRegisters &Registers() const { return *this; }
  const PSInputs &Inputs() const { return m_Inputs; }

private:
  friend class PSQuad;

  void Start(const PSEmulator &emu, const PSInputs &inputs, const VSConstants &consts,
             PSCallbacks *callbacks);
  // the source the next instruction needs from all four pixels (dsx's, dsy's and the coordinate of
  // texld, texldp and texldb), or NULL
  const D3D9BC::SrcParam *QuadSource() const;
  bool Step(rdcarray<PSRegisterChange> *changes);

  int32_t RelValue(const D3D9BC::RelativeAddress &rel) const;
  Vec4 ReadRaw(const D3D9BC::SrcParam &src, int32_t regOffset) const;
  Vec4 Read(const D3D9BC::SrcParam &src, int32_t regOffset = 0) const;
  Vec4 ReadFlushed(const D3D9BC::SrcParam &src, int32_t regOffset = 0) const;
  bool ReadBool(const D3D9BC::SrcParam &src) const;
  IVec4 ReadInt(const D3D9BC::SrcParam &src) const;
  uint8_t WriteMask(const D3D9BC::Instruction &inst) const;
  void Write(const D3D9BC::Instruction &inst, Vec4 val);
  bool Sample(const D3D9BC::Instruction &inst, Vec4 &result);
  bool Fail(const rdcstr &msg);

  const PSEmulator *m_Emu = NULL;
  PSInputs m_Inputs;
  const VSConstants *m_Consts = NULL;
  PSCallbacks *m_Callbacks = NULL;

  // QuadSource's value in each pixel of the quad, before this instruction ran in any of them
  Vec4 m_Quad[QuadPixels];

  uint32_t m_IP = 0;
  uint32_t m_Executed = 0;
  bool m_Finished = true;
  rdcstr m_Error;

  // where Write records changes, during Step
  rdcarray<PSRegisterChange> *m_Changes = NULL;
};

// The four pixels of a 2x2 quad, run in lockstep: each step runs one instruction in each pixel
// that hasn't finished. The neighbours' values dsx, dsy and texld read are taken before any pixel
// runs the instruction. Derivatives are per quad, as measured on the GPU: dsx is the top row's
// difference and dsy the left column's, in all four pixels. Pixels outside the primitive run like
// the others (the GPU's helper pixels), with their inputs extrapolated by the caller.
class PSQuad
{
public:
  // starts all four pixels at the first instruction. inputs are copied; emu (initialised), consts
  // (from emu.ApplyLiterals) and callbacks must outlive the quad. callbacks may be NULL for a
  // shader that doesn't sample.
  void Start(const PSEmulator &emu, const PSInputs inputs[QuadPixels], const VSConstants &consts,
             PSCallbacks *callbacks);

  const PSThread &Pixel(uint32_t i) const { return m_Pixels[i & 3]; }
  // every pixel has finished (or failed)
  bool Finished() const;

  // runs the next instruction in each pixel that hasn't finished. changes, if given, gets what
  // pixel changed. Returns false if pixel had nothing to run, or its instruction failed: it's then
  // finished with its error set, and its registers as they were before. The other pixels go on.
  bool Step(uint32_t pixel, rdcarray<PSRegisterChange> *changes = NULL);
  // runs every pixel to the end. Returns false if any failed.
  bool Run();

private:
  PSThread m_Pixels[QuadPixels];
};

class PSEmulator
{
public:
  // checks the program is a ps_2_0, ps_2_x or ps_3_0 shader that can be run and matches up its
  // flow control. Returns false, with the reason in GetError(), if it can't.
  bool Init(const D3D9BC::Program &prog);
  const rdcstr &GetError() const { return m_Error; }
  const D3D9BC::Version &GetVersion() const { return m_Version; }
  // the program given to Init, which must outlive the emulator
  const D3D9BC::Program &GetProgram() const { return *m_Prog; }
  // the texture type a sampler is declared with, Unknown if it isn't
  D3D9BC::SamplerTextureType SamplerType(uint32_t sampler) const
  {
    return sampler < NumPixelSamplers ? m_SamplerTypes[sampler] : D3D9BC::SamplerTextureType::Unknown;
  }

  // the constants with the shader's literals applied, and padded to cover them
  VSConstants ApplyLiterals(const VSConstants &consts) const
  {
    return m_Layout.ApplyLiterals(consts);
  }

private:
  friend class PSThread;

  const D3D9BC::Program *m_Prog = NULL;
  D3D9BC::Version m_Version;
  rdcstr m_Error;
  ProgramLayout m_Layout;
  D3D9BC::SamplerTextureType m_SamplerTypes[NumPixelSamplers] = {};
};

};    // namespace D3D9Emu
