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

#include "d3d9_ps_emulate.h"
#include <float.h>
#include <math.h>
#include <string.h>
#include "common/common.h"
#include "common/formatting.h"

// How the pixel interpreter works
// -------------------------------
// The flow control, constants and literals work as in the vertex interpreter (ProgramLayout). A
// PSQuad steps its four pixels together: before each step it reads, for every pixel whose next
// instruction needs them, that instruction's source in all four pixels, so a pixel writing the
// register it differentiates (dsx r0, r0) doesn't change what its neighbours see. Pixels can take
// different paths through dynamic flow control; their neighbours' values are then whatever those
// hold, which is undefined on the GPU too.
//
// The arithmetic is the GPU's, as measured natively on an RTX 4070 Ti in step 5.4:
// - registers are fp32; _pp changes nothing.
// - denormal sources act as a zero of the same sign in every arithmetic instruction, and denormal
//   results are flushed to a zero of the same sign. mov (and abs, and cmp's selected values)
//   keep them.
// - zero times anything (infinity and NaN too) is +0, in mul, mad, the dot products, lrp and nrm.
// - mad is fused. dp3 and dp4 round the first product and fuse each further one in order;
//   dp2add is fma(a.y, b.y, a.x * b.x) + c; lrp is fma(s0, s1 - s2, s2).
// - min and max return the other operand when one is NaN, and put -0 below +0.
// - cmp takes src1 when src0 >= 0, -0 included.
// The transcendentals go through PSCallbacks::Transcendental, the C library's by default with the
// GPU's special cases (log(0) is -inf, sin(-0) is +0, pow(x, 0) and pow(1, y) are 1).
// texkill marks the pixel discarded and it carries on, as a helper pixel would on the GPU.
//
// ps_1_x (step 5.6) runs on the same arithmetic, with what the GPU was measured doing in 5.4 and
// 5.6:
// - constants are clamped to [-1, 1] as they're read, before any source modifier (but not
//   texm3x3spec's eye vector), and r0 to [0, 1] when it becomes oC0 at the end. Nothing else is
//   clamped but _sat, texcoord and texbeml's luminance.
// - cnd takes src1 when src0 > 0.5.
// - a co-issued pair reads its sources before either writes; the pair is one step.
// - ps_1_1 to ps_1_3's t# are registers the texture instructions write, each sampling the stage
//   and reading the coordinate set its destination names. ps_1_4's texld samples the stage its
//   r# names, and its t# are the coordinates.
// - texcoord clamps all four components; texcrd doesn't clamp. texkill tests x, y and z of the
//   coordinate (ps_1_1-1_3) or register (ps_1_4).
// - tex divides by w when the stage has D3DTTFF_PROJECTED, whatever the count; ps_1_4 ignores it.
// - texbem offsets (u, v) by the stage's bump matrix times (r, g); texbeml then scales all four
//   channels by b * scale + offset, clamped to [0, 1].
// The derivatives of a ps_1_x lookup are those of the coordinate it works out, across the quad.

using namespace D3D9BC;

