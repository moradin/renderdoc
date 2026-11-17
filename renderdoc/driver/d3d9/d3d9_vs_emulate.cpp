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
// with its partner, finds each label, and collects the def/defi/defb literals. A VSThread then runs
// the instructions in order with an instruction pointer, a stack of loops (each with its
// remaining count and, for loop, its aL) and a stack of calls. Execute runs one to the end; a
// debugger steps it, one instruction at a time, and gets the registers each one changed with what
// they held before. Registers are fp32 throughout, partial precision hints are ignored, as the
// reference rasterizer does.
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

// clears the low 8 bits of the mantissa, as the documentation's partial precision pseudo code does
float PartialPrecision(float f)
{
  uint32_t bits;
  memcpy(&bits, &f, sizeof(bits));
  bits &= 0xFFFFFF00;
  memcpy(&f, &bits, sizeof(bits));
  return f;
}
};    // namespace

void VSThread::Start(const VSEmulator &emu, const Vec4 inputs[MaxVSInputs],
                     const VSConstants &consts, const VSTexture textures[NumVertexSamplers])
{
  // a new thread already has its registers cleared, which is worth not doing twice for each
  // vertex of a draw
  if(m_Emu)
  {
    VSRegisters &regs = *this;
    regs = VSRegisters();
  }

  m_Emu = &emu;
  m_Inputs = inputs;
  m_Consts = &consts;
  m_Textures = textures;
  m_IP = 0;
  m_Executed = 0;
  m_Error.clear();
  m_Changes = NULL;
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

inline int32_t VSThread::RelValue(const RelativeAddress &rel) const
{
  if(!rel.present)
    return 0;
  if(rel.type == RegType::Loop)
    return aL();
  return a0.v[rel.component & 3];
}

inline Vec4 VSThread::ReadRaw(const SrcParam &src, int32_t regOffset) const
{
  int64_t idx = int64_t(src.index) + regOffset + RelValue(src.rel);
  const VSConstants *c = m_Consts;
  Vec4 ret;

  switch(src.type)
  {
    case RegType::Temp:
      if(idx >= 0 && idx < MaxVSTemps)
        ret = r[idx];
      break;
    case RegType::Input:
      if(idx >= 0 && idx < MaxVSInputs)
        ret = m_Inputs[idx];
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

inline Vec4 VSThread::Read(const SrcParam &src, int32_t regOffset) const
{
  Vec4 raw = ReadRaw(src, regOffset);
  Vec4 ret;
  for(int i = 0; i < 4; i++)
    ret[i] = ApplyMod(src.mod, raw[src.swizzle[i] & 3]);
  return ret;
}

// a boolean condition: a boolean constant or a predicate component, with ! applied
inline bool VSThread::ReadBool(const SrcParam &src) const
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

inline IVec4 VSThread::ReadInt(const SrcParam &src) const
{
  IVec4 ret;
  if(src.type == RegType::ConstInt && src.index < m_Consts->ints.size())
    ret = m_Consts->ints[src.index];
  return ret;
}

inline uint8_t VSThread::WriteMask(const Instruction &inst) const
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

template <bool Record>
inline void VSThread::Write(const Instruction &inst, Vec4 val)
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
        dst = &out.rast[idx];
      break;
    case RegType::AttrOut:
      if(idx >= 0 && idx < 2)
        dst = &out.attr[idx];
      break;
    case RegType::TexCrdOutOrOutput:
      if(idx >= 0 && idx < 12)
        dst = &out.tex[idx];
      break;
    case RegType::AddrOrTexture:
    {
      if(Record)
      {
        if(mask)
        {
          VSRegisterChange change;
          change.type = RegType::AddrOrTexture;
          change.i = a0;
          m_Changes->push_back(change);
        }
      }
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
      if(Record)
      {
        if(mask)
        {
          VSRegisterChange change;
          change.type = RegType::Predicate;
          for(int i = 0; i < 4; i++)
            change.i.v[i] = p[i] ? 1 : 0;
          m_Changes->push_back(change);
        }
      }
      for(int i = 0; i < 4; i++)
        if(mask & (1 << i))
          p[i] = val[i] != 0.0f;
      return;
    default: break;
  }

  if(dst && mask)
  {
    if(Record)
    {
      VSRegisterChange change;
      change.type = d.type;
      change.index = uint32_t(idx);
      change.f = *dst;
      m_Changes->push_back(change);
    }
    for(int i = 0; i < 4; i++)
      if(mask & (1 << i))
        (*dst)[i] = val[i];
  }
}

namespace
{

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

// The texels a linear filter reads along one axis, and their weights: the two either side of the
// coordinate, addressed separately. The GPU keeps 8 bits of the weight, rounded to the nearest.
struct AxisTaps
{
  int32_t idx[2] = {0, 0};
  float weight[2] = {1.0f, 0.0f};
  int count = 1;
};

// a texel index outside the texture after addressing, or -1 for the border colour
int32_t AddressIndex(int32_t i, int32_t size, TexAddress mode)
{
  switch(mode)
  {
    case TexAddress::Wrap: i %= size; return i < 0 ? i + size : i;
    case TexAddress::Mirror:
    {
      int32_t p = i % (2 * size);
      if(p < 0)
        p += 2 * size;
      return p < size ? p : 2 * size - 1 - p;
    }
    case TexAddress::MirrorOnce:
      if(i < 0)
        i = -i - 1;
      return RDCMIN(i, size - 1);
    case TexAddress::Border: return i < 0 || i >= size ? -1 : i;
    case TexAddress::Clamp:
    default: return RDCCLAMP(i, 0, size - 1);
  }
}

AxisTaps Taps(float u, uint32_t size, TexAddress mode, bool linear)
{
  AxisTaps ret;
  if(!linear)
  {
    ret.idx[0] = AddressTexel(u, size, mode);
    return ret;
  }

  if(!(u == u))
    u = 0.0f;
  // far enough out that the integer maths can't overflow
  u = RDCCLAMP(u, -65536.0f, 65536.0f);

  // texel centres are at half texels
  const float p = u * float(size) - 0.5f;
  const float base = floorf(p);
  const float f = floorf((p - base) * 256.0f + 0.5f) / 256.0f;
  const int32_t i = int32_t(base);

  ret.idx[0] = AddressIndex(i, int32_t(size), mode);
  ret.idx[1] = AddressIndex(i + 1, int32_t(size), mode);
  ret.weight[0] = 1.0f - f;
  ret.weight[1] = f;
  ret.count = 2;
  return ret;
}

bool IsLinear(TexFilter f)
{
  return uint32_t(f) > uint32_t(TexFilter::Point);
}
};    // namespace

// What was measured on hardware vertex processing (an RTX 4070 Ti), which this follows:
// - the LOD is the coordinate's w plus D3DSAMP_MIPMAPLODBIAS, with 8 bits of fraction, rounded
//   down.
// - the LOD is clamped to D3DSAMP_MAXMIPLEVEL and the last level. MAGFILTER applies when that's
//   0 or less, MINFILTER above.
// - with D3DSAMP_MIPFILTER NONE it's level 0, though MAXMIPLEVEL still clamps the LOD that picks
//   the filter. A point mip filter takes the nearest level, a linear one blends the two either
//   side.
// - linear (and anisotropic) filters blend 2, 4 or 8 texels, each axis addressed on its own, so a
//   border texel blends in the border colour.
// - cube maps pick the face of the largest axis, z then y then x when two are equal, and clamp at
//   the face's edges rather than filtering across faces.
Vec4 VSTexture::SampleLevel(const Vec4 &coord, float lod) const
{
  if(levels.empty())
    return Vec4(0.0f, 0.0f, 0.0f, 1.0f);

  lod += mipLODBias;
  if(!(lod == lod))
    lod = 0.0f;
  lod = RDCCLAMP(lod, -1000.0f, 1000.0f);
  lod = floorf(lod * 256.0f) / 256.0f;

  const uint32_t last = uint32_t(levels.size()) - 1;
  lod = RDCCLAMP(lod, float(RDCMIN(maxMipLevel, last)), float(last));
  const bool linear = IsLinear(lod <= 0.0f ? magFilter : minFilter);

  if(mipFilter == TexFilter::None)
    return SampleTexels(0, coord, linear);

  if(!IsLinear(mipFilter))
    return SampleTexels(uint32_t(floorf(lod + 0.5f)), coord, linear);

  const uint32_t l0 = uint32_t(floorf(lod));
  const float f = lod - float(l0);
  Vec4 ret = SampleTexels(l0, coord, linear);
  if(f > 0.0f && l0 < last)
  {
    const Vec4 next = SampleTexels(l0 + 1, coord, linear);
    for(int c = 0; c < 4; c++)
      ret[c] = ret[c] * (1.0f - f) + next[c] * f;
  }
  return ret;
}

Vec4 VSTexture::SampleTexels(uint32_t level, const Vec4 &coord, bool linear) const
{
  level = RDCMIN(level, uint32_t(levels.size()) - 1);
  const Level &l = levels[level];
  if(l.width == 0 || l.height == 0 || l.depth == 0)
    return Vec4(0.0f, 0.0f, 0.0f, 1.0f);

  AxisTaps taps[3];

  if(type == SamplerTextureType::Cube)
  {
    // the major axis picks the face (+X, -X, +Y, -Y, +Z, -Z), as D3D9 numbers them
    const float cx = coord[0], cy = coord[1], cz = coord[2];
    const float ax = fabsf(cx), ay = fabsf(cy), az = fabsf(cz);
    int32_t face;
    float sc, tc, ma;
    if(az >= ax && az >= ay)
    {
      face = cz >= 0.0f ? 4 : 5;
      sc = cz >= 0.0f ? cx : -cx;
      tc = -cy;
      ma = az;
    }
    else if(ay >= ax)
    {
      face = cy >= 0.0f ? 2 : 3;
      sc = cx;
      tc = cy >= 0.0f ? cz : -cz;
      ma = ay;
    }
    else
    {
      face = cx >= 0.0f ? 0 : 1;
      sc = cx >= 0.0f ? -cz : cz;
      tc = -cy;
      ma = ax;
    }
    if(ma == 0.0f)
      ma = 1.0f;
    taps[0] = Taps((sc / ma + 1.0f) * 0.5f, l.width, TexAddress::Clamp, linear);
    taps[1] = Taps((tc / ma + 1.0f) * 0.5f, l.height, TexAddress::Clamp, linear);
    taps[2].idx[0] = RDCMIN(face, int32_t(l.depth) - 1);
  }
  else
  {
    taps[0] = Taps(coord[0], l.width, address[0], linear);
    if(type != SamplerTextureType::Tex1D)
      taps[1] = Taps(coord[1], l.height, address[1], linear);
    if(type == SamplerTextureType::Volume)
      taps[2] = Taps(coord[2], l.depth, address[2], linear);
  }

  Vec4 ret;
  for(int z = 0; z < taps[2].count; z++)
  {
    for(int y = 0; y < taps[1].count; y++)
    {
      for(int x = 0; x < taps[0].count; x++)
      {
        const float w = taps[0].weight[x] * taps[1].weight[y] * taps[2].weight[z];
        if(w == 0.0f)
          continue;

        Vec4 t = border;
        const int32_t tx = taps[0].idx[x], ty = taps[1].idx[y], tz = taps[2].idx[z];
        if(tx >= 0 && ty >= 0 && tz >= 0)
        {
          const size_t idx = (size_t(tz) * l.height + size_t(ty)) * l.width + size_t(tx);
          t = idx < l.texels.size() ? l.texels[idx] : Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        for(int c = 0; c < 4; c++)
          ret[c] += w * t[c];
      }
    }
  }
  return ret;
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

  return m_Layout.Build(prog, m_Error);
}

bool ProgramLayout::Build(const Program &prog, rdcstr &error)
{
  match.clear();
  labels.clear();
  floatLiterals.clear();
  intLiterals.clear();
  boolLiterals.clear();

  const rdcarray<Instruction> &insts = prog.instructions;
  match.fill(insts.size(), NoMatch);

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
          error = StringFormat::Fmt("else without if at instruction %u", i);
          return false;
        }
        match[stack.back()] = i;
        stack.back() = i;
        break;
      case Opcode::EndIf:
        if(stack.empty() ||
           (insts[stack.back()].op != Opcode::If && insts[stack.back()].op != Opcode::IfC &&
            insts[stack.back()].op != Opcode::Else))
        {
          error = StringFormat::Fmt("endif without if at instruction %u", i);
          return false;
        }
        match[stack.back()] = i;
        stack.pop_back();
        break;
      case Opcode::EndLoop:
      case Opcode::EndRep:
      {
        const Opcode start = inst.op == Opcode::EndLoop ? Opcode::Loop : Opcode::Rep;
        if(stack.empty() || insts[stack.back()].op != start)
        {
          error = StringFormat::Fmt("%s without %s at instruction %u",
                                    inst.op == Opcode::EndLoop ? "endloop" : "endrep",
                                    inst.op == Opcode::EndLoop ? "loop" : "rep", i);
          return false;
        }
        match[stack.back()] = i;
        match[i] = stack.back();
        stack.pop_back();

        // the breaks inside break to here
        for(uint32_t b = match[i] + 1; b < i; b++)
        {
          const Opcode op = insts[b].op;
          if((op == Opcode::Break || op == Opcode::BreakC || op == Opcode::BreakP) &&
             match[b] == NoMatch)
            match[b] = i;
        }
        break;
      }
      case Opcode::Label:
        if(!inst.srcs.empty())
        {
          uint32_t l = inst.srcs[0].index;
          if(l >= 2048)
          {
            error = StringFormat::Fmt("label l%u out of range", l);
            return false;
          }
          while(labels.size() <= l)
            labels.push_back(NoMatch);
          labels[l] = i;
        }
        // flow control doesn't continue across subroutines
        if(!stack.empty())
        {
          error = StringFormat::Fmt("label inside flow control at instruction %u", i);
          return false;
        }
        break;
      case Opcode::Def:
      {
        Literal lit;
        lit.reg = inst.dst.index + ConstantRegisterOffset(inst.dst.type);
        memcpy(lit.value, inst.literal, sizeof(lit.value));
        floatLiterals.push_back(lit);
        break;
      }
      case Opcode::DefI:
      {
        Literal lit;
        lit.reg = inst.dst.index;
        memcpy(lit.value, inst.literal, sizeof(lit.value));
        intLiterals.push_back(lit);
        break;
      }
      case Opcode::DefB:
      {
        Literal lit = {};
        lit.reg = inst.dst.index;
        lit.value[0] = inst.literal[0];
        boolLiterals.push_back(lit);
        break;
      }
      default: break;
    }
  }

  if(!stack.empty())
  {
    error = StringFormat::Fmt("unterminated flow control starting at instruction %u", stack.back());
    return false;
  }

  for(uint32_t i = 0; i < insts.size(); i++)
  {
    const Instruction &inst = insts[i];
    const Opcode op = inst.op;
    if((op == Opcode::Break || op == Opcode::BreakC || op == Opcode::BreakP) && match[i] == NoMatch)
    {
      error = StringFormat::Fmt("break outside a loop at instruction %u", i);
      return false;
    }
    if(op == Opcode::Call || op == Opcode::CallNZ)
    {
      uint32_t l = inst.srcs.empty() ? NoMatch : inst.srcs[0].index;
      if(l >= labels.size() || labels[l] == NoMatch)
      {
        error = StringFormat::Fmt("call to missing label at instruction %u", i);
        return false;
      }
    }
  }

  return true;
}

