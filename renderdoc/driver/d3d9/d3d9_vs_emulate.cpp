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

#include "d3d9_vs_emulate.h"
#include <float.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include "common/common.h"
#include "common/formatting.h"
#include "maths/half_convert.h"
#include "d3d9_ffp_vertex.h"

// How the interpreter works
// -------------------------
// Init walks the program once: it matches each if/else/endif, loop/endloop, rep/endrep and break
// with its partner, finds each label, and collects the def/defi/defb literals. Execute then runs
// the instructions in order with an instruction pointer, a stack of loops (each with its
// remaining count and, for loop, its aL) and a stack of calls. Registers are fp32 throughout,
// partial precision hints are ignored, as the reference rasterizer does.
//
// Where the D3D9 documentation gives an instruction's exact behaviour it's followed: rcp and rsq
// of 0 give +inf (and of 1 exactly 1), rsq and log take the absolute value, log of 0 gives
// -FLT_MAX, vs_1_1's expp and logp give their four documented components with z cut to partial
// precision (the low 8 mantissa bits cleared, as in the documentation's pseudo code; vs_2_0's
// partial precision expp and logp are cut the same way), lit clamps its power to +-127.9961, nrm of
// a zero vector gives zero. mova rounds to the nearest integer, and vs_1_1's mov to a0 rounds down.
// Scalar instructions read the w of the swizzled source: SM2 and later require a replicate
// swizzle there, and vs_1_1 uses w when no component is given. Constants read out of range
// (relative addressing past the end) are 0.

using namespace D3D9BC;

namespace D3D9Emu
{
namespace
{
const uint32_t NoMatch = ~0U;
// a vertex running more instructions than this is assumed to loop forever
const uint32_t MaxInstructionsPerVertex = 1U << 20;
// SM3 allows four nested calls, plus some slack
const uint32_t MaxCallDepth = 16;

float Saturate(float f)
{
  // NaN saturates to 0
  if(!(f > 0.0f))
    return 0.0f;
  return f > 1.0f ? 1.0f : f;
}

bool Compare(Comparison c, float a, float b)
{
  switch(c)
  {
    case Comparison::GT: return a > b;
    case Comparison::EQ: return a == b;
    case Comparison::GE: return a >= b;
    case Comparison::LT: return a < b;
    case Comparison::NE: return a != b;
    case Comparison::LE: return a <= b;
    default: return false;
  }
}

float ApplyMod(SrcMod mod, float f)
{
  switch(mod)
  {
    case SrcMod::None: return f;
    case SrcMod::Neg: return -f;
    case SrcMod::Bias: return f - 0.5f;
    case SrcMod::BiasNeg: return -(f - 0.5f);
    case SrcMod::Sign: return 2.0f * f - 1.0f;
    case SrcMod::SignNeg: return -(2.0f * f - 1.0f);
    case SrcMod::Comp: return 1.0f - f;
    case SrcMod::X2: return 2.0f * f;
    case SrcMod::X2Neg: return -2.0f * f;
    case SrcMod::Abs: return fabsf(f);
    case SrcMod::AbsNeg: return -fabsf(f);
    case SrcMod::Not: return f != 0.0f ? 0.0f : 1.0f;
    // _dz and _dw only apply to ps_1_4's texld and texcrd
    default: return f;
  }
}

// clears the low 8 bits of the mantissa, as the documentation's partial precision pseudo code does
float PartialPrecision(float f)
{
  uint32_t bits;
  memcpy(&bits, &f, sizeof(bits));
  bits &= 0xFFFFFF00;
  memcpy(&f, &bits, sizeof(bits));
  return f;
}

uint32_t ConstantRegisterOffset(RegType type)
{
  switch(type)
  {
    case RegType::Const2: return 2048;
    case RegType::Const3: return 4096;
    case RegType::Const4: return 6144;
    default: return 0;
  }
}

struct LoopFrame
{
  uint32_t start = 0;
  int32_t remaining = 0;
  int32_t aL = 0;
  int32_t step = 0;
  // rep doesn't have an aL
  bool isLoop = false;
};

struct CallFrame
{
  uint32_t ret = 0;
  size_t loopDepth = 0;
};

struct ExecState
{
  Vec4 r[MaxVSTemps];
  IVec4 a0;
  bool p[4] = {false, false, false, false};
  rdcarray<LoopFrame> loops;
  rdcarray<CallFrame> calls;

  const Vec4 *v = NULL;
  const VSConstants *c = NULL;
  VSOutputs *o = NULL;

  int32_t aL() const
  {
    for(size_t i = loops.size(); i > 0; i--)
      if(loops[i - 1].isLoop)
        return loops[i - 1].aL;
    return 0;
  }

  int32_t RelValue(const RelativeAddress &rel) const
  {
    if(!rel.present)
      return 0;
    if(rel.type == RegType::Loop)
      return aL();
    return a0.v[rel.component & 3];
  }

  Vec4 ReadRaw(const SrcParam &src, int32_t regOffset) const
  {
    int64_t idx = int64_t(src.index) + regOffset + RelValue(src.rel);
    Vec4 ret;

    switch(src.type)
    {
      case RegType::Temp:
        if(idx >= 0 && idx < MaxVSTemps)
          ret = r[idx];
        break;
      case RegType::Input:
        if(idx >= 0 && idx < MaxVSInputs)
          ret = v[idx];
        break;
      case RegType::Const:
      case RegType::Const2:
      case RegType::Const3:
      case RegType::Const4:
        idx += ConstantRegisterOffset(src.type);
        if(idx >= 0 && idx < (int64_t)c->floats.size())
          ret = c->floats[(size_t)idx];
        break;
      case RegType::AddrOrTexture:
        for(int i = 0; i < 4; i++)
          ret[i] = float(a0.v[i]);
        break;
      case RegType::ConstInt:
        if(idx >= 0 && idx < (int64_t)c->ints.size())
          for(int i = 0; i < 4; i++)
            ret[i] = float(c->ints[(size_t)idx].v[i]);
        break;
      case RegType::ConstBool:
      {
        float b = (idx >= 0 && idx < (int64_t)c->bools.size() && c->bools[(size_t)idx]) ? 1.0f : 0.0f;
        ret = Vec4(b, b, b, b);
        break;
      }
      case RegType::Loop:
      {
        float l = float(aL());
        ret = Vec4(l, l, l, l);
        break;
      }
      case RegType::Predicate:
        for(int i = 0; i < 4; i++)
          ret[i] = p[i] ? 1.0f : 0.0f;
        break;
      default: break;
    }

    return ret;
  }

  Vec4 Read(const SrcParam &src, int32_t regOffset = 0) const
  {
    Vec4 raw = ReadRaw(src, regOffset);
    Vec4 ret;
    for(int i = 0; i < 4; i++)
      ret[i] = ApplyMod(src.mod, raw[src.swizzle[i] & 3]);
    return ret;
  }