namespace D3D9Emu
{
namespace
{
// a pixel running more instructions than this is assumed to loop forever
const uint32_t MaxInstructionsPerPixel = 1U << 20;
const uint32_t MaxCallDepth = 16;

inline float Flush(float f)
{
  // NaN fails the comparison and is kept
  if(fabsf(f) < FLT_MIN && f != 0.0f)
    return copysignf(0.0f, f);
  return f;
}

inline float FAdd(float a, float b)
{
  return Flush(Flush(a) + Flush(b));
}

inline float FMul(float a, float b)
{
  a = Flush(a);
  b = Flush(b);
  if(a == 0.0f || b == 0.0f)
    return 0.0f;
  return Flush(a * b);
}

inline float FFma(float a, float b, float c)
{
  a = Flush(a);
  b = Flush(b);
  c = Flush(c);
  if(a == 0.0f || b == 0.0f)
    return 0.0f + c;
  return Flush(fmaf(a, b, c));
}

inline float FMin(float a, float b)
{
  a = Flush(a);
  b = Flush(b);
  if(a != a)
    return b;
  if(b != b)
    return a;
  if(a == b)
    return signbit(a) ? a : b;
  return a < b ? a : b;
}

inline float FMax(float a, float b)
{
  a = Flush(a);
  b = Flush(b);
  if(a != a)
    return b;
  if(b != b)
    return a;
  if(a == b)
    return signbit(a) ? b : a;
  return a > b ? a : b;
}

float Dot(const Vec4 &a, const Vec4 &b, int n)
{
  float ret = FMul(a[0], b[0]);
  for(int i = 1; i < n; i++)
    ret = FFma(a[i], b[i], ret);
  return ret;
}

Vec4 Splat(float f)
{
  return Vec4(f, f, f, f);
}

Vec4 FlushVec(Vec4 v)
{
  for(int i = 0; i < 4; i++)
    v[i] = Flush(v[i]);
  return v;
}

Vec4 Diff(const Vec4 &a, const Vec4 &b)
{
  Vec4 ret;
  for(int i = 0; i < 4; i++)
    ret[i] = FAdd(a[i], -b[i]);
  return ret;
}

bool IsTranscendental(Opcode op)
{
  switch(op)
  {
    case Opcode::Rcp:
    case Opcode::Rsq:
    case Opcode::Exp:
    case Opcode::Log:
    case Opcode::Pow:
    case Opcode::SinCos: return true;
    default: return false;
  }
}
};    // namespace

Vec4 CPUTranscendental(Opcode op, float a, float b)
{
  switch(op)
  {
    case Opcode::Rcp: return Splat(a == 0.0f ? copysignf(INFINITY, a) : 1.0f / a);
    case Opcode::Rsq: a = fabsf(a); return Splat(a == 0.0f ? INFINITY : 1.0f / sqrtf(a));
    case Opcode::Exp: return Splat(exp2f(a));
    case Opcode::Log: a = fabsf(a); return Splat(a == 0.0f ? -INFINITY : log2f(a));
    case Opcode::Pow:
    {
      a = fabsf(a);
      if(b == 0.0f || a == 1.0f)
        return Splat(1.0f);
      return Splat(powf(a, b));
    }
    case Opcode::SinCos:
      // the GPU's sin(-0) is +0
      return Vec4(cosf(a), a == 0.0f ? 0.0f : sinf(a), 0.0f, 0.0f);
    default: return Vec4();
  }
}

namespace
{
// ps_1_x reads its constants clamped to [-1, 1], before any source modifier
inline float ClampSM1Constant(float f)
{
  return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

// texbem's and bem's offset of (u, v) by the bump matrix times (s.x, s.y), in the form dp2add was
// measured to take
Vec4 Bump(const Vec4 &uv, const Vec4 &s, const float *m)
{
  return Vec4(FAdd(uv[0], FFma(m[2], s[1], FMul(m[0], s[0]))),
              FAdd(uv[1], FFma(m[3], s[1], FMul(m[1], s[0]))), 0.0f, 0.0f);
}
};    // namespace

bool PSEmulator::Init(const Program &prog)
{
  m_Prog = &prog;
  m_Version = prog.version;
  m_Error.clear();
  m_SM1 = m_TexRegisters = false;
  for(SamplerTextureType &t : m_SamplerTypes)
    t = SamplerTextureType::Unknown;

  if(!prog.validVersion || !prog.version.IsPS())
  {
    m_Error = "Not a pixel shader";
    return false;
  }

  const Version &v = prog.version;
  const bool sm1 = v.major == 1 && v.minor >= 1 && v.minor <= 4;
  if(!sm1 && (v.major < 2 || v.major > 3))
  {
    m_Error = StringFormat::Fmt("Unsupported shader model %s", VersionString(v).c_str());
    return false;
  }
  m_SM1 = sm1;
  m_TexRegisters = sm1 && v.minor < 4;

  if(!prog.error.empty())
  {
    m_Error = "The shader couldn't be fully parsed: " + prog.error;
    return false;
  }

  for(const Instruction &inst : prog.instructions)
    if(inst.op == Opcode::Dcl && inst.dst.type == RegType::Sampler &&
       inst.dst.index < NumPixelSamplers)
      m_SamplerTypes[inst.dst.index] = inst.textureType;

  return m_Layout.Build(prog, m_Error);
}

void PSThread::Start(const PSEmulator &emu, const PSInputs &inputs, const VSConstants &consts,
                     PSCallbacks *callbacks, const PSStageState *stages)
{
  PSRegisters &regs = *this;
  regs = PSRegisters();

  m_Emu = &emu;
  m_Inputs = inputs;
  m_Consts = &consts;
  m_Callbacks = callbacks;
  m_Stages = stages;
  m_IP = 0;
  m_Executed = 0;
  m_Error.clear();
  m_Changes = NULL;
  m_Defer = false;
  m_NumPending = 0;
  m_Finished = false;

  if(emu.m_Prog == NULL || !emu.m_Error.empty())
  {
    m_Error = emu.m_Error.empty() ? "No shader" : emu.m_Error;
    m_Finished = true;
  }
  else if(emu.m_Prog->instructions.empty())
  {
    m_Finished = true;
  }
}

int32_t PSThread::RelValue(const RelativeAddress &rel) const
{
  // pixel shaders only index with aL
  if(!rel.present || rel.type != RegType::Loop)
    return 0;
  return aL();
}

Vec4 PSThread::ReadRaw(const SrcParam &src, int32_t regOffset, bool clampConsts) const
{
  int64_t idx = int64_t(src.index) + regOffset + RelValue(src.rel);
  const VSConstants *c = m_Consts;
  Vec4 ret;

  switch(src.type)
  {
    case RegType::Temp:
      if(idx >= 0 && idx < MaxPSTemps)
        ret = r[idx];
      break;
    case RegType::Input:
      if(idx >= 0 && idx < MaxPSInputs)
        ret = m_Inputs.v[idx];
      break;
    case RegType::AddrOrTexture:
      // ps_1_1 to ps_1_3's t# are registers, the others' are the coordinates
      if(idx >= 0 && idx < MaxPSTexCoords)
        ret = m_Emu->m_TexRegisters ? t[idx] : m_Inputs.t[idx];
      break;
    case RegType::Const:
    case RegType::Const2:
    case RegType::Const3:
    case RegType::Const4:
      idx += ConstantRegisterOffset(src.type);
      if(idx >= 0 && idx < (int64_t)c->floats.size())
        ret = c->floats[(size_t)idx];
      if(m_Emu->m_SM1 && clampConsts)
        for(int i = 0; i < 4; i++)
          ret[i] = ClampSM1Constant(ret[i]);
      break;
    case RegType::ConstInt:
      if(idx >= 0 && idx < (int64_t)c->ints.size())
        for(int i = 0; i < 4; i++)
          ret[i] = float(c->ints[(size_t)idx].v[i]);
      break;
    case RegType::ConstBool:
    {
      float b = (idx >= 0 && idx < (int64_t)c->bools.size() && c->bools[(size_t)idx]) ? 1.0f : 0.0f;
      ret = Splat(b);
      break;
    }
    case RegType::Loop: ret = Splat(float(aL())); break;
    case RegType::Predicate:
      for(int i = 0; i < 4; i++)
        ret[i] = p[i] ? 1.0f : 0.0f;
      break;
    case RegType::MiscType:
      if(src.index == 0)
        ret = m_Inputs.pos;
      else if(src.index == 1)
        ret = Splat(m_Inputs.face);
      break;
    default: break;
  }

  return ret;
}

Vec4 PSThread::Read(const SrcParam &src, int32_t regOffset, bool clampConsts) const
{
  Vec4 raw = ReadRaw(src, regOffset, clampConsts);
  Vec4 ret;
  for(int i = 0; i < 4; i++)
    ret[i] = ApplyMod(src.mod, raw[src.swizzle[i] & 3]);
  return ret;
}

Vec4 PSThread::ReadFlushed(const SrcParam &src, int32_t regOffset) const
{
  return FlushVec(Read(src, regOffset));
}

bool PSThread::ReadBool(const SrcParam &src) const
{
  const VSConstants *c = m_Consts;
  bool ret = false;
  if(src.type == RegType::ConstBool)
  {
    int64_t idx = int64_t(src.index) + RelValue(src.rel);
    ret = idx >= 0 && idx < (int64_t)c->bools.size() && c->bools[(size_t)idx] != 0;
  }
  else if(src.type == RegType::Predicate)
  {
    ret = p[src.swizzle[0] & 3];
  }
  else
  {
    ret = ReadRaw(src, 0)[src.swizzle[0] & 3] != 0.0f;
  }
  if(src.mod == SrcMod::Not)
    ret = !ret;
  return ret;
}

IVec4 PSThread::ReadInt(const SrcParam &src) const
{
  IVec4 ret;
  if(src.type == RegType::ConstInt && src.index < m_Consts->ints.size())
    ret = m_Consts->ints[src.index];
  return ret;
}

uint8_t PSThread::WriteMask(const Instruction &inst) const
{
  uint8_t mask = inst.dst.writeMask;
  if(inst.predicated)
  {
    for(int i = 0; i < 4; i++)
    {
      bool pc = p[inst.predicate.swizzle[i] & 3];
      if(inst.predicate.mod == SrcMod::Not)
        pc = !pc;
      if(!pc)
        mask &= ~(1 << i);
    }
  }
  return mask;
}

void PSThread::Write(const Instruction &inst, Vec4 val)
{
  // in a co-issued pair, nothing is written until both have read their sources
  if(m_Defer)
  {
    if(m_NumPending < 2)
      m_Pending[m_NumPending++] = {&inst, val};
    return;
  }
  WriteNow(inst, val);
}

void PSThread::WriteNow(const Instruction &inst, Vec4 val)
{
  const DstParam &d = inst.dst;
  const uint8_t mask = WriteMask(inst);

  if(d.shift != 0)
  {
    const float scale = ldexpf(1.0f, d.shift);
    for(int i = 0; i < 4; i++)
      val[i] *= scale;
  }

  if(d.mods & DstMod_Saturate)
    for(int i = 0; i < 4; i++)
      val[i] = Saturate(val[i]);

  int64_t idx = int64_t(d.index) + RelValue(d.rel);

  Vec4 *dst = NULL;
  uint32_t written = 0;
  switch(d.type)
  {
    case RegType::Temp:
      if(idx >= 0 && idx < MaxPSTemps)
        dst = &r[idx];
      break;
    case RegType::AddrOrTexture:
      if(m_Emu->m_TexRegisters && idx >= 0 && idx < MaxPSTexCoords)
        dst = &t[idx];
      break;
    case RegType::ColorOut:
      if(idx >= 0 && idx < MaxPSColorOutputs)
      {
        dst = &out.color[idx];
        written = 1U << idx;
      }
      break;
    case RegType::DepthOut:
      dst = &out.depth;
      written = PSWroteDepth;
      idx = 0;
      break;
    case RegType::Predicate:
      if(m_Changes && mask)
      {
        PSRegisterChange change;
        change.type = RegType::Predicate;
        for(int i = 0; i < 4; i++)
          change.i.v[i] = p[i] ? 1 : 0;
        m_Changes->push_back(change);
      }
      for(int i = 0; i < 4; i++)
        if(mask & (1 << i))
          p[i] = val[i] != 0.0f;
      return;
    default: break;
  }

  if(dst && mask)
  {
    if(m_Changes)
    {
      PSRegisterChange change;
      change.type = d.type;
      change.index = uint32_t(idx);
      change.f = *dst;
      m_Changes->push_back(change);
    }
    for(int i = 0; i < 4; i++)
      if(mask & (1 << i))
        (*dst)[i] = val[i];
    out.written |= written;
  }
}

void PSThread::SetTexRegister(uint32_t index, uint8_t mask, const Vec4 &val)
{
  if(index >= MaxPSTexCoords)
    return;
  if(m_Changes)
  {
    PSRegisterChange change;
    change.type = RegType::AddrOrTexture;
    change.index = index;
    change.f = t[index];
    m_Changes->push_back(change);
  }
  for(int i = 0; i < 4; i++)
    if(mask & (1 << i))
      t[index][i] = val[i];
}

void PSThread::SetDepth(float depth)
{
  if(m_Changes)
  {
    PSRegisterChange change;
    change.type = RegType::DepthOut;
    change.f = out.depth;
    m_Changes->push_back(change);
  }
  out.depth[0] = depth;
  out.written |= PSWroteDepth;
}

float PSThread::Reciprocal(float f) const
{
  f = Flush(f);
  return Flush(m_Callbacks ? m_Callbacks->Transcendental(Opcode::Rcp, f, 0.0f)[0]
                           : CPUTranscendental(Opcode::Rcp, f, 0.0f)[0]);
}

Vec4 PSThread::Project(Vec4 v, SrcMod mod) const
{
  // ps_1_4's _dz and _dw: x and y divided by z or w of the swizzled source. The GPU has no
  // divide: texcrd's results match times the reciprocal, not a divide.
  if(mod == SrcMod::DZ || mod == SrcMod::DW)
  {
    const float rcp = Reciprocal(v[mod == SrcMod::DZ ? 2 : 3]);
    v[0] = FMul(v[0], rcp);
    v[1] = FMul(v[1], rcp);
  }
  return v;
}

Vec4 PSThread::Reflect(const Vec4 &n, const Vec4 &e) const
{
  // texm3x3spec and texm3x3vspec: 2 * n * (n.e) / (n.n) - e
  const float f = FMul(FMul(2.0f, Dot(n, e, 3)), Reciprocal(Dot(n, n, 3)));
  Vec4 ret;
  for(int i = 0; i < 3; i++)
    ret[i] = FAdd(FMul(f, n[i]), -e[i]);
  return ret;
}

bool PSThread::QuadOperand(const Instruction &inst, Vec4 &value) const
{
  if(m_Emu->m_SM1)
    return LookupCoord(inst, value);

  switch(inst.op)
  {
    case Opcode::Dsx:
    case Opcode::Dsy:
    case Opcode::Tex:
      if(inst.srcs.empty())
        return false;
      value = Read(inst.srcs[0]);
      return true;
    default: return false;
  }
}

bool PSThread::Sample(const Instruction &inst, Vec4 &result)
{
  const rdcarray<SrcParam> &srcs = inst.srcs;
  if(srcs.size() < 2 || (inst.op == Opcode::TexLdd && srcs.size() < 4))
    return Fail("Missing operands");
  if(srcs[1].type != RegType::Sampler)
    return Fail(StringFormat::Fmt("%s without a sampler", OpcodeName(m_Emu->m_Version, inst).c_str()));
  if(!m_Callbacks)
    return Fail("Sampling isn't available");

  PSSample s;
  s.sampler = srcs[1].index;
  s.type = m_Emu->SamplerType(s.sampler);
  s.coord = Read(srcs[0]);

  switch(inst.op)
  {
    case Opcode::TexLdd:
      s.kind = SampleKind::Gradient;
      s.ddx = Read(srcs[2]);
      s.ddy = Read(srcs[3]);
      break;
    case Opcode::TexLdl:
      s.kind = SampleKind::Level;
      s.lod = s.coord[3];
      break;
    default:
    {
      // texld, texldp and texldb, with derivatives across the quad of the coordinate looked up
      Vec4 c[QuadPixels];
      if(inst.control & 0x1)
      {
        s.kind = SampleKind::Projected;
        for(uint32_t i = 0; i < QuadPixels; i++)
          for(int j = 0; j < 3; j++)
            c[i][j] = m_Quad[i][j] / m_Quad[i][3];
      }
      else
      {
        s.kind = (inst.control & 0x2) ? SampleKind::Bias : SampleKind::Implicit;
        if(s.kind == SampleKind::Bias)
          s.lod = s.coord[3];
        for(uint32_t i = 0; i < QuadPixels; i++)
          c[i] = m_Quad[i];
      }
      s.ddx = Diff(c[QuadTopRight], c[QuadTopLeft]);
      s.ddy = Diff(c[QuadBottomLeft], c[QuadTopLeft]);
      // w is the projection or the bias, not part of the coordinate
      s.ddx[3] = s.ddy[3] = 0.0f;
      for(uint32_t i = 0; i < QuadPixels; i++)
        s.quad[i] = m_Quad[i];
      s.pixel = m_QuadIndex;
      break;
    }
  }

  Vec4 res = m_Callbacks->Sample(s);
  // the sampler's swizzle applies to the result
  for(int i = 0; i < 4; i++)
    result[i] = res[srcs[1].swizzle[i] & 3];
  return true;
}

bool PSThread::Fail(const rdcstr &msg)
{
  const rdcarray<Instruction> &insts = m_Emu->m_Prog->instructions;
  m_Error = StringFormat::Fmt(
      "%s at instruction %u (%s)", msg.c_str(), m_IP,
      m_IP < insts.size() ? OpcodeName(m_Emu->m_Version, insts[m_IP]).c_str() : "");
  m_Finished = true;
  return false;
}

bool PSThread::StepFail(const rdcstr &msg)
{
  // a failed instruction changes nothing, so nothing it recorded is kept
  m_Changes = NULL;
  m_NumPending = 0;
  return Fail(msg);
}

const PSStageState &PSThread::Stage(uint32_t stage) const
{
  static const PSStageState defaults;
  return m_Stages && stage < NumTextureStages ? m_Stages[stage] : defaults;
}

Vec4 PSThread::TexCoord(uint32_t n) const
{
  return n < MaxPSTexCoords ? m_Inputs.t[n] : Vec4();
}

bool PSThread::LookupCoord(const Instruction &inst, Vec4 &coord) const
{
  if(!inst.hasDst)
    return false;

  const rdcarray<SrcParam> &srcs = inst.srcs;
  // the texture register or coordinate set the instruction writes, and the stage it samples
  const uint32_t d = inst.dst.index;

  switch(inst.op)
  {
    case Opcode::Tex:
      if(m_Emu->m_TexRegisters)
      {
        // tex t#: its own coordinate set, projected if the stage says
        coord = TexCoord(d);
        if(Stage(d).projected)
        {
          const float rcp = Reciprocal(coord[3]);
          for(int i = 0; i < 3; i++)
            coord[i] = FMul(coord[i], rcp);
        }
        return true;
      }
      // ps_1_4's texld r#, src: the stage is the destination's number
      if(srcs.empty())
        return false;
      coord = Project(Read(srcs[0]), srcs[0].mod);
      return true;
    case Opcode::TexBem:
    case Opcode::TexBemL:
      if(srcs.empty())
        return false;
      coord = Bump(TexCoord(d), Read(srcs[0]), Stage(d).bumpEnvMat);
      return true;
    case Opcode::TexReg2AR:
    case Opcode::TexReg2GB:
    case Opcode::TexReg2RGB:
    {
      if(srcs.empty())
        return false;
      const Vec4 s = Read(srcs[0]);
      if(inst.op == Opcode::TexReg2AR)
        coord = Vec4(s[3], s[0], 0.0f, 0.0f);
      else if(inst.op == Opcode::TexReg2GB)
        coord = Vec4(s[1], s[2], 0.0f, 0.0f);
      else
        coord = Vec4(s[0], s[1], s[2], 0.0f);
      return true;
    }
    case Opcode::TexDP3Tex:
      if(srcs.empty())
        return false;
      coord = Vec4(Dot(TexCoord(d), Read(srcs[0]), 3), 0.0f, 0.0f, 0.0f);
      return true;
    case Opcode::TexM3x2Tex:
      if(srcs.empty() || d < 1)
        return false;
      // u is the pad's dot product, kept in its register's x
      coord = Vec4(t[d - 1][0], Dot(TexCoord(d), Read(srcs[0]), 3), 0.0f, 0.0f);
      return true;
    case Opcode::TexM3x3Tex:
    case Opcode::TexM3x3Spec:
    case Opcode::TexM3x3VSpec:
    {
      if(srcs.empty() || d < 2 || (inst.op == Opcode::TexM3x3Spec && srcs.size() < 2))
        return false;
      const Vec4 n(t[d - 2][0], t[d - 1][0], Dot(TexCoord(d), Read(srcs[0]), 3), 0.0f);
      if(inst.op == Opcode::TexM3x3Tex)
        coord = n;
      else if(inst.op == Opcode::TexM3x3Spec)
        // the eye vector constant isn't clamped to [-1, 1] (measured)
        coord = Reflect(n, Read(srcs[1], 0, false));
      else
        coord = Reflect(n, Vec4(TexCoord(d - 2)[3], TexCoord(d - 1)[3], TexCoord(d)[3], 0.0f));
      return true;
    }
    default: return false;
  }
}

bool PSThread::SampleStage(uint32_t stage, const Vec4 &coord, Vec4 &result)
{
  if(!m_Callbacks)
    return StepFail("Sampling isn't available");

  PSSample s;
  s.kind = SampleKind::Implicit;
  s.sampler = stage;
  s.coord = coord;
  s.ddx = Diff(m_Quad[QuadTopRight], m_Quad[QuadTopLeft]);
  s.ddy = Diff(m_Quad[QuadBottomLeft], m_Quad[QuadTopLeft]);
  s.ddx[3] = s.ddy[3] = 0.0f;
  for(uint32_t i = 0; i < QuadPixels; i++)
    s.quad[i] = m_Quad[i];
  s.pixel = m_QuadIndex;
  result = m_Callbacks->Sample(s);
  return true;
}

bool PSThread::ExecuteSM1Texture(const Instruction &inst)
{
  if(!inst.hasDst)
    return StepFail("Missing destination");

  const rdcarray<SrcParam> &srcs = inst.srcs;
  const uint32_t d = inst.dst.index;

  switch(inst.op)
  {
    case Opcode::TexCoord:
    {
      // ps_1_4's texcrd copies the coordinate as it is, with _dz/_dw
      if(!m_Emu->m_TexRegisters)
      {
        if(srcs.empty())
          return StepFail("Missing operands");
        Write(inst, Project(Read(srcs[0]), srcs[0].mod));
        return true;
      }
      // texcoord clamps all four components to [0, 1]
      Vec4 c = TexCoord(d);
      for(int i = 0; i < 4; i++)
        c[i] = Saturate(c[i]);
      Write(inst, c);
      return true;
    }
    case Opcode::TexM3x2Pad:
    case Opcode::TexM3x3Pad:
      if(srcs.empty())
        return StepFail("Missing operands");
      SetTexRegister(d, 0x1, Vec4(Dot(TexCoord(d), Read(srcs[0]), 3), 0.0f, 0.0f, 0.0f));
      return true;
    case Opcode::TexDP3:
      if(srcs.empty())
        return StepFail("Missing operands");
      Write(inst, Splat(Dot(TexCoord(d), Read(srcs[0]), 3)));
      return true;
    case Opcode::TexM3x3:
      if(srcs.empty() || d < 2)
        return StepFail("Missing operands");
      Write(inst, Vec4(t[d - 2][0], t[d - 1][0], Dot(TexCoord(d), Read(srcs[0]), 3), 1.0f));
      return true;
    case Opcode::TexM3x2Depth:
    {
      if(srcs.empty() || d < 1)
        return StepFail("Missing operands");
      const float z = t[d - 1][0], w = Dot(TexCoord(d), Read(srcs[0]), 3);
      SetDepth(w == 0.0f ? 1.0f : FMul(z, Reciprocal(w)));
      return true;
    }
    case Opcode::TexDepth:
    {
      // r5.x / r5.y
      const Vec4 v = d < MaxPSTemps ? r[d] : Vec4();
      SetDepth(v[1] == 0.0f ? 1.0f : FMul(v[0], Reciprocal(v[1])));
      return true;
    }
    default:
    {
      Vec4 coord, res;
      if(!LookupCoord(inst, coord))
        return StepFail("Missing operands");
      if(!SampleStage(d, coord, res))
        return false;
      if(inst.op == Opcode::TexBemL)
      {
        // the luminance is clamped to [0, 1], and scales all four channels
        const PSStageState &st = Stage(d);
        const float l = Saturate(FFma(Read(srcs[0])[2], st.lumScale, st.lumOffset));
        for(int i = 0; i < 4; i++)
          res[i] = FMul(res[i], l);
      }
      Write(inst, res);
      return true;
    }
  }
}

void PSThread::FinishSM1()
{
  // r0 is the colour, clamped to [0, 1] even into a float target
  Vec4 c = r[0];
  for(int i = 0; i < 4; i++)
    c[i] = Saturate(c[i]);
  if(m_Changes)
  {
    PSRegisterChange change;
    change.type = RegType::ColorOut;
    change.index = 0;
    change.f = out.color[0];
    m_Changes->push_back(change);
  }
  out.color[0] = c;
  out.written |= 1U;
}

bool PSThread::Step(rdcarray<PSRegisterChange> *changes)
{
  if(m_Finished)
    return false;

  const PSEmulator &emu = *m_Emu;
  const rdcarray<Instruction> &insts = emu.m_Prog->instructions;
  const uint32_t ip = m_IP;

  // ps_1_x's next instruction may be co-issued with this one, and they run as one step
  const bool pair = emu.m_SM1 && ip + 1 < insts.size() && insts[ip + 1].coissue;
  const uint32_t count = pair ? 2 : 1;

  if(m_Executed + count > MaxInstructionsPerPixel)
    return Fail("Too many instructions executed, the shader may loop forever");

  // a failed instruction changes nothing, so its changes are only recorded once it's run
  rdcarray<PSRegisterChange> recorded;
  m_Changes = changes ? &recorded : NULL;

  // aL changes when the innermost loop does, or its counter
  const int32_t loopBefore = InnermostLoop(), aLBefore = aL();

  uint32_t next = ip + count;
  if(pair)
  {
    // both read the registers as they were before either writes, as measured
    m_Defer = true;
    m_NumPending = 0;
    bool ok = Execute(insts[ip], ip, next) && Execute(insts[ip + 1], ip + 1, next);
    m_Defer = false;
    if(!ok)
      return false;
    const size_t first = recorded.size();
    for(uint32_t i = 0; i < m_NumPending; i++)
      WriteNow(*m_Pending[i].inst, m_Pending[i].val);
    m_NumPending = 0;
    // both halves may write one register (r0.rgb and r0.a): it's recorded once, as it was before
    // the pair
    if(recorded.size() == first + 2 && recorded[first].type == recorded[first + 1].type &&
       recorded[first].index == recorded[first + 1].index)
      recorded.pop_back();
  }
  else if(!Execute(insts[ip], ip, next))
  {
    return false;
  }

  m_Executed += count;
  m_IP = next;
  if(m_IP >= insts.size())
  {
    if(emu.m_SM1)
      FinishSM1();
    m_Finished = true;
  }

  if(changes)
  {
    if(InnermostLoop() != loopBefore || aL() != aLBefore)
    {
      PSRegisterChange change;
      change.type = RegType::Loop;
      change.i.v[0] = aLBefore;
      recorded.push_back(change);
    }
    changes->append(recorded);
  }
  m_Changes = NULL;
  return true;
}

bool PSThread::Execute(const Instruction &inst, uint32_t ip, uint32_t &next)
{
  const PSEmulator &emu = *m_Emu;
  const Version &version = emu.m_Version;
  const rdcarray<Instruction> &insts = emu.m_Prog->instructions;
  const rdcarray<SrcParam> &srcs = inst.srcs;

  // registers aren't written until the instruction has all it needs, so failing leaves them as
  // they were
#define NEED_SRCS(n)                     \
  if(srcs.size() < n)                    \
    return StepFail("Missing operands"); \
  if(!inst.hasDst)                       \
    return StepFail("Missing destination");

  switch(inst.op)
  {
    case Opcode::Nop:
    case Opcode::Dcl:
    case Opcode::Def:
    case Opcode::DefI:
    case Opcode::DefB:
    case Opcode::EndIf:
    case Opcode::Comment:
    // ps_1_4's phase marker. The alpha of r# written before it is lost on some hardware, but the
    // validator doesn't let a shader read it.
    case Opcode::Phase: break;

    case Opcode::End: next = (uint32_t)insts.size(); break;

    // the main program falls through into the subroutines, which ends it
    case Opcode::Label:
      if(calls.empty())
        next = (uint32_t)insts.size();
      break;

    case Opcode::Ret:
      if(calls.empty())
      {
        next = (uint32_t)insts.size();
      }
      else
      {
        VSCallFrame f = calls.back();
        calls.pop_back();
        loops.resize(f.loopDepth);
        next = f.ret;
      }
      break;

    case Opcode::Call:
    case Opcode::CallNZ:
    {
      if(srcs.empty() || (inst.op == Opcode::CallNZ && srcs.size() < 2))
        return StepFail("Missing operands");
      if(inst.op == Opcode::Call || ReadBool(srcs[1]))
      {
        if(calls.size() >= MaxCallDepth)
          return StepFail("Calls nested too deeply");
        VSCallFrame f;
        f.call = ip;
        f.label = srcs[0].index;
        f.ret = ip + 1;
        f.loopDepth = loops.size();
        calls.push_back(f);
        next = emu.m_Layout.labels[srcs[0].index] + 1;
      }
      break;
    }

    case Opcode::If:
      if(srcs.empty())
        return StepFail("Missing operands");
      if(!ReadBool(srcs[0]))
        next = emu.m_Layout.match[ip] + 1;
      break;

    case Opcode::IfC:
      if(srcs.size() < 2)
        return StepFail("Missing operands");
      if(!Compare(inst.GetComparison(), ReadFlushed(srcs[0])[0], ReadFlushed(srcs[1])[0]))
        next = emu.m_Layout.match[ip] + 1;
      break;

    // reached at the end of the if block, skip the else block
    case Opcode::Else: next = emu.m_Layout.match[ip] + 1; break;

    case Opcode::Loop:
    case Opcode::Rep:
    {
      const bool isLoop = inst.op == Opcode::Loop;
      if(srcs.size() < (isLoop ? 2U : 1U))
        return StepFail("Missing operands");
      IVec4 i = ReadInt(srcs[isLoop ? 1 : 0]);
      int32_t count = RDCCLAMP(i.v[0], 0, 255);
      if(count == 0)
      {
        next = emu.m_Layout.match[ip] + 1;
      }
      else
      {
        VSLoopFrame f;
        f.start = ip;
        f.remaining = count;
        f.isLoop = isLoop;
        f.aL = isLoop ? i.v[1] : 0;
        f.step = isLoop ? i.v[2] : 0;
        loops.push_back(f);
      }
      break;
    }

    case Opcode::EndLoop:
    case Opcode::EndRep:
    {
      if(loops.empty())
        return StepFail("End of a loop that isn't running");
      VSLoopFrame &f = loops.back();
      if(--f.remaining > 0)
      {
        f.aL += f.step;
        next = f.start + 1;
      }
      else
      {
        loops.pop_back();
      }
      break;
    }

    case Opcode::Break:
    case Opcode::BreakC:
    case Opcode::BreakP:
    {
      bool take = true;
      if(inst.op == Opcode::BreakC)
      {
        if(srcs.size() < 2)
          return StepFail("Missing operands");
        take = Compare(inst.GetComparison(), ReadFlushed(srcs[0])[0], ReadFlushed(srcs[1])[0]);
      }
      else if(inst.op == Opcode::BreakP)
      {
        if(srcs.empty())
          return StepFail("Missing operands");
        take = ReadBool(srcs[0]);
      }
      if(take)
      {
        if(loops.empty())
          return StepFail("break outside a running loop");
        loops.pop_back();
        next = emu.m_Layout.match[ip] + 1;
      }
      break;
    }

    // moves keep denormals
    case Opcode::Mov:
    {
      NEED_SRCS(1);
      Write(inst, Read(srcs[0]));
      break;
    }

    case Opcode::Abs:
    {
      NEED_SRCS(1);
      Vec4 a = Read(srcs[0]);
      for(int i = 0; i < 4; i++)
        a[i] = fabsf(a[i]);
      Write(inst, a);
      break;
    }

    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Min:
    case Opcode::Max:
    {
      NEED_SRCS(2);
      Vec4 a = Read(srcs[0]), b = Read(srcs[1]), res;
      for(int i = 0; i < 4; i++)
      {
        switch(inst.op)
        {
          case Opcode::Add: res[i] = FAdd(a[i], b[i]); break;
          case Opcode::Sub: res[i] = FAdd(a[i], -b[i]); break;
          case Opcode::Mul: res[i] = FMul(a[i], b[i]); break;
          case Opcode::Min: res[i] = FMin(a[i], b[i]); break;
          case Opcode::Max: res[i] = FMax(a[i], b[i]); break;
          default: break;
        }
      }
      Write(inst, res);
      break;
    }

    case Opcode::Mad:
    case Opcode::Lrp:
    case Opcode::Cmp:
    case Opcode::Cnd:
    {
      NEED_SRCS(3);
      Vec4 a = Read(srcs[0]), b = Read(srcs[1]), c = Read(srcs[2]), res;
      for(int i = 0; i < 4; i++)
      {
        switch(inst.op)
        {
          case Opcode::Mad: res[i] = FFma(a[i], b[i], c[i]); break;
          case Opcode::Lrp: res[i] = FFma(a[i], FAdd(b[i], -c[i]), c[i]); break;
          // -0 and negative denormals count as >= 0, NaN doesn't
          case Opcode::Cmp: res[i] = Flush(a[i]) >= 0.0f ? b[i] : c[i]; break;
          // ps_1_x: strictly above 0.5, at fp32
          case Opcode::Cnd: res[i] = Flush(a[i]) > 0.5f ? b[i] : c[i]; break;
          default: break;
        }
      }
      Write(inst, res);
      break;
    }

    case Opcode::Dp3:
    case Opcode::Dp4:
    {
      NEED_SRCS(2);
      Write(inst, Splat(Dot(Read(srcs[0]), Read(srcs[1]), inst.op == Opcode::Dp3 ? 3 : 4)));
      break;
    }

    case Opcode::Dp2Add:
    {
      NEED_SRCS(3);
      Write(inst, Splat(FAdd(Dot(Read(srcs[0]), Read(srcs[1]), 2), Read(srcs[2])[0])));
      break;
    }

    case Opcode::Frc:
    {
      NEED_SRCS(1);
      Vec4 a = ReadFlushed(srcs[0]), res;
      for(int i = 0; i < 4; i++)
        res[i] = Flush(a[i] - floorf(a[i]));
      Write(inst, res);
      break;
    }

    case Opcode::Rcp:
    case Opcode::Rsq:
    case Opcode::Exp:
    case Opcode::Log:
    case Opcode::Pow:
    case Opcode::SinCos:
    {
      NEED_SRCS(1);
      if(inst.op == Opcode::Pow && srcs.size() < 2)
        return StepFail("Missing operands");
      // scalar sources have a replicate swizzle; ps_2_0's sincos has two more, which only help
      // hardware approximate it
      float a = Flush(Read(srcs[0])[3]);
      float b = inst.op == Opcode::Pow ? Flush(Read(srcs[1])[3]) : 0.0f;
      Vec4 res;
      if(m_Callbacks)
        res = m_Callbacks->Transcendental(inst.op, a, b);
      else
        res = CPUTranscendental(inst.op, a, b);
      res = FlushVec(res);
      if(inst.op != Opcode::SinCos)
        res = Splat(res[0]);
      Write(inst, res);
      break;
    }

    case Opcode::Nrm:
    {
      NEED_SRCS(1);
      Vec4 a = Read(srcs[0]);
      float d = Dot(a, a, 3);
      float f = m_Callbacks ? m_Callbacks->Transcendental(Opcode::Rsq, d, 0.0f)[0]
                            : CPUTranscendental(Opcode::Rsq, d, 0.0f)[0];
      Vec4 res;
      // a zero vector gives 0, as zero times infinity does
      for(int i = 0; i < 4; i++)
        res[i] = FMul(a[i], Flush(f));
      Write(inst, res);
      break;
    }

    case Opcode::Crs:
    {
      NEED_SRCS(2);
      Vec4 a = Read(srcs[0]), b = Read(srcs[1]);
      Write(inst, Vec4(FAdd(FMul(a[1], b[2]), -FMul(a[2], b[1])),
                       FAdd(FMul(a[2], b[0]), -FMul(a[0], b[2])),
                       FAdd(FMul(a[0], b[1]), -FMul(a[1], b[0])), 0.0f));
      break;
    }

    case Opcode::M4x4:
    case Opcode::M4x3:
    case Opcode::M3x4:
    case Opcode::M3x3:
    case Opcode::M3x2:
    {
      NEED_SRCS(2);
      int rows = 4, size = 4;
      switch(inst.op)
      {
        case Opcode::M4x3: rows = 3; break;
        case Opcode::M3x4: size = 3; break;
        case Opcode::M3x3:
          rows = 3;
          size = 3;
          break;
        case Opcode::M3x2:
          rows = 2;
          size = 3;
          break;
        default: break;
      }
      Vec4 a = Read(srcs[0]), res;
      for(int i = 0; i < rows; i++)
        res[i] = Dot(a, Read(srcs[1], i), size);
      Write(inst, res);
      break;
    }

    case Opcode::SetP:
    {
      NEED_SRCS(2);
      Vec4 a = ReadFlushed(srcs[0]), b = ReadFlushed(srcs[1]), res;
      for(int i = 0; i < 4; i++)
        res[i] = Compare(inst.GetComparison(), a[i], b[i]) ? 1.0f : 0.0f;
      Write(inst, res);
      break;
    }

    case Opcode::Dsx:
    case Opcode::Dsy:
    {
      NEED_SRCS(1);
      // per quad: the top row's difference, and the left column's
      Write(inst, inst.op == Opcode::Dsx ? Diff(m_Quad[QuadTopRight], m_Quad[QuadTopLeft])
                                         : Diff(m_Quad[QuadBottomLeft], m_Quad[QuadTopLeft]));
      break;
    }

    case Opcode::TexKill:
    {
      if(!inst.hasDst)
        return StepFail("Missing operands");
      // the operand is a register with a write mask: any of its components below zero discards
      SrcParam src;
      src.type = inst.dst.type;
      src.index = inst.dst.index;
      src.rel = inst.dst.rel;
      uint8_t mask = inst.dst.writeMask;
      Vec4 v;
      if(emu.m_SM1)
      {
        // ps_1_x tests x, y and z: a negative w alone was measured not to discard. ps_1_1 to
        // ps_1_3's t# is the texture coordinate, not the register.
        mask = 0x7;
        if(emu.m_TexRegisters && src.type == RegType::AddrOrTexture)
          v = FlushVec(TexCoord(src.index));
        else
          v = ReadFlushed(src);
      }
      else
      {
        v = ReadFlushed(src);
      }
      bool kill = false;
      for(int i = 0; i < 4; i++)
        if((mask & (1 << i)) && v[i] < 0.0f)
          kill = true;
      if(kill)
        killed = true;
      break;
    }

    case Opcode::Bem:
    {
      NEED_SRCS(2);
      // ps_1_4: dst.xy = src0.xy offset by the destination's stage's bump matrix times src1.xy
      Write(inst, Bump(Read(srcs[0]), Read(srcs[1]), Stage(inst.dst.index).bumpEnvMat));
      break;
    }

    // ps_1_x's texture instructions
    case Opcode::TexCoord:
    case Opcode::TexBem:
    case Opcode::TexBemL:
    case Opcode::TexReg2AR:
    case Opcode::TexReg2GB:
    case Opcode::TexReg2RGB:
    case Opcode::TexM3x2Pad:
    case Opcode::TexM3x2Tex:
    case Opcode::TexM3x2Depth:
    case Opcode::TexM3x3Pad:
    case Opcode::TexM3x3Tex:
    case Opcode::TexM3x3Spec:
    case Opcode::TexM3x3VSpec:
    case Opcode::TexM3x3:
    case Opcode::TexDP3:
    case Opcode::TexDP3Tex:
    case Opcode::TexDepth:
      if(!emu.m_SM1)
        return StepFail(
            StringFormat::Fmt("Unsupported instruction %s", OpcodeName(version, inst).c_str()));
      if(!ExecuteSM1Texture(inst))
        return false;
      break;

    case Opcode::Tex:
    case Opcode::TexLdd:
    case Opcode::TexLdl:
    {
      if(emu.m_SM1 && inst.op == Opcode::Tex)
      {
        if(!ExecuteSM1Texture(inst))
          return false;
        break;
      }
      if(!inst.hasDst)
        return StepFail("Missing destination");
      Vec4 res;
      if(!Sample(inst, res))
      {
        m_Changes = NULL;
        return false;
      }
      Write(inst, res);
      break;
    }

    default:
      return StepFail(
          StringFormat::Fmt("Unsupported instruction %s", OpcodeName(version, inst).c_str()));
  }

#undef NEED_SRCS

  return true;
}

void PSQuad::Start(const PSEmulator &emu, const PSInputs inputs[QuadPixels],
                   const VSConstants &consts, PSCallbacks *callbacks, const PSStageState *stages)
{
  for(uint32_t i = 0; i < QuadPixels; i++)
  {
    m_Pixels[i].Start(emu, inputs[i], consts, callbacks, stages);
    m_Pixels[i].m_QuadIndex = i;
  }
}

bool PSQuad::Finished() const
{
  for(uint32_t i = 0; i < QuadPixels; i++)
    if(!m_Pixels[i].Finished())
      return false;
  return true;
}

bool PSQuad::Step(uint32_t pixel, rdcarray<PSRegisterChange> *changes)
{
  pixel &= 3;

  // what each pixel's instruction reads from its neighbours, before any of them runs it
  for(uint32_t i = 0; i < QuadPixels; i++)
  {
    PSThread &p = m_Pixels[i];
    if(p.Finished())
      continue;
    const Instruction &inst = p.m_Emu->GetProgram().instructions[p.m_IP];
    Vec4 own;
    if(p.QuadOperand(inst, own))
      for(uint32_t j = 0; j < QuadPixels; j++)
        m_Pixels[j].QuadOperand(inst, p.m_Quad[j]);
  }

  bool ret = false;
  for(uint32_t i = 0; i < QuadPixels; i++)
  {
    if(m_Pixels[i].Finished())
      continue;
    bool ok = m_Pixels[i].Step(i == pixel ? changes : NULL);
    if(i == pixel)
      ret = ok;
  }
  return ret;
}

bool PSQuad::Run()
{
  while(!Finished())
    Step(QuadTopLeft);
  for(uint32_t i = 0; i < QuadPixels; i++)
    if(m_Pixels[i].Failed())
      return false;
  return true;
}

};    // namespace D3D9Emu

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"
#include "d3d9_bytecode_test.h"

using namespace D3D9BCTest;
using namespace D3D9Emu;

namespace
{
const uint32_t X = Swz(0, 0, 0, 0), Y = Swz(1, 1, 1, 1), Z = Swz(2, 2, 2, 2), W = Swz(3, 3, 3, 3);
const uint32_t XYZW = 0xE4;

uint32_t R(uint32_t i, uint32_t swz = XYZW, SrcMod mod = SrcMod::None)
{
  return Src(RegType::Temp, i, swz, mod);
}
uint32_t C(uint32_t i, uint32_t swz = XYZW, SrcMod mod = SrcMod::None)
{
  return Src(RegType::Const, i, swz, mod);
}
uint32_t V(uint32_t i, uint32_t swz = XYZW)
{
  return Src(RegType::Input, i, swz);
}
uint32_t T(uint32_t i, uint32_t swz = XYZW)
{
  return Src(RegType::AddrOrTexture, i, swz);
}
uint32_t S(uint32_t i, uint32_t swz = XYZW)
{
  return Src(RegType::Sampler, i, swz);
}
uint32_t DR(uint32_t i, uint32_t mask = 0xF)
{
  return Dst(RegType::Temp, i, mask);
}
uint32_t OC(uint32_t i, uint32_t mask = 0xF)
{
  return Dst(RegType::ColorOut, i, mask);
}

float FromBits(uint32_t u)
{
  float f;
  memcpy(&f, &u, sizeof(f));
  return f;
}

bool SameBits(const Vec4 &a, const Vec4 &b)
{
  return memcmp(a.v, b.v, sizeof(a.v)) == 0;
}

bool SameBits(const IVec4 &a, const IVec4 &b)
{
  return memcmp(a.v, b.v, sizeof(a.v)) == 0;
}

// the same value, bit for bit (any NaN matches any NaN)
bool Same(float a, float b)
{
  if(a != a && b != b)
    return true;
  return FloatBits(a) == FloatBits(b);
}

#define CHECK_SAME(a, b)                                                                        \
  do                                                                                            \
  {                                                                                             \
    float a_ = (a), b_ = (b);                                                                   \
    INFO(StringFormat::Fmt("%s = %.9g (%08x), expected %.9g (%08x)", #a, a_, FloatBits(a_), b_, \
                           FloatBits(b_))                                                       \
             .c_str());                                                                         \
    CHECK(Same(a_, b_));                                                                        \
  } while(0)

void CheckVec(const Vec4 &v, float x, float y, float z, float w, float eps = 1e-6f)
{
  INFO(StringFormat::Fmt("(%.9g, %.9g, %.9g, %.9g) vs (%.9g, %.9g, %.9g, %.9g)", v[0], v[1], v[2],
                         v[3], x, y, z, w)
           .c_str());
  CHECK(fabsf(v[0] - x) <= eps * RDCMAX(1.0f, fabsf(x)));
  CHECK(fabsf(v[1] - y) <= eps * RDCMAX(1.0f, fabsf(y)));
  CHECK(fabsf(v[2] - z) <= eps * RDCMAX(1.0f, fabsf(z)));
  CHECK(fabsf(v[3] - w) <= eps * RDCMAX(1.0f, fabsf(w)));
}

Vec4 Splat4(float f)
{
  return Vec4(f, f, f, f);
}

// records each lookup and returns (coord.x, coord.y, sampler, kind)
struct StubSampler : PSCallbacks
{
  rdcarray<PSSample> samples;
  Vec4 Sample(const PSSample &s) override
  {
    samples.push_back(s);
    return Vec4(s.coord[0], s.coord[1], float(s.sampler), float(s.kind));
  }
};

// what a debugger would show of a pixel's registers, kept up to date only from the changes each
// step reports
struct Shadow
{
  Vec4 r[MaxPSTemps];
  Vec4 t[MaxPSTexCoords];
  IVec4 p;
  int32_t aL = 0;
  int32_t loop = -1;
  Vec4 color[MaxPSColorOutputs];
  Vec4 depth;