VSConstants ProgramLayout::ApplyLiterals(const VSConstants &consts) const
{
  VSConstants ret = consts;

  for(const Literal &l : floatLiterals)
  {
    if(ret.floats.size() <= l.reg)
      ret.floats.resize(l.reg + 1);
    memcpy(ret.floats[l.reg].v, l.value, sizeof(l.value));
  }
  for(const Literal &l : intLiterals)
  {
    if(ret.ints.size() <= l.reg)
      ret.ints.resize(l.reg + 1);
    memcpy(ret.ints[l.reg].v, l.value, sizeof(l.value));
  }
  for(const Literal &l : boolLiterals)
  {
    if(ret.bools.size() <= l.reg)
      ret.bools.resize(l.reg + 1);
    ret.bools[l.reg] = l.value[0] ? 1 : 0;
  }

  return ret;
}

VSConstants VSEmulator::ApplyLiterals(const VSConstants &consts) const
{
  return m_Layout.ApplyLiterals(consts);
}

bool VSEmulator::Execute(const Vec4 inputs[MaxVSInputs], const VSConstants &consts,
                         const VSTexture textures[NumVertexSamplers], VSOutputs &out,
                         rdcstr &error) const
{
  VSThread thread;
  thread.Start(*this, inputs, consts, textures);
  bool ret = thread.Run();
  out = thread.Registers().out;
  if(!ret)
    error = thread.GetError();
  return ret;
}