  // a boolean condition: a boolean constant or a predicate component, with ! applied
  bool ReadBool(const SrcParam &src) const
  {
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

  IVec4 ReadInt(const SrcParam &src) const
  {
    IVec4 ret;
    if(src.type == RegType::ConstInt && src.index < c->ints.size())
      ret = c->ints[src.index];
    return ret;
  }

  uint8_t WriteMask(const Instruction &inst) const
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

  void Write(const Instruction &inst, Vec4 val)
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
    switch(d.type)
    {
      case RegType::Temp:
        if(idx >= 0 && idx < MaxVSTemps)
          dst = &r[idx];
        break;
      case RegType::RastOut:
        if(idx >= 0 && idx < 3)
          dst = &o->rast[idx];
        break;
      case RegType::AttrOut:
        if(idx >= 0 && idx < 2)
          dst = &o->attr[idx];
        break;
      case RegType::TexCrdOutOrOutput:
        if(idx >= 0 && idx < 12)
          dst = &o->tex[idx];
        break;
      case RegType::AddrOrTexture:
      {
        // mova rounds to the nearest integer, vs_1_1's mov rounds down
        const bool round = inst.op == Opcode::MovA;
        for(int i = 0; i < 4; i++)
        {
          if(!(mask & (1 << i)))
            continue;
          float f = round ? floorf(val[i] + 0.5f) : floorf(val[i]);
          if(!(f == f))
            f = 0.0f;
          f = RDCCLAMP(f, -65536.0f, 65536.0f);
          a0.v[i] = int32_t(f);
        }
        return;
      }
      case RegType::Predicate:
        for(int i = 0; i < 4; i++)
          if(mask & (1 << i))
            p[i] = val[i] != 0.0f;
        return;
      default: break;
    }

    if(dst)
    {
      for(int i = 0; i < 4; i++)
        if(mask & (1 << i))
          (*dst)[i] = val[i];
    }
  }
};

float Dot(const Vec4 &a, const Vec4 &b, int n)
{
  float ret = 0.0f;
  for(int i = 0; i < n; i++)
    ret += a[i] * b[i];
  return ret;
}

Vec4 Splat(float f)
{
  return Vec4(f, f, f, f);
}

int32_t AddressTexel(float u, uint32_t size, TexAddress mode)
{
  if(!(u == u))
    u = 0.0f;

  switch(mode)
  {
    case TexAddress::Wrap: u = u - floorf(u); break;
    case TexAddress::Mirror:
    {
      float t = u - 2.0f * floorf(u * 0.5f);
      u = t > 1.0f ? 2.0f - t : t;
      break;
    }
    case TexAddress::MirrorOnce: u = RDCMIN(fabsf(u), 1.0f); break;
    case TexAddress::Border:
      if(u < 0.0f || u > 1.0f)
        return -1;
      break;
    case TexAddress::Clamp:
    default: u = RDCCLAMP(u, 0.0f, 1.0f); break;
  }

  float f = floorf(u * float(size));
  if(f < 0.0f)
    return 0;
  if(f >= float(size))
    return int32_t(size) - 1;
  return int32_t(f);
}
};    // namespace

Vec4 VSTexture::SampleLevel(const Vec4 &coord, float lod) const
{
  if(levels.empty())
    return Vec4(0.0f, 0.0f, 0.0f, 1.0f);

  lod += mipLODBias;
  if(!(lod == lod))
    lod = 0.0f;
  lod = RDCCLAMP(lod, -1000.0f, 1000.0f);

  int32_t level = int32_t(floorf(lod + 0.5f));
  level = RDCMAX(level, int32_t(maxMipLevel));
  level = RDCCLAMP(level, 0, int32_t(levels.size()) - 1);

  const Level &l = levels[level];
  if(l.width == 0 || l.height == 0 || l.depth == 0)
    return Vec4(0.0f, 0.0f, 0.0f, 1.0f);

  int32_t x = 0, y = 0, z = 0;

  if(type == SamplerTextureType::Cube)
  {
    // the major axis picks the face (+X, -X, +Y, -Y, +Z, -Z), as D3D9 numbers them
    const float cx = coord[0], cy = coord[1], cz = coord[2];
    const float ax = fabsf(cx), ay = fabsf(cy), az = fabsf(cz);
    float sc, tc, ma;
    if(ax >= ay && ax >= az)
    {
      z = cx >= 0.0f ? 0 : 1;
      sc = cx >= 0.0f ? -cz : cz;
      tc = -cy;
      ma = ax;
    }
    else if(ay >= az)
    {
      z = cy >= 0.0f ? 2 : 3;
      sc = cx;
      tc = cy >= 0.0f ? cz : -cz;
      ma = ay;
    }
    else
    {
      z = cz >= 0.0f ? 4 : 5;
      sc = cz >= 0.0f ? cx : -cx;
      tc = -cy;
      ma = az;
    }
    if(ma == 0.0f)
      ma = 1.0f;
    x = AddressTexel((sc / ma + 1.0f) * 0.5f, l.width, TexAddress::Clamp);
    y = AddressTexel((tc / ma + 1.0f) * 0.5f, l.height, TexAddress::Clamp);
    z = RDCMIN(z, int32_t(l.depth) - 1);
  }
  else
  {
    x = AddressTexel(coord[0], l.width, address[0]);
    if(type != SamplerTextureType::Tex1D)
      y = AddressTexel(coord[1], l.height, address[1]);
    if(type == SamplerTextureType::Volume)
      z = AddressTexel(coord[2], l.depth, address[2]);
  }

  if(x < 0 || y < 0 || z < 0)
    return border;

  size_t idx = (size_t(z) * l.height + size_t(y)) * l.width + size_t(x);
  if(idx >= l.texels.size())
    return Vec4(0.0f, 0.0f, 0.0f, 1.0f);
  return l.texels[idx];
}

const Vec4 *OutputRegister(const VSOutputs &out, const Version &version, const SigParameter &sig)
{
  const uint32_t idx = sig.regIndex;

  if(version.major >= 3)
    return idx < 12 ? &out.tex[idx] : NULL;

  if(sig.semanticName == "POSITION")
    return &out.rast[0];
  if(sig.semanticName == "FOG")
    return &out.rast[1];
  if(sig.semanticName == "PSIZE")
    return &out.rast[2];
  if(sig.semanticName == "COLOR")
    return idx < 2 ? &out.attr[idx] : NULL;
  if(sig.semanticName == "TEXCOORD")
    return idx < 12 ? &out.tex[idx] : NULL;
  return NULL;
}

bool VSEmulator::Init(const Program &prog)
{
  m_Prog = &prog;
  m_Version = prog.version;
  m_Error.clear();
  m_Match.clear();
  m_Labels.clear();
  m_FloatLiterals.clear();
  m_IntLiterals.clear();
  m_BoolLiterals.clear();

  if(!prog.validVersion || !prog.version.IsVS())
  {
    m_Error = "Not a vertex shader";
    return false;
  }

  if(prog.version.major < 1 || prog.version.major > 3)
  {
    m_Error = StringFormat::Fmt("Unsupported shader model %s", VersionString(prog.version).c_str());
    return false;
  }

  if(!prog.error.empty())
  {
    m_Error = "The shader couldn't be fully parsed: " + prog.error;
    return false;
  }

  const rdcarray<Instruction> &insts = prog.instructions;
  m_Match.fill(insts.size(), NoMatch);

  rdcarray<uint32_t> stack;

  for(uint32_t i = 0; i < insts.size(); i++)
  {
    const Instruction &inst = insts[i];

    switch(inst.op)
    {
      case Opcode::If:
      case Opcode::IfC:
      case Opcode::Loop:
      case Opcode::Rep: stack.push_back(i); break;
      case Opcode::Else:
        if(stack.empty() ||
           (insts[stack.back()].op != Opcode::If && insts[stack.back()].op != Opcode::IfC))
        {
          m_Error = StringFormat::Fmt("else without if at instruction %u", i);
          return false;
        }
        m_Match[stack.back()] = i;
        stack.back() = i;
        break;
      case Opcode::EndIf:
        if(stack.empty() ||
           (insts[stack.back()].op != Opcode::If && insts[stack.back()].op != Opcode::IfC &&
            insts[stack.back()].op != Opcode::Else))
        {
          m_Error = StringFormat::Fmt("endif without if at instruction %u", i);
          return false;
        }
        m_Match[stack.back()] = i;
        stack.pop_back();
        break;
      case Opcode::EndLoop:
      case Opcode::EndRep:
      {
        const Opcode start = inst.op == Opcode::EndLoop ? Opcode::Loop : Opcode::Rep;
        if(stack.empty() || insts[stack.back()].op != start)
        {
          m_Error = StringFormat::Fmt("%s without %s at instruction %u",
                                      inst.op == Opcode::EndLoop ? "endloop" : "endrep",
                                      inst.op == Opcode::EndLoop ? "loop" : "rep", i);
          return false;
        }
        m_Match[stack.back()] = i;
        m_Match[i] = stack.back();
        stack.pop_back();

        // the breaks inside break to here
        for(uint32_t b = m_Match[i] + 1; b < i; b++)
        {
          const Opcode op = insts[b].op;
          if((op == Opcode::Break || op == Opcode::BreakC || op == Opcode::BreakP) &&
             m_Match[b] == NoMatch)
            m_Match[b] = i;
        }
        break;
      }
      case Opcode::Label:
        if(!inst.srcs.empty())
        {
          uint32_t l = inst.srcs[0].index;
          if(l >= 2048)
          {
            m_Error = StringFormat::Fmt("label l%u out of range", l);
            return false;
          }
          while(m_Labels.size() <= l)
            m_Labels.push_back(NoMatch);
          m_Labels[l] = i;
        }
        // flow control doesn't continue across subroutines
        if(!stack.empty())
        {
          m_Error = StringFormat::Fmt("label inside flow control at instruction %u", i);
          return false;
        }
        break;
      case Opcode::Def:
      {
        Literal lit;
        lit.reg = inst.dst.index + ConstantRegisterOffset(inst.dst.type);
        memcpy(lit.value, inst.literal, sizeof(lit.value));
        m_FloatLiterals.push_back(lit);
        break;
      }
      case Opcode::DefI:
      {
        Literal lit;
        lit.reg = inst.dst.index;
        memcpy(lit.value, inst.literal, sizeof(lit.value));
        m_IntLiterals.push_back(lit);
        break;
      }
      case Opcode::DefB:
      {
        Literal lit = {};
        lit.reg = inst.dst.index;
        lit.value[0] = inst.literal[0];
        m_BoolLiterals.push_back(lit);
        break;
      }
      default: break;
    }
  }

  if(!stack.empty())
  {
    m_Error = StringFormat::Fmt("unterminated flow control starting at instruction %u", stack.back());
    return false;
  }

  for(uint32_t i = 0; i < insts.size(); i++)
  {
    const Instruction &inst = insts[i];
    const Opcode op = inst.op;
    if((op == Opcode::Break || op == Opcode::BreakC || op == Opcode::BreakP) && m_Match[i] == NoMatch)
    {
      m_Error = StringFormat::Fmt("break outside a loop at instruction %u", i);
      return false;
    }
    if(op == Opcode::Call || op == Opcode::CallNZ)
    {
      uint32_t l = inst.srcs.empty() ? NoMatch : inst.srcs[0].index;
      if(l >= m_Labels.size() || m_Labels[l] == NoMatch)
      {
        m_Error = StringFormat::Fmt("call to missing label at instruction %u", i);
        return false;
      }
    }
  }

  return true;
}

VSConstants VSEmulator::ApplyLiterals(const VSConstants &consts) const
{
  VSConstants ret = consts;

  for(const Literal &l : m_FloatLiterals)
  {
    if(ret.floats.size() <= l.reg)
      ret.floats.resize(l.reg + 1);
    memcpy(ret.floats[l.reg].v, l.value, sizeof(l.value));
  }
  for(const Literal &l : m_IntLiterals)
  {
    if(ret.ints.size() <= l.reg)
      ret.ints.resize(l.reg + 1);
    memcpy(ret.ints[l.reg].v, l.value, sizeof(l.value));
  }
  for(const Literal &l : m_BoolLiterals)
  {
    if(ret.bools.size() <= l.reg)
      ret.bools.resize(l.reg + 1);
    ret.bools[l.reg] = l.value[0] ? 1 : 0;
  }

  return ret;
}

bool VSEmulator::Execute(const Vec4 inputs[MaxVSInputs], const VSConstants &consts,
                         const VSTexture textures[NumVertexSamplers], VSOutputs &out,
                         rdcstr &error) const
{
  out = VSOutputs();

  if(m_Prog == NULL || !m_Error.empty())
  {
    error = m_Error.empty() ? "No shader" : m_Error;
    return false;
  }

  ExecState s;
  s.v = inputs;
  s.c = &consts;
  s.o = &out;

  const rdcarray<Instruction> &insts = m_Prog->instructions;
  uint32_t executed = 0;
  uint32_t ip = 0;

  auto fail = [&](const rdcstr &msg) {
    error = StringFormat::Fmt("%s at instruction %u (%s)", msg.c_str(), ip,
                              ip < insts.size() ? OpcodeName(m_Version, insts[ip]).c_str() : "");
    return false;
  };

  while(ip < insts.size())
  {
    if(++executed > MaxInstructionsPerVertex)
      return fail("Too many instructions executed, the shader may loop forever");

    const Instruction &inst = insts[ip];
    const rdcarray<SrcParam> &srcs = inst.srcs;
    uint32_t next = ip + 1;

    // sources an instruction needs, beyond which it's malformed
    auto need = [&](size_t n) { return srcs.size() >= n; };
#define NEED_SRCS(n)                 \
  if(!need(n))                       \
    return fail("Missing operands"); \
  if(!inst.hasDst)                   \
    return fail("Missing destination");

    switch(inst.op)
    {
      case Opcode::Nop:
      case Opcode::Dcl:
      case Opcode::Def:
      case Opcode::DefI:
      case Opcode::DefB:
      case Opcode::EndIf:
      case Opcode::Comment: break;

      case Opcode::End: next = (uint32_t)insts.size(); break;

      // the main program falls through into the subroutines, which ends it
      case Opcode::Label:
        if(s.calls.empty())
          next = (uint32_t)insts.size();
        break;

      case Opcode::Ret:
        if(s.calls.empty())
        {
          next = (uint32_t)insts.size();
        }
        else
        {
          CallFrame f = s.calls.back();
          s.calls.pop_back();
          s.loops.resize(f.loopDepth);
          next = f.ret;
        }
        break;

      case Opcode::Call:
      case Opcode::CallNZ:
      {
        if(srcs.empty() || (inst.op == Opcode::CallNZ && srcs.size() < 2))
          return fail("Missing operands");
        bool take = inst.op == Opcode::Call || s.ReadBool(srcs[1]);
        if(take)
        {
          if(s.calls.size() >= MaxCallDepth)
            return fail("Calls nested too deeply");
          CallFrame f;
          f.ret = ip + 1;
          f.loopDepth = s.loops.size();
          s.calls.push_back(f);
          next = m_Labels[srcs[0].index] + 1;
        }
        break;
      }

      case Opcode::If:
        if(srcs.empty())
          return fail("Missing operands");
        if(!s.ReadBool(srcs[0]))
          next = m_Match[ip] + 1;
        break;

      case Opcode::IfC:
        if(!need(2))
          return fail("Missing operands");
        if(!Compare(inst.GetComparison(), s.Read(srcs[0])[0], s.Read(srcs[1])[0]))
          next = m_Match[ip] + 1;
        break;

      // reached at the end of the if block, skip the else block
      case Opcode::Else: next = m_Match[ip] + 1; break;

      case Opcode::Loop:
      case Opcode::Rep:
      {
        const bool isLoop = inst.op == Opcode::Loop;
        if(srcs.size() < (isLoop ? 2U : 1U))
          return fail("Missing operands");
        IVec4 i = s.ReadInt(srcs[isLoop ? 1 : 0]);
        int32_t count = RDCCLAMP(i.v[0], 0, 255);
        if(count == 0)
        {
          next = m_Match[ip] + 1;
        }
        else
        {
          LoopFrame f;
          f.start = ip;
          f.remaining = count;
          f.isLoop = isLoop;
          f.aL = isLoop ? i.v[1] : 0;
          f.step = isLoop ? i.v[2] : 0;
          s.loops.push_back(f);
        }
        break;
      }

      case Opcode::EndLoop:
      case Opcode::EndRep:
      {
        if(s.loops.empty())
          return fail("End of a loop that isn't running");
        LoopFrame &f = s.loops.back();
        if(--f.remaining > 0)
        {
          f.aL += f.step;
          next = f.start + 1;
        }
        else
        {
          s.loops.pop_back();
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
          if(!need(2))
            return fail("Missing operands");
          take = Compare(inst.GetComparison(), s.Read(srcs[0])[0], s.Read(srcs[1])[0]);
        }
        else if(inst.op == Opcode::BreakP)
        {
          if(srcs.empty())
            return fail("Missing operands");
          take = s.ReadBool(srcs[0]);
        }
        if(take)
        {
          if(s.loops.empty())
            return fail("break outside a running loop");
          s.loops.pop_back();
          next = m_Match[ip] + 1;
        }
        break;
      }

      case Opcode::Mov:
      case Opcode::MovA:
      {
        NEED_SRCS(1);
        s.Write(inst, s.Read(srcs[0]));
        break;
      }

      case Opcode::Add:
      case Opcode::Sub:
      case Opcode::Mul:
      case Opcode::Min:
      case Opcode::Max:
      case Opcode::Slt:
      case Opcode::Sge:
      {
        NEED_SRCS(2);
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]), r;
        for(int i = 0; i < 4; i++)
        {
          switch(inst.op)
          {
            case Opcode::Add: r[i] = a[i] + b[i]; break;
            case Opcode::Sub: r[i] = a[i] - b[i]; break;
            case Opcode::Mul: r[i] = a[i] * b[i]; break;
            case Opcode::Min: r[i] = a[i] < b[i] ? a[i] : b[i]; break;
            case Opcode::Max: r[i] = a[i] >= b[i] ? a[i] : b[i]; break;
            case Opcode::Slt: r[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
            case Opcode::Sge: r[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
            default: break;
          }
        }
        s.Write(inst, r);
        break;
      }

      case Opcode::Mad:
      case Opcode::Lrp:
      case Opcode::Cmp:
      case Opcode::Cnd:
      {
        NEED_SRCS(3);
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]), c = s.Read(srcs[2]), r;
        for(int i = 0; i < 4; i++)
        {
          switch(inst.op)
          {
            case Opcode::Mad: r[i] = a[i] * b[i] + c[i]; break;
            case Opcode::Lrp: r[i] = a[i] * (b[i] - c[i]) + c[i]; break;
            case Opcode::Cmp: r[i] = a[i] >= 0.0f ? b[i] : c[i]; break;
            case Opcode::Cnd: r[i] = a[i] > 0.5f ? b[i] : c[i]; break;
            default: break;
          }
        }
        s.Write(inst, r);
        break;
      }

      case Opcode::Dp3:
      case Opcode::Dp4:
      {
        NEED_SRCS(2);
        s.Write(inst, Splat(Dot(s.Read(srcs[0]), s.Read(srcs[1]), inst.op == Opcode::Dp3 ? 3 : 4)));
        break;
      }

      case Opcode::Dp2Add:
      {
        NEED_SRCS(3);
        s.Write(inst, Splat(Dot(s.Read(srcs[0]), s.Read(srcs[1]), 2) + s.Read(srcs[2])[0]));
        break;
      }

      case Opcode::Abs:
      case Opcode::Frc:
      case Opcode::Sgn:
      {
        NEED_SRCS(1);
        Vec4 a = s.Read(srcs[0]), r;
        for(int i = 0; i < 4; i++)
        {
          if(inst.op == Opcode::Abs)
            r[i] = fabsf(a[i]);
          else if(inst.op == Opcode::Frc)
            r[i] = a[i] - floorf(a[i]);
          else
            r[i] = a[i] > 0.0f ? 1.0f : (a[i] < 0.0f ? -1.0f : 0.0f);
        }
        s.Write(inst, r);
        break;
      }

      case Opcode::Rcp:
      {
        NEED_SRCS(1);
        float f = s.Read(srcs[0])[3];
        float r = f == 1.0f ? 1.0f : (f == 0.0f ? INFINITY : 1.0f / f);
        s.Write(inst, Splat(r));
        break;
      }

      case Opcode::Rsq:
      {
        NEED_SRCS(1);
        float f = fabsf(s.Read(srcs[0])[3]);
        float r = f == 1.0f ? 1.0f : (f == 0.0f ? INFINITY : 1.0f / sqrtf(f));
        s.Write(inst, Splat(r));
        break;
      }

      case Opcode::Exp:
      {
        NEED_SRCS(1);
        s.Write(inst, Splat(exp2f(s.Read(srcs[0])[3])));
        break;
      }

      case Opcode::Log:
      {
        NEED_SRCS(1);
        float f = fabsf(s.Read(srcs[0])[3]);
        s.Write(inst, Splat(f != 0.0f ? log2f(f) : -FLT_MAX));
        break;
      }

      case Opcode::ExpP:
      {
        NEED_SRCS(1);
        float w = s.Read(srcs[0])[3];
        if(m_Version.major < 2)
        {
          float fl = floorf(w);
          s.Write(inst, Vec4(exp2f(fl), w - fl, PartialPrecision(exp2f(w)), 1.0f));
        }
        else
        {
          s.Write(inst, Splat(PartialPrecision(exp2f(w))));
        }
        break;
      }

      case Opcode::LogP:
      {
        NEED_SRCS(1);
        float f = fabsf(s.Read(srcs[0])[3]);
        if(m_Version.major < 2)
        {
          if(f != 0.0f)
          {
            uint32_t bits;
            memcpy(&bits, &f, sizeof(bits));
            float exponent = float(int32_t((bits >> 23) & 0xFF) - 127);
            uint32_t mbits = (bits & 0x7FFFFF) | 0x3F800000;
            float mantissa;
            memcpy(&mantissa, &mbits, sizeof(mantissa));
            s.Write(inst, Vec4(exponent, mantissa, PartialPrecision(log2f(f)), 1.0f));
          }
          else
          {
            s.Write(inst, Vec4(-FLT_MAX, 1.0f, -FLT_MAX, 1.0f));
          }
        }
        else
        {
          s.Write(inst, Splat(f != 0.0f ? PartialPrecision(log2f(f)) : -FLT_MAX));
        }
        break;
      }

      case Opcode::Pow:
      {
        NEED_SRCS(2);
        s.Write(inst, Splat(powf(fabsf(s.Read(srcs[0])[3]), s.Read(srcs[1])[3])));
        break;
      }

      case Opcode::Lit:
      {
        NEED_SRCS(1);
        Vec4 a = s.Read(srcs[0]);
        const float MaxPower = 127.9961f;
        float power = RDCCLAMP(a[3], -MaxPower, MaxPower);
        Vec4 r(1.0f, 0.0f, 0.0f, 1.0f);
        if(a[0] > 0.0f)
        {
          r[1] = a[0];
          if(a[1] > 0.0f)
            r[2] = powf(a[1], power);
        }
        s.Write(inst, r);
        break;
      }

      case Opcode::Dst:
      {
        NEED_SRCS(2);
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]);
        s.Write(inst, Vec4(1.0f, a[1] * b[1], a[2], b[3]));
        break;
      }

      case Opcode::Crs:
      {
        NEED_SRCS(2);
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]);
        s.Write(inst, Vec4(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                           a[0] * b[1] - a[1] * b[0], 0.0f));
        break;
      }

      case Opcode::Nrm:
      {
        NEED_SRCS(1);
        Vec4 a = s.Read(srcs[0]);
        float f = Dot(a, a, 3);
        f = f != 0.0f ? 1.0f / sqrtf(f) : FLT_MAX;
        // FLT_MAX * 0 is still 0
        Vec4 r;
        for(int i = 0; i < 4; i++)
          r[i] = a[i] == 0.0f ? 0.0f : a[i] * f;
        s.Write(inst, r);
        break;
      }

      case Opcode::SinCos:
      {
        NEED_SRCS(1);
        // vs_2_0's two extra constants only help hardware approximate it
        float a = s.Read(srcs[0])[3];
        s.Write(inst, Vec4(cosf(a), sinf(a), 0.0f, 0.0f));
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
        Vec4 a = s.Read(srcs[0]), r;
        for(int i = 0; i < rows; i++)
          r[i] = Dot(a, s.Read(srcs[1], i), size);
        s.Write(inst, r);
        break;
      }

      case Opcode::SetP:
      {
        NEED_SRCS(2);
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]), r;
        for(int i = 0; i < 4; i++)
          r[i] = Compare(inst.GetComparison(), a[i], b[i]) ? 1.0f : 0.0f;
        s.Write(inst, r);
        break;
      }

      case Opcode::TexLdl:
      {
        NEED_SRCS(2);
        if(srcs[1].type != RegType::Sampler)
          return fail("texldl without a sampler");
        Vec4 coord = s.Read(srcs[0]);
        Vec4 r(0.0f, 0.0f, 0.0f, 1.0f);
        if(srcs[1].index < NumVertexSamplers && textures)
          r = textures[srcs[1].index].SampleLevel(coord, coord[3]);
        // the sampler's swizzle applies to the result
        Vec4 sw;
        for(int i = 0; i < 4; i++)
          sw[i] = r[srcs[1].swizzle[i] & 3];
        s.Write(inst, sw);
        break;
      }

      default:
        return fail(
            StringFormat::Fmt("Unsupported instruction %s", OpcodeName(m_Version, inst).c_str()));
    }