  void Take(const PSRegisters &regs)
  {
    for(uint32_t i = 0; i < MaxPSTemps; i++)
      r[i] = regs.r[i];
    for(uint32_t i = 0; i < MaxPSTexCoords; i++)
      t[i] = regs.t[i];
    for(int i = 0; i < 4; i++)
      p.v[i] = regs.p[i] ? 1 : 0;
    aL = regs.aL();
    loop = regs.InnermostLoop();
    for(uint32_t i = 0; i < MaxPSColorOutputs; i++)
      color[i] = regs.out.color[i];
    depth = regs.out.depth;
  }

  // checks a change's old value is what the shadow has, then takes the new value from regs
  bool Apply(const PSRegisterChange &c, const PSRegisters &regs)
  {
    switch(c.type)
    {
      case RegType::Temp:
        if(c.index >= MaxPSTemps || !SameBits(c.f, r[c.index]))
          return false;
        r[c.index] = regs.r[c.index];
        return true;
      case RegType::AddrOrTexture:
        if(c.index >= MaxPSTexCoords || !SameBits(c.f, t[c.index]))
          return false;
        t[c.index] = regs.t[c.index];
        return true;
      case RegType::Predicate:
        if(!SameBits(c.i, p))
          return false;
        for(int i = 0; i < 4; i++)
          p.v[i] = regs.p[i] ? 1 : 0;
        return true;
      case RegType::Loop:
        if(c.i.v[0] != aL)
          return false;
        aL = regs.aL();
        loop = regs.InnermostLoop();
        return true;
      case RegType::ColorOut:
        if(c.index >= MaxPSColorOutputs || !SameBits(c.f, color[c.index]))
          return false;
        color[c.index] = regs.out.color[c.index];
        return true;
      case RegType::DepthOut:
        if(!SameBits(c.f, depth))
          return false;
        depth = regs.out.depth;
        return true;
      default: return false;
    }
  }

