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

// The pixel shader interpreter for ps_1_1 to ps_3_0 (steps 5.5 and 5.6). It runs a 2x2 quad of
// pixels in lockstep, so dsx, dsy and the derivatives of texture lookups come from the neighbouring
// pixels as they do on the GPU. Sampling goes through a callback, and so do the transcendentals,
// since the GPU's differ from the C library in the last bits (5.8 runs both on the GPU). The
// arithmetic follows what the GPU was measured doing (STATUS.md, "What the GPU does with pixel
// shaders"). Like the vertex interpreter, whose flow control and constants it shares, it doesn't
// need any Windows headers, so it's unit tested on every platform.

#include "d3d9_vs_emulate.h"

namespace D3D9Emu
{
enum
{
  MaxPSTemps = 32,
  // ps_3_0's v0-v9; ps_2_0 and ps_2_x have v0 and v1, the colours
  MaxPSInputs = 10,
  // ps_2_0 and ps_2_x's t0-t7, ps_1_x's t0-t3 (t0-t5 in ps_1_4)
  MaxPSTexCoords = 8,
  MaxPSColorOutputs = 4,
  NumPixelSamplers = 16,
  NumTextureStages = 8,
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
  // the texture coordinates. ps_1_1 to ps_1_3 read them through their texture instructions, and
  // their t# registers are what those wrote (PSRegisters::t).
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
  // ps_1_1 to ps_1_3's texture registers, written by the texture instructions. texm3x2pad and
  // texm3x3pad leave their dot product in x, which the shader can't read but the next
  // texm3x2*/texm3x3* uses.
  Vec4 t[MaxPSTexCoords];
  // oC0-oC3 and oDepth. ps_1_x's r0 is copied to oC0, clamped to [0, 1], when the shader ends.
  PSOutputs out;
  // texkill discarded the pixel. It still runs to the end, as the GPU runs it for its neighbours'
  // derivatives.
  bool killed = false;
};

// a register an instruction changed, and what it held before: r#, ps_1_1-1_3's t#
// (AddrOrTexture), p0, aL, oC# (ColorOut) or oDepth (DepthOut)
typedef VSRegisterChange PSRegisterChange;

// what ps_1_x reads from a texture stage's state
struct PSStageState
{
  // D3DTSS_BUMPENVMAT00, 01, 10 and 11, for texbem, texbeml and bem
  float bumpEnvMat[4] = {};
  // D3DTSS_BUMPENVLSCALE and BUMPENVLOFFSET, for texbeml
  float lumScale = 0.0f, lumOffset = 0.0f;
  // D3DTSS_TEXTURETRANSFORMFLAGS has D3DTTFF_PROJECTED: ps_1_1 to ps_1_3's tex divides the
  // coordinate by w, whatever the count (measured with a vertex shader). ps_1_4 ignores it.
  bool projected = false;
};

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
  // as the shader declares the sampler (dcl_2d, dcl_cube, dcl_volume). Unknown in ps_1_x, which
  // has no declarations: the texture bound decides.
  D3D9BC::SamplerTextureType type = D3D9BC::SamplerTextureType::Unknown;
  // the coordinate the shader gave, with texldp's not divided by w yet. ps_1_x's is the one its
  // instruction worked out (bumped, reflected, projected), with 0 in the components it doesn't use.
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
             PSCallbacks *callbacks, const PSStageState *stages);
  // whether inst needs a value from all four pixels (dsx's and dsy's source, a lookup's
  // coordinate), and this pixel's value of it
  bool QuadOperand(const D3D9BC::Instruction &inst, Vec4 &value) const;
  bool Step(rdcarray<PSRegisterChange> *changes);
  // runs one instruction at ip, setting next if it jumps
  bool Execute(const D3D9BC::Instruction &inst, uint32_t ip, uint32_t &next);
  bool StepFail(const rdcstr &msg);

  int32_t RelValue(const D3D9BC::RelativeAddress &rel) const;
  // clampConsts: ps_1_x's constants clamped to [-1, 1], as every instruction but texm3x3spec
  // reads them
  Vec4 ReadRaw(const D3D9BC::SrcParam &src, int32_t regOffset, bool clampConsts = true) const;
  Vec4 Read(const D3D9BC::SrcParam &src, int32_t regOffset = 0, bool clampConsts = true) const;
  Vec4 ReadFlushed(const D3D9BC::SrcParam &src, int32_t regOffset = 0) const;
  bool ReadBool(const D3D9BC::SrcParam &src) const;
  IVec4 ReadInt(const D3D9BC::SrcParam &src) const;
  uint8_t WriteMask(const D3D9BC::Instruction &inst) const;
  void Write(const D3D9BC::Instruction &inst, Vec4 val);
  void WriteNow(const D3D9BC::Instruction &inst, Vec4 val);
  void SetTexRegister(uint32_t index, uint8_t mask, const Vec4 &val);
  void SetDepth(float depth);
  bool Sample(const D3D9BC::Instruction &inst, Vec4 &result);
  bool Fail(const rdcstr &msg);

  // ps_1_x
  const PSStageState &Stage(uint32_t stage) const;
  // texture coordinate set n, as the rasterizer gave it
  Vec4 TexCoord(uint32_t n) const;
  // 1 / f through the transcendental callback, for ps_1_x's divides
  float Reciprocal(float f) const;
  Vec4 Project(Vec4 v, D3D9BC::SrcMod mod) const;
  Vec4 Reflect(const Vec4 &n, const Vec4 &e) const;
  // the coordinate a ps_1_x texture instruction looks up, false if it doesn't sample
  bool LookupCoord(const D3D9BC::Instruction &inst, Vec4 &coord) const;
  // looks up stage with coord, its derivatives from m_Quad
  bool SampleStage(uint32_t stage, const Vec4 &coord, Vec4 &result);
  bool ExecuteSM1Texture(const D3D9BC::Instruction &inst);
  // r0 to oC0 at the end
  void FinishSM1();

  const PSEmulator *m_Emu = NULL;
  PSInputs m_Inputs;
  const VSConstants *m_Consts = NULL;
  PSCallbacks *m_Callbacks = NULL;
  // NumTextureStages of them, or NULL for the defaults
  const PSStageState *m_Stages = NULL;

  // QuadOperand's value in each pixel of the quad, before this instruction ran in any of them
  Vec4 m_Quad[QuadPixels];

  // ps_1_x's co-issued pairs read their sources before either writes: while running the pair,
  // Write keeps the results here
  struct PendingWrite
  {
    const D3D9BC::Instruction *inst;
    Vec4 val;
  };
  bool m_Defer = false;
  uint32_t m_NumPending = 0;
  PendingWrite m_Pending[2];

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
  // (from emu.ApplyLiterals), callbacks and stages must outlive the quad. callbacks may be NULL
  // for a shader that doesn't sample. stages, NumTextureStages of them, are only read by ps_1_x,
  // and may be NULL for D3D9's defaults.
  void Start(const PSEmulator &emu, const PSInputs inputs[QuadPixels], const VSConstants &consts,
             PSCallbacks *callbacks, const PSStageState *stages = NULL);

  const PSThread &Pixel(uint32_t i) const { return m_Pixels[i & 3]; }
  // every pixel has finished (or failed)
  bool Finished() const;

  // runs the next instruction in each pixel that hasn't finished (ps_1_x's co-issued pairs as one
  // step). changes, if given, gets what pixel changed. Returns false if pixel had nothing to run,
  // or its instruction failed: it's then finished with its error set, and its registers as they
  // were before. The other pixels go on.
  bool Step(uint32_t pixel, rdcarray<PSRegisterChange> *changes = NULL);
  // runs every pixel to the end. Returns false if any failed.
  bool Run();

private:
  PSThread m_Pixels[QuadPixels];
};

class PSEmulator
{
public:
  // checks the program is a ps_1_1 to ps_3_0 shader that can be run and matches up its flow
  // control. Returns false, with the reason in GetError(), if it can't.
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
  // ps_1_x, and of those ps_1_1 to ps_1_3, whose t# are texture registers
  bool m_SM1 = false, m_TexRegisters = false;
  rdcstr m_Error;
  ProgramLayout m_Layout;
  D3D9BC::SamplerTextureType m_SamplerTypes[NumPixelSamplers] = {};
};

};    // namespace D3D9Emu