#undef NEED_SRCS

    ip = next;
  }

  return true;
}

//////////////////////////////////////////////////////////////////////////
// reading vertices

uint32_t DeclTypeSize(DeclType type)
{
  switch(type)
  {
    case DeclType::Float1: return 4;
    case DeclType::Float2: return 8;
    case DeclType::Float3: return 12;
    case DeclType::Float4: return 16;
    case DeclType::D3DColor:
    case DeclType::UByte4:
    case DeclType::UByte4N:
    case DeclType::Short2:
    case DeclType::Short2N:
    case DeclType::UShort2N:
    case DeclType::UDec3:
    case DeclType::Dec3N:
    case DeclType::Float16_2: return 4;
    case DeclType::Short4:
    case DeclType::Short4N:
    case DeclType::UShort4N:
    case DeclType::Float16_4: return 8;
    default: return 0;
  }
}

uint32_t DeclTypeComponents(DeclType type)
{
  switch(type)
  {
    case DeclType::Float1: return 1;
    case DeclType::Float2:
    case DeclType::Short2:
    case DeclType::Short2N:
    case DeclType::UShort2N:
    case DeclType::Float16_2: return 2;
    case DeclType::Float3:
    case DeclType::UDec3:
    case DeclType::Dec3N: return 3;
    case DeclType::Float4:
    case DeclType::D3DColor:
    case DeclType::UByte4:
    case DeclType::UByte4N:
    case DeclType::Short4:
    case DeclType::Short4N:
    case DeclType::UShort4N:
    case DeclType::Float16_4: return 4;
    default: return 0;
  }
}