  rdcstr Differences(const PSRegisters &regs) const
  {
    Shadow now;
    now.Take(regs);
    rdcstr ret;
    for(uint32_t i = 0; i < MaxPSTemps; i++)
      if(!SameBits(r[i], now.r[i]))
        ret += StringFormat::Fmt("r%u ", i);
    for(uint32_t i = 0; i < MaxPSTexCoords; i++)
      if(!SameBits(t[i], now.t[i]))
        ret += StringFormat::Fmt("t%u ", i);
    if(!SameBits(p, now.p))
      ret += "p0 ";
    if(aL != now.aL || loop != now.loop)
      ret += "aL ";
    for(uint32_t i = 0; i < MaxPSColorOutputs; i++)
      if(!SameBits(color[i], now.color[i]))
        ret += StringFormat::Fmt("oC%u ", i);
    if(!SameBits(depth, now.depth))
      ret += "oDepth ";
    return ret;
  }
};

struct PSRunner
{
  Program prog;
  PSEmulator emu;
  VSConstants consts;
  PSInputs inputs[QuadPixels];
  StubSampler stub;
  PSCallbacks *callbacks = &stub;
  PSStageState stages[NumTextureStages];

  PSOutputs out[QuadPixels];
  // each pixel's registers at the end
  PSRegisters regs[QuadPixels];
  bool killed[QuadPixels] = {};
  uint32_t executed[QuadPixels] = {};
  // the first pixel's error, if any failed
  rdcstr error;
  // whether Run steps each pixel again, checking the changes it reports
  bool checkStepping = true;

  PSRunner()
  {
    // pixels (4, 2) to (5, 3), as in the GPU probe's derivative test
    for(uint32_t i = 0; i < QuadPixels; i++)
      inputs[i].pos = Vec4(float(4 + (i & 1)), float(2 + (i >> 1)), 0.0f, 0.0f);
  }

  bool Run(const rdcarray<uint32_t> &tokens)
  {
    error.clear();
    stub.samples.clear();
    ParseProgram(tokens.data(), tokens.size(), prog);
    if(!emu.Init(prog))
    {
      error = emu.GetError();
      return false;
    }
    VSConstants c = emu.ApplyLiterals(consts);
    PSQuad quad;
    quad.Start(emu, inputs, c, callbacks, stages);
    bool ret = quad.Run();
    for(uint32_t i = 0; i < QuadPixels; i++)
    {
      const PSThread &t = quad.Pixel(i);
      regs[i] = t.Registers();
      out[i] = t.Registers().out;
      killed[i] = t.Registers().killed;
      executed[i] = t.InstructionsExecuted();
      if(error.empty() && t.Failed())
        error = t.GetError();
    }
    CHECK(ret == error.empty());

    rdcarray<PSSample> samples = stub.samples;
    for(uint32_t i = 0; checkStepping && i < QuadPixels; i++)
      CheckStepping(c, i);
    stub.samples = samples;
    return ret;
  }

  // steps the quad again with pixel's changes recorded, checking they account for everything that
  // changed and that it ends the same way
  void CheckStepping(const VSConstants &c, uint32_t pixel)
  {
    PSQuad quad;
    quad.Start(emu, inputs, c, callbacks, stages);

    Shadow shadow;
    shadow.Take(quad.Pixel(pixel).Registers());

    rdcarray<PSRegisterChange> changes;
    while(!quad.Pixel(pixel).Finished())
    {
      const uint32_t inst = quad.Pixel(pixel).NextInstruction();
      changes.clear();
      if(!quad.Step(pixel, &changes))
      {
        // nothing changed by a failed instruction
        CHECK(changes.empty());
        CHECK(quad.Pixel(pixel).NextInstruction() == inst);
        break;
      }
      for(const PSRegisterChange &ch : changes)
      {
        INFO(StringFormat::Fmt("pixel %u instruction %u, change to type %u index %u", pixel, inst,
                               uint32_t(ch.type), ch.index)
                 .c_str());
        CHECK(shadow.Apply(ch, quad.Pixel(pixel).Registers()));
      }
      rdcstr diff = shadow.Differences(quad.Pixel(pixel).Registers());
      INFO(StringFormat::Fmt("pixel %u instruction %u changed %s without reporting it", pixel, inst,
                             diff.c_str())
               .c_str());
      CHECK(diff.empty());
    }

    const PSRegisters &ended = quad.Pixel(pixel).Registers();
    CHECK(memcmp(&ended.out, &out[pixel], sizeof(PSOutputs)) == 0);
    CHECK(ended.killed == killed[pixel]);
    // nothing runs after the end
    CHECK_FALSE(quad.Step(pixel, &changes));
  }