bool VSThread::Fail(uint32_t ip, const rdcstr &msg)
{
  const rdcarray<Instruction> &insts = m_Emu->m_Prog->instructions;
  m_IP = ip;
  m_Error =
      StringFormat::Fmt("%s at instruction %u (%s)", msg.c_str(), ip,
                        ip < insts.size() ? OpcodeName(m_Emu->m_Version, insts[ip]).c_str() : "");
  m_Finished = true;
  m_Changes = NULL;
  return false;
}

bool VSThread::Run()
{
  if(m_Finished)
    return m_Error.empty();
  return Exec<false, false>(NULL);
}

bool VSThread::Step(rdcarray<VSRegisterChange> *changes)
{
  return changes ? Exec<true, true>(changes) : Exec<false, true>(NULL);
}

// runs instructions until the end, or only one with Single. With Record, each change is appended
// to changes. Running to the end is the mesh viewer's path for every vertex, so it keeps the
// instruction pointer in a local and has nothing for stepping in its loop.
template <bool Record, bool Single>
bool VSThread::Exec(rdcarray<VSRegisterChange> *changes)
{
  if(m_Finished)
    return false;

  const VSEmulator &emu = *m_Emu;
  const Version &version = emu.m_Version;
  const rdcarray<Instruction> &insts = emu.m_Prog->instructions;
  const uint32_t numInsts = (uint32_t)insts.size();
  VSThread &s = *this;

  uint32_t ip = m_IP;
  uint32_t executed = m_Executed;
  m_Changes = Record ? changes : NULL;

  // a failed instruction isn't counted, and leaves ip on it
  auto fail = [&](const rdcstr &msg) {
    m_Executed = executed - 1;
    return Fail(ip, msg);
  };

  for(;;)
  {
    if(++executed > MaxInstructionsPerVertex)
      return fail("Too many instructions executed, the shader may loop forever");

    // aL changes when the innermost loop does, or its counter
    int32_t loopBefore = -1, aLBefore = 0;
    if(Record)
    {
      loopBefore = InnermostLoop();
      aLBefore = aL();
    }

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
          VSCallFrame f = s.calls.back();
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
          VSCallFrame f;
          f.call = ip;
          f.label = srcs[0].index;
          f.ret = ip + 1;
          f.loopDepth = s.loops.size();
          s.calls.push_back(f);
          next = emu.m_Layout.labels[srcs[0].index] + 1;
        }
        break;
      }

      case Opcode::If:
        if(srcs.empty())
          return fail("Missing operands");
        if(!s.ReadBool(srcs[0]))
          next = emu.m_Layout.match[ip] + 1;
        break;

      case Opcode::IfC:
        if(!need(2))
          return fail("Missing operands");
        if(!Compare(inst.GetComparison(), s.Read(srcs[0])[0], s.Read(srcs[1])[0]))
          next = emu.m_Layout.match[ip] + 1;
        break;

      // reached at the end of the if block, skip the else block
      case Opcode::Else: next = emu.m_Layout.match[ip] + 1; break;

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
          s.loops.push_back(f);
        }
        break;
      }

      case Opcode::EndLoop:
      case Opcode::EndRep:
      {
        if(s.loops.empty())
          return fail("End of a loop that isn't running");
        VSLoopFrame &f = s.loops.back();
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
          next = emu.m_Layout.match[ip] + 1;
        }
        break;
      }

      case Opcode::Mov:
      case Opcode::MovA:
      {
        NEED_SRCS(1);
        s.Write<Record>(inst, s.Read(srcs[0]));
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
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]), res;
        for(int i = 0; i < 4; i++)
        {
          switch(inst.op)
          {
            case Opcode::Add: res[i] = a[i] + b[i]; break;
            case Opcode::Sub: res[i] = a[i] - b[i]; break;
            case Opcode::Mul: res[i] = a[i] * b[i]; break;
            case Opcode::Min: res[i] = a[i] < b[i] ? a[i] : b[i]; break;
            case Opcode::Max: res[i] = a[i] >= b[i] ? a[i] : b[i]; break;
            case Opcode::Slt: res[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
            case Opcode::Sge: res[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
            default: break;
          }
        }
        s.Write<Record>(inst, res);
        break;
      }

      case Opcode::Mad:
      case Opcode::Lrp:
      case Opcode::Cmp:
      case Opcode::Cnd:
      {
        NEED_SRCS(3);
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]), c = s.Read(srcs[2]), res;
        for(int i = 0; i < 4; i++)
        {
          switch(inst.op)
          {
            case Opcode::Mad: res[i] = a[i] * b[i] + c[i]; break;
            case Opcode::Lrp: res[i] = a[i] * (b[i] - c[i]) + c[i]; break;
            case Opcode::Cmp: res[i] = a[i] >= 0.0f ? b[i] : c[i]; break;
            case Opcode::Cnd: res[i] = a[i] > 0.5f ? b[i] : c[i]; break;
            default: break;
          }
        }
        s.Write<Record>(inst, res);
        break;
      }

      case Opcode::Dp3:
      case Opcode::Dp4:
      {
        NEED_SRCS(2);
        s.Write<Record>(
            inst, Splat(Dot(s.Read(srcs[0]), s.Read(srcs[1]), inst.op == Opcode::Dp3 ? 3 : 4)));
        break;
      }

      case Opcode::Dp2Add:
      {
        NEED_SRCS(3);
        s.Write<Record>(inst, Splat(Dot(s.Read(srcs[0]), s.Read(srcs[1]), 2) + s.Read(srcs[2])[0]));
        break;
      }

      case Opcode::Abs:
      case Opcode::Frc:
      case Opcode::Sgn:
      {
        NEED_SRCS(1);
        Vec4 a = s.Read(srcs[0]), res;
        for(int i = 0; i < 4; i++)
        {
          if(inst.op == Opcode::Abs)
            res[i] = fabsf(a[i]);
          else if(inst.op == Opcode::Frc)
            res[i] = a[i] - floorf(a[i]);
          else
            res[i] = a[i] > 0.0f ? 1.0f : (a[i] < 0.0f ? -1.0f : 0.0f);
        }
        s.Write<Record>(inst, res);
        break;
      }

      case Opcode::Rcp:
      {
        NEED_SRCS(1);
        float f = s.Read(srcs[0])[3];
        float res = f == 1.0f ? 1.0f : (f == 0.0f ? INFINITY : 1.0f / f);
        s.Write<Record>(inst, Splat(res));
        break;
      }

      case Opcode::Rsq:
      {
        NEED_SRCS(1);
        float f = fabsf(s.Read(srcs[0])[3]);
        float res = f == 1.0f ? 1.0f : (f == 0.0f ? INFINITY : 1.0f / sqrtf(f));
        s.Write<Record>(inst, Splat(res));
        break;
      }

      case Opcode::Exp:
      {
        NEED_SRCS(1);
        s.Write<Record>(inst, Splat(exp2f(s.Read(srcs[0])[3])));
        break;
      }

      case Opcode::Log:
      {
        NEED_SRCS(1);
        float f = fabsf(s.Read(srcs[0])[3]);
        s.Write<Record>(inst, Splat(f != 0.0f ? log2f(f) : -FLT_MAX));
        break;
      }

      case Opcode::ExpP:
      {
        NEED_SRCS(1);
        float w = s.Read(srcs[0])[3];
        if(version.major < 2)
        {
          float fl = floorf(w);
          s.Write<Record>(inst, Vec4(exp2f(fl), w - fl, PartialPrecision(exp2f(w)), 1.0f));
        }
        else
        {
          s.Write<Record>(inst, Splat(PartialPrecision(exp2f(w))));
        }
        break;
      }

      case Opcode::LogP:
      {
        NEED_SRCS(1);
        float f = fabsf(s.Read(srcs[0])[3]);
        if(version.major < 2)
        {
          if(f != 0.0f)
          {
            uint32_t bits;
            memcpy(&bits, &f, sizeof(bits));
            float exponent = float(int32_t((bits >> 23) & 0xFF) - 127);
            uint32_t mbits = (bits & 0x7FFFFF) | 0x3F800000;
            float mantissa;
            memcpy(&mantissa, &mbits, sizeof(mantissa));
            s.Write<Record>(inst, Vec4(exponent, mantissa, PartialPrecision(log2f(f)), 1.0f));
          }
          else
          {
            s.Write<Record>(inst, Vec4(-FLT_MAX, 1.0f, -FLT_MAX, 1.0f));
          }
        }
        else
        {
          s.Write<Record>(inst, Splat(f != 0.0f ? PartialPrecision(log2f(f)) : -FLT_MAX));
        }
        break;
      }

      case Opcode::Pow:
      {
        NEED_SRCS(2);
        s.Write<Record>(inst, Splat(powf(fabsf(s.Read(srcs[0])[3]), s.Read(srcs[1])[3])));
        break;
      }

      case Opcode::Lit:
      {
        NEED_SRCS(1);
        Vec4 a = s.Read(srcs[0]);
        const float MaxPower = 127.9961f;
        float power = RDCCLAMP(a[3], -MaxPower, MaxPower);
        Vec4 res(1.0f, 0.0f, 0.0f, 1.0f);
        if(a[0] > 0.0f)
        {
          res[1] = a[0];
          if(a[1] > 0.0f)
            res[2] = powf(a[1], power);
        }
        s.Write<Record>(inst, res);
        break;
      }

      case Opcode::Dst:
      {
        NEED_SRCS(2);
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]);
        s.Write<Record>(inst, Vec4(1.0f, a[1] * b[1], a[2], b[3]));
        break;
      }

      case Opcode::Crs:
      {
        NEED_SRCS(2);
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]);
        s.Write<Record>(inst, Vec4(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
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
        Vec4 res;
        for(int i = 0; i < 4; i++)
          res[i] = a[i] == 0.0f ? 0.0f : a[i] * f;
        s.Write<Record>(inst, res);
        break;
      }

      case Opcode::SinCos:
      {
        NEED_SRCS(1);
        // vs_2_0's two extra constants only help hardware approximate it
        float a = s.Read(srcs[0])[3];
        s.Write<Record>(inst, Vec4(cosf(a), sinf(a), 0.0f, 0.0f));
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
        Vec4 a = s.Read(srcs[0]), res;
        for(int i = 0; i < rows; i++)
          res[i] = Dot(a, s.Read(srcs[1], i), size);
        s.Write<Record>(inst, res);
        break;
      }

      case Opcode::SetP:
      {
        NEED_SRCS(2);
        Vec4 a = s.Read(srcs[0]), b = s.Read(srcs[1]), res;
        for(int i = 0; i < 4; i++)
          res[i] = Compare(inst.GetComparison(), a[i], b[i]) ? 1.0f : 0.0f;
        s.Write<Record>(inst, res);
        break;
      }

      case Opcode::TexLdl:
      {
        NEED_SRCS(2);
        if(srcs[1].type != RegType::Sampler)
          return fail("texldl without a sampler");
        Vec4 coord = s.Read(srcs[0]);
        Vec4 res(0.0f, 0.0f, 0.0f, 1.0f);
        if(srcs[1].index < NumVertexSamplers && m_Textures)
          res = m_Textures[srcs[1].index].SampleLevel(coord, coord[3]);
        // the sampler's swizzle applies to the result
        Vec4 sw;
        for(int i = 0; i < 4; i++)
          sw[i] = res[srcs[1].swizzle[i] & 3];
        s.Write<Record>(inst, sw);
        break;
      }

      default:
        return fail(
            StringFormat::Fmt("Unsupported instruction %s", OpcodeName(version, inst).c_str()));
    }