Vec4 DecodeDeclType(DeclType type, const byte *data)
{
  Vec4 ret(0.0f, 0.0f, 0.0f, 1.0f);

  auto f32 = [data](int i) {
    float f;
    memcpy(&f, data + i * 4, 4);
    return f;
  };
  auto s16 = [data](int i) {
    int16_t v;
    memcpy(&v, data + i * 2, 2);
    return v;
  };
  auto u16 = [data](int i) {
    uint16_t v;
    memcpy(&v, data + i * 2, 2);
    return v;
  };
  uint32_t u32;
  memcpy(&u32, data, 4);

  switch(type)
  {
    case DeclType::Float1:
    case DeclType::Float2:
    case DeclType::Float3:
    case DeclType::Float4:
      for(uint32_t i = 0; i < DeclTypeComponents(type); i++)
        ret[i] = f32(i);
      break;
    case DeclType::D3DColor:
      // 0xAARRGGBB, read as (R, G, B, A)
      ret = Vec4(float((u32 >> 16) & 0xFF) / 255.0f, float((u32 >> 8) & 0xFF) / 255.0f,
                 float(u32 & 0xFF) / 255.0f, float(u32 >> 24) / 255.0f);
      break;
    case DeclType::UByte4:
      for(int i = 0; i < 4; i++)
        ret[i] = float(data[i]);
      break;
    case DeclType::UByte4N:
      for(int i = 0; i < 4; i++)
        ret[i] = float(data[i]) / 255.0f;
      break;
    case DeclType::Short2:
    case DeclType::Short4:
      for(uint32_t i = 0; i < DeclTypeComponents(type); i++)
        ret[i] = float(s16(i));
      break;
    case DeclType::Short2N:
    case DeclType::Short4N:
      for(uint32_t i = 0; i < DeclTypeComponents(type); i++)
        ret[i] = RDCMAX(float(s16(i)) / 32767.0f, -1.0f);
      break;
    case DeclType::UShort2N:
    case DeclType::UShort4N:
      for(uint32_t i = 0; i < DeclTypeComponents(type); i++)
        ret[i] = float(u16(i)) / 65535.0f;
      break;
    case DeclType::UDec3:
      ret = Vec4(float(u32 & 0x3FF), float((u32 >> 10) & 0x3FF), float((u32 >> 20) & 0x3FF), 1.0f);
      break;
    case DeclType::Dec3N:
      for(int i = 0; i < 3; i++)
      {
        int32_t v = int32_t((u32 >> (i * 10)) & 0x3FF);
        if(v & 0x200)
          v -= 0x400;
        ret[i] = RDCMAX(float(v) / 511.0f, -1.0f);
      }
      break;
    case DeclType::Float16_2:
    case DeclType::Float16_4:
      for(uint32_t i = 0; i < DeclTypeComponents(type); i++)
        ret[i] = ConvertFromHalf(u16(i));
      break;
    default: break;
  }

  return ret;
}

uint32_t StreamElementIndex(const InputStream &stream, uint32_t vertex, uint32_t instance)
{
  const uint32_t divider = stream.frequency & 0x3FFFFFFF;
  if(stream.frequency & StreamInstanceData)
    return instance / RDCMAX(1U, divider);
  if(stream.frequency & StreamIndexedData)
    return vertex;
  return vertex / RDCMAX(1U, divider);
}

bool FetchElement(const InputElement &el, const InputStream *streams, uint32_t numStreams,
                  uint32_t vertex, uint32_t instance, Vec4 &ret)
{
  ret = Vec4(0.0f, 0.0f, 0.0f, 1.0f);

  const uint32_t size = DeclTypeSize(el.type);
  if(el.stream >= numStreams || size == 0 || el.method != 0)
    return false;

  const InputStream &s = streams[el.stream];
  const uint64_t offs =
      uint64_t(StreamElementIndex(s, vertex, instance)) * s.stride + uint64_t(el.offset);
  if(offs + size > s.data.size())
    return false;

  ret = DecodeDeclType(el.type, s.data.data() + offs);
  return true;
}

//////////////////////////////////////////////////////////////////////////
// running a draw

const rdcarray<size_t> PackOrder(const rdcarray<SigParameter> &sig)
{
  // as the mesh viewer lays out its columns (ConfigureColumnsForShader): the position moves to
  // the front, the others keep their order
  int posidx = -1;
  for(size_t i = 0; i < sig.size(); i++)
    if(sig[i].systemValue == ShaderBuiltin::Position)
      posidx = (int)i;

  rdcarray<size_t> ret;
  if(posidx >= 0)
    ret.push_back((size_t)posidx);
  for(size_t i = 0; i < sig.size(); i++)
    if((int)i != posidx)
      ret.push_back(i);
  return ret;
}

void EstimateNearFar(const byte *data, uint32_t stride, uint32_t count, float &nearPlane,
                     float &farPlane)
{
  // see D3D11Replay::InitPostVSBuffers: assuming a standard perspective matrix, Zpost = Zpre * m +
  // c with Wpost = Zpre, so two points distinct in z and w give m and c, and N = -c/m, F = c/(1-m)
  nearPlane = 0.1f;
  farPlane = 100.0f;

  if(count == 0 || stride < sizeof(Vec4))
    return;

  Vec4 pos0;
  memcpy(pos0.v, data, sizeof(pos0.v));

  bool found = false;
  for(uint32_t i = 1; i < count; i++)
  {
    Vec4 pos;
    memcpy(pos.v, data + size_t(i) * stride, sizeof(pos.v));

    if(fabsf(pos[3] - pos0[3]) > 0.01f && fabsf(pos[2] - pos0[2]) > 0.01f)
    {
      float m = (pos[2] - pos0[2]) / (pos[3] - pos0[3]);
      float c = pos[2] - pos[3] * m;

      if(m == 1.0f || c == 0.0f)
        continue;
      if(-c / m <= 0.000001f)
        continue;

      nearPlane = -c / m;
      farPlane = c / (1 - m);
      found = true;
      break;
    }
  }

  // all z's and w's identical: a reversed z projection with an infinite far plane if z is
  // positive and w bigger
  if(!found && pos0[2] > 0.0f && pos0[3] > pos0[2])
  {
    nearPlane = pos0[2];
    farPlane = FLT_MAX;
  }
}

void RunPostVS(const DrawInputs &draw, uint32_t instance, const VSEmulator *vs,
               const VSConstants &consts, const VSTexture textures[NumVertexSamplers],
               const FFPState *ffp, const rdcarray<SigParameter> &signature, PostVSResult &result)
{
  result = PostVSResult();

  if(vs == NULL && ffp == NULL)
  {
    result.status = "No vertex processing to emulate";
    return;
  }

  if(draw.numVertices == 0)
  {
    result.status = "Empty draw (0 vertices)";
    return;
  }

  const rdcarray<size_t> order = PackOrder(signature);
  uint32_t stride = 0;
  for(const SigParameter &sig : signature)
    stride += sig.compCount * sizeof(float);

  for(const SigParameter &sig : signature)
    if(sig.systemValue == ShaderBuiltin::Position)
      result.hasPosition = true;

  // the vertices to process, and each row's vertex among them
  rdcarray<uint32_t> vertices;
  result.numRows = draw.numVertices;
  if(draw.indexed)
  {
    result.indexed = true;
    const uint32_t rows = (uint32_t)RDCMIN((size_t)draw.numVertices, draw.indices.size());
    result.numRows = rows;

    rdcarray<uint32_t> fetched;
    fetched.resize(rows);
    for(uint32_t i = 0; i < rows; i++)
      fetched[i] = uint32_t(int64_t(draw.indices[i]) + draw.baseVertex);

    vertices = fetched;
    std::sort(vertices.begin(), vertices.end());
    vertices.resize(std::unique(vertices.begin(), vertices.end()) - vertices.begin());

    result.indices.resize(rows);
    for(uint32_t i = 0; i < rows; i++)
      result.indices[i] =
          uint32_t(std::lower_bound(vertices.begin(), vertices.end(), fetched[i]) - vertices.begin());
  }
  else
  {
    vertices.resize(draw.numVertices);
    for(uint32_t i = 0; i < draw.numVertices; i++)
      vertices[i] = draw.firstVertex + i;
  }

  result.numUnique = (uint32_t)vertices.size();
  result.stride = stride;
  result.vertexData.resize(size_t(stride) * vertices.size());

  const InputStream *streams = draw.streams.data();
  const uint32_t numStreams = (uint32_t)draw.streams.size();

  // which element each input register reads
  struct InputMap
  {
    uint32_t reg;
    const InputElement *el;
  };
  rdcarray<InputMap> inputMap;

  FFPInputs ffpInputs;
  FFPOutputLayout ffpLayout;

  if(vs)
  {
    for(const Instruction &inst : vs->GetProgram().instructions)
    {
      if(inst.op != Opcode::Dcl || inst.dst.type != RegType::Input || inst.dst.index >= MaxVSInputs)
        continue;
      for(const InputElement &el : draw.elements)
      {
        if(el.usage == inst.usage && el.usageIndex == inst.usageIndex)
        {
          inputMap.push_back({inst.dst.index, &el});
          break;
        }
      }
    }
    result.clipSpace = true;
  }
  else
  {
    ffpInputs.FromElements(draw.elements);
    ffpLayout = FFPLayout(*ffp, ffpInputs);
    result.clipSpace = !ffpLayout.pretransformed;
  }

  float values[64];

  for(size_t v = 0; v < vertices.size(); v++)
  {
    byte *dst = result.vertexData.data() + v * stride;

    if(vs)
    {
      Vec4 inputs[MaxVSInputs];
      for(uint32_t i = 0; i < MaxVSInputs; i++)
        inputs[i] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
      for(const InputMap &m : inputMap)
        FetchElement(*m.el, streams, numStreams, vertices[v], instance, inputs[m.reg]);

      VSOutputs out;
      rdcstr error;
      if(!vs->Execute(inputs, consts, textures, out, error))
      {
        result.status = StringFormat::Fmt("Vertex %u: %s", vertices[v], error.c_str());
        result.vertexData.clear();
        result.indices.clear();
        result.numUnique = 0;
        return;
      }

      for(size_t o : order)
      {
        const SigParameter &sig = signature[o];
        const Vec4 *reg = OutputRegister(out, vs->GetVersion(), sig);
        for(uint32_t c = 0; c < sig.compCount; c++)
          values[c] = reg && c < 4 ? (*reg)[c] : 0.0f;
        memcpy(dst, values, sig.compCount * sizeof(float));
        dst += sig.compCount * sizeof(float);
      }
    }
    else
    {
      FFPVertex vert;
      for(const InputElement &el : draw.elements)
      {
        Vec4 val;
        if(!FetchElement(el, streams, numStreams, vertices[v], instance, val))
          continue;
        switch(el.usage)
        {
          case DeclUsage::Position:
            if(el.usageIndex == 0)
              vert.position = val;
            else if(el.usageIndex == 1)
              vert.position1 = val;
            break;
          case DeclUsage::PositionT:
            if(el.usageIndex == 0)
              vert.position = val;
            break;
          case DeclUsage::Normal:
            if(el.usageIndex == 0)
              vert.normal = val;
            else if(el.usageIndex == 1)
              vert.normal1 = val;
            break;
          case DeclUsage::Color:
            if(el.usageIndex == 0)
              vert.diffuse = val;
            else if(el.usageIndex == 1)
              vert.specular = val;
            break;
          case DeclUsage::PSize:
            if(el.usageIndex == 0)
              vert.psize = val[0];
            break;
          case DeclUsage::BlendWeight:
            if(el.usageIndex == 0)
              vert.blendWeight = val;
            break;
          case DeclUsage::BlendIndices:
            if(el.usageIndex == 0)
              vert.blendIndices = val;
            break;
          case DeclUsage::TexCoord:
            if(el.usageIndex < MaxTextureStages)
              vert.texcoords[el.usageIndex] = val;
            break;
          default: break;
        }
      }

      FFPOutput out;
      FFPProcess(*ffp, ffpInputs, ffpLayout, vert, out);

      for(size_t o : order)
      {
        const SigParameter &sig = signature[o];
        FFPOutputValues(ffpLayout, out, sig, values);
        memcpy(dst, values, sig.compCount * sizeof(float));
        dst += sig.compCount * sizeof(float);
      }
    }
  }

  if(result.clipSpace && result.hasPosition && !order.empty() && signature[order[0]].compCount == 4)
    EstimateNearFar(result.vertexData.data(), stride, result.numUnique, result.nearPlane,
                    result.farPlane);
}

};    // namespace D3D9Emu

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"
#include "d3d9_bytecode_test.h"
#include "d3d9_shader_reflect.h"