  // runs a shader with one instruction on c0, c1 and c2 into oC0, returning pixel 0's oC0
  Vec4 OneOp(Opcode o, int numSrcs, Vec4 a, Vec4 b = Vec4(), Vec4 c = Vec4(), uint32_t swz = XYZW,
             uint32_t mask = 0xF, uint8_t major = 3)
  {
    consts.floats.resize(3);
    consts.floats[0] = a;
    consts.floats[1] = b;
    consts.floats[2] = c;
    Assembler as(major, 0, true);
    if(numSrcs == 1)
      as.op(o, {OC(0, mask), C(0, swz)});
    else if(numSrcs == 2)
      as.op(o, {OC(0, mask), C(0, swz), C(1, swz)});
    else
      as.op(o, {OC(0, mask), C(0, swz), C(1, swz), C(2, swz)});
    bool ok = Run(as.finish());
    INFO(error.c_str());
    CHECK(ok);
    return out[0].color[0];
  }

  // a scalar instruction's result, in x
  float Scalar(Opcode o, float a, float b = 0.0f)
  {
    return OneOp(o, o == Opcode::Pow ? 2 : 1, Vec4(0, 0, 0, a), Vec4(0, 0, 0, b), Vec4(), W)[0];
  }
};

const float Inf = INFINITY;
const float NaN = FromBits(0x7FC00000);
// the smallest denormal, and the largest
const float Den = FromBits(0x00000001);
const float BigDen = FromBits(0x007FFFFF);
};    // namespace

TEST_CASE("D3D9 pixel shader emulation - arithmetic as measured", "[d3d9][psemu]")
{
  // each case is one the GPU was measured doing in step 5.4 (g_analysis.txt there)
  PSRunner r;

  SECTION("zero times anything is +0, and denormals act as zero")
  {
    Vec4 m = r.OneOp(Opcode::Mul, 2, Vec4(0.0f, -0.0f, 0.0f, 1.0f), Vec4(-1.0f, 1.0f, Inf, -0.0f));
    CHECK_SAME(m[0], 0.0f);
    CHECK_SAME(m[1], 0.0f);
    CHECK_SAME(m[2], 0.0f);
    CHECK_SAME(m[3], 0.0f);
    m = r.OneOp(Opcode::Mul, 2, Vec4(-0.0f, NaN, 1.0f, Den), Vec4(NaN, 0.0f, Den, Inf));
    CHECK_SAME(m[0], 0.0f);
    CHECK_SAME(m[1], 0.0f);
    CHECK_SAME(m[2], 0.0f);
    CHECK_SAME(m[3], 0.0f);
    // a denormal result is flushed, keeping its sign
    m = r.OneOp(Opcode::Mul, 2, Vec4(FLT_MIN, -FLT_MIN, FLT_MAX, 3.0f), Vec4(0.5f, 0.5f, Den, 5.0f));
    CHECK_SAME(m[0], 0.0f);
    CHECK_SAME(m[1], -0.0f);
    CHECK_SAME(m[2], 0.0f);
    CHECK_SAME(m[3], 15.0f);

    Vec4 a = r.OneOp(Opcode::Add, 2, Vec4(0.0f, Den, FLT_MIN, -FLT_MIN), Vec4(Den, -0.0f, Den, Den));
    CHECK_SAME(a[0], 0.0f);
    CHECK_SAME(a[1], 0.0f);
    CHECK_SAME(a[2], FLT_MIN);
    CHECK_SAME(a[3], -FLT_MIN);

    // the probe's other denormal cases
    Vec4 f = r.OneOp(Opcode::Frc, 1, Vec4(Den, FromBits(0x803E0B60), -0.25f, 2.5f));
    CHECK_SAME(f[0], 0.0f);
    CHECK_SAME(f[1], 0.0f);
    CHECK_SAME(f[2], 0.75f);
    CHECK_SAME(f[3], 0.5f);
    CHECK_SAME(r.Scalar(Opcode::Exp, Den), 1.0f);
    CHECK_SAME(r.Scalar(Opcode::Rcp, Den), Inf);
    CHECK_SAME(r.Scalar(Opcode::Log, -BigDen), -Inf);
  }

  SECTION("mov keeps denormals")
  {
    Vec4 m = r.OneOp(Opcode::Mov, 1, Vec4(Den, -BigDen, 1.0f, -0.0f));
    CHECK_SAME(m[0], Den);
    CHECK_SAME(m[1], -BigDen);
    CHECK_SAME(m[3], -0.0f);
  }

  SECTION("mad is fused, and the dot products have fixed forms")
  {
    const float a = 3.75393772f, b = 3.91935158f, c = -14.7130022f;
    CHECK_SAME(r.OneOp(Opcode::Mad, 3, Vec4(a, 0, 0, 0), Vec4(b, 0, 0, 0), Vec4(c, 0, 0, 0))[0],
               fmaf(a, b, c));
    // a separate multiply and add would round twice
    volatile float prod = a * b;
    CHECK(fmaf(a, b, c) != prod + c);

    // the GPU's cases with denormals and denormal results
    Vec4 m = r.OneOp(Opcode::Mad, 3, Vec4(FromBits(0x80001F07), 1.26500582e-37f, 0, 0),
                     Vec4(2.09675908f, 1.35305715f, 0, 0),
                     Vec4(FromBits(0x00003FB0), -1.80360946e-37f, 0, 0));
    CHECK_SAME(m[0], 0.0f);
    CHECK_SAME(m[1], -0.0f);
    // zero times infinity is +0 there too
    CHECK_SAME(
        r.OneOp(Opcode::Mad, 3, Vec4(0, 0, 0, 0), Vec4(Inf, 0, 0, 0), Vec4(-0.0f, 0, 0, 0))[0], 0.0f);

    const Vec4 p(1.66144085f, -3.63141918f, -3.06645632f, -1.30401039f),
        q(3.85177231f, 3.52655411f, -3.77186584f, -3.6399889f);
    float d3 = fmaf(p[2], q[2], fmaf(p[1], q[1], p[0] * q[0]));
    CHECK_SAME(r.OneOp(Opcode::Dp3, 2, p, q)[0], d3);
    CHECK_SAME(r.OneOp(Opcode::Dp4, 2, p, q)[0], fmaf(p[3], q[3], d3));
    const Vec4 s(0.5f, 0.25f, 0.125f, 1.08046341f);
    CHECK_SAME(r.OneOp(Opcode::Dp2Add, 3, p, q, s)[0], fmaf(p[1], q[1], p[0] * q[0]) + s[0]);
    CHECK_SAME(r.OneOp(Opcode::Lrp, 3, p, q, s)[1], fmaf(p[1], q[1] - s[1], s[1]));

    // infinity times zero in a dot product, and lrp with NaN, both from the probe
    CHECK_SAME(r.OneOp(Opcode::Dp3, 2, Vec4(Inf, 1, 1, 0), Vec4(0, 1, 1, 0))[0], 2.0f);
    CHECK_SAME(r.OneOp(Opcode::Lrp, 3, Splat4(NaN), Splat4(1.0f), Splat4(1.0f))[0], 1.0f);
    CHECK_SAME(r.OneOp(Opcode::Lrp, 3, Splat4(Inf), Splat4(0.0f), Splat4(0.0f))[0], 0.0f);
  }

  SECTION("min, max and cmp")
  {
    Vec4 mn = r.OneOp(Opcode::Min, 2, Vec4(0.0f, -0.0f, 1.0f, Inf), Vec4(NaN, 0.0f, NaN, NaN));
    CHECK_SAME(mn[0], 0.0f);
    CHECK_SAME(mn[1], -0.0f);
    CHECK_SAME(mn[2], 1.0f);
    CHECK_SAME(mn[3], Inf);
    Vec4 mx = r.OneOp(Opcode::Max, 2, Vec4(-0.0f, 0.0f, -0.0f, Den), Vec4(0.0f, -0.0f, NaN, -1.0f));
    CHECK_SAME(mx[0], 0.0f);
    CHECK_SAME(mx[1], 0.0f);
    CHECK_SAME(mx[2], -0.0f);
    CHECK_SAME(mx[3], 0.0f);
    CHECK_SAME(r.OneOp(Opcode::Min, 2, Splat4(FromBits(0x8003B6CC)), Splat4(3.673527f))[0], -0.0f);
    float nn = r.OneOp(Opcode::Min, 2, Splat4(NaN), Splat4(NaN))[0];
    CHECK(nn != nn);

    // -0 and negative denormals count as >= 0, NaN doesn't
    Vec4 c = r.OneOp(Opcode::Cmp, 3, Vec4(-0.0f, FromBits(0x803E0B60), NaN, -1e-30f), Splat4(1.0f),
                     Splat4(2.0f));
    CheckVec(c, 1.0f, 1.0f, 2.0f, 2.0f);
    // the value chosen is moved, denormals and all
    CHECK_SAME(r.OneOp(Opcode::Cmp, 3, Splat4(1.0f), Splat4(Den), Splat4(2.0f))[0], Den);
  }

  SECTION("transcendentals' special cases")
  {
    CHECK_SAME(r.Scalar(Opcode::Rcp, -0.0f), -Inf);
    CHECK_SAME(r.Scalar(Opcode::Rcp, -Inf), -0.0f);
    CHECK_SAME(r.Scalar(Opcode::Rcp, 4.0f), 0.25f);
    CHECK_SAME(r.Scalar(Opcode::Rsq, -0.0f), Inf);
    CHECK_SAME(r.Scalar(Opcode::Rsq, -Inf), 0.0f);
    CHECK_SAME(r.Scalar(Opcode::Rsq, -4.0f), 0.5f);
    CHECK_SAME(r.Scalar(Opcode::Log, 0.0f), -Inf);
    CHECK_SAME(r.Scalar(Opcode::Log, -Inf), Inf);
    CHECK_SAME(r.Scalar(Opcode::Log, -8.0f), 3.0f);
    // exp(-127) would be a denormal
    CHECK_SAME(r.Scalar(Opcode::Exp, -127.0f), 0.0f);
    CHECK_SAME(r.Scalar(Opcode::Exp, 128.0f), Inf);
    CHECK_SAME(r.Scalar(Opcode::Exp, -Inf), 0.0f);
    CHECK_SAME(r.Scalar(Opcode::Pow, 0.0f, 0.0f), 1.0f);
    CHECK_SAME(r.Scalar(Opcode::Pow, NaN, 0.0f), 1.0f);
    CHECK_SAME(r.Scalar(Opcode::Pow, -1.0f, NaN), 1.0f);
    CHECK_SAME(r.Scalar(Opcode::Pow, 1.0f, Inf), 1.0f);
    CHECK_SAME(r.Scalar(Opcode::Pow, 0.0f, -1.0f), Inf);
    CHECK_SAME(r.Scalar(Opcode::Pow, -0.0f, NaN), NaN);
    CHECK_SAME(r.Scalar(Opcode::Pow, 0.5f, Inf), 0.0f);
    CHECK_SAME(r.Scalar(Opcode::Pow, -2.0f, 3.0f), 8.0f);

    // sin(-0) is +0
    Vec4 sc = r.OneOp(Opcode::SinCos, 1, Splat4(-0.0f), Vec4(), Vec4(), W, 0x3);
    CHECK_SAME(sc[0], 1.0f);
    CHECK_SAME(sc[1], 0.0f);
    sc = r.OneOp(Opcode::SinCos, 1, Splat4(Inf), Vec4(), Vec4(), W, 0x3);
    CHECK_SAME(sc[0], NaN);
    CHECK_SAME(sc[1], NaN);
    // only the masked components are written: sin alone
    sc = r.OneOp(Opcode::SinCos, 1, Splat4(1.0f), Vec4(), Vec4(), W, 0x2);
    CHECK_SAME(sc[0], 0.0f);
    CHECK_SAME(sc[1], sinf(1.0f));
    // ps_2_0's has two more sources
    sc = r.OneOp(Opcode::SinCos, 3, Splat4(1.0f), Splat4(5.0f), Splat4(6.0f), W, 0x3, 2);
    CHECK_SAME(sc[0], cosf(1.0f));
    CHECK_SAME(sc[1], sinf(1.0f));

    // nrm of a zero vector, or of an infinite one, is 0 (w is src.w times the same factor)
    Vec4 n = r.OneOp(Opcode::Nrm, 1, Vec4(0.0f, 0.0f, 0.0f, 0.0f));
    CheckVec(n, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    n = r.OneOp(Opcode::Nrm, 1, Vec4(Inf, 0.0f, 0.0f, 0.0f));
    CheckVec(n, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    n = r.OneOp(Opcode::Nrm, 1, Vec4(3.0f, 0.0f, 4.0f, 10.0f));
    CheckVec(n, 0.6f, 0.0f, 0.8f, 2.0f);
  }

  SECTION("_sat and _pp")
  {
    Assembler a(3, 0, true);
    a.op(Opcode::Mov, {OC(0) | (uint32_t(DstMod_Saturate) << 20), C(0)});
    a.op(Opcode::Rcp, {Dst(RegType::Temp, 0, 0xF, DstMod_PartialPrecision), C(0, Y)});
    a.op(Opcode::Mov, {OC(1), R(0)});
    r.consts.floats.resize(1);
    r.consts.floats[0] = Vec4(NaN, 3.0f, -1.0f, 0.5f);
    REQUIRE(r.Run(a.finish()));
    CheckVec(r.out[0].color[0], 0.0f, 1.0f, 0.0f, 0.5f, 0.0f);
    // _pp is ignored
    CHECK_SAME(r.out[0].color[1][0], 1.0f / 3.0f);
  }
}

TEST_CASE("D3D9 pixel shader emulation - quads", "[d3d9][psemu]")
{
  PSRunner r;

  SECTION("derivatives are per quad")
  {
    // f = x * y at pixels (4, 2) to (5, 3): 8, 10, 12, 15. The GPU gave dsx 2 and dsy 4 in all
    // four pixels.
    for(uint32_t i = 0; i < QuadPixels; i++)
      r.inputs[i].v[0] = Vec4(r.inputs[i].pos[0], r.inputs[i].pos[1], 0.0f, 0.0f);

    Assembler a(3, 0, true);
    a.dcl(DeclUsage::TexCoord, 0, 0);
    a.op(Opcode::Mul, {DR(0), V(0, X), V(0, Y)});
    a.op(Opcode::Dsx, {DR(1), R(0)});
    a.op(Opcode::Dsy, {DR(2), R(0, XYZW, SrcMod::Neg)});
    // reading and writing the same register sees the neighbours' values from before
    a.op(Opcode::Dsx, {DR(0), R(0)});
    a.op(Opcode::Mov, {OC(0), R(1)});
    a.op(Opcode::Mov, {OC(1), R(2)});
    a.op(Opcode::Mov, {OC(2), R(0)});
    REQUIRE(r.Run(a.finish()));
    for(uint32_t i = 0; i < QuadPixels; i++)
    {
      INFO("pixel " << i);
      CheckVec(r.out[i].color[0], 2.0f, 2.0f, 2.0f, 2.0f);
      CheckVec(r.out[i].color[1], -4.0f, -4.0f, -4.0f, -4.0f);
      CheckVec(r.out[i].color[2], 2.0f, 2.0f, 2.0f, 2.0f);
      CHECK(r.out[i].written == 0x7);
    }

    // ps_2_x has them too, on t#
    for(uint32_t i = 0; i < QuadPixels; i++)
      r.inputs[i].t[3] = r.inputs[i].v[0];
    Assembler b(2, 1, true);
    b.op(Opcode::Dcl, {DclNone(), Dst(RegType::AddrOrTexture, 3)});
    b.op(Opcode::Mul, {DR(0), T(3, X), T(3, Y)});
    b.op(Opcode::Dsy, {DR(1), R(0)});
    b.op(Opcode::Mov, {OC(0), R(1)});
    REQUIRE(r.Run(b.finish()));
    for(uint32_t i = 0; i < QuadPixels; i++)
      CheckVec(r.out[i].color[0], 4.0f, 4.0f, 4.0f, 4.0f);
  }

  SECTION("texture lookups")
  {
    // a coordinate with different derivatives in each component, and a w for texldp, texldb and
    // texldl
    for(uint32_t i = 0; i < QuadPixels; i++)
    {
      float x = r.inputs[i].pos[0], y = r.inputs[i].pos[1];
      r.inputs[i].v[0] = Vec4(0.25f * x, 0.5f * y, 0.125f * (x + y), 1.0f + 0.5f * x * y);
    }
    r.consts.floats.resize(2);
    r.consts.floats[0] = Vec4(1.0f, 2.0f, 3.0f, 4.0f);
    r.consts.floats[1] = Vec4(5.0f, 6.0f, 7.0f, 8.0f);

    Assembler a(3, 0, true);
    a.dcl(DeclUsage::TexCoord, 0, 0);
    a.op(Opcode::Dcl, {DclSampler(2), Dst(RegType::Sampler, 0)});
    a.op(Opcode::Dcl, {DclSampler(3), Dst(RegType::Sampler, 3)});
    a.op(Opcode::Tex, {DR(0), V(0), S(0)});
    a.op(Opcode::Tex, {DR(1), V(0), S(0)}, 1);
    a.op(Opcode::Tex, {DR(2), V(0), S(3)}, 2);
    a.op(Opcode::TexLdd, {DR(3), V(0), S(0), C(0), C(1)});
    a.op(Opcode::TexLdl, {DR(4), V(0), S(3)});
    // the sampler's swizzle applies to the result
    a.op(Opcode::Tex, {DR(5), V(0), S(0, Swz(3, 2, 1, 0))});
    for(uint32_t i = 0; i < 4; i++)
      a.op(Opcode::Mov, {OC(i), R(i)});
    a.op(Opcode::Mov, {Dst(RegType::DepthOut, 0, 0x1), R(5, X)});
    REQUIRE(r.Run(a.finish()));

    // one lookup per pixel per instruction, the pixels in order
    REQUIRE(r.stub.samples.size() == 6 * QuadPixels);
    const PSInputs *in = r.inputs;
    auto proj = [](const Vec4 &v) { return Vec4(v[0] / v[3], v[1] / v[3], v[2] / v[3], 0.0f); };
    const Vec4 p0 = proj(in[0].v[0]), p1 = proj(in[1].v[0]), p2 = proj(in[2].v[0]);
    for(uint32_t i = 0; i < QuadPixels; i++)
    {
      INFO("pixel " << i);
      const PSSample &ld = r.stub.samples[0 * QuadPixels + i];
      CHECK((ld.kind == SampleKind::Implicit));
      CHECK(ld.sampler == 0);
      CHECK(ld.type == SamplerTextureType::Tex2D);
      CHECK(SameBits(ld.coord, in[i].v[0]));
      // per quad: the top row's and the left column's differences
      CheckVec(ld.ddx, 0.25f, 0.0f, 0.125f, 0.0f, 0.0f);
      CheckVec(ld.ddy, 0.0f, 0.5f, 0.125f, 0.0f, 0.0f);
      CHECK(ld.lod == 0.0f);

      const PSSample &ldp = r.stub.samples[1 * QuadPixels + i];
      CHECK((ldp.kind == SampleKind::Projected));
      CHECK(SameBits(ldp.coord, in[i].v[0]));
      CheckVec(ldp.ddx, p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2], 0.0f);
      CheckVec(ldp.ddy, p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2], 0.0f);

      const PSSample &ldb = r.stub.samples[2 * QuadPixels + i];
      CHECK((ldb.kind == SampleKind::Bias));
      CHECK(ldb.sampler == 3);
      CHECK(ldb.type == SamplerTextureType::Cube);
      CHECK(ldb.lod == in[i].v[0][3]);
      CheckVec(ldb.ddx, 0.25f, 0.0f, 0.125f, 0.0f, 0.0f);

      const PSSample &ldd = r.stub.samples[3 * QuadPixels + i];
      CHECK((ldd.kind == SampleKind::Gradient));
      CHECK(SameBits(ldd.ddx, r.consts.floats[0]));
      CHECK(SameBits(ldd.ddy, r.consts.floats[1]));

      const PSSample &ldl = r.stub.samples[4 * QuadPixels + i];
      CHECK((ldl.kind == SampleKind::Level));
      CHECK(ldl.lod == in[i].v[0][3]);
      CHECK(SameBits(ldl.ddx, Vec4()));

      // the stub returns (coord.x, coord.y, sampler, kind)
      CheckVec(r.out[i].color[0], in[i].v[0][0], in[i].v[0][1], 0.0f, 0.0f);
      CheckVec(r.out[i].color[1], in[i].v[0][0], in[i].v[0][1], 0.0f, 1.0f);
      CheckVec(r.out[i].color[2], in[i].v[0][0], in[i].v[0][1], 3.0f, 2.0f);
      CheckVec(r.out[i].color[3], in[i].v[0][0], in[i].v[0][1], 0.0f, 3.0f);
      // texld r5 swizzled .wzyx: x is the kind
      CHECK(r.out[i].depth[0] == 0.0f);
      CHECK(r.out[i].written == (0xFU | PSWroteDepth));
    }

    // without callbacks, sampling fails
    r.callbacks = NULL;
    Assembler b(2, 0, true);
    b.op(Opcode::Dcl, {DclNone(), Dst(RegType::AddrOrTexture, 0)});
    b.op(Opcode::Dcl, {DclSampler(2), Dst(RegType::Sampler, 0)});
    b.op(Opcode::Tex, {DR(0), T(0), S(0)});
    b.op(Opcode::Mov, {OC(0), R(0)});
    CHECK_FALSE(r.Run(b.finish()));
    CHECK(r.error == "Sampling isn't available at instruction 2 (texld)");
  }

  SECTION("texkill discards, and the pixel runs on")
  {
    r.consts.floats.resize(1);
    r.consts.floats[0] = Vec4(1.0f, 2.0f, 3.0f, 4.0f);
    r.inputs[0].t[0] = Vec4(0.0f, 1.0f, 2.0f, 3.0f);
    r.inputs[1].t[0] = Vec4(0.0f, -1.0f, 2.0f, 3.0f);
    r.inputs[2].t[0] = Vec4(-0.0f, 1.0f, 2.0f, FromBits(0x80000001));
    // w counts too
    r.inputs[3].t[0] = Vec4(0.0f, 1.0f, 2.0f, -3.0f);

    Assembler a(2, 0, true);
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::AddrOrTexture, 0)});
    a.op(Opcode::TexKill, {Dst(RegType::AddrOrTexture, 0)});
    a.op(Opcode::Mov, {OC(0), C(0)});
    REQUIRE(r.Run(a.finish()));
    CHECK_FALSE(r.killed[0]);
    CHECK(r.killed[1]);
    // -0 and negative denormals aren't below zero
    CHECK_FALSE(r.killed[2]);
    CHECK(r.killed[3]);
    for(uint32_t i = 0; i < QuadPixels; i++)
    {
      CheckVec(r.out[i].color[0], 1.0f, 2.0f, 3.0f, 4.0f);
      CHECK(r.executed[i] == 3);
    }
  }

  SECTION("pixels take their own paths")
  {
    for(uint32_t i = 0; i < QuadPixels; i++)
      for(uint32_t j = 0; j < 3; j++)
        r.inputs[i].v[j] = Vec4(float(i * 100 + j), 1.0f, 0.0f, 0.0f);
    r.inputs[3].face = -1.0f;

    Assembler a(3, 0, true);
    a.def(0, 4.5f, 0.0f, 1.0f, -1.0f);
    a.defi(0, 3, 0, 1, 0);
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::MiscType, 0, 0x3)});
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::MiscType, 1)});
    a.dcl(DeclUsage::TexCoord, 0, 0);
    a.dcl(DeclUsage::TexCoord, 1, 1);
    a.dcl(DeclUsage::TexCoord, 2, 2);
    a.op(Opcode::Mov, {DR(0), C(0, Y)});
    // the left column: r0 = v0 + v1 + v2, with v[aL]
    a.op(Opcode::IfC, {Src(RegType::MiscType, 0, X), C(0, X)}, uint32_t(Comparison::LT));
    a.op(Opcode::Loop, {Src(RegType::Loop, 0), Src(RegType::ConstInt, 0)});
    a.op(Opcode::Add, {DR(0), R(0), Src(RegType::Input, 0) | RelBit, Src(RegType::Loop, 0)});
    a.op(Opcode::EndLoop, {});
    a.op(Opcode::Else, {});
    a.op(Opcode::Mov, {DR(0), C(0, W)});
    a.op(Opcode::EndIf, {});
    // back-facing pixels
    a.op(Opcode::IfC, {Src(RegType::MiscType, 1, X), C(0, Y)}, uint32_t(Comparison::LT));
    a.op(Opcode::Mov, {DR(0, 0x2), C(0, X)});
    a.op(Opcode::EndIf, {});
    a.op(Opcode::Mov, {OC(0), R(0)});
    a.op(Opcode::Mov, {OC(1), Src(RegType::MiscType, 0)});
    REQUIRE(r.Run(a.finish()));
    CheckVec(r.out[0].color[0], 3.0f, 3.0f, 0.0f, 0.0f);
    CheckVec(r.out[2].color[0], 603.0f, 3.0f, 0.0f, 0.0f);
    CheckVec(r.out[1].color[0], -1.0f, -1.0f, -1.0f, -1.0f);
    CheckVec(r.out[3].color[0], -1.0f, 4.5f, -1.0f, -1.0f);
    CheckVec(r.out[3].color[1], 5.0f, 3.0f, 0.0f, 0.0f);
    // the left column ran the loop
    CHECK(r.executed[0] > r.executed[1]);
  }

  SECTION("predicates")
  {
    r.inputs[0].v[0] = Vec4(1.0f, -1.0f, 1.0f, -1.0f);
    Assembler a(3, 0, true);
    a.def(0, 0.0f, 0.0f, 0.0f, 0.0f);
    a.def(1, 5.0f, 6.0f, 7.0f, 8.0f);
    a.dcl(DeclUsage::Color, 0, 0);
    a.op(Opcode::Mov, {OC(0), C(0)});
    a.op(Opcode::SetP, {Dst(RegType::Predicate, 0), V(0), C(0)}, uint32_t(Comparison::GT));
    a.op(Opcode::Mov, {OC(0), Src(RegType::Predicate, 0), C(1)}, 0, PredBit);
    a.op(Opcode::Mov, {OC(1), Src(RegType::Predicate, 0, Y), C(1)}, 0, PredBit);
    REQUIRE(r.Run(a.finish()));
    CheckVec(r.out[0].color[0], 5.0f, 0.0f, 7.0f, 0.0f);
    // p0.y is false, so oC1 wasn't written
    CHECK(r.out[0].written == 0x1);
  }

  SECTION("transcendentals go through the callbacks")
  {
    struct Callbacks : StubSampler
    {
      rdcarray<Opcode> ops;
      Vec4 Transcendental(Opcode op, float a, float b) override
      {
        ops.push_back(op);
        if(op == Opcode::Rsq)
          return Splat4(0.5f);
        return Vec4(7.0f, 8.0f, 9.0f, 10.0f);
      }
    } cb;
    r.callbacks = &cb;
    r.consts.floats.resize(1);
    r.consts.floats[0] = Vec4(1.0f, 2.0f, 3.0f, 4.0f);
    Assembler a(3, 0, true);
    a.op(Opcode::Rcp, {OC(0), C(0, X)});
    a.op(Opcode::SinCos, {OC(1, 0x3), C(0, X)});
    a.op(Opcode::Nrm, {OC(2), C(0)});
    REQUIRE(r.Run(a.finish()));
    CheckVec(r.out[0].color[0], 7.0f, 7.0f, 7.0f, 7.0f);
    CheckVec(r.out[0].color[1], 7.0f, 8.0f, 0.0f, 0.0f);
    CheckVec(r.out[0].color[2], 0.5f, 1.0f, 1.5f, 2.0f);
    // 3 instructions in 4 pixels, in the quad's run and in each pixel's stepping check
    CHECK(cb.ops.size() == 3 * 4 * 5);
  }
}