#undef NEED_SRCS

    if(Record)
    {
      if(InnermostLoop() != loopBefore || aL() != aLBefore)
      {
        VSRegisterChange change;
        change.type = RegType::Loop;
        change.i.v[0] = aLBefore;
        changes->push_back(change);
      }
    }

    ip = next;
    if(ip >= numInsts)
    {
      m_Finished = true;
      break;
    }
    if(Single)
      break;
  }

  m_IP = ip;
  m_Executed = executed;
  m_Changes = NULL;
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

rdcarray<VSInputBinding> BindVSInputs(const Program &prog, const rdcarray<InputElement> &elements)
{
  rdcarray<VSInputBinding> ret;
  for(const Instruction &inst : prog.instructions)
  {
    if(inst.op != Opcode::Dcl || inst.dst.type != RegType::Input || inst.dst.index >= MaxVSInputs)
      continue;
    for(const InputElement &el : elements)
    {
      if(el.usage == inst.usage && el.usageIndex == inst.usageIndex)
      {
        VSInputBinding b;
        b.reg = inst.dst.index;
        b.element = &el;
        ret.push_back(b);
        break;
      }
    }
  }
  return ret;
}

void FetchVSInputs(const DrawInputs &draw, const rdcarray<VSInputBinding> &bindings,
                   uint32_t vertex, uint32_t instance, Vec4 inputs[MaxVSInputs])
{
  for(uint32_t i = 0; i < MaxVSInputs; i++)
    inputs[i] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
  for(const VSInputBinding &b : bindings)
    FetchElement(*b.element, draw.streams.data(), (uint32_t)draw.streams.size(), vertex, instance,
                 inputs[b.reg]);
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
  rdcarray<VSInputBinding> inputMap;

  FFPInputs ffpInputs;
  FFPOutputLayout ffpLayout;

  if(vs)
  {
    inputMap = BindVSInputs(vs->GetProgram(), draw.elements);
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
      FetchVSInputs(draw, inputMap, vertices[v], instance, inputs);

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
        // the register's components the element has, in order: a vs_3_0 register can hold two or
        // more elements (dcl_texcoord0 o1.xy, dcl_texcoord1 o1.zw)
        uint32_t c = 0;
        for(uint32_t comp = 0; comp < 4 && c < sig.compCount; comp++)
          if(sig.regChannelMask & (1 << comp))
            values[c++] = reg ? (*reg)[comp] : 0.0f;
        for(; c < sig.compCount; c++)
          values[c] = 0.0f;
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

#include <chrono>
#include "catch/catch.hpp"
#include "d3d9_bytecode_test.h"
#include "d3d9_shader_reflect.h"

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

bool SameBits(const Vec4 &a, const Vec4 &b)
{
  return memcmp(a.v, b.v, sizeof(a.v)) == 0;
}

bool SameBits(const IVec4 &a, const IVec4 &b)
{
  return memcmp(a.v, b.v, sizeof(a.v)) == 0;
}

// what a debugger would show of a thread's registers, kept up to date only from the changes
// each step reports
struct Shadow
{
  Vec4 r[MaxVSTemps];
  IVec4 a0;
  IVec4 p;
  int32_t aL = 0;
  int32_t loop = -1;
  VSOutputs out;

  void Take(const VSRegisters &regs)
  {
    for(uint32_t i = 0; i < MaxVSTemps; i++)
      r[i] = regs.r[i];
    a0 = regs.a0;
    for(int i = 0; i < 4; i++)
      p.v[i] = regs.p[i] ? 1 : 0;
    aL = regs.aL();
    loop = regs.InnermostLoop();
    out = regs.out;
  }

  static const Vec4 *Output(const VSOutputs &o, RegType type, uint32_t index)
  {
    if(type == RegType::RastOut && index < 3)
      return &o.rast[index];
    if(type == RegType::AttrOut && index < 2)
      return &o.attr[index];
    if(type == RegType::TexCrdOutOrOutput && index < 12)
      return &o.tex[index];
    return NULL;
  }

  // checks a change's old value is what the shadow has, then takes the new value from regs.
  // Returns false if the change is wrong.
  bool Apply(const VSRegisterChange &c, const VSRegisters &regs)
  {
    switch(c.type)
    {
      case RegType::Temp:
        if(c.index >= MaxVSTemps || !SameBits(c.f, r[c.index]))
          return false;
        r[c.index] = regs.r[c.index];
        return true;
      case RegType::AddrOrTexture:
        if(!SameBits(c.i, a0))
          return false;
        a0 = regs.a0;
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
      default:
      {
        Vec4 *o = (Vec4 *)Output(out, c.type, c.index);
        if(!o || !SameBits(c.f, *o))
          return false;
        *o = *Output(regs.out, c.type, c.index);
        return true;
      }
    }
  }

  // the registers that differ from regs, which no change reported
  rdcstr Differences(const VSRegisters &regs) const
  {
    Shadow now;
    now.Take(regs);
    rdcstr ret;
    for(uint32_t i = 0; i < MaxVSTemps; i++)
      if(!SameBits(r[i], now.r[i]))
        ret += StringFormat::Fmt("r%u ", i);
    if(!SameBits(a0, now.a0))
      ret += "a0 ";
    if(!SameBits(p, now.p))
      ret += "p0 ";
    if(aL != now.aL || loop != now.loop)
      ret += "aL ";
    if(memcmp(&out, &now.out, sizeof(out)) != 0)
      ret += "outputs ";
    return ret;
  }
};

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
    bool ret = emu.Execute(inputs, c, textures, out, error);
    CheckStepping(c, ret);
    return ret;
  }

  // steps the same run one instruction at a time, checking it ends the same way and that each
  // step's changes account for everything that changed
  void CheckStepping(const VSConstants &c, bool executed)
  {
    VSThread thread;
    thread.Start(emu, inputs, c, textures);

    Shadow shadow;
    shadow.Take(thread.Registers());

    rdcarray<VSRegisterChange> changes;
    bool ok = true;
    while(!thread.Finished())
    {
      const uint32_t inst = thread.NextInstruction();
      changes.clear();
      if(!thread.Step(&changes))
      {
        ok = false;
        // nothing changed by a failed instruction
        CHECK(changes.empty());
        CHECK(thread.NextInstruction() == inst);
        break;
      }
      for(const VSRegisterChange &ch : changes)
      {
        INFO(StringFormat::Fmt("instruction %u, change to type %u index %u", inst,
                               uint32_t(ch.type), ch.index)
                 .c_str());
        CHECK(shadow.Apply(ch, thread.Registers()));
      }
      rdcstr diff = shadow.Differences(thread.Registers());
      INFO(StringFormat::Fmt("instruction %u changed %s without reporting it", inst, diff.c_str())
               .c_str());
      CHECK(diff.empty());
    }

    CHECK(ok == executed);
    CHECK(thread.GetError() == error);
    CHECK(memcmp(&thread.Registers().out, &out, sizeof(out)) == 0);
    // nothing runs after the end
    CHECK_FALSE(thread.Step(&changes));
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

// a thread's run, one Step at a time: the instruction each step ran and what it changed
struct StepTrace
{
  rdcarray<uint32_t> insts;
  rdcarray<rdcarray<VSRegisterChange>> changes;
};

StepTrace StepAll(VSThread &thread)
{
  StepTrace ret;
  while(!thread.Finished())
  {
    ret.insts.push_back(thread.NextInstruction());
    ret.changes.push_back({});
    if(!thread.Step(&ret.changes.back()))
      break;
  }
  return ret;
}

TEST_CASE("D3D9 vertex shader emulation - stepping", "[d3d9][vsemu]")
{
  Program prog;
  VSEmulator emu;
  Vec4 inputs[MaxVSInputs];
  VSConstants consts;
  consts.floats.resize(4);
  consts.ints.resize(4);
  consts.bools.resize(4);

  auto init = [&](const rdcarray<uint32_t> &tokens) {
    ParseProgram(tokens.data(), tokens.size(), prog);
    REQUIRE(emu.Init(prog));
    consts = emu.ApplyLiterals(consts);
  };

  SECTION("each step's changes, with the values before")
  {
    Assembler a(2, 1);
    a.def(0, 1, 2, 3, 4);
    a.def(1, 10, 0, 0, 0);
    a.op(Opcode::Mov, {DR(0), C(0)});
    a.op(Opcode::Add, {DR(0, 0x1), R(0), C(1, X)});
    // p0 is all false, so this writes nothing
    a.op(Opcode::Mov, {DR(1), Src(RegType::Predicate, 0), C(0)}, 0, PredBit);
    a.op(Opcode::MovA, {Dst(RegType::AddrOrTexture, 0, 0x1), C(0, Y)});
    a.op(Opcode::SetP, {Dst(RegType::Predicate, 0, 0x2), C(0), C(1)}, uint32_t(Comparison::GT));
    a.op(Opcode::Mov, {OPos(), R(0)});
    init(a.finish());

    VSThread thread;
    thread.Start(emu, inputs, consts, NULL);
    CHECK_FALSE(thread.Finished());
    CHECK(thread.NextInstruction() == 0);

    StepTrace t = StepAll(thread);
    CHECK(thread.Finished());
    CHECK_FALSE(thread.Failed());
    CHECK(thread.InstructionsExecuted() == 8);
    CHECK(t.insts == rdcarray<uint32_t>({0, 1, 2, 3, 4, 5, 6, 7}));
    REQUIRE(t.changes.size() == 8);

    // the defs change nothing
    CHECK(t.changes[0].empty());
    CHECK(t.changes[1].empty());

    REQUIRE(t.changes[2].size() == 1);
    CHECK(t.changes[2][0].type == RegType::Temp);
    CHECK(t.changes[2][0].index == 0);
    CheckVec(t.changes[2][0].f, 0, 0, 0, 0);

    REQUIRE(t.changes[3].size() == 1);
    CHECK(t.changes[3][0].type == RegType::Temp);
    CheckVec(t.changes[3][0].f, 1, 2, 3, 4);

    CHECK(t.changes[4].empty());

    REQUIRE(t.changes[5].size() == 1);
    CHECK(t.changes[5][0].type == RegType::AddrOrTexture);
    CHECK(t.changes[5][0].i.v[0] == 0);

    REQUIRE(t.changes[6].size() == 1);
    CHECK(t.changes[6][0].type == RegType::Predicate);
    CHECK(t.changes[6][0].i.v[1] == 0);

    REQUIRE(t.changes[7].size() == 1);
    CHECK(t.changes[7][0].type == RegType::RastOut);
    CHECK(t.changes[7][0].index == 0);
    CheckVec(t.changes[7][0].f, 0, 0, 0, 0);

    const VSRegisters &regs = thread.Registers();
    CheckVec(regs.r[0], 11, 2, 3, 4);
    CHECK(regs.a0.v[0] == 2);
    CHECK(regs.p[1]);
    CHECK_FALSE(regs.p[0]);
    CheckVec(regs.out.rast[0], 11, 2, 3, 4);

    // nothing runs after the end
    rdcarray<VSRegisterChange> changes;
    CHECK_FALSE(thread.Step(&changes));
    CHECK(changes.empty());
    CHECK_FALSE(thread.Failed());
  }

  SECTION("aL changes when a loop starts, steps and ends, not for rep")
  {
    Assembler a(2, 0);
    a.defi(0, 2, 5, 3, 0);
    a.defi(1, 1, 0, 0, 0);
    a.op(Opcode::Mov, {DR(0), C(2)});
    a.op(Opcode::Loop, {Src(RegType::Loop, 0), Src(RegType::ConstInt, 0)});
    a.op(Opcode::Rep, {Src(RegType::ConstInt, 1)});
    a.op(Opcode::Add, {DR(0, 0x1), R(0), Src(RegType::Loop, 0)});
    a.op(Opcode::EndRep, {});
    a.op(Opcode::EndLoop, {});
    init(a.finish());

    VSThread thread;
    thread.Start(emu, inputs, consts, NULL);
    StepTrace t = StepAll(thread);
    CHECK_FALSE(thread.Failed());
    CHECK(t.insts == rdcarray<uint32_t>({0, 1, 2, 3, 4, 5, 6, 7, 4, 5, 6, 7}));

    auto loopChange = [&](size_t step) -> int32_t {
      for(const VSRegisterChange &c : t.changes[step])
        if(c.type == RegType::Loop)
          return c.i.v[0];
      return -1;
    };
    // loop: aL was 0 outside it
    CHECK(loopChange(3) == 0);
    // rep and endrep don't change it
    CHECK(loopChange(4) == -1);
    CHECK(loopChange(6) == -1);
    // the first endloop: 5 -> 8, the second ends the loop
    CHECK(loopChange(7) == 5);
    CHECK(loopChange(11) == 8);
    CHECK(thread.Registers().aL() == 0);
    CHECK(thread.Registers().loops.empty());
    CHECK(thread.Registers().r[0][0] == 13.0f);
  }

  SECTION("the call stack")
  {
    Assembler a(2, 0);
    a.op(Opcode::Call, {Src(RegType::Label, 0)});
    a.op(Opcode::Ret, {});
    a.op(Opcode::Label, {Src(RegType::Label, 0)});
    a.op(Opcode::Call, {Src(RegType::Label, 1)});
    a.op(Opcode::Ret, {});
    a.op(Opcode::Label, {Src(RegType::Label, 1)});
    a.op(Opcode::Mov, {DR(0), V(0)});
    a.op(Opcode::Ret, {});
    init(a.finish());

    VSThread thread;
    thread.Start(emu, inputs, consts, NULL);
    rdcarray<size_t> depths;
    rdcarray<uint32_t> insts;
    while(!thread.Finished())
    {
      insts.push_back(thread.NextInstruction());
      depths.push_back(thread.Registers().calls.size());
      if(thread.NextInstruction() == 6)
      {
        const rdcarray<VSCallFrame> &calls = thread.Registers().calls;
        REQUIRE(calls.size() == 2);
        CHECK(calls[0].call == 0);
        CHECK(calls[0].label == 0);
        CHECK(calls[0].ret == 1);
        CHECK(calls[1].call == 3);
        CHECK(calls[1].label == 1);
        CHECK(calls[1].ret == 4);
      }
      REQUIRE(thread.Step());
    }
    CHECK(insts == rdcarray<uint32_t>({0, 3, 6, 7, 4, 1}));
    CHECK(depths == rdcarray<size_t>({0, 1, 2, 2, 1, 0}));
  }

  SECTION("a failed instruction ends the thread and changes nothing")
  {
    // l0 calls itself until the calls nest too deeply
    Assembler a(2, 0);
    a.op(Opcode::Mov, {DR(0), V(0)});
    a.op(Opcode::Call, {Src(RegType::Label, 0)});
    a.op(Opcode::Ret, {});
    a.op(Opcode::Label, {Src(RegType::Label, 0)});
    a.op(Opcode::Call, {Src(RegType::Label, 0)});
    a.op(Opcode::Ret, {});
    init(a.finish());

    inputs[0] = Vec4(1, 2, 3, 4);

    VSThread thread;
    thread.Start(emu, inputs, consts, NULL);
    StepTrace t = StepAll(thread);
    CHECK(thread.Finished());
    CHECK(thread.Failed());
    CHECK(thread.GetError().contains("Calls nested too deeply at instruction 4"));
    CHECK(thread.NextInstruction() == 4);
    CHECK(t.changes.back().empty());
    CHECK(thread.Registers().calls.size() == 16);
    CheckVec(thread.Registers().r[0], 1, 2, 3, 4);
    CHECK_FALSE(thread.Step());

    // Execute fails the same way
    VSOutputs out;
    rdcstr error;
    CHECK_FALSE(emu.Execute(inputs, consts, NULL, out, error));
    CHECK(error == thread.GetError());
  }

  SECTION("an emulator that failed to initialise")
  {
    Assembler a(2, 0);
    a.op(Opcode::EndIf, {});
    rdcarray<uint32_t> tokens = a.finish();
    ParseProgram(tokens.data(), tokens.size(), prog);
    REQUIRE_FALSE(emu.Init(prog));

    VSThread thread;
    thread.Start(emu, inputs, consts, NULL);
    CHECK(thread.Finished());
    CHECK(thread.Failed());
    CHECK(thread.GetError() == emu.GetError());
    CHECK_FALSE(thread.Step());
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
  // with a point mip filter, the nearest level to the LOD in w
  tex.mipFilter = TexFilter::Point;
  CHECK(sample(1, Vec4(0.9f, 0.6f, 0.0f, 0.6f)) == 101.0f);
  CHECK(sample(1, Vec4(0.9f, 0.6f, 0.0f, 0.4f)) == 7.0f);
  CHECK(sample(1, Vec4(0.9f, 0.6f, 0.0f, 9.0f)) == 101.0f);
  // the most detailed level is MAXMIPLEVEL
  tex.maxMipLevel = 1;
  CHECK(sample(1, Vec4(0.1f, 0.1f, 0.0f, 0.0f)) == 100.0f);
  // and without a mip filter it's always level 0
  tex.mipFilter = TexFilter::None;
  CHECK(sample(1, Vec4(0.1f, 0.1f, 0.0f, 0.0f)) == 0.0f);
  CHECK(sample(1, Vec4(0.1f, 0.1f, 0.0f, 5.0f)) == 0.0f);
  tex.maxMipLevel = 0;
  // clamp, border and mirror
  tex.address[0] = TexAddress::Clamp;
  CHECK(sample(1, Vec4(1.5f, 0.1f, 0.0f, 0.0f)) == 3.0f);
  tex.address[0] = TexAddress::Border;
  tex.border = Vec4(-1.0f, 0.0f, 0.0f, 0.0f);
  CHECK(sample(1, Vec4(1.5f, 0.1f, 0.0f, 0.0f)) == -1.0f);
  tex.address[0] = TexAddress::Mirror;
  CHECK(sample(1, Vec4(1.1f, 0.1f, 0.0f, 0.0f)) == 3.0f);

  // cube faces by major axis, z then y then x when two are as long
  CHECK(sample(2, Vec4(1.0f, 1.0f, 0.3f, 0.0f)) == 2.0f);
  CHECK(sample(2, Vec4(1.0f, -1.0f, 1.0f, 0.0f)) == 4.0f);
  CHECK(sample(2, Vec4(-1.0f, 0.5f, -1.0f, 0.0f)) == 5.0f);
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

TEST_CASE("D3D9 vertex shader emulation - texldl filtering", "[d3d9][vsemu]")
{
  // 4x2 with x + 4y in red, then a 2x1 level of 100 and 101
  VSTexture tex;
  tex.levels.resize(2);
  tex.levels[0].width = 4;
  tex.levels[0].height = 2;
  for(uint32_t i = 0; i < 8; i++)
    tex.levels[0].texels.push_back(Vec4(float(i), 0.0f, 0.0f, 1.0f));
  tex.levels[1].width = 2;
  tex.levels[1].height = 1;
  tex.levels[1].texels.push_back(Vec4(100.0f, 0.0f, 0.0f, 1.0f));
  tex.levels[1].texels.push_back(Vec4(101.0f, 0.0f, 0.0f, 1.0f));
  tex.magFilter = tex.minFilter = TexFilter::Linear;

  auto red = [&tex](float u, float v, float lod) {
    return tex.SampleLevel(Vec4(u, v, 0.0f, lod), lod)[0];
  };
  auto closeTo = [](float a, float b) { return fabsf(a - b) < 1e-5f; };

  SECTION("bilinear")
  {
    // texel centres are at half texels: v = 0.25 is row 0's centre
    CHECK(closeTo(red(0.375f, 0.25f, 0.0f), 1.0f));
    CHECK(closeTo(red(0.5f, 0.25f, 0.0f), 1.5f));
    CHECK(closeTo(red(0.375f, 0.5f, 0.0f), 3.0f));
    CHECK(closeTo(red(0.5f, 0.5f, 0.0f), 3.5f));
    // the weight keeps 8 bits, rounded to the nearest: 0.3 becomes 77/256
    CHECK(closeTo(red(0.45f, 0.25f, 0.0f), 1.0f + 77.0f / 256.0f));
    // the anisotropic filter is linear
    tex.magFilter = TexFilter::Anisotropic;
    CHECK(closeTo(red(0.5f, 0.25f, 0.0f), 1.5f));
  }

  SECTION("addressing each texel")
  {
    // u = 0 is between texels -1 and 0
    tex.border = Vec4(-1.0f, 0.0f, 0.0f, 0.0f);
    CHECK(closeTo(red(0.0f, 0.25f, 0.0f), 1.5f));
    tex.address[0] = TexAddress::Clamp;
    CHECK(closeTo(red(0.0f, 0.25f, 0.0f), 0.0f));
    tex.address[0] = TexAddress::Border;
    CHECK(closeTo(red(0.0f, 0.25f, 0.0f), -0.5f));
    tex.address[0] = TexAddress::Mirror;
    CHECK(closeTo(red(0.0f, 0.25f, 0.0f), 0.0f));
    CHECK(closeTo(red(1.0f, 0.25f, 0.0f), 3.0f));
    tex.address[0] = TexAddress::MirrorOnce;
    CHECK(closeTo(red(0.0f, 0.25f, 0.0f), 0.0f));
    CHECK(closeTo(red(1.0f, 0.25f, 0.0f), 3.0f));
    // the border colour blends in on each axis
    tex.address[0] = TexAddress::Clamp;
    tex.address[1] = TexAddress::Border;
    CHECK(closeTo(red(0.375f, 0.0f, 0.0f), 0.5f * 1.0f + 0.5f * -1.0f));
  }

  SECTION("magnification and minification")
  {
    // without a mip filter: MAGFILTER at a LOD of 0 or less, MINFILTER above
    tex.minFilter = TexFilter::Point;
    CHECK(closeTo(red(0.5f, 0.25f, -1.0f), 1.5f));
    CHECK(closeTo(red(0.5f, 0.25f, 0.0f), 1.5f));
    CHECK(closeTo(red(0.5f, 0.25f, 0.5f), 2.0f));
    // the LOD keeps 8 bits of fraction, rounded down: 1/512 is 0
    CHECK(closeTo(red(0.5f, 0.25f, 1.0f / 512.0f), 1.5f));
    CHECK(closeTo(red(0.5f, 0.25f, 1.0f / 256.0f), 2.0f));

    // with one, the LOD clamped to MAXMIPLEVEL decides
    tex.magFilter = TexFilter::Point;
    tex.minFilter = TexFilter::Linear;
    tex.mipFilter = TexFilter::Point;
    tex.maxMipLevel = 1;
    CHECK(closeTo(red(0.5f, 0.5f, -1.0f), 100.5f));
    // without one it's level 0, but MAXMIPLEVEL still picks the filter
    tex.mipFilter = TexFilter::None;
    CHECK(closeTo(red(0.5f, 0.25f, -1.0f), 1.5f));
    tex.maxMipLevel = 0;
    CHECK(closeTo(red(0.5f, 0.25f, -1.0f), 2.0f));
  }

  SECTION("mip levels")
  {
    tex.magFilter = tex.minFilter = TexFilter::Point;
    tex.mipFilter = TexFilter::Linear;
    // level 0's texel 2 and level 1's texel 1
    CHECK(closeTo(red(0.6f, 0.25f, 0.5f), 0.5f * 2.0f + 0.5f * 101.0f));
    // 0.3 becomes 76/256, rounded down
    const float f = 76.0f / 256.0f;
    CHECK(closeTo(red(0.6f, 0.25f, 0.3f), (1.0f - f) * 2.0f + f * 101.0f));
    // beyond the last level
    CHECK(closeTo(red(0.6f, 0.25f, 3.0f), 101.0f));
    // the bias adds to the LOD
    tex.mipLODBias = 0.25f;
    CHECK(closeTo(red(0.6f, 0.25f, 0.25f), 0.5f * 2.0f + 0.5f * 101.0f));
    tex.mipLODBias = 0.0f;
    // the point mip filter rounds to the nearest level
    tex.mipFilter = TexFilter::Point;
    CHECK(closeTo(red(0.6f, 0.25f, 0.49f), 2.0f));
    CHECK(closeTo(red(0.6f, 0.25f, 0.5f), 101.0f));
  }

  SECTION("cube maps clamp at the face's edges")
  {
    VSTexture cube;
    cube.type = SamplerTextureType::Cube;
    cube.magFilter = cube.minFilter = TexFilter::Linear;
    cube.levels.resize(1);
    cube.levels[0].width = cube.levels[0].height = 2;
    cube.levels[0].depth = 6;
    for(uint32_t i = 0; i < 24; i++)
      cube.levels[0].texels.push_back(Vec4(float(i), 0.0f, 0.0f, 1.0f));
    // +Z is face 4, texels 16 to 19
    CHECK(closeTo(cube.SampleLevel(Vec4(0.0f, 0.0f, 1.0f, 0.0f), 0.0f)[0], 17.5f));
    // at the right edge only its right column
    CHECK(closeTo(cube.SampleLevel(Vec4(0.99f, 0.0f, 1.0f, 0.0f), 0.0f)[0], 18.0f));
  }

  SECTION("volumes are trilinear")
  {
    VSTexture vol;
    vol.type = SamplerTextureType::Volume;
    vol.magFilter = vol.minFilter = TexFilter::Linear;
    vol.levels.resize(1);
    vol.levels[0].width = vol.levels[0].height = vol.levels[0].depth = 2;
    for(uint32_t i = 0; i < 8; i++)
      vol.levels[0].texels.push_back(Vec4(float(i), 0.0f, 0.0f, 1.0f));
    CHECK(closeTo(vol.SampleLevel(Vec4(0.5f, 0.5f, 0.5f, 0.0f), 0.0f)[0], 3.5f));
    CHECK(closeTo(vol.SampleLevel(Vec4(0.25f, 0.25f, 0.5f, 0.0f), 0.0f)[0], 2.0f));
    vol.address[2] = TexAddress::Border;
    vol.border = Vec4(10.0f, 0.0f, 0.0f, 0.0f);
    CHECK(closeTo(vol.SampleLevel(Vec4(0.25f, 0.25f, 1.0f, 0.0f), 0.0f)[0],
                  0.5f * 4.0f + 0.5f * 10.0f));
  }
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

TEST_CASE("D3D9 post-transform draws - vs_3_0 outputs packed into one register", "[d3d9][vsemu]")
{
  // o1 = v0 + (1, 2, 3, 4) holds TEXCOORD0 in .xy and TEXCOORD1 in .zw, o2 = v0 * 2 holds
  // TEXCOORD2 in .xz and FOG in .y (declared after TEXCOORD2), and o3 = v0 holds COLOR0 in .yzw
  Assembler a(3, 0);
  a.def(0, 1.0f, 2.0f, 3.0f, 4.0f);
  a.dcl(DeclUsage::Position, 0, 0);
  a.dcl(DeclUsage::Position, 0, 0, RegType::TexCrdOutOrOutput);
  a.dcl(DeclUsage::TexCoord, 0, 1, RegType::TexCrdOutOrOutput, 0x3);
  a.dcl(DeclUsage::TexCoord, 1, 1, RegType::TexCrdOutOrOutput, 0xC);
  a.dcl(DeclUsage::TexCoord, 2, 2, RegType::TexCrdOutOrOutput, 0x5);
  a.dcl(DeclUsage::Fog, 0, 2, RegType::TexCrdOutOrOutput, 0x2);
  a.dcl(DeclUsage::Color, 0, 3, RegType::TexCrdOutOrOutput, 0xE);
  a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 0), V(0)});
  a.op(Opcode::Add, {Dst(RegType::TexCrdOutOrOutput, 1), V(0), C(0)});
  a.op(Opcode::Add, {Dst(RegType::TexCrdOutOrOutput, 2, 0x7), V(0), V(0)});
  a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 3, 0xE), V(0)});
  rdcarray<uint32_t> tokens = a.finish();

  ShaderInfo info;
  info.Init(ResourceId(), tokens.data(), tokens.size());
  VSEmulator emu;
  REQUIRE(emu.Init(info.GetProgram()));

  // an element for each declaration, by register then first component
  const rdcarray<SigParameter> &sig = info.GetReflection().outputSignature;
  REQUIRE(sig.size() == 6);
  CHECK(sig[0].semanticIdxName == "POSITION");
  CHECK(sig[1].semanticIdxName == "TEXCOORD0");
  CHECK(sig[1].regIndex == 1);
  CHECK(sig[1].regChannelMask == 0x3);
  CHECK(sig[1].compCount == 2);
  CHECK(sig[2].semanticIdxName == "TEXCOORD1");
  CHECK(sig[2].regIndex == 1);
  CHECK(sig[2].regChannelMask == 0xC);
  CHECK(sig[2].compCount == 2);
  CHECK(sig[3].semanticIdxName == "TEXCOORD2");
  CHECK(sig[3].regChannelMask == 0x5);
  CHECK(sig[3].compCount == 2);
  CHECK(sig[3].channelUsedMask == 0x5);
  CHECK(sig[4].semanticIdxName == "FOG");
  CHECK(sig[4].regIndex == 2);
  CHECK(sig[4].compCount == 1);
  CHECK(sig[5].semanticIdxName == "COLOR");
  CHECK(sig[5].regChannelMask == 0xE);
  CHECK(sig[5].compCount == 3);

  DrawInputs draw;
  draw.streams.resize(1);
  float v[4] = {10.0f, 20.0f, 30.0f, 40.0f};
  draw.streams[0].data.append((const byte *)v, sizeof(v));
  draw.streams[0].stride = 16;
  InputElement pos;
  pos.type = DeclType::Float4;
  draw.elements = {pos};
  draw.numVertices = 1;

  VSTexture textures[NumVertexSamplers];
  VSConstants consts = emu.ApplyLiterals(VSConstants());
  PostVSResult res;
  RunPostVS(draw, 0, &emu, consts, textures, NULL, sig, res);
  INFO(res.status.c_str());
  REQUIRE(res.status.empty());
  CHECK(res.stride == (4 + 2 + 2 + 2 + 1 + 3) * 4);
  rdcarray<float> f;
  f.resize(res.stride / 4);
  memcpy(f.data(), res.vertexData.data(), res.stride);
  CHECK(f == rdcarray<float>({10.0f, 20.0f, 30.0f, 40.0f,    // POSITION
                              11.0f, 22.0f,                  // TEXCOORD0: o1.xy
                              33.0f, 44.0f,                  // TEXCOORD1: o1.zw
                              20.0f, 60.0f,                  // TEXCOORD2: o2.xz
                              40.0f,                         // FOG: o2.y
                              20.0f, 30.0f, 40.0f}));        // COLOR: o3.yzw
}