using namespace D3D9BCTest;
using namespace D3D9Emu;

namespace
{
// assembles a shader a token at a time: op() writes the instruction token with the right length
// for the shader model, then its parameters
struct Assembler
{
  rdcarray<uint32_t> tokens;
  bool sm1 = false;

  Assembler(uint8_t major, uint8_t minor)
  {
    sm1 = major < 2;
    tokens.push_back(VersionToken(false, major, minor));
  }

  Assembler &op(Opcode o, std::initializer_list<uint32_t> params, uint32_t control = 0,
                uint32_t flags = 0)
  {
    tokens.push_back(Inst(o, control, sm1 ? 0 : (uint32_t)params.size()) | flags);
    for(uint32_t p : params)
      tokens.push_back(p);
    return *this;
  }

  Assembler &def(uint32_t reg, float x, float y, float z, float w)
  {
    return op(Opcode::Def,
              {Dst(RegType::Const, reg), FloatBits(x), FloatBits(y), FloatBits(z), FloatBits(w)});
  }

  Assembler &defi(uint32_t reg, int32_t x, int32_t y, int32_t z, int32_t w)
  {
    return op(Opcode::DefI,
              {Dst(RegType::ConstInt, reg), uint32_t(x), uint32_t(y), uint32_t(z), uint32_t(w)});
  }

  Assembler &dcl(DeclUsage usage, uint32_t index, uint32_t reg, RegType type = RegType::Input)
  {
    return op(Opcode::Dcl, {Dcl(usage, index), Dst(type, reg)});
  }

  rdcarray<uint32_t> finish()
  {
    rdcarray<uint32_t> ret = tokens;
    ret.push_back(EndTok());
    return ret;
  }
};

const uint32_t X = Swz(0, 0, 0, 0), Y = Swz(1, 1, 1, 1), Z = Swz(2, 2, 2, 2), W = Swz(3, 3, 3, 3);
const uint32_t XYZW = 0xE4;

uint32_t R(uint32_t i, uint32_t swz = XYZW, SrcMod mod = SrcMod::None)
{
  return Src(RegType::Temp, i, swz, mod);
}
uint32_t C(uint32_t i, uint32_t swz = XYZW)
{
  return Src(RegType::Const, i, swz);
}
uint32_t V(uint32_t i, uint32_t swz = XYZW)
{
  return Src(RegType::Input, i, swz);
}
uint32_t DR(uint32_t i, uint32_t mask = 0xF)
{
  return Dst(RegType::Temp, i, mask);
}
uint32_t OPos()
{
  return Dst(RegType::RastOut, 0);
}

struct Runner
{
  Program prog;
  VSEmulator emu;
  Vec4 inputs[MaxVSInputs];
  VSConstants consts;
  VSTexture textures[NumVertexSamplers];
  VSOutputs out;
  rdcstr error;

  bool Run(const rdcarray<uint32_t> &tokens)
  {
    error.clear();
    ParseProgram(tokens.data(), tokens.size(), prog);
    if(!emu.Init(prog))
    {
      error = emu.GetError();
      return false;
    }
    VSConstants c = emu.ApplyLiterals(consts);
    return emu.Execute(inputs, c, textures, out, error);
  }

  // runs a shader whose result is in oPos
  Vec4 Pos(const rdcarray<uint32_t> &tokens)
  {
    bool ok = Run(tokens);
    INFO(error.c_str());
    CHECK(ok);
    return out.rast[0];
  }
};

void CheckVec(const Vec4 &v, float x, float y, float z, float w, float eps = 1e-5f)
{
  INFO(StringFormat::Fmt("(%f, %f, %f, %f) vs (%f, %f, %f, %f)", v[0], v[1], v[2], v[3], x, y, z, w)
           .c_str());
  CHECK(fabsf(v[0] - x) <= eps * RDCMAX(1.0f, fabsf(x)));
  CHECK(fabsf(v[1] - y) <= eps * RDCMAX(1.0f, fabsf(y)));
  CHECK(fabsf(v[2] - z) <= eps * RDCMAX(1.0f, fabsf(z)));
  CHECK(fabsf(v[3] - w) <= eps * RDCMAX(1.0f, fabsf(w)));
}

// a shader computing one instruction on c0, c1 and c2 into oPos, which starts as 0
rdcarray<uint32_t> OneOp(uint8_t major, Opcode o, int numSrcs, uint32_t mask = 0xF,
                         uint32_t swz = XYZW)
{
  Assembler a(major, 0);
  a.def(10, 0, 0, 0, 0);
  a.op(Opcode::Mov, {OPos(), C(10)});
  if(numSrcs == 1)
    a.op(o, {Dst(RegType::RastOut, 0, mask), C(0, swz)});
  else if(numSrcs == 2)
    a.op(o, {Dst(RegType::RastOut, 0, mask), C(0, swz), C(1)});
  else
    a.op(o, {Dst(RegType::RastOut, 0, mask), C(0, swz), C(1), C(2)});
  return a.finish();
}
};    // namespace

TEST_CASE("D3D9 vertex shader emulation - arithmetic", "[d3d9][vsemu]")
{
  Runner r;
  r.consts.floats.resize(16);
  r.consts.floats[0] = Vec4(1.5f, -2.0f, 0.25f, 4.0f);
  r.consts.floats[1] = Vec4(2.0f, 3.0f, -1.0f, 0.5f);
  r.consts.floats[2] = Vec4(10.0f, 20.0f, 30.0f, 40.0f);

  SECTION("per component")
  {
    CheckVec(r.Pos(OneOp(2, Opcode::Mov, 1)), 1.5f, -2.0f, 0.25f, 4.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Add, 2)), 3.5f, 1.0f, -0.75f, 4.5f);
    CheckVec(r.Pos(OneOp(2, Opcode::Sub, 2)), -0.5f, -5.0f, 1.25f, 3.5f);
    CheckVec(r.Pos(OneOp(2, Opcode::Mul, 2)), 3.0f, -6.0f, -0.25f, 2.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Mad, 3)), 13.0f, 14.0f, 29.75f, 42.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Min, 2)), 1.5f, -2.0f, -1.0f, 0.5f);
    CheckVec(r.Pos(OneOp(2, Opcode::Max, 2)), 2.0f, 3.0f, 0.25f, 4.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Slt, 2)), 1.0f, 1.0f, 0.0f, 0.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Sge, 2)), 0.0f, 0.0f, 1.0f, 1.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Abs, 1)), 1.5f, 2.0f, 0.25f, 4.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Frc, 1)), 0.5f, 0.0f, 0.25f, 0.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Sgn, 3)), 1.0f, -1.0f, 1.0f, 1.0f);
    // src0 * (src1 - src2) + src2
    CheckVec(r.Pos(OneOp(2, Opcode::Lrp, 3)), -2.0f, 54.0f, 22.25f, -118.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Dp3, 2)), -3.25f, -3.25f, -3.25f, -3.25f);
    CheckVec(r.Pos(OneOp(2, Opcode::Dp4, 2)), -1.25f, -1.25f, -1.25f, -1.25f);
    CheckVec(r.Pos(OneOp(2, Opcode::Crs, 2, 0x7)), -2.0f * -1.0f - 0.25f * 3.0f,
             0.25f * 2.0f - 1.5f * -1.0f, 1.5f * 3.0f - -2.0f * 2.0f, 0.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Dst, 2)), 1.0f, -6.0f, 0.25f, 0.5f);
  }

  SECTION("scalar")
  {
    // the w of the swizzled source
    CheckVec(r.Pos(OneOp(2, Opcode::Rcp, 1)), 0.25f, 0.25f, 0.25f, 0.25f);
    const float third = 1.0f / 1.5f;
    CheckVec(r.Pos(OneOp(2, Opcode::Rcp, 1, 0xF, X)), third, third, third, third);
    CheckVec(r.Pos(OneOp(2, Opcode::Rsq, 1)), 0.5f, 0.5f, 0.5f, 0.5f);
    // rsq takes the absolute value
    const float rs2 = 1.0f / sqrtf(2.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Rsq, 1, 0xF, Y)), rs2, rs2, rs2, rs2);
    CheckVec(r.Pos(OneOp(2, Opcode::Exp, 1)), 16.0f, 16.0f, 16.0f, 16.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Log, 1, 0xF, Y)), 1.0f, 1.0f, 1.0f, 1.0f);
    // |0.25| ^ 0.5
    CheckVec(r.Pos(OneOp(2, Opcode::Pow, 2, 0xF, Z)), 0.5f, 0.5f, 0.5f, 0.5f);

    // rcp and rsq of 0 are +inf, log of 0 is -FLT_MAX
    r.consts.floats[0] = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(r.Pos(OneOp(2, Opcode::Rcp, 1))[0] == INFINITY);
    CHECK(r.Pos(OneOp(2, Opcode::Rsq, 1))[0] == INFINITY);
    CHECK(r.Pos(OneOp(2, Opcode::Log, 1))[0] == -FLT_MAX);
    // rcp of -0 is still +inf
    r.consts.floats[0] = Vec4(-0.0f, -0.0f, -0.0f, -0.0f);
    CHECK(r.Pos(OneOp(2, Opcode::Rcp, 1))[0] == INFINITY);
  }

  SECTION("macros")
  {
    r.consts.floats[0] = Vec4(0.5f, 0.25f, 0.0f, 2.0f);
    // lit: (1, N.L, (N.H)^power, 1)
    CheckVec(r.Pos(OneOp(2, Opcode::Lit, 1)), 1.0f, 0.5f, 0.0625f, 1.0f);
    r.consts.floats[0] = Vec4(-0.5f, 0.25f, 0.0f, 2.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Lit, 1)), 1.0f, 0.0f, 0.0f, 1.0f);
    r.consts.floats[0] = Vec4(0.5f, -0.25f, 0.0f, 2.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Lit, 1)), 1.0f, 0.5f, 0.0f, 1.0f);
    // the power is clamped to +-127.9961
    r.consts.floats[0] = Vec4(0.5f, 0.5f, 0.0f, 1000.0f);
    CHECK(r.Pos(OneOp(2, Opcode::Lit, 1))[2] == powf(0.5f, 127.9961f));

    r.consts.floats[0] = Vec4(3.0f, 0.0f, 4.0f, 10.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Nrm, 1)), 0.6f, 0.0f, 0.8f, 2.0f);
    r.consts.floats[0] = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::Nrm, 1)), 0.0f, 0.0f, 0.0f, 0.0f);

    // sincos writes cos to x and sin to y, vs_2_0's has two more constants
    r.consts.floats[0] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CheckVec(r.Pos(OneOp(2, Opcode::SinCos, 3, 0x3, W)), cosf(1.0f), sinf(1.0f), 0.0f, 0.0f);
    CheckVec(r.Pos(OneOp(3, Opcode::SinCos, 1, 0x2, W)), 0.0f, sinf(1.0f), 0.0f, 0.0f);
  }

  SECTION("matrices")
  {
    // c0 is the vector, c1-c4 the rows
    r.consts.floats[0] = Vec4(1.0f, 2.0f, 3.0f, 4.0f);
    for(uint32_t i = 0; i < 4; i++)
      r.consts.floats[1 + i] = Vec4(float(i + 1), 0.0f, 1.0f, 0.5f);

    auto mat = [](Opcode o, uint32_t mask) {
      Assembler a(1, 1);
      a.def(10, 9, 9, 9, 9);
      a.op(Opcode::Mov, {OPos(), C(10)});
      a.op(o, {Dst(RegType::RastOut, 0, mask), C(0), C(1)});
      return a.finish();
    };
    // dp4 per row: i + 1 + 3 + 2
    CheckVec(r.Pos(mat(Opcode::M4x4, 0xF)), 6.0f, 7.0f, 8.0f, 9.0f);
    CheckVec(r.Pos(mat(Opcode::M4x3, 0x7)), 6.0f, 7.0f, 8.0f, 9.0f);
    // dp3 per row: i + 1 + 3
    CheckVec(r.Pos(mat(Opcode::M3x4, 0xF)), 4.0f, 5.0f, 6.0f, 7.0f);
    CheckVec(r.Pos(mat(Opcode::M3x3, 0x7)), 4.0f, 5.0f, 6.0f, 9.0f);
    CheckVec(r.Pos(mat(Opcode::M3x2, 0x3)), 4.0f, 5.0f, 9.0f, 9.0f);
  }

  SECTION("modifiers and write masks")
  {
    Assembler a(2, 0);
    a.op(Opcode::Mov, {DR(0), Src(RegType::Const, 0, Swz(3, 2, 1, 0), SrcMod::Neg)});
    a.op(Opcode::Add, {Dst(RegType::Temp, 1, 0xF, DstMod_Saturate), R(0),
                       Src(RegType::Const, 1, XYZW, SrcMod::Abs)});
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 0, 0x5), R(0)});
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 0, 0xA), R(1)});
    // r0 = (-4, -0.25, 2, -1.5), r1 = saturate(r0 + (2, 3, 1, 0.5))
    CheckVec(r.Pos(a.finish()), -4.0f, 1.0f, 2.0f, 0.0f);
  }

  SECTION("vs_1_1 expp and logp")
  {
    r.consts.floats[0] = Vec4(0.0f, 0.0f, 0.0f, 2.5f);
    Vec4 e = r.Pos(OneOp(1, Opcode::ExpP, 1, 0xF, W));
    CHECK(e[0] == 4.0f);
    CHECK(e[1] == 0.5f);
    CHECK(fabsf(e[2] - exp2f(2.5f)) < 1e-3f);
    CHECK(e[3] == 1.0f);
    r.consts.floats[0] = Vec4(0.0f, 0.0f, 0.0f, 12.0f);
    Vec4 l = r.Pos(OneOp(1, Opcode::LogP, 1, 0xF, W));
    CHECK(l[0] == 3.0f);
    CHECK(l[1] == 1.5f);
    CHECK(fabsf(l[2] - log2f(12.0f)) < 1e-3f);
    CHECK(l[3] == 1.0f);

    // vs_2_0's are scalar
    Vec4 e2 = r.Pos(OneOp(2, Opcode::ExpP, 1, 0xF, W));
    CHECK(fabsf(e2[0] - 4096.0f) < 1.0f);
    CHECK(e2[0] == e2[3]);
  }
}