namespace
{
uint32_t DT(uint32_t i, uint32_t mask = 0xF)
{
  return Dst(RegType::AddrOrTexture, i, mask);
}

// a runner with the texture coordinates the ps_1_x tests use: pixel (x, y) of the quad has
// t0 = (0.1 + 0.25x, 0.2 + 0.5y, 0.3, 0.4)
struct PS1Runner : PSRunner
{
  PS1Runner()
  {
    for(uint32_t i = 0; i < QuadPixels; i++)
    {
      const float x = float(i & 1), y = float(i >> 1);
      inputs[i].t[0] = Vec4(0.1f + 0.25f * x, 0.2f + 0.5f * y, 0.3f, 0.4f);
      inputs[i].t[1] = Vec4(-0.5f, 0.5f, 1.5f, 0.75f);
      inputs[i].t[2] = Vec4(0.125f, 0.25f, 0.0f, 0.0f);
      inputs[i].t[3] = Vec4(1.0f, 2.0f, 3.0f, 4.0f);
      inputs[i].v[0] = Vec4(0.1f, 0.2f, 0.3f, 0.4f);
      inputs[i].v[1] = Vec4(0.5f, 0.5f, 0.5f, 0.5f);
    }
  }

  bool RunOK(const rdcarray<uint32_t> &tokens)
  {
    bool ok = Run(tokens);
    INFO(error.c_str());
    CHECK(ok);
    return ok;
  }
};

// the reflection texm3x3spec and texm3x3vspec look up, in doubles
Vec4 ReflectD(const Vec4 &n, const Vec4 &e)
{
  double ne = 0, nn = 0;
  for(int i = 0; i < 3; i++)
  {
    ne += double(n[i]) * e[i];
    nn += double(n[i]) * n[i];
  }
  Vec4 ret;
  for(int i = 0; i < 3; i++)
    ret[i] = float(2.0 * n[i] * ne / nn - e[i]);
  return ret;
}
};    // namespace