// Not run by default (run it with "[d3d9bench]"): how long RunPostVS takes for a big draw, so a
// change to the interpreter can be checked for speed. Prints the best of 5 runs.
TEST_CASE("D3D9 vertex shader emulation - speed", "[.][d3d9bench]")
{
  // a vs_2_0 shader like a lit, skinned-free mesh: transform, normal, four lights in a loop
  Assembler a(2, 0);
  a.def(20, 0, 0, 0, 0);
  a.defi(0, 4, 8, 1, 0);
  a.dcl(DeclUsage::Position, 0, 0);
  a.dcl(DeclUsage::Normal, 0, 1);
  a.dcl(DeclUsage::TexCoord, 0, 2);
  a.op(Opcode::M4x4, {OPos(), V(0), C(0)});
  a.op(Opcode::M3x3, {DR(0, 0x7), V(1), C(4)});
  a.op(Opcode::Nrm, {DR(1, 0x7), R(0)});
  a.op(Opcode::Mov, {DR(2), C(20)});
  // aL = 8, 9, 10, 11: the light directions and colours are c[aL] and c[aL + 4]
  a.op(Opcode::Loop, {Src(RegType::Loop, 0), Src(RegType::ConstInt, 0)});
  a.op(Opcode::Dp3, {DR(3, 0x1), R(1), Src(RegType::Const, 0) | RelBit, Src(RegType::Loop, 0)});
  a.op(Opcode::Max, {DR(3, 0x1), R(3, X), C(20, X)});
  a.op(Opcode::Mad, {DR(2), R(3, X), Src(RegType::Const, 4) | RelBit, Src(RegType::Loop, 0), R(2)});
  a.op(Opcode::EndLoop, {});
  a.op(Opcode::Mov, {Dst(RegType::AttrOut, 0), R(2)});
  a.op(Opcode::Mul, {Dst(RegType::TexCrdOutOrOutput, 0, 0x3), V(2), C(16)});
  rdcarray<uint32_t> tokens = a.finish();

  ShaderInfo info;
  info.Init(ResourceId(), tokens.data(), tokens.size());
  VSEmulator emu;
  REQUIRE(emu.Init(info.GetProgram()));
  const rdcarray<SigParameter> &sig = info.GetReflection().outputSignature;

  const uint32_t numVerts = 200000;
  DrawInputs draw;
  draw.streams.resize(1);
  for(uint32_t i = 0; i < numVerts; i++)
  {
    float v[12] = {float(i % 512),      float(i / 512),       1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                   float(i % 7) * 0.1f, float(i % 11) * 0.1f, 0.0f, 0.0f};
    draw.streams[0].data.append((const byte *)v, sizeof(v));
  }
  draw.streams[0].stride = sizeof(float) * 12;
  InputElement pos, norm, uv;
  pos.type = DeclType::Float4;
  norm.offset = 16;
  norm.type = DeclType::Float4;
  norm.usage = DeclUsage::Normal;
  uv.offset = 32;
  uv.type = DeclType::Float2;
  uv.usage = DeclUsage::TexCoord;
  draw.elements = {pos, norm, uv};
  draw.numVertices = numVerts;

  VSConstants c;
  c.floats.resize(24);
  for(uint32_t i = 0; i < 24; i++)
    c.floats[i] = Vec4(0.1f * i, 0.2f, 0.3f - 0.01f * i, 1.0f);
  VSConstants consts = emu.ApplyLiterals(c);
  VSTexture textures[NumVertexSamplers];

  double best = 1e30;
  for(int run = 0; run < 5; run++)
  {
    PostVSResult res;
    auto start = std::chrono::high_resolution_clock::now();
    RunPostVS(draw, 0, &emu, consts, textures, NULL, sig, res);
    auto end = std::chrono::high_resolution_clock::now();
    REQUIRE(res.status.empty());
    REQUIRE(res.numUnique == numVerts);
    best = RDCMIN(best, std::chrono::duration<double, std::milli>(end - start).count());
  }
  WARN(
      StringFormat::Fmt("RunPostVS: %u vertices of a %zu instruction shader in %.1f ms (best of 5)",
                        numVerts, info.GetProgram().instructions.size(), best)
          .c_str());
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