TEST_CASE("D3D9 vertex shader emulation - registers and outputs", "[d3d9][vsemu]")
{
  Runner r;
  r.consts.floats.resize(16);
  for(uint32_t i = 0; i < 16; i++)
    r.consts.floats[i] = Vec4(float(i), float(i) + 0.25f, float(i) + 0.5f, float(i) + 0.75f);

  SECTION("vs_1_1 inputs, outputs and relative addressing")
  {
    r.inputs[0] = Vec4(1.0f, 2.0f, 3.0f, 1.0f);
    r.inputs[3] = Vec4(2.7f, -1.2f, 0.0f, 0.0f);

    Assembler a(1, 1);
    a.dcl(DeclUsage::Position, 0, 0);
    a.dcl(DeclUsage::Normal, 0, 3);
    a.op(Opcode::Mov, {OPos(), V(0)});
    // mov a0.x rounds down: 2.7 -> 2, -1.2 -> -2
    a.op(Opcode::Mov, {Dst(RegType::AddrOrTexture, 0, 0x1), V(3, X)});
    a.op(Opcode::Mov, {Dst(RegType::AttrOut, 0), Src(RegType::Const, 3, XYZW) | RelBit});
    a.op(Opcode::Mov, {Dst(RegType::AddrOrTexture, 0, 0x1), V(3, Y)});
    a.op(Opcode::Mov, {Dst(RegType::AttrOut, 1), Src(RegType::Const, 5, XYZW) | RelBit});
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 7, 0x3), V(0, Swz(2, 1, 0, 0))});
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 1, 0x1), V(0, Y)});
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 2, 0x1), V(0, Z)});
    // out of range reads give 0
    a.op(Opcode::Mov, {Dst(RegType::AddrOrTexture, 0, 0x1), C(15, W)});
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 0), Src(RegType::Const, 4, XYZW) | RelBit});
    rdcarray<uint32_t> tokens = a.finish();
    REQUIRE(r.Run(tokens));
    CheckVec(r.out.rast[0], 1.0f, 2.0f, 3.0f, 1.0f);
    CheckVec(r.out.attr[0], 5.0f, 5.25f, 5.5f, 5.75f);
    CheckVec(r.out.attr[1], 3.0f, 3.25f, 3.5f, 3.75f);
    CheckVec(r.out.tex[7], 3.0f, 2.0f, 0.0f, 0.0f);
    CHECK(r.out.rast[1][0] == 2.0f);
    CHECK(r.out.rast[2][0] == 3.0f);
    CheckVec(r.out.tex[0], 0.0f, 0.0f, 0.0f, 0.0f);

    // the outputs the reflection lists map to these registers
    ShaderInfo info;
    info.Init(ResourceId(), tokens.data(), tokens.size());
    const rdcarray<SigParameter> &sig = info.GetReflection().outputSignature;
    REQUIRE(sig.size() == 7);
    for(const SigParameter &s : sig)
    {
      INFO(s.semanticIdxName.c_str());
      CHECK(OutputRegister(r.out, info.GetProgram().version, s) != NULL);
    }
    CHECK(OutputRegister(r.out, info.GetProgram().version, sig[0]) == &r.out.rast[0]);
  }

  SECTION("mova rounds to nearest")
  {
    r.inputs[0] = Vec4(1.5f, -1.5f, 2.4f, -2.6f);
    Assembler a(2, 0);
    a.dcl(DeclUsage::Position, 0, 0);
    a.op(Opcode::MovA, {Dst(RegType::AddrOrTexture, 0), V(0)});
    a.op(Opcode::Mov, {OPos(), Src(RegType::AddrOrTexture, 0)});
    // c[4 + a0.y]
    a.op(Opcode::Mov, {Dst(RegType::AttrOut, 0), Src(RegType::Const, 4) | RelBit,
                       Src(RegType::AddrOrTexture, 0, Y)});
    REQUIRE(r.Run(a.finish()));
    CheckVec(r.out.rast[0], 2.0f, -1.0f, 2.0f, -3.0f);
    CheckVec(r.out.attr[0], 3.0f, 3.25f, 3.5f, 3.75f);
  }

  SECTION("literals override the device's constants")
  {
    Assembler a(2, 0);
    a.def(2, 7.0f, 8.0f, 9.0f, 10.0f);
    a.op(Opcode::Add, {OPos(), C(2), C(3)});
    REQUIRE(r.Run(a.finish()));
    CheckVec(r.out.rast[0], 10.0f, 11.25f, 12.5f, 13.75f);

    // and extend the register file
    Assembler b(2, 0);
    b.def(200, 1.0f, 2.0f, 3.0f, 4.0f);
    b.op(Opcode::Mov, {OPos(), C(200)});
    REQUIRE(r.Run(b.finish()));
    CheckVec(r.out.rast[0], 1.0f, 2.0f, 3.0f, 4.0f);
  }

  SECTION("vs_3_0 outputs, relative inputs and outputs")
  {
    for(uint32_t i = 0; i < 4; i++)
      r.inputs[i] = Vec4(float(i * 10), 1.0f, 2.0f, 3.0f);

    Assembler a(3, 0);
    a.defi(0, 3, 1, 1, 0);
    a.dcl(DeclUsage::Position, 0, 0);
    a.dcl(DeclUsage::TexCoord, 0, 1);
    a.dcl(DeclUsage::TexCoord, 1, 2);
    a.dcl(DeclUsage::TexCoord, 2, 3);
    a.dcl(DeclUsage::Position, 0, 0, RegType::TexCrdOutOrOutput);
    a.dcl(DeclUsage::TexCoord, 0, 1, RegType::TexCrdOutOrOutput);
    a.dcl(DeclUsage::TexCoord, 1, 2, RegType::TexCrdOutOrOutput);
    a.dcl(DeclUsage::TexCoord, 2, 3, RegType::TexCrdOutOrOutput);
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 0), V(0)});
    // loop aL = 1..3: o[aL] = v[aL] + c[aL]
    a.op(Opcode::Loop, {Src(RegType::Loop, 0), Src(RegType::ConstInt, 0)});
    a.op(Opcode::Add, {Dst(RegType::TexCrdOutOrOutput, 0) | RelBit, Src(RegType::Loop, 0),
                       Src(RegType::Input, 0) | RelBit, Src(RegType::Loop, 0),
                       Src(RegType::Const, 0) | RelBit, Src(RegType::Loop, 0)});
    a.op(Opcode::EndLoop, {});
    rdcarray<uint32_t> tokens = a.finish();
    REQUIRE(r.Run(tokens));
    CheckVec(r.out.tex[0], 0.0f, 1.0f, 2.0f, 3.0f);
    CheckVec(r.out.tex[1], 11.0f, 2.25f, 3.5f, 4.75f);
    CheckVec(r.out.tex[2], 22.0f, 3.25f, 4.5f, 5.75f);
    CheckVec(r.out.tex[3], 33.0f, 4.25f, 5.5f, 6.75f);

    ShaderInfo info;
    info.Init(ResourceId(), tokens.data(), tokens.size());
    const rdcarray<SigParameter> &sig = info.GetReflection().outputSignature;
    REQUIRE(sig.size() == 4);
    CHECK(OutputRegister(r.out, info.GetProgram().version, sig[2]) == &r.out.tex[2]);
  }
}