TEST_CASE("D3D9 pixel shader emulation - ps_1_x", "[d3d9][psemu]")
{
  PS1Runner r;

  SECTION("constants are clamped as they're read, and r0 when it's written out")
  {
    r.consts.floats = {Vec4(2.0f, -2.0f, 0.25f, 0.75f)};
    Assembler as(1, 1, true);
    // the modifier applies after the clamp
    as.op(Opcode::Mov, {DR(1), C(0, XYZW, SrcMod::Sign)});
    // registers have no range limit
    as.op(Opcode::Add, {DR(0), C(0), C(0)});
    r.RunOK(as.finish());
    CheckVec(r.regs[0].r[1], 1.0f, -3.0f, -0.5f, 0.5f);
    CheckVec(r.regs[0].r[0], 2.0f, -2.0f, 0.5f, 1.5f);
    CheckVec(r.out[0].color[0], 1.0f, 0.0f, 0.5f, 1.0f);
    CHECK(r.out[0].written == 1U);

    // not in ps_2_0
    CHECK_SAME(r.OneOp(Opcode::Mov, 1, Vec4(2.0f, 0, 0, 0), Vec4(), Vec4(), XYZW, 0xF, 2)[0], 2.0f);
  };

  SECTION("cnd and co-issue")
  {
    r.consts.floats = {Vec4(0.5f, nextafterf(0.5f, 1.0f), NaN, -1.0f),
                       Vec4(0.25f, 0.25f, 0.25f, 0.25f), Vec4(0.75f, 0.75f, 0.75f, 0.75f)};
    Assembler as(1, 4, true);
    as.op(Opcode::Cnd, {DR(0), C(0), C(1), C(2)});
    r.RunOK(as.finish());
    // strictly above 0.5 takes src1
    CheckVec(r.out[0].color[0], 0.75f, 0.25f, 0.75f, 0.75f);

    // the co-issued mov reads r0.b from before the mul writes it
    Assembler co(1, 1, true);
    co.op(Opcode::Mov, {DR(0), V(0)});
    co.op(Opcode::Mul, {DR(0, 0x7), V(1), V(1)});
    co.op(Opcode::Mov, {DR(0, 0x8), R(0, Z)}, 0, CoIssue);
    r.RunOK(co.finish());
    CheckVec(r.out[0].color[0], 0.25f, 0.25f, 0.25f, 0.3f);
    CHECK(r.executed[0] == 3);
  };

  SECTION("ps_1_1 to ps_1_3's texture registers")
  {
    r.stages[2].bumpEnvMat[0] = 1.0f;
    r.stages[2].bumpEnvMat[1] = 2.0f;
    r.stages[2].bumpEnvMat[2] = 3.0f;
    r.stages[2].bumpEnvMat[3] = 4.0f;

    Assembler as(1, 1, true);
    as.op(Opcode::Tex, {DT(0)});
    as.op(Opcode::TexCoord, {DT(1)});
    as.op(Opcode::TexBem, {DT(2), T(0)});
    as.op(Opcode::TexReg2AR, {DT(3), T(0)});
    as.op(Opcode::Mov, {DR(0), T(2)});
    r.RunOK(as.finish());

    const PSRegisters &regs = r.regs[0];
    // the stub returns (coord.xy, stage, kind)
    CheckVec(regs.t[0], 0.1f, 0.2f, 0.0f, 0.0f);
    // texcoord clamps all four
    CheckVec(regs.t[1], 0.0f, 0.5f, 1.0f, 0.75f);
    // (u, v) + the stage's matrix times (r, g)
    CheckVec(regs.t[2], 0.825f, 1.25f, 2.0f, 0.0f);
    // (a, r)
    CheckVec(regs.t[3], 0.0f, 0.1f, 3.0f, 0.0f);
    CheckVec(r.out[0].color[0], 0.825f, 1.0f, 1.0f, 0.0f);

    // one lookup per pixel per instruction, pixel 0 first
    REQUIRE(r.stub.samples.size() == 12);
    const PSSample &bem = r.stub.samples[4];
    CHECK(bem.sampler == 2);
    CHECK((bem.kind == SampleKind::Implicit));
    CHECK((bem.type == SamplerTextureType::Unknown));
    // the derivatives are of the bumped coordinate
    CheckVec(bem.ddx, 0.25f, 0.5f, 0.0f, 0.0f);
    CheckVec(bem.ddy, 1.5f, 2.0f, 0.0f, 0.0f);

    // texbeml scales by b * scale + offset, clamped to [0, 1]
    r.stages[2].lumScale = 2.0f;
    r.stages[2].lumOffset = 0.25f;
    Assembler lum(1, 1, true);
    lum.op(Opcode::Tex, {DT(0)});
    lum.op(Opcode::TexBemL, {DT(2), T(0)});
    r.RunOK(lum.finish());
    CheckVec(r.regs[0].t[2], 0.825f * 0.25f, 1.25f * 0.25f, 0.5f, 0.0f);
    r.stages[2].lumOffset = -1.0f;
    r.RunOK(lum.finish());
    CheckVec(r.regs[0].t[2], 0.0f, 0.0f, 0.0f, 0.0f);
    r.stages[2].lumOffset = 1.5f;
    r.RunOK(lum.finish());
    CheckVec(r.regs[0].t[2], 0.825f, 1.25f, 2.0f, 0.0f);
  };

  SECTION("the matrix instructions")
  {
    // t0 = (0.1, 0.2, 0, 0) from the stub, so with t1-t3's coordinates the rows give 0.05, 0.0625
    // and 0.5
    Assembler m2(1, 1, true);
    m2.op(Opcode::Tex, {DT(0)});
    m2.op(Opcode::TexM3x2Pad, {DT(1), T(0)});
    m2.op(Opcode::TexM3x2Tex, {DT(2), T(0)});
    r.RunOK(m2.finish());
    // the pad's dot product is kept in x
    CheckVec(r.regs[0].t[1], 0.05f, 0.0f, 0.0f, 0.0f);
    CheckVec(r.regs[0].t[2], 0.05f, 0.0625f, 2.0f, 0.0f);

    Assembler m3(1, 2, true);
    m3.op(Opcode::Tex, {DT(0)});
    m3.op(Opcode::TexM3x3Pad, {DT(1), T(0)});
    m3.op(Opcode::TexM3x3Pad, {DT(2), T(0)});
    m3.op(Opcode::TexM3x3, {DT(3), T(0)});
    r.RunOK(m3.finish());
    CheckVec(r.regs[0].t[3], 0.05f, 0.0625f, 0.5f, 1.0f);

    const Vec4 n(0.05f, 0.0625f, 0.5f, 0.0f);

    // the eye vector from the coordinates' w
    Assembler vspec(1, 1, true);
    vspec.op(Opcode::Tex, {DT(0)});
    vspec.op(Opcode::TexM3x3Pad, {DT(1), T(0)});
    vspec.op(Opcode::TexM3x3Pad, {DT(2), T(0)});
    vspec.op(Opcode::TexM3x3VSpec, {DT(3), T(0)});
    r.RunOK(vspec.finish());
    Vec4 refl = ReflectD(n, Vec4(0.75f, 0.0f, 4.0f, 0.0f));
    CheckVec(r.stub.samples.back().coord, refl[0], refl[1], refl[2], 0.0f, 1e-5f);

    // or a constant, which isn't clamped
    r.consts.floats = {Vec4(2.0f, 0.0f, 0.0f, 0.0f)};
    Assembler spec(1, 1, true);
    spec.op(Opcode::Tex, {DT(0)});
    spec.op(Opcode::TexM3x3Pad, {DT(1), T(0)});
    spec.op(Opcode::TexM3x3Pad, {DT(2), T(0)});
    spec.op(Opcode::TexM3x3Spec, {DT(3), T(0), C(0)});
    r.RunOK(spec.finish());
    refl = ReflectD(n, Vec4(2.0f, 0.0f, 0.0f, 0.0f));
    CheckVec(r.stub.samples.back().coord, refl[0], refl[1], refl[2], 0.0f, 1e-5f);
    CHECK(r.stub.samples.back().sampler == 3);

    Assembler dp3(1, 2, true);
    dp3.op(Opcode::Tex, {DT(0)});
    dp3.op(Opcode::TexDP3, {DT(1), T(0)});
    dp3.op(Opcode::TexDP3Tex, {DT(2), T(0)});
    r.RunOK(dp3.finish());
    CheckVec(r.regs[0].t[1], 0.05f, 0.05f, 0.05f, 0.05f);
    CheckVec(r.regs[0].t[2], 0.0625f, 0.0f, 2.0f, 0.0f);

    // depth is the pad's row over the next
    Assembler depth(1, 3, true);
    depth.op(Opcode::Tex, {DT(0)});
    depth.op(Opcode::TexM3x2Pad, {DT(1), T(0)});
    depth.op(Opcode::TexM3x2Depth, {DT(2), T(0)});
    r.RunOK(depth.finish());
    CHECK(fabsf(r.out[0].depth[0] - 0.8f) < 1e-6f);
    CHECK((r.out[0].written & PSWroteDepth) != 0);
  };

  SECTION("projection and texkill")
  {
    // D3DTTFF_PROJECTED divides by w in ps_1_1-1_3
    r.stages[0].projected = true;
    Assembler tex(1, 1, true);
    tex.op(Opcode::Tex, {DT(0)});
    r.RunOK(tex.finish());
    CheckVec(r.regs[0].t[0], 0.25f, 0.5f, 0.0f, 0.0f);

    // and not in ps_1_4
    Assembler ld(1, 4, true);
    ld.op(Opcode::Tex, {DR(0), T(0)});
    r.RunOK(ld.finish());
    CheckVec(r.regs[0].r[0], 0.1f, 0.2f, 0.0f, 0.0f);

    // texkill t# tests its coordinate's x, y and z
    Assembler kill(1, 1, true);
    kill.op(Opcode::TexKill, {DT(1)});
    r.RunOK(kill.finish());
    for(uint32_t i = 0; i < QuadPixels; i++)
      CHECK(r.killed[i]);
    for(uint32_t i = 0; i < QuadPixels; i++)
      r.inputs[i].t[1] = Vec4(0.5f, 0.5f, 0.5f, -1.0f);
    r.RunOK(kill.finish());
    for(uint32_t i = 0; i < QuadPixels; i++)
      CHECK_FALSE(r.killed[i]);
  };

  SECTION("ps_1_4")
  {
    r.stages[3].bumpEnvMat[0] = 2.0f;
    r.stages[3].bumpEnvMat[3] = 2.0f;

    Assembler as(1, 4, true);
    as.op(Opcode::TexCoord, {DR(0, 0x7), T(0)});
    as.op(Opcode::Tex, {DR(1), T(1)});
    as.op(Opcode::Tex, {DR(2), Src(RegType::AddrOrTexture, 3, Swz(0, 1, 3, 3), SrcMod::DW)});
    as.op(Opcode::Bem, {DR(3, 0x3), R(0), R(1)});
    as.op(Opcode::Phase, {});
    as.op(Opcode::Tex, {DR(4), R(3)});
    as.op(Opcode::Tex, {DR(5), Src(RegType::Temp, 2, XYZW, SrcMod::DZ)});
    as.op(Opcode::TexDepth, {DR(5)});
    as.op(Opcode::Mov, {DR(0), R(4)});
    r.RunOK(as.finish());

    const PSRegisters &regs = r.regs[0];
    // texld r1 samples stage 1 at t1
    CheckVec(regs.r[1], -0.5f, 0.5f, 1.0f, 0.0f);
    // _dw: (1, 2) / 4
    CheckVec(regs.r[2], 0.25f, 0.5f, 2.0f, 0.0f);
    // texcrd's (0.1, 0.2) + 2 * (-0.5, 0.5)
    CheckVec(regs.r[3], -0.9f, 1.2f, 0.0f, 0.0f);
    CheckVec(regs.r[4], -0.9f, 1.2f, 4.0f, 0.0f);
    // _dz: (0.25, 0.5) / 2
    CheckVec(regs.r[5], 0.125f, 0.25f, 5.0f, 0.0f);
    // texdepth: r5.x / r5.y
    CHECK(fabsf(r.out[0].depth[0] - 0.5f) < 1e-6f);
    CheckVec(r.out[0].color[0], 0.0f, 1.0f, 1.0f, 0.0f);

    // each texld samples the stage its destination names
    REQUIRE(r.stub.samples.size() == 16);
    CHECK(r.stub.samples[0].sampler == 1);
    CHECK(r.stub.samples[4].sampler == 2);
    CHECK(r.stub.samples[8].sampler == 4);
    CHECK(r.stub.samples[12].sampler == 5);

    // texkill r# tests the register
    Assembler kill(1, 4, true);
    kill.op(Opcode::TexCoord, {DR(0, 0x7), T(1)});
    kill.op(Opcode::TexKill, {DR(0)});
    r.RunOK(kill.finish());
    CHECK(r.killed[0]);
  };
}