TEST_CASE("D3D9 vertex shader emulation - flow control", "[d3d9][vsemu]")
{
  Runner r;
  r.consts.floats.resize(16);
  r.consts.ints.resize(16);
  r.consts.bools.resize(16);

  SECTION("static if/else with booleans")
  {
    auto shader = [](uint32_t mod) {
      Assembler a(2, 0);
      a.def(0, 1, 1, 1, 1);
      a.def(1, 2, 2, 2, 2);
      a.op(Opcode::If, {Src(RegType::ConstBool, 3, XYZW, SrcMod(mod))});
      a.op(Opcode::Mov, {OPos(), C(0)});
      a.op(Opcode::Else, {});
      a.op(Opcode::Mov, {OPos(), C(1)});
      a.op(Opcode::EndIf, {});
      return a.finish();
    };
    r.consts.bools[3] = 1;
    CheckVec(r.Pos(shader(0)), 1, 1, 1, 1);
    r.consts.bools[3] = 0;
    CheckVec(r.Pos(shader(0)), 2, 2, 2, 2);
    CheckVec(r.Pos(shader(uint32_t(SrcMod::Not))), 1, 1, 1, 1);
  }

  SECTION("loops and reps")
  {
    Assembler a(2, 0);
    a.def(0, 0, 0, 0, 0);
    a.def(1, 1, 0, 0, 0);
    a.defi(0, 4, 2, 3, 0);
    a.defi(1, 3, 0, 0, 0);
    a.op(Opcode::Mov, {DR(0), C(0)});
    a.op(Opcode::Mov, {DR(1), C(0)});
    // aL = 2, 5, 8, 11: r0.x += aL, r0.y += 1
    a.op(Opcode::Loop, {Src(RegType::Loop, 0), Src(RegType::ConstInt, 0)});
    a.op(Opcode::Add, {DR(0, 0x1), R(0), Src(RegType::Loop, 0)});
    a.op(Opcode::Add, {DR(0, 0x2), R(0), C(1, X)});
    // a nested rep 3 times: r1.x += 1
    a.op(Opcode::Rep, {Src(RegType::ConstInt, 1)});
    a.op(Opcode::Add, {DR(1, 0x1), R(1), C(1)});
    a.op(Opcode::EndRep, {});
    a.op(Opcode::EndLoop, {});
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 0, 0x3), R(0)});
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 0, 0x4), R(1, X)});
    CheckVec(r.Pos(a.finish()), 26.0f, 4.0f, 12.0f, 0.0f);
  }

  SECTION("a loop with a count of 0 doesn't run")
  {
    Assembler a(2, 0);
    a.def(0, 5, 5, 5, 5);
    a.defi(0, 0, 0, 0, 0);
    a.op(Opcode::Mov, {OPos(), C(0)});
    a.op(Opcode::Rep, {Src(RegType::ConstInt, 0)});
    a.op(Opcode::Mov, {OPos(), C(1)});
    a.op(Opcode::EndRep, {});
    CheckVec(r.Pos(a.finish()), 5, 5, 5, 5);
  }

  SECTION("break, breakc and if_comp")
  {
    Assembler a(2, 1);
    a.def(0, 0, 0, 0, 0);
    a.def(1, 1, 0, 3, 7);
    a.defi(0, 100, 0, 1, 0);
    a.op(Opcode::Mov, {DR(0), C(0)});
    a.op(Opcode::Rep, {Src(RegType::ConstInt, 0)});
    a.op(Opcode::Add, {DR(0, 0x1), R(0), C(1, X)});
    // break when r0.x >= 3
    a.op(Opcode::BreakC, {R(0, X), C(1, Z)}, uint32_t(Comparison::GE));
    a.op(Opcode::EndRep, {});
    // if r0.x == 3 (the else isn't taken)
    a.op(Opcode::IfC, {R(0, X), C(1, Z)}, uint32_t(Comparison::EQ));
    a.op(Opcode::Mov, {DR(0, 0x2), C(1, W)});
    a.op(Opcode::Else, {});
    a.op(Opcode::Mov, {DR(0, 0x2), C(1, Y)});
    a.op(Opcode::EndIf, {});
    // an unconditional break in the first iteration
    a.op(Opcode::Rep, {Src(RegType::ConstInt, 0)});
    a.op(Opcode::Add, {DR(0, 0x4), R(0), C(1, X)});
    a.op(Opcode::Break, {});
    a.op(Opcode::EndRep, {});
    a.op(Opcode::Mov, {OPos(), R(0)});
    CheckVec(r.Pos(a.finish()), 3.0f, 7.0f, 1.0f, 0.0f);
  }

  SECTION("predicates")
  {
    Assembler a(2, 1);
    a.def(0, 1, -1, 2, -2);
    a.def(1, 0, 0, 0, 0);
    a.def(2, 9, 9, 9, 9);
    a.defi(0, 10, 0, 1, 0);
    // p0 = c0 > 0: (1, 0, 1, 0)
    a.op(Opcode::SetP, {Dst(RegType::Predicate, 0), C(0), C(1)}, uint32_t(Comparison::GT));
    a.op(Opcode::Mov, {OPos(), C(1)});
    // (p0) mov oPos, c2: only x and z
    a.op(Opcode::Mov, {OPos(), Src(RegType::Predicate, 0), C(2)}, 0, PredBit);
    // (!p0.x) mov oD0, c2: nothing
    a.op(Opcode::Mov, {Dst(RegType::AttrOut, 0), C(1)});
    a.op(Opcode::Mov, {Dst(RegType::AttrOut, 0), Src(RegType::Predicate, 0, X, SrcMod::Not), C(2)},
         0, PredBit);
    // if p0.y isn't taken, if !p0.y is
    a.op(Opcode::Mov, {Dst(RegType::AttrOut, 1), C(1)});
    a.op(Opcode::If, {Src(RegType::Predicate, 0, Y)});
    a.op(Opcode::Mov, {Dst(RegType::AttrOut, 1, 0x1), C(2)});
    a.op(Opcode::EndIf, {});
    a.op(Opcode::If, {Src(RegType::Predicate, 0, Y, SrcMod::Not)});
    a.op(Opcode::Mov, {Dst(RegType::AttrOut, 1, 0x2), C(2)});
    a.op(Opcode::EndIf, {});
    // breakp p0.x in the first iteration
    a.op(Opcode::Mov, {DR(0), C(1)});
    a.op(Opcode::Rep, {Src(RegType::ConstInt, 0)});
    a.op(Opcode::Add, {DR(0), R(0), C(0, X)});
    a.op(Opcode::BreakP, {Src(RegType::Predicate, 0, X)});
    a.op(Opcode::EndRep, {});
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 0), R(0)});
    REQUIRE(r.Run(a.finish()));
    CheckVec(r.out.rast[0], 9.0f, 0.0f, 9.0f, 0.0f);
    CheckVec(r.out.attr[0], 0.0f, 0.0f, 0.0f, 0.0f);
    CheckVec(r.out.attr[1], 0.0f, 9.0f, 0.0f, 0.0f);
    CheckVec(r.out.tex[0], 1.0f, 1.0f, 1.0f, 1.0f);
  }

  SECTION("subroutines")
  {
    r.consts.bools[0] = 1;
    r.consts.bools[1] = 0;
    Assembler a(2, 0);
    a.def(0, 0, 0, 0, 0);
    a.def(1, 1, 10, 100, 1000);
    a.defi(0, 2, 0, 0, 0);
    a.op(Opcode::Mov, {DR(0), C(0)});
    a.op(Opcode::Call, {Src(RegType::Label, 0)});
    // callnz l1, b0 is taken, callnz l1, b1 isn't
    a.op(Opcode::CallNZ, {Src(RegType::Label, 1), Src(RegType::ConstBool, 0)});
    a.op(Opcode::CallNZ, {Src(RegType::Label, 1), Src(RegType::ConstBool, 1)});
    // a subroutine called from a loop, twice
    a.op(Opcode::Rep, {Src(RegType::ConstInt, 0)});
    a.op(Opcode::Call, {Src(RegType::Label, 2)});
    a.op(Opcode::EndRep, {});
    a.op(Opcode::Mov, {OPos(), R(0)});
    a.op(Opcode::Ret, {});
    // l0: r0.x += 1
    a.op(Opcode::Label, {Src(RegType::Label, 0)});
    a.op(Opcode::Add, {DR(0, 0x1), R(0), C(1, X)});
    a.op(Opcode::Ret, {});
    // l1: r0.y += 10, and calls l0
    a.op(Opcode::Label, {Src(RegType::Label, 1)});
    a.op(Opcode::Add, {DR(0, 0x2), R(0), C(1, Y)});
    a.op(Opcode::Call, {Src(RegType::Label, 0)});
    a.op(Opcode::Ret, {});
    // l2: r0.z += 100, in a rep of its own that it returns from in the first iteration
    a.op(Opcode::Label, {Src(RegType::Label, 2)});
    a.op(Opcode::Rep, {Src(RegType::ConstInt, 0)});
    a.op(Opcode::Add, {DR(0, 0x4), R(0), C(1, Z)});
    a.op(Opcode::Ret, {});
    a.op(Opcode::EndRep, {});
    a.op(Opcode::Ret, {});
    CheckVec(r.Pos(a.finish()), 2.0f, 10.0f, 200.0f, 0.0f);
  }

  SECTION("bad flow control")
  {
    {
      Assembler a(2, 0);
      a.op(Opcode::EndIf, {});
      CHECK_FALSE(r.Run(a.finish()));
      CHECK(r.error.contains("endif"));
    }
    {
      Assembler a(2, 0);
      a.op(Opcode::Rep, {Src(RegType::ConstInt, 0)});
      CHECK_FALSE(r.Run(a.finish()));
      CHECK(r.error.contains("unterminated"));
    }
    {
      Assembler a(2, 0);
      a.op(Opcode::Break, {});
      CHECK_FALSE(r.Run(a.finish()));
    }
    {
      Assembler a(2, 0);
      a.op(Opcode::Call, {Src(RegType::Label, 5)});
      CHECK_FALSE(r.Run(a.finish()));
      CHECK(r.error.contains("label"));
    }
    {
      // infinite recursion
      Assembler a(2, 0);
      a.op(Opcode::Call, {Src(RegType::Label, 0)});
      a.op(Opcode::Ret, {});
      a.op(Opcode::Label, {Src(RegType::Label, 0)});
      a.op(Opcode::Call, {Src(RegType::Label, 0)});
      a.op(Opcode::Ret, {});
      CHECK_FALSE(r.Run(a.finish()));
      CHECK(r.error.contains("nested"));
    }
    {
      // pixel shader instructions
      Assembler a(2, 0);
      a.op(Opcode::Dsx, {DR(0), R(0)});
      CHECK_FALSE(r.Run(a.finish()));
      CHECK(r.error.contains("Unsupported"));
    }
  }
}