TEST_CASE("D3D9 pixel shader emulation - what it won't run", "[d3d9][psemu]")
{
  PSRunner r;

  Assembler vs(3, 0);
  vs.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 0), C(0)});
  CHECK_FALSE(r.Run(vs.finish()));
  CHECK(r.error == "Not a pixel shader");

  Assembler ps10(1, 0, true);
  ps10.op(Opcode::Mov, {Dst(RegType::Temp, 0), C(0)});
  CHECK_FALSE(r.Run(ps10.finish()));
  CHECK(r.error == "Unsupported shader model ps_1_0");

  // ps_1_x's texture instructions aren't in later models
  Assembler bem(2, 0, true);
  bem.op(Opcode::TexBem, {Dst(RegType::AddrOrTexture, 1), T(0)});
  CHECK_FALSE(r.Run(bem.finish()));
  CHECK(r.error == "Unsupported instruction texbem at instruction 0 (texbem)");

  // 255^3 iterations, too many to check each step of
  r.checkStepping = false;
  Assembler loops(3, 0, true);
  loops.defi(0, 255, 0, 0, 0);
  for(int i = 0; i < 3; i++)
    loops.op(Opcode::Rep, {Src(RegType::ConstInt, 0)});
  loops.op(Opcode::Add, {DR(0), R(0), C(0)});
  for(int i = 0; i < 3; i++)
    loops.op(Opcode::EndRep, {});
  CHECK_FALSE(r.Run(loops.finish()));
  CHECK(r.error.beginsWith("Too many instructions executed"));
}

#include "d3d9_ps_test_data.h"

namespace
{
// samples d3d9_ps_test_data.h's texture as the GPU did: one level, point sampled, wrapped
struct DataSampler : PSCallbacks
{
  VSTexture tex;
  DataSampler()
  {
    VSTexture::Level level;
    level.width = level.height = 16;
    for(uint32_t y = 0; y < 16; y++)
    {
      for(uint32_t x = 0; x < 16; x++)
      {
        Vec4 t;
        PSTestData::Texel(x, y, t.v);
        level.texels.push_back(t);
      }
    }
    tex.levels.push_back(level);
  }
  Vec4 Sample(const PSSample &s) override
  {
    Vec4 c = s.coord;
    if(s.kind == SampleKind::Projected)
      for(int i = 0; i < 3; i++)
        c[i] /= c[3];
    return tex.SampleLevel(c, 0.0f);
  }
};

struct CaseResult
{
  uint32_t pixels = 0, discarded = 0, values = 0, exact = 0;
  float maxAbs = 0.0f, maxRel = 0.0f;
  rdcarray<rdcstr> failures;
  // the values that weren't exact, within the tolerance
  rdcarray<rdcstr> inexact;
};

// how far the interpreter may be from the GPU: the transcendentals differ in the last bits, and
// the driver fuses some separate multiplies and adds, which compounds through a shader
const float AbsTolerance = 1e-5f, RelTolerance = 1e-4f;

// compares each pixel of a quad with what the GPU wrote to its targets ([pixel][target][4])
void CompareQuad(const PSQuad &quad, const uint32_t idx[QuadPixels], const uint32_t *outputs,
                 uint32_t numTargets, uint32_t discarded, CaseResult &res)
{
  for(uint32_t p = 0; p < QuadPixels; p++)
  {
    const uint32_t *gpu = outputs + idx[p] * numTargets * 4;
    bool gpuDiscarded = true;
    for(uint32_t i = 0; i < numTargets * 4; i++)
      if(gpu[i] != discarded)
        gpuDiscarded = false;

    const PSRegisters &regs = quad.Pixel(p).Registers();
    res.pixels++;
    if(regs.killed != gpuDiscarded)
    {
      res.failures.push_back(StringFormat::Fmt("pixel %u: %s, the GPU %s", idx[p],
                                               regs.killed ? "discarded" : "not discarded",
                                               gpuDiscarded ? "discarded it" : "didn't"));
      continue;
    }
    if(gpuDiscarded)
    {
      res.discarded++;
      continue;
    }

    for(uint32_t t = 0; t < numTargets; t++)
    {
      if(!(regs.out.written & (1U << t)))
        res.failures.push_back(StringFormat::Fmt("pixel %u: oC%u not written", idx[p], t));
      for(uint32_t c = 0; c < 4; c++)
      {
        const float want = FromBits(gpu[t * 4 + c]), got = regs.out.color[t][c];
        res.values++;
        if(Same(got, want))
        {
          res.exact++;
          continue;
        }
        const float diff = fabsf(got - want);
        res.maxAbs = RDCMAX(res.maxAbs, diff);
        if(want != 0.0f)
          res.maxRel = RDCMAX(res.maxRel, diff / fabsf(want));
        rdcstr desc = StringFormat::Fmt("pixel %u oC%u.%c: %.9g, the GPU %.9g", idx[p], t,
                                        "xyzw"[c], got, want);
        if(!(diff <= AbsTolerance || diff <= RelTolerance * fabsf(want)))
          res.failures.push_back(desc);
        else
          res.inexact.push_back(desc);
      }
    }
  }
}

CaseResult RunCase(const PSTestData::Case &tc)
{
  using namespace PSTestData;
  CaseResult res;

  Program prog;
  ParseProgram(tc.code, tc.codeDWORDs, prog);
  PSEmulator emu;
  if(!emu.Init(prog))
  {
    res.failures.push_back("Init: " + emu.GetError());
    return res;
  }

  VSConstants consts;
  for(uint32_t i = 0; i < 8; i++)
    consts.floats.push_back(
        Vec4(FloatConsts[i][0], FloatConsts[i][1], FloatConsts[i][2], FloatConsts[i][3]));
  IVec4 i0;
  memcpy(i0.v, IntConsts, sizeof(i0.v));
  consts.ints.push_back(i0);
  consts.bools.push_back(BoolConsts[0]);
  consts.bools.push_back(BoolConsts[1]);
  consts = emu.ApplyLiterals(consts);

  // ps_3_0's input registers, from their declarations: the slot each reads in the data, TEXCOORD0-3
  // then COLOR0-1
  const bool sm3 = prog.version.major == 3;
  rdcarray<rdcpair<uint32_t, uint32_t>> sm3Inputs;
  for(const Instruction &inst : prog.instructions)
  {
    if(inst.op != Opcode::Dcl || inst.dst.type != RegType::Input)
      continue;
    if(inst.usage == DeclUsage::TexCoord && inst.usageIndex < 4)
      sm3Inputs.push_back({inst.dst.index, inst.usageIndex});
    else if(inst.usage == DeclUsage::Color && inst.usageIndex < 2)
      sm3Inputs.push_back({inst.dst.index, 4 + inst.usageIndex});
  }

  DataSampler sampler;

  for(uint32_t qy = 0; qy < Height; qy += 2)
  {
    for(uint32_t qx = 0; qx < Width; qx += 2)
    {
      PSInputs in[QuadPixels];
      uint32_t idx[QuadPixels];
      for(uint32_t p = 0; p < QuadPixels; p++)
      {
        const uint32_t x = qx + (p & 1), y = qy + (p >> 1);
        idx[p] = y * Width + x;
        Vec4 slots[6];
        memcpy(slots, tc.inputs + idx[p] * 24, sizeof(slots));
        if(sm3)
        {
          for(const rdcpair<uint32_t, uint32_t> &m : sm3Inputs)
            if(m.first < MaxPSInputs)
              in[p].v[m.first] = slots[m.second];
          in[p].pos = Vec4(float(x), float(y), 0.0f, 0.0f);
          in[p].face = FromBits(tc.face[idx[p]]);
        }
        else
        {
          for(int t = 0; t < 4; t++)
            in[p].t[t] = slots[t];
          in[p].v[0] = slots[4];
          in[p].v[1] = slots[5];
        }
      }

      PSQuad quad;
      quad.Start(emu, in, consts, &sampler);
      if(!quad.Run())
      {
        for(uint32_t p = 0; p < QuadPixels; p++)
          if(quad.Pixel(p).Failed())
            res.failures.push_back(quad.Pixel(p).GetError());
        continue;
      }

      CompareQuad(quad, idx, tc.outputs, tc.numTargets, Discarded, res);
    }
  }

  return res;
}
};    // namespace

TEST_CASE("D3D9 pixel shader emulation - compiled shaders against the GPU", "[d3d9][psemu]")
{
  // shaders compiled by D3DCompile and run natively; see d3d9_ps_test_data.h
  for(const PSTestData::Case &tc : PSTestData::Cases)
  {
    CaseResult res = RunCase(tc);
    INFO(tc.name);
    for(const rdcstr &f : res.failures)
    {
      INFO(f.c_str());
      CHECK(false);
    }
    CHECK(res.pixels == PSTestData::Width * PSTestData::Height);
  }
}

#include "d3d9_ps1_test_data.h"

namespace
{
// samples d3d9_ps1_test_data.h's textures as the GPU did: a texture on each stage, one level,
// point sampled, wrapped. Counts the lookups within 1/64 of a texel of an edge, where the GPU's
// rounding of the coordinate could pick the neighbouring texel.
struct StageSampler : PSCallbacks
{
  VSTexture tex[PS1TestData::NumStages];
  uint32_t lookups = 0, nearEdges = 0;
  StageSampler()
  {
    for(uint32_t s = 0; s < PS1TestData::NumStages; s++)
    {
      VSTexture::Level level;
      level.width = level.height = PS1TestData::TexSize;
      for(uint32_t y = 0; y < PS1TestData::TexSize; y++)
      {
        for(uint32_t x = 0; x < PS1TestData::TexSize; x++)
        {
          Vec4 t;
          PS1TestData::Texel(s, x, y, t.v);
          level.texels.push_back(t);
        }
      }
      tex[s].levels.push_back(level);
    }
  }
  Vec4 Sample(const PSSample &s) override
  {
    if(s.sampler >= PS1TestData::NumStages)
      return Vec4();
    lookups++;
    for(int i = 0; i < 2; i++)
    {
      const float f = s.coord[i] * float(PS1TestData::TexSize), frac = f - floorf(f);
      if(frac != 0.0f && (frac < 1.0f / 64.0f || frac > 63.0f / 64.0f))
        nearEdges++;
    }
    return tex[s.sampler].SampleLevel(s.coord, 0.0f);
  }
};

CaseResult RunCase1(const PS1TestData::Case &tc, uint32_t &lookups, uint32_t &nearEdges)
{
  using namespace PS1TestData;
  CaseResult res;

  Program prog;
  ParseProgram(tc.code, tc.codeDWORDs, prog);
  PSEmulator emu;
  if(!emu.Init(prog))
  {
    res.failures.push_back("Init: " + emu.GetError());
    return res;
  }

  VSConstants consts;
  for(uint32_t i = 0; i < 8; i++)
    consts.floats.push_back(
        Vec4(FloatConsts[i][0], FloatConsts[i][1], FloatConsts[i][2], FloatConsts[i][3]));
  consts = emu.ApplyLiterals(consts);

  PSStageState stages[NumTextureStages];
  for(uint32_t s = 0; s < NumStages; s++)
  {
    memcpy(stages[s].bumpEnvMat, BumpEnv[s], sizeof(stages[s].bumpEnvMat));
    stages[s].lumScale = BumpEnv[s][4];
    stages[s].lumOffset = BumpEnv[s][5];
  }
  // D3DTTFF_PROJECTED
  if(tc.projFlags & 0x100)
    stages[tc.projStage].projected = true;

  StageSampler sampler;

  for(uint32_t qy = 0; qy < Height; qy += 2)
  {
    for(uint32_t qx = 0; qx < Width; qx += 2)
    {
      PSInputs in[QuadPixels];
      uint32_t idx[QuadPixels];
      for(uint32_t p = 0; p < QuadPixels; p++)
      {
        const uint32_t x = qx + (p & 1), y = qy + (p >> 1);
        idx[p] = y * Width + x;
        // TEXCOORD0-5, COLOR0-1
        Vec4 slots[8];
        memcpy(slots, Inputs + idx[p] * 32, sizeof(slots));
        for(int t = 0; t < 6; t++)
          in[p].t[t] = slots[t];
        in[p].v[0] = slots[6];
        in[p].v[1] = slots[7];
      }

      PSQuad quad;
      quad.Start(emu, in, consts, &sampler, stages);
      if(!quad.Run())
      {
        for(uint32_t p = 0; p < QuadPixels; p++)
          if(quad.Pixel(p).Failed())
            res.failures.push_back(quad.Pixel(p).GetError());
        continue;
      }

      CompareQuad(quad, idx, tc.outputs, 1, Discarded, res);
    }
  }

  lookups = sampler.lookups;
  nearEdges = sampler.nearEdges;
  return res;
}
};    // namespace

TEST_CASE("D3D9 pixel shader emulation - ps_1_x against the GPU", "[d3d9][psemu]")
{
  // shaders assembled by D3DAssemble and run natively; see d3d9_ps1_test_data.h
  for(const PS1TestData::Case &tc : PS1TestData::Cases)
  {
    uint32_t lookups = 0, nearEdges = 0;
    CaseResult res = RunCase1(tc, lookups, nearEdges);
    INFO(tc.name);
    for(const rdcstr &f : res.failures)
    {
      INFO(f.c_str());
      CHECK(false);
    }
    CHECK(res.pixels == PS1TestData::Width * PS1TestData::Height);
  }
}

// prints how closely each case matches the GPU, for the report
TEST_CASE("D3D9 pixel shader emulation - compiled shaders, statistics", "[.][d3d9psstats]")
{
  auto print = [](const char *name, const CaseResult &res) {
    printf(
        "%-24s %2u pixels, %2u discarded, %3u of %3u values exact, max error %.3g (relative "
        "%.3g), %zu failures\n",
        name, res.pixels, res.discarded, res.exact, res.values, res.maxAbs, res.maxRel,
        res.failures.size());
    for(const rdcstr &f : res.failures)
      printf("    failed: %s\n", f.c_str());
    for(const rdcstr &f : res.inexact)
      printf("    inexact: %s\n", f.c_str());
  };
  for(const PSTestData::Case &tc : PSTestData::Cases)
    print(tc.name, RunCase(tc));
  for(const PS1TestData::Case &tc : PS1TestData::Cases)
  {
    uint32_t lookups = 0, nearEdges = 0;
    CaseResult res = RunCase1(tc, lookups, nearEdges);
    print(tc.name, res);
    if(lookups)
      printf("    %u lookups, %u within 1/64 of a texel of an edge\n", lookups, nearEdges);
  }
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