TEST_CASE("D3D9 vertex shader emulation - texldl", "[d3d9][vsemu]")
{
  Runner r;

  // a 4x2 2D texture with 2 levels, and a 1x1 cube with a value per face
  VSTexture &tex = r.textures[1];
  tex.levels.resize(2);
  tex.levels[0].width = 4;
  tex.levels[0].height = 2;
  for(uint32_t i = 0; i < 8; i++)
    tex.levels[0].texels.push_back(Vec4(float(i), 0.0f, 0.0f, 1.0f));
  tex.levels[1].width = 2;
  tex.levels[1].height = 1;
  tex.levels[1].texels.push_back(Vec4(100.0f, 0.0f, 0.0f, 1.0f));
  tex.levels[1].texels.push_back(Vec4(101.0f, 0.0f, 0.0f, 1.0f));

  VSTexture &cube = r.textures[2];
  cube.type = SamplerTextureType::Cube;
  cube.levels.resize(1);
  cube.levels[0].width = cube.levels[0].height = 1;
  cube.levels[0].depth = 6;
  for(uint32_t i = 0; i < 6; i++)
    cube.levels[0].texels.push_back(Vec4(float(i), 0.0f, 0.0f, 1.0f));

  r.consts.floats.resize(4);

  auto sample = [&r](uint32_t sampler, Vec4 coord) {
    r.consts.floats[0] = coord;
    Assembler a(3, 0);
    a.op(Opcode::Dcl, {DclSampler(sampler == 2 ? 3 : 2), Dst(RegType::Sampler, sampler)});
    a.dcl(DeclUsage::Position, 0, 0, RegType::TexCrdOutOrOutput);
    a.op(Opcode::TexLdl, {Dst(RegType::TexCrdOutOrOutput, 0), C(0), Src(RegType::Sampler, sampler)});
    bool ok = r.Run(a.finish());
    INFO(r.error.c_str());
    CHECK(ok);
    return r.out.tex[0][0];
  };

  // point sampling: texel (floor(u * w), floor(v * h))
  CHECK(sample(1, Vec4(0.1f, 0.1f, 0.0f, 0.0f)) == 0.0f);
  CHECK(sample(1, Vec4(0.3f, 0.1f, 0.0f, 0.0f)) == 1.0f);
  CHECK(sample(1, Vec4(0.9f, 0.6f, 0.0f, 0.0f)) == 7.0f);
  // wrapping
  CHECK(sample(1, Vec4(1.3f, -0.4f, 0.0f, 0.0f)) == 5.0f);
  // the nearest level to the LOD in w
  CHECK(sample(1, Vec4(0.9f, 0.6f, 0.0f, 0.6f)) == 101.0f);
  CHECK(sample(1, Vec4(0.9f, 0.6f, 0.0f, 0.4f)) == 7.0f);
  CHECK(sample(1, Vec4(0.9f, 0.6f, 0.0f, 9.0f)) == 101.0f);
  // the most detailed level is MAXMIPLEVEL
  tex.maxMipLevel = 1;
  CHECK(sample(1, Vec4(0.1f, 0.1f, 0.0f, 0.0f)) == 100.0f);
  tex.maxMipLevel = 0;
  // clamp, border and mirror
  tex.address[0] = TexAddress::Clamp;
  CHECK(sample(1, Vec4(1.5f, 0.1f, 0.0f, 0.0f)) == 3.0f);
  tex.address[0] = TexAddress::Border;
  tex.border = Vec4(-1.0f, 0.0f, 0.0f, 0.0f);
  CHECK(sample(1, Vec4(1.5f, 0.1f, 0.0f, 0.0f)) == -1.0f);
  tex.address[0] = TexAddress::Mirror;
  CHECK(sample(1, Vec4(1.1f, 0.1f, 0.0f, 0.0f)) == 3.0f);

  // cube faces by major axis
  CHECK(sample(2, Vec4(1.0f, 0.2f, 0.3f, 0.0f)) == 0.0f);
  CHECK(sample(2, Vec4(-1.0f, 0.2f, 0.3f, 0.0f)) == 1.0f);
  CHECK(sample(2, Vec4(0.1f, 1.0f, 0.3f, 0.0f)) == 2.0f);
  CHECK(sample(2, Vec4(0.1f, -1.0f, 0.3f, 0.0f)) == 3.0f);
  CHECK(sample(2, Vec4(0.1f, 0.2f, 1.0f, 0.0f)) == 4.0f);
  CHECK(sample(2, Vec4(0.1f, 0.2f, -1.0f, 0.0f)) == 5.0f);

  // unbound samplers read (0, 0, 0, 1)
  r.consts.floats[0] = Vec4();
  Assembler a(3, 0);
  a.dcl(DeclUsage::Position, 0, 0, RegType::TexCrdOutOrOutput);
  a.op(Opcode::TexLdl, {Dst(RegType::TexCrdOutOrOutput, 0), C(0), Src(RegType::Sampler, 0)});
  REQUIRE(r.Run(a.finish()));
  CheckVec(r.out.tex[0], 0.0f, 0.0f, 0.0f, 1.0f);
}

TEST_CASE("D3D9 vertex fetch", "[d3d9][vsemu]")
{
  SECTION("declaration types")
  {
    byte data[16];
    auto set = [&data](std::initializer_list<uint32_t> words) {
      size_t i = 0;
      for(uint32_t w : words)
        memcpy(data + 4 * i++, &w, 4);
    };

    set({FloatBits(1.0f), FloatBits(2.0f), FloatBits(3.0f), FloatBits(4.0f)});
    CheckVec(DecodeDeclType(DeclType::Float1, data), 1, 0, 0, 1);
    CheckVec(DecodeDeclType(DeclType::Float2, data), 1, 2, 0, 1);
    CheckVec(DecodeDeclType(DeclType::Float3, data), 1, 2, 3, 1);
    CheckVec(DecodeDeclType(DeclType::Float4, data), 1, 2, 3, 4);
    set({0x80FF4000});
    CheckVec(DecodeDeclType(DeclType::D3DColor, data), 1.0f, 64.0f / 255.0f, 0.0f, 128.0f / 255.0f);
    CheckVec(DecodeDeclType(DeclType::UByte4, data), 0.0f, 64.0f, 255.0f, 128.0f);
    CheckVec(DecodeDeclType(DeclType::UByte4N, data), 0.0f, 64.0f / 255.0f, 1.0f, 128.0f / 255.0f);
    set({0x8000FFFF, 0x00027FFF});
    CheckVec(DecodeDeclType(DeclType::Short2, data), -1.0f, -32768.0f, 0.0f, 1.0f);
    CheckVec(DecodeDeclType(DeclType::Short4, data), -1.0f, -32768.0f, 32767.0f, 2.0f);
    CheckVec(DecodeDeclType(DeclType::Short2N, data), -1.0f / 32767.0f, -1.0f, 0.0f, 1.0f);
    CheckVec(DecodeDeclType(DeclType::Short4N, data), -1.0f / 32767.0f, -1.0f, 1.0f, 2.0f / 32767.0f);
    CheckVec(DecodeDeclType(DeclType::UShort2N, data), 1.0f, 32768.0f / 65535.0f, 0.0f, 1.0f);
    CheckVec(DecodeDeclType(DeclType::UShort4N, data), 1.0f, 32768.0f / 65535.0f,
             32767.0f / 65535.0f, 2.0f / 65535.0f);
    // x = 1, y = 0x3FF, z = 0x200
    set({1U | (0x3FFU << 10) | (0x200U << 20)});
    CheckVec(DecodeDeclType(DeclType::UDec3, data), 1.0f, 1023.0f, 512.0f, 1.0f);
    CheckVec(DecodeDeclType(DeclType::Dec3N, data), 1.0f / 511.0f, -1.0f / 511.0f, -1.0f, 1.0f);
    // 1.0, -2.0, 0.5, 0 as halfs
    set({0xC0003C00, 0x00003800});
    CheckVec(DecodeDeclType(DeclType::Float16_2, data), 1.0f, -2.0f, 0.0f, 1.0f);
    CheckVec(DecodeDeclType(DeclType::Float16_4, data), 1.0f, -2.0f, 0.5f, 0.0f);
    CHECK(DeclTypeSize(DeclType::Unused) == 0);
  }

  SECTION("stream frequencies")
  {
    InputStream s;
    CHECK(StreamElementIndex(s, 7, 3) == 7);
    s.frequency = StreamIndexedData | 10;
    CHECK(StreamElementIndex(s, 7, 3) == 7);
    s.frequency = StreamInstanceData | 2;
    CHECK(StreamElementIndex(s, 7, 3) == 1);
    CHECK(StreamElementIndex(s, 7, 4) == 2);
    s.frequency = 3;
    CHECK(StreamElementIndex(s, 7, 3) == 2);
  }

  SECTION("out of range elements read (0, 0, 0, 1)")
  {
    InputStream s;
    s.stride = 8;
    s.data.resize(16);
    InputElement el;
    el.type = DeclType::Float2;
    Vec4 v;
    CHECK(FetchElement(el, &s, 1, 1, 0, v));
    CHECK_FALSE(FetchElement(el, &s, 1, 2, 0, v));
    CheckVec(v, 0, 0, 0, 1);
    el.stream = 1;
    CHECK_FALSE(FetchElement(el, &s, 1, 0, 0, v));
  }
}

TEST_CASE("D3D9 post-transform draws", "[d3d9][vsemu]")
{
  // vs_2_0: oPos = v0 + v1 (v1 is a per-instance offset), oT0.xy = v0 * 2, oD0 = v1
  Assembler a(2, 0);
  a.dcl(DeclUsage::Position, 0, 0);
  a.dcl(DeclUsage::TexCoord, 0, 1);
  a.op(Opcode::Add, {OPos(), V(0), V(1)});
  a.op(Opcode::Add, {Dst(RegType::TexCrdOutOrOutput, 0, 0x3), V(0), V(0)});
  a.op(Opcode::Mov, {Dst(RegType::AttrOut, 0), V(1)});
  rdcarray<uint32_t> tokens = a.finish();

  ShaderInfo info;
  info.Init(ResourceId(), tokens.data(), tokens.size());
  VSEmulator emu;
  REQUIRE(emu.Init(info.GetProgram()));
  const rdcarray<SigParameter> &sig = info.GetReflection().outputSignature;
  REQUIRE(sig.size() == 3);

  DrawInputs draw;
  draw.streams.resize(2);
  // 6 vertices: (i, 10 * i, 0.5, i + 1)
  for(uint32_t i = 0; i < 6; i++)
  {
    float v[4] = {float(i), float(i * 10), 0.5f, float(i + 1)};
    draw.streams[0].data.append((const byte *)v, sizeof(v));
  }
  draw.streams[0].stride = 16;
  draw.streams[0].frequency = StreamIndexedData | 3;
  // per instance offsets
  for(uint32_t i = 0; i < 3; i++)
  {
    float v[4] = {float(i * 100), 0.0f, 0.0f, 0.0f};
    draw.streams[1].data.append((const byte *)v, sizeof(v));
  }
  draw.streams[1].stride = 16;
  draw.streams[1].frequency = StreamInstanceData | 1;

  InputElement pos;
  pos.type = DeclType::Float4;
  InputElement inst;
  inst.stream = 1;
  inst.type = DeclType::Float4;
  inst.usage = DeclUsage::TexCoord;
  draw.elements = {pos, inst};

  const rdcarray<size_t> order = PackOrder(sig);
  CHECK(sig[order[0]].systemValue == ShaderBuiltin::Position);

  VSTexture textures[NumVertexSamplers];
  VSConstants consts = emu.ApplyLiterals(VSConstants());

  SECTION("non-indexed")
  {
    draw.firstVertex = 2;
    draw.numVertices = 3;
    PostVSResult res;
    RunPostVS(draw, 1, &emu, consts, textures, NULL, sig, res);
    INFO(res.status.c_str());
    REQUIRE(res.status.empty());
    CHECK_FALSE(res.indexed);
    CHECK(res.numUnique == 3);
    CHECK(res.stride == 3 * 16);
    CHECK(res.clipSpace);
    const float *f = (const float *)res.vertexData.data();
    // vertex 2 of instance 1
    CHECK(f[0] == 102.0f);
    CHECK(f[1] == 20.0f);
    CHECK(f[3] == 3.0f);
  }

  SECTION("indexed, with a base vertex")
  {
    draw.indexed = true;
    draw.indices = {3, 1, 3, 2, 1, 0};
    draw.baseVertex = 1;
    draw.numVertices = 6;
    PostVSResult res;
    RunPostVS(draw, 2, &emu, consts, textures, NULL, sig, res);
    REQUIRE(res.status.empty());
    CHECK(res.indexed);
    CHECK(res.numUnique == 4);
    CHECK(res.numRows == 6);
    CHECK(res.indices == rdcarray<uint32_t>({3, 1, 3, 2, 1, 0}));
    const float *f = (const float *)res.vertexData.data();
    const uint32_t floatsPerVertex = res.stride / 4;
    // unique vertex 0 is vertex 1 (index 0 + base vertex 1)
    CHECK(f[0] == 201.0f);
    CHECK(f[floatsPerVertex * 3 + 0] == 204.0f);
  }

  SECTION("near and far planes")
  {
    // a perspective projection with near 1 and far 11: z' = z * 1.1 - 1.1, w' = z
    bytebuf data;
    for(float z : {2.0f, 5.0f})
    {
      float v[4] = {0.0f, 0.0f, z * 1.1f - 1.1f, z};
      data.append((const byte *)v, sizeof(v));
    }
    float n = 0, fp = 0;
    EstimateNearFar(data.data(), 16, 2, n, fp);
    CHECK(fabsf(n - 1.0f) < 1e-4f);
    CHECK(fabsf(fp - 11.0f) < 1e-3f);
  }

  SECTION("errors are reported")
  {
    draw.numVertices = 0;
    PostVSResult res;
    RunPostVS(draw, 0, &emu, consts, textures, NULL, sig, res);
    CHECK_FALSE(res.status.empty());
  }
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
