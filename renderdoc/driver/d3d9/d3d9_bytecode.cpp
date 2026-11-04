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

#include "d3d9_bytecode.h"
#include <string.h>
#include "common/common.h"
#include "common/formatting.h"

namespace D3D9BC
{
namespace
{
// token layout, see d3d9types.h
const uint32_t OpcodeMask = 0x0000FFFF;
const uint32_t ControlShift = 16;
const uint32_t LengthShift = 24;
const uint32_t PredicatedBit = 0x10000000;
const uint32_t CoissueBit = 0x40000000;
const uint32_t CommentLengthShift = 16;
const uint32_t CommentLengthMask = 0x7FFF;
const uint32_t ParamBit = 0x80000000;
const uint32_t RegNumMask = 0x000007FF;
const uint32_t RelativeBit = 0x00002000;
const uint32_t EndToken = 0x0000FFFF;

RegType ParamRegType(uint32_t token)
{
  return RegType(((token >> 28) & 0x7) | ((token >> 8) & 0x18));
}

struct OpcodeInfo
{
  const char *name;
  // whether the first parameter is a destination
  bool dst;
  // the number of source parameters in shader model 1, where instructions have no length. -1 for
  // opcodes 1_x shaders can't have.
  int8_t sm1Srcs;
};

// indexed by opcode, up to BreakP
const OpcodeInfo opcodes[] = {
    {"nop", false, 0},            // Nop
    {"mov", true, 1},             // Mov
    {"add", true, 2},             // Add
    {"sub", true, 2},             // Sub
    {"mad", true, 3},             // Mad
    {"mul", true, 2},             // Mul
    {"rcp", true, 1},             // Rcp
    {"rsq", true, 1},             // Rsq
    {"dp3", true, 2},             // Dp3
    {"dp4", true, 2},             // Dp4
    {"min", true, 2},             // Min
    {"max", true, 2},             // Max
    {"slt", true, 2},             // Slt
    {"sge", true, 2},             // Sge
    {"exp", true, 1},             // Exp
    {"log", true, 1},             // Log
    {"lit", true, 1},             // Lit
    {"dst", true, 2},             // Dst
    {"lrp", true, 3},             // Lrp
    {"frc", true, 1},             // Frc
    {"m4x4", true, 2},            // M4x4
    {"m4x3", true, 2},            // M4x3
    {"m3x4", true, 2},            // M3x4
    {"m3x3", true, 2},            // M3x3
    {"m3x2", true, 2},            // M3x2
    {"call", false, -1},          // Call
    {"callnz", false, -1},        // CallNZ
    {"loop", false, -1},          // Loop
    {"ret", false, -1},           // Ret
    {"endloop", false, -1},       // EndLoop
    {"label", false, -1},         // Label
    {"dcl", true, 0},             // Dcl, special
    {"pow", true, -1},            // Pow
    {"crs", true, -1},            // Crs
    {"sgn", true, -1},            // Sgn
    {"abs", true, -1},            // Abs
    {"nrm", true, -1},            // Nrm
    {"sincos", true, -1},         // SinCos
    {"rep", false, -1},           // Rep
    {"endrep", false, -1},        // EndRep
    {"if", false, -1},            // If
    {"if", false, -1},            // IfC
    {"else", false, -1},          // Else
    {"endif", false, -1},         // EndIf
    {"break", false, -1},         // Break
    {"break", false, -1},         // BreakC
    {"mova", true, -1},           // MovA
    {"defb", true, 0},            // DefB, special
    {"defi", true, 0},            // DefI, special
    {NULL, false, -1},            // 49
    {NULL, false, -1},            // 50
    {NULL, false, -1},            // 51
    {NULL, false, -1},            // 52
    {NULL, false, -1},            // 53
    {NULL, false, -1},            // 54
    {NULL, false, -1},            // 55
    {NULL, false, -1},            // 56
    {NULL, false, -1},            // 57
    {NULL, false, -1},            // 58
    {NULL, false, -1},            // 59
    {NULL, false, -1},            // 60
    {NULL, false, -1},            // 61
    {NULL, false, -1},            // 62
    {NULL, false, -1},            // 63
    {"texcoord", true, 0},        // TexCoord, ps_1_4 has a source
    {"texkill", true, 0},         // TexKill
    {"tex", true, 0},             // Tex, ps_1_4 has a source
    {"texbem", true, 1},          // TexBem
    {"texbeml", true, 1},         // TexBemL
    {"texreg2ar", true, 1},       // TexReg2AR
    {"texreg2gb", true, 1},       // TexReg2GB
    {"texm3x2pad", true, 1},      // TexM3x2Pad
    {"texm3x2tex", true, 1},      // TexM3x2Tex
    {"texm3x3pad", true, 1},      // TexM3x3Pad
    {"texm3x3tex", true, 1},      // TexM3x3Tex
    {NULL, false, -1},            // Reserved0
    {"texm3x3spec", true, 2},     // TexM3x3Spec
    {"texm3x3vspec", true, 1},    // TexM3x3VSpec
    {"expp", true, 1},            // ExpP
    {"logp", true, 1},            // LogP
    {"cnd", true, 3},             // Cnd
    {"def", true, 0},             // Def, special
    {"texreg2rgb", true, 1},      // TexReg2RGB
    {"texdp3tex", true, 1},       // TexDP3Tex
    {"texm3x2depth", true, 1},    // TexM3x2Depth
    {"texdp3", true, 1},          // TexDP3
    {"texm3x3", true, 1},         // TexM3x3
    {"texdepth", true, 0},        // TexDepth
    {"cmp", true, 3},             // Cmp
    {"bem", true, 2},             // Bem
    {"dp2add", true, -1},         // Dp2Add
    {"dsx", true, -1},            // Dsx
    {"dsy", true, -1},            // Dsy
    {"texldd", true, -1},         // TexLdd
    {"setp", true, -1},           // SetP
    {"texldl", true, -1},         // TexLdl
    {"breakp", false, -1},        // BreakP
};

const OpcodeInfo phaseInfo = {"phase", false, 0};

const OpcodeInfo *GetOpcodeInfo(Opcode op)
{
  if(op == Opcode::Phase)
    return &phaseInfo;
  if(size_t(op) < ARRAY_COUNT(opcodes) && opcodes[size_t(op)].name)
    return &opcodes[size_t(op)];
  return NULL;
}

// the number of DWORDs after the instruction token, for 1_x shaders. -1 if it isn't known.
int SM1Length(const Version &version, Opcode op)
{
  switch(op)
  {
    case Opcode::Dcl: return 2;
    case Opcode::Def: return 5;
    case Opcode::DefI: return -1;
    case Opcode::DefB: return -1;
    // ps_1_4 reads a texture coordinate register or a temporary, earlier versions only name the
    // t# register to fill
    case Opcode::Tex:
    case Opcode::TexCoord: return version.IsPS() && version.AtLeast(1, 4) ? 2 : 1;
    default: break;
  }

  const OpcodeInfo *info = GetOpcodeInfo(op);
  if(!info || info->sm1Srcs < 0)
    return -1;
  return (info->dst ? 1 : 0) + info->sm1Srcs;
}

class TokenReader
{
public:
  TokenReader(const Program &prog, const Version &version, uint32_t start, uint32_t end)
      : m_Tokens(prog.tokens.data()), m_Version(version), m_Pos(start), m_End(end)
  {
  }

  bool AtEnd() const { return m_Pos >= m_End; }
  uint32_t Remaining() const { return m_End - m_Pos; }
  uint32_t Pos() const { return m_Pos; }

  bool ReadRaw(uint32_t &token)
  {
    if(m_Pos >= m_End)
      return Fail("the instruction is missing parameters");
    token = m_Tokens[m_Pos++];
    return true;
  }

  bool ReadDst(DstParam &dst)
  {
    uint32_t token = 0;
    if(!ReadParamToken(token))
      return false;

    dst.type = ParamRegType(token);
    dst.index = token & RegNumMask;
    dst.writeMask = uint8_t((token >> 16) & 0xF);
    dst.mods = uint8_t((token >> 20) & 0xF);
    // a signed 4-bit value
    int shift = int((token >> 24) & 0xF);
    dst.shift = int8_t(shift >= 8 ? shift - 16 : shift);

    // only vs_3_0's outputs can be relatively addressed, by aL
    if(token & RelativeBit)
      return ReadRelative(dst.rel);

    return true;
  }

  bool ReadSrc(SrcParam &src)
  {
    uint32_t token = 0;
    if(!ReadParamToken(token))
      return false;

    src.type = ParamRegType(token);
    src.index = token & RegNumMask;
    uint32_t swizzle = (token >> 16) & 0xFF;
    for(int i = 0; i < 4; i++)
      src.swizzle[i] = uint8_t((swizzle >> (i * 2)) & 0x3);
    src.mod = SrcMod((token >> 24) & 0xF);

    if(token & RelativeBit)
      return ReadRelative(src.rel);

    return true;
  }

  const rdcstr &Error() const { return m_Error; }

private:
  bool Fail(const char *msg)
  {
    if(m_Error.empty())
      m_Error = msg;
    return false;
  }

  bool ReadParamToken(uint32_t &token)
  {
    if(!ReadRaw(token))
      return false;
    // parameter tokens always have the top bit set, instruction tokens don't. Checking this
    // catches lengths we got wrong, rather than decoding the next instruction as parameters.
    if((token & ParamBit) == 0)
      return Fail("expected a parameter token");
    return true;
  }

  bool ReadRelative(RelativeAddress &rel)
  {
    rel.present = true;

    // vs_1_1 can only index constants by a0.x, and doesn't store which register it uses
    if(m_Version.IsSM1())
    {
      rel.type = RegType::AddrOrTexture;
      rel.index = 0;
      rel.component = 0;
      return true;
    }

    // otherwise a source token for the address register follows, its x swizzle selecting the
    // component used
    uint32_t token = 0;
    if(!ReadParamToken(token))
      return false;
    rel.type = ParamRegType(token);
    rel.index = token & RegNumMask;
    rel.component = uint8_t((token >> 16) & 0x3);
    return true;
  }

  const uint32_t *m_Tokens;
  const Version &m_Version;
  uint32_t m_Pos;
  uint32_t m_End;
  rdcstr m_Error;
};

bool ParseOperands(const Program &prog, Instruction &inst, uint32_t start, uint32_t end, rdcstr &error)
{
  const Version &version = prog.version;
  TokenReader reader(prog, version, start, end);
  const OpcodeInfo *info = GetOpcodeInfo(inst.op);

  bool ok = true;

  if(inst.op == Opcode::Dcl)
  {
    uint32_t dcl = 0;
    ok = reader.ReadRaw(dcl);
    if(ok)
    {
      inst.usage = DeclUsage(dcl & 0xF);
      inst.usageIndex = (dcl >> 16) & 0xF;
      inst.textureType = SamplerTextureType((dcl >> 27) & 0xF);
      inst.hasDst = true;
      ok = reader.ReadDst(inst.dst);
    }
  }
  else if(inst.op == Opcode::Def || inst.op == Opcode::DefI || inst.op == Opcode::DefB)
  {
    inst.hasDst = true;
    ok = reader.ReadDst(inst.dst);
    uint32_t count = inst.op == Opcode::DefB ? 1 : 4;
    for(uint32_t i = 0; ok && i < count; i++)
      ok = reader.ReadRaw(inst.literal[i]);
  }
  else
  {
    if(info && info->dst)
    {
      inst.hasDst = true;
      ok = reader.ReadDst(inst.dst);
    }

    // the predicate comes after the destination, before the sources
    if(ok && inst.predicated)
      ok = reader.ReadSrc(inst.predicate);

    if(version.IsSM1())
    {
      int len = SM1Length(version, inst.op);
      int srcs = len - (inst.hasDst ? 1 : 0);
      for(int i = 0; ok && i < srcs; i++)
      {
        inst.srcs.push_back(SrcParam());
        ok = reader.ReadSrc(inst.srcs.back());
      }
    }
    else
    {
      // the rest are sources. They have one or two tokens each, so the length doesn't say how many
      // there are directly
      while(ok && !reader.AtEnd())
      {
        inst.srcs.push_back(SrcParam());
        ok = reader.ReadSrc(inst.srcs.back());
      }
    }
  }

  if(!ok)
  {
    error = reader.Error();
    return false;
  }

  if(!reader.AtEnd())
  {
    error = StringFormat::Fmt("%u more parameter DWORDs than expected", reader.Remaining());
    return false;
  }

  return true;
}

void AppendComponent(rdcstr &str, uint8_t comp)
{
  str.push_back("xyzw"[comp & 3]);
}

rdcstr WriteMaskString(uint8_t mask)
{
  if(mask == 0xF || mask == 0)
    return rdcstr();
  rdcstr ret = ".";
  for(uint8_t c = 0; c < 4; c++)
    if(mask & (1 << c))
      AppendComponent(ret, c);
  return ret;
}

rdcstr SwizzleString(const Version &version, const SrcParam &src)
{
  if(src.IdentitySwizzle())
    return rdcstr();

  // fxc prints one letter for a replicate swizzle (.x) and all four otherwise (.xyww, .yxxx),
  // never two or three. The exception is ps_1_4's texcrd/texld, the only readers of t#, whose
  // source selectors are documented as .xyz and .xyw: those leave off the repeated last component.
  // (That exception hasn't been compared against fxc.)
  int len = 4;
  if(src.swizzle[0] == src.swizzle[1] && src.swizzle[1] == src.swizzle[2] &&
     src.swizzle[2] == src.swizzle[3])
  {
    len = 1;
  }
  else if(version.IsPS() && version.major == 1 && version.minor == 4 &&
          src.type == RegType::AddrOrTexture)
  {
    while(len > 1 && src.swizzle[len - 1] == src.swizzle[len - 2])
      len--;
  }

  rdcstr ret = ".";
  for(int i = 0; i < len; i++)
    AppendComponent(ret, src.swizzle[i]);
  return ret;
}

rdcstr RelativeString(const Version &version, const RelativeAddress &rel)
{
  if(!rel.present)
    return rdcstr();

  rdcstr ret = "[";
  ret += RegisterName(version, rel.type, rel.index);
  // aL is a scalar
  if(rel.type != RegType::Loop)
  {
    ret += ".";
    AppendComponent(ret, rel.component);
  }
  ret += "]";
  return ret;
}

rdcstr DstString(const Version &version, const DstParam &dst, bool withMask)
{
  rdcstr ret = RegisterName(version, dst.type, dst.index);
  ret += RelativeString(version, dst.rel);
  if(withMask)
    ret += WriteMaskString(dst.writeMask);
  return ret;
}

rdcstr SrcString(const Version &version, const SrcParam &src)
{
  rdcstr prefix, suffix;

  switch(src.mod)
  {
    case SrcMod::None: break;
    case SrcMod::Neg: prefix = "-"; break;
    case SrcMod::Bias: suffix = "_bias"; break;
    case SrcMod::BiasNeg:
      prefix = "-";
      suffix = "_bias";
      break;
    case SrcMod::Sign: suffix = "_bx2"; break;
    case SrcMod::SignNeg:
      prefix = "-";
      suffix = "_bx2";
      break;
    case SrcMod::Comp: prefix = "1-"; break;
    case SrcMod::X2: suffix = "_x2"; break;
    case SrcMod::X2Neg:
      prefix = "-";
      suffix = "_x2";
      break;
    case SrcMod::DZ: suffix = "_dz"; break;
    case SrcMod::DW: suffix = "_dw"; break;
    case SrcMod::Abs: suffix = "_abs"; break;
    case SrcMod::AbsNeg:
      prefix = "-";
      suffix = "_abs";
      break;
    case SrcMod::Not: prefix = "!"; break;
    default: suffix = StringFormat::Fmt("_unknownmod%u", uint32_t(src.mod)); break;
  }

  return prefix + RegisterName(version, src.type, src.index) + RelativeString(version, src.rel) +
         suffix + SwizzleString(version, src);
}

rdcstr FloatString(uint32_t bits)
{
  float f;
  memcpy(&f, &bits, sizeof(f));
  // fxc prints 9 significant digits, enough to give back the same float
  return StringFormat::Fmt("%.9g", f);
}

const char *UsageName(DeclUsage usage)
{
  switch(usage)
  {
    case DeclUsage::Position: return "position";
    case DeclUsage::BlendWeight: return "blendweight";
    case DeclUsage::BlendIndices: return "blendindices";
    case DeclUsage::Normal: return "normal";
    case DeclUsage::PSize: return "psize";
    case DeclUsage::TexCoord: return "texcoord";
    case DeclUsage::Tangent: return "tangent";
    case DeclUsage::Binormal: return "binormal";
    case DeclUsage::TessFactor: return "tessfactor";
    case DeclUsage::PositionT: return "positiont";
    case DeclUsage::Color: return "color";
    case DeclUsage::Fog: return "fog";
    case DeclUsage::Depth: return "depth";
    case DeclUsage::Sample: return "sample";
    default: break;
  }
  return NULL;
}

rdcstr DstModString(const Instruction &inst)
{
  rdcstr ret;
  if(!inst.hasDst)
    return ret;

  switch(inst.dst.shift)
  {
    case 1: ret += "_x2"; break;
    case 2: ret += "_x4"; break;
    case 3: ret += "_x8"; break;
    case -1: ret += "_d2"; break;
    case -2: ret += "_d4"; break;
    case -3: ret += "_d8"; break;
    case 0: break;
    default: ret += StringFormat::Fmt("_shift%d", inst.dst.shift); break;
  }

  if(inst.dst.mods & DstMod_Saturate)
    ret += "_sat";
  if(inst.dst.mods & DstMod_PartialPrecision)
    ret += "_pp";
  if(inst.dst.mods & DstMod_Centroid)
    ret += "_centroid";
  return ret;
}

rdcstr DclName(const Version &version, const Instruction &inst)
{
  rdcstr ret = "dcl";

  if(inst.dst.type == RegType::Sampler)
  {
    switch(inst.textureType)
    {
      case SamplerTextureType::Tex1D: ret += "_1d"; break;
      case SamplerTextureType::Tex2D: ret += "_2d"; break;
      case SamplerTextureType::Cube: ret += "_cube"; break;
      case SamplerTextureType::Volume: ret += "_volume"; break;
      case SamplerTextureType::Unknown: break;
      default: ret += StringFormat::Fmt("_unknowntype%u", uint32_t(inst.textureType)); break;
    }
    return ret;
  }

  // ps_2_0 and ps_2_x declare v# and t# without a usage, the register says what it is. vPos and
  // vFace don't have one either.
  if(inst.dst.type == RegType::MiscType)
    return ret;
  if(version.IsPS() && version.major < 3 &&
     (inst.dst.type == RegType::Input || inst.dst.type == RegType::AddrOrTexture))
    return ret;

  const char *usage = UsageName(inst.usage);
  if(usage)
    ret += StringFormat::Fmt("_%s", usage);
  else
    ret += StringFormat::Fmt("_unknownusage%u", uint32_t(inst.usage));

  // fxc leaves off index 0, dcl_texcoord is TEXCOORD0
  if(inst.usageIndex > 0)
    ret += StringFormat::Fmt("%u", inst.usageIndex);

  return ret;
}

void ParseProgramInternal(Program &prog)
{
  const uint32_t *tokens = prog.tokens.data();
  const uint32_t numTokens = (uint32_t)prog.tokens.size();

  if(numTokens == 0)
  {
    prog.error = "The shader is empty";
    return;
  }

  const uint32_t versionToken = tokens[0];
  const uint32_t typeBits = versionToken >> 16;
  if(typeBits != 0xFFFE && typeBits != 0xFFFF)
  {
    prog.error = StringFormat::Fmt("Unrecognised version token 0x%08x", versionToken);
    return;
  }

  prog.version.type = typeBits == 0xFFFE ? ShaderType::Vertex : ShaderType::Pixel;
  prog.version.major = uint8_t((versionToken >> 8) & 0xFF);
  prog.version.minor = uint8_t(versionToken & 0xFF);

  if(prog.version.major < 1 || prog.version.major > 3)
  {
    prog.error = StringFormat::Fmt("Unsupported shader version %s (token 0x%08x)",
                                   VersionString(prog.version).c_str(), versionToken);
    return;
  }

  prog.validVersion = true;

  const Version &version = prog.version;

  uint32_t pos = 1;
  bool foundEnd = false;
  while(pos < numTokens)
  {
    const uint32_t token = tokens[pos];

    if(token == EndToken)
    {
      foundEnd = true;
      break;
    }

    const Opcode op = Opcode(token & OpcodeMask);

    if(op == Opcode::Comment)
    {
      CommentBlock comment;
      comment.offset = pos + 1;
      comment.length = (token >> CommentLengthShift) & CommentLengthMask;
      if(comment.length > numTokens - comment.offset)
      {
        prog.error =
            StringFormat::Fmt("The comment at DWORD %u runs past the end of the shader", pos);
        return;
      }
      prog.comments.push_back(comment);
      pos = comment.offset + comment.length;
      continue;
    }

    if(token & ParamBit)
    {
      prog.error = StringFormat::Fmt("Expected an instruction at DWORD %u, found 0x%08x", pos, token);
      return;
    }

    const OpcodeInfo *info = GetOpcodeInfo(op);
    if(!info)
    {
      prog.error = StringFormat::Fmt("Unknown opcode %u at DWORD %u", uint32_t(op), pos);
      return;
    }

    Instruction inst;
    inst.op = op;
    inst.offset = pos;
    inst.control = uint8_t((token >> ControlShift) & 0xFF);

    if(version.IsSM1())
    {
      // 1_x shaders don't store the length, it comes from the opcode. Some opcodes 1_x shaders
      // can't have, so if the token gives a length anyway, use that rather than give up.
      int len = SM1Length(version, op);
      if(len < 0)
        len = int((token >> LengthShift) & 0xF);
      if(len <= 0 && op != Opcode::Nop && op != Opcode::Phase && op != Opcode::Ret &&
         op != Opcode::EndLoop)
      {
        prog.error = StringFormat::Fmt("Opcode %s (%u) at DWORD %u isn't valid in %s", info->name,
                                       uint32_t(op), pos, VersionString(version).c_str());
        return;
      }
      inst.length = uint32_t(len);
      inst.coissue = (token & CoissueBit) != 0;
    }
    else
    {
      inst.length = (token >> LengthShift) & 0xF;
      inst.predicated = (token & PredicatedBit) != 0;
    }

    if(inst.length > numTokens - pos - 1)
    {
      prog.error = StringFormat::Fmt(
          "The %s instruction at DWORD %u runs past the end of the shader", info->name, pos);
      return;
    }

    rdcstr error;
    if(!ParseOperands(prog, inst, pos + 1, pos + 1 + inst.length, error))
    {
      prog.error = StringFormat::Fmt("Couldn't decode the %s instruction at DWORD %u: %s",
                                     info->name, pos, error.c_str());
      return;
    }

    prog.instructions.push_back(inst);
    pos += 1 + inst.length;
  }

  if(!foundEnd)
    prog.error = "The shader has no end token";
}

};    // anonymous namespace

const uint32_t *Program::FindComment(uint32_t fourcc, uint32_t &length) const
{
  for(const CommentBlock &c : comments)
  {
    if(c.length >= 1 && tokens[c.offset] == fourcc)
    {
      length = c.length - 1;
      return tokens.data() + c.offset + 1;
    }
  }
  length = 0;
  return NULL;
}

void ParseProgram(const uint32_t *tokens, size_t numTokens, Program &prog)
{
  prog = Program();
  if(tokens && numTokens > 0)
  {
    // the length is stored in 32 bits everywhere else, and no real shader is near that
    if(numTokens > 0x3FFFFFFF)
      numTokens = 0x3FFFFFFF;
    prog.tokens.assign(tokens, numTokens);
  }
  ParseProgramInternal(prog);
}

rdcstr VersionString(const Version &version)
{
  const char *type = version.IsVS() ? "vs" : "ps";
  // 2_1 is what 2_x shaders' version tokens say, and 0xFF is a software shader
  if(version.major == 2 && version.minor == 1)
    return StringFormat::Fmt("%s_2_x", type);
  if(version.minor == 0xFF)
    return StringFormat::Fmt("%s_%u_sw", type, uint32_t(version.major));
  return StringFormat::Fmt("%s_%u_%u", type, uint32_t(version.major), uint32_t(version.minor));
}

rdcstr OpcodeName(const Version &version, const Instruction &inst)
{
  const OpcodeInfo *info = GetOpcodeInfo(inst.op);
  if(!info)
    return StringFormat::Fmt("unknown_opcode_%u", uint32_t(inst.op));

  rdcstr ret = info->name;

  switch(inst.op)
  {
    case Opcode::Tex:
      // tex in ps_1_0 to ps_1_3, texld from ps_1_4. texldp and texldb are controls.
      if(version.IsPS() && !version.AtLeast(1, 4))
        return ret;
      ret = "texld";
      if(inst.control & 0x1)
        ret += "p";
      else if(inst.control & 0x2)
        ret += "b";
      return ret;
    case Opcode::TexCoord:
      if(version.IsPS() && version.major == 1 && version.minor == 4)
        return "texcrd";
      return ret;
    case Opcode::IfC:
    case Opcode::BreakC:
    case Opcode::SetP:
    {
      static const char *const comparisons[] = {"", "_gt", "_eq", "_ge", "_lt", "_ne", "_le", ""};
      ret += comparisons[uint32_t(inst.GetComparison()) & 0x7];
      return ret;
    }
    default: break;
  }

  return ret;
}

rdcstr RegisterName(const Version &version, RegType type, uint32_t index)
{
  switch(type)
  {
    case RegType::Temp: return StringFormat::Fmt("r%u", index);
    case RegType::Input: return StringFormat::Fmt("v%u", index);
    case RegType::Const: return StringFormat::Fmt("c%u", index);
    case RegType::AddrOrTexture: return StringFormat::Fmt(version.IsVS() ? "a%u" : "t%u", index);
    case RegType::RastOut:
      if(index == 0)
        return "oPos";
      if(index == 1)
        return "oFog";
      if(index == 2)
        return "oPts";
      return StringFormat::Fmt("oRast%u", index);
    case RegType::AttrOut: return StringFormat::Fmt("oD%u", index);
    case RegType::TexCrdOutOrOutput:
      if(version.IsVS() && version.major >= 3)
        return StringFormat::Fmt("o%u", index);
      return StringFormat::Fmt("oT%u", index);
    case RegType::ConstInt: return StringFormat::Fmt("i%u", index);
    case RegType::ColorOut: return StringFormat::Fmt("oC%u", index);
    case RegType::DepthOut: return "oDepth";
    case RegType::Sampler: return StringFormat::Fmt("s%u", index);
    // the constant registers above 2047 are in their own types
    case RegType::Const2: return StringFormat::Fmt("c%u", index + 2048);
    case RegType::Const3: return StringFormat::Fmt("c%u", index + 4096);
    case RegType::Const4: return StringFormat::Fmt("c%u", index + 6144);
    case RegType::ConstBool: return StringFormat::Fmt("b%u", index);
    case RegType::Loop: return "aL";
    case RegType::TempFloat16: return StringFormat::Fmt("h%u", index);
    case RegType::MiscType:
      if(index == 0)
        return "vPos";
      if(index == 1)
        return "vFace";
      return StringFormat::Fmt("vMisc%u", index);
    case RegType::Label: return StringFormat::Fmt("l%u", index);
    case RegType::Predicate: return StringFormat::Fmt("p%u", index);
    default: break;
  }

  return StringFormat::Fmt("unknownreg%u_%u", uint32_t(type), index);
}

rdcstr Disassemble(const Program &prog)
{
  rdcstr ret;

  if(!prog.validVersion)
  {
    ret = "// Couldn't disassemble the shader: " + prog.error + "\n";
    return ret;
  }

  const Version &version = prog.version;

  ret += "    " + VersionString(version) + "\n";

  int indent = 0;

  for(const Instruction &inst : prog.instructions)
  {
    if(inst.op == Opcode::Else || inst.op == Opcode::EndIf || inst.op == Opcode::EndLoop ||
       inst.op == Opcode::EndRep)
      indent = RDCMAX(indent - 1, 0);

    // fxc puts a co-issued instruction's + two columns left of the opcode, then a space
    rdcstr line = inst.coissue ? "  + " : "    ";
    for(int i = 0; i < indent; i++)
      line += "  ";

    if(inst.predicated)
      line += "(" + SrcString(version, inst.predicate) + ") ";

    rdcstr operands;

    if(inst.op == Opcode::Dcl)
    {
      line += DclName(version, inst) + DstModString(inst);
      operands = DstString(version, inst.dst, inst.dst.type != RegType::Sampler);
    }
    else if(inst.op == Opcode::Def)
    {
      line += OpcodeName(version, inst);
      operands = DstString(version, inst.dst, false);
      for(int i = 0; i < 4; i++)
        operands += ", " + FloatString(inst.literal[i]);
    }
    else if(inst.op == Opcode::DefI)
    {
      line += OpcodeName(version, inst);
      operands = DstString(version, inst.dst, false);
      for(int i = 0; i < 4; i++)
        operands += StringFormat::Fmt(", %d", int32_t(inst.literal[i]));
    }
    else if(inst.op == Opcode::DefB)
    {
      line += OpcodeName(version, inst);
      operands = DstString(version, inst.dst, false);
      operands += inst.literal[0] ? ", true" : ", false";
    }
    else
    {
      line += OpcodeName(version, inst) + DstModString(inst);
      if(inst.hasDst)
        operands = DstString(version, inst.dst, true);
      for(const SrcParam &src : inst.srcs)
      {
        if(!operands.empty())
          operands += ", ";
        operands += SrcString(version, src);
      }
    }

    if(!operands.empty())
      line += " " + operands;

    ret += line + "\n";

    if(inst.op == Opcode::If || inst.op == Opcode::IfC || inst.op == Opcode::Else ||
       inst.op == Opcode::Loop || inst.op == Opcode::Rep)
      indent++;
  }

  if(!prog.error.empty())
    ret += "\n// Disassembly stopped here: " + prog.error + "\n";
  else
    ret += StringFormat::Fmt("\n// %u instructions\n", (uint32_t)prog.instructions.size());

  return ret;
}

};    // namespace D3D9BC

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"
#include "strings/string_utils.h"
#include "d3d9_bytecode_test.h"

using namespace D3D9BC;
using namespace D3D9BCTest;

// Catch prints values through ToStr
template <>
rdcstr DoStringise(const D3D9BC::RegType &el)
{
  return StringFormat::Fmt("RegType(%u)", uint32_t(el));
}

template <>
rdcstr DoStringise(const D3D9BC::SamplerTextureType &el)
{
  return StringFormat::Fmt("SamplerTextureType(%u)", uint32_t(el));
}

template <>
rdcstr DoStringise(const D3D9BC::DeclUsage &el)
{
  return StringFormat::Fmt("DeclUsage(%u)", uint32_t(el));
}

static rdcstr DisassembleTokens(const rdcarray<uint32_t> &tokens, Program &prog)
{
  ParseProgram(tokens.data(), tokens.size(), prog);
  return Disassemble(prog);
}

// the instruction lines of a disassembly, without the indentation, the version line and the
// trailing comment
static rdcarray<rdcstr> InstructionLines(const rdcstr &disasm, bool keepIndent = false)
{
  rdcarray<rdcstr> ret;
  rdcarray<rdcstr> lines;
  split(disasm, lines, '\n');
  for(size_t i = 1; i < lines.size(); i++)
  {
    rdcstr line = lines[i];
    if(line.empty() || line.beginsWith("//"))
      continue;
    if(!keepIndent)
      line.trim();
    else if(line.beginsWith("    "))
      line.erase(0, 4);
    ret.push_back(line);
  }
  return ret;
}

TEST_CASE("D3D9 bytecode parsing and disassembly", "[d3d9][shader]")
{
  Program prog;

  SECTION("vs_1_1")
  {
    rdcarray<uint32_t> tokens = {
        VersionToken(false, 1, 1),
        // dcl_position v0
        Inst(Opcode::Dcl, 0, 0),
        Dcl(DeclUsage::Position, 0),
        Dst(RegType::Input, 0),
        // dcl_normal v3
        Inst(Opcode::Dcl, 0, 0),
        Dcl(DeclUsage::Normal, 0),
        Dst(RegType::Input, 3),
        // dcl_texcoord1 v7
        Inst(Opcode::Dcl, 0, 0),
        Dcl(DeclUsage::TexCoord, 1),
        Dst(RegType::Input, 7),
        // def c95, 1, 0.5, -2, 0.25
        Inst(Opcode::Def, 0, 0),
        Dst(RegType::Const, 95),
        FloatBits(1.0f),
        FloatBits(0.5f),
        FloatBits(-2.0f),
        FloatBits(0.25f),
        // dp4 oPos.x, v0, c0
        Inst(Opcode::Dp4, 0, 0),
        Dst(RegType::RastOut, 0, 0x1),
        Src(RegType::Input, 0),
        Src(RegType::Const, 0),
        // mov a0.x, v7.y
        Inst(Opcode::Mov, 0, 0),
        Dst(RegType::AddrOrTexture, 0, 0x1),
        Src(RegType::Input, 7, Swz(1, 1, 1, 1)),
        // mad oD0, c[a0.x + 10], -v3.xyzz, c95.w (relative addressing has no extra token in 1_x)
        Inst(Opcode::Mad, 0, 0),
        Dst(RegType::AttrOut, 0),
        Src(RegType::Const, 10) | RelBit,
        Src(RegType::Input, 3, Swz(0, 1, 2, 2), SrcMod::Neg),
        Src(RegType::Const, 95, Swz(3, 3, 3, 3)),
        // mov oT0.xy, v7
        Inst(Opcode::Mov, 0, 0),
        Dst(RegType::TexCrdOutOrOutput, 0, 0x3),
        Src(RegType::Input, 7),
        // expp oFog, v0.z
        Inst(Opcode::ExpP, 0, 0),
        Dst(RegType::RastOut, 1, 0x1),
        Src(RegType::Input, 0, Swz(2, 2, 2, 2)),
        // mov oPts, c1.x
        Inst(Opcode::Mov, 0, 0),
        Dst(RegType::RastOut, 2, 0x1),
        Src(RegType::Const, 1, Swz(0, 0, 0, 0)),
        EndTok(),
    };

    rdcstr disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());
    CHECK(prog.version.IsVS());
    CHECK(prog.version.major == 1);
    CHECK(prog.version.minor == 1);
    REQUIRE(prog.instructions.size() == 10);
    CHECK(prog.instructions[0].usage == DeclUsage::Position);
    CHECK(prog.instructions[2].usage == DeclUsage::TexCoord);
    CHECK(prog.instructions[2].usageIndex == 1);
    CHECK(prog.instructions[6].srcs[0].rel.present);

    rdcarray<rdcstr> expected = {
        "dcl_position v0",
        "dcl_normal v3",
        "dcl_texcoord1 v7",
        "def c95, 1, 0.5, -2, 0.25",
        "dp4 oPos.x, v0, c0",
        "mov a0.x, v7.y",
        "mad oD0, c10[a0.x], -v3.xyzz, c95.w",
        "mov oT0.xy, v7",
        "expp oFog.x, v0.z",
        "mov oPts.x, c1.x",
    };
    CHECK(InstructionLines(disasm) == expected);
    CHECK(disasm.beginsWith("    vs_1_1\n"));
  };

  SECTION("vs_2_0 and vs_2_x")
  {
    rdcarray<uint32_t> tokens = {
        VersionToken(false, 2, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Position, 0),
        Dst(RegType::Input, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::BlendIndices, 0),
        Dst(RegType::Input, 1),
        // defi i0, 4, 0, 1, 0
        Inst(Opcode::DefI, 0, 5),
        Dst(RegType::ConstInt, 0),
        4,
        0,
        1,
        0,
        // defb b1, true
        Inst(Opcode::DefB, 0, 2),
        Dst(RegType::ConstBool, 1),
        1,
        // mova a0.x, v1.x
        Inst(Opcode::MovA, 0, 2),
        Dst(RegType::AddrOrTexture, 0, 0x1),
        Src(RegType::Input, 1, Swz(0, 0, 0, 0)),
        // m4x4 oPos, v0, c4[a0.x]: the relative address is an extra token in 2_0
        Inst(Opcode::M4x4, 0, 4),
        Dst(RegType::RastOut, 0),
        Src(RegType::Input, 0),
        Src(RegType::Const, 4) | RelBit,
        Src(RegType::AddrOrTexture, 0, Swz(0, 0, 0, 0)),
        // rep i0
        Inst(Opcode::Rep, 0, 1),
        Src(RegType::ConstInt, 0),
        //   sincos r0.xy, v0.x, c0, c1 (2_0 sincos has two more constants)
        Inst(Opcode::SinCos, 0, 4),
        Dst(RegType::Temp, 0, 0x3),
        Src(RegType::Input, 0, Swz(0, 0, 0, 0)),
        Src(RegType::Const, 0),
        Src(RegType::Const, 1),
        //   if b1
        Inst(Opcode::If, 0, 1),
        Src(RegType::ConstBool, 1),
        //     mov_sat oD0, r0_abs (abs on a source isn't valid here, but checks the printing)
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::AttrOut, 0, 0xF, DstMod_Saturate),
        Src(RegType::Temp, 0, Swz(0, 1, 2, 3), SrcMod::Abs),
        //   else
        Inst(Opcode::Else, 0, 0),
        //     mul_pp oT3.zw, r0.yxzw, -r0_abs
        Inst(Opcode::Mul, 0, 3),
        Dst(RegType::TexCrdOutOrOutput, 3, 0xC, DstMod_PartialPrecision),
        Src(RegType::Temp, 0, Swz(1, 0, 2, 3)),
        Src(RegType::Temp, 0, Swz(0, 1, 2, 3), SrcMod::AbsNeg),
        //   endif
        Inst(Opcode::EndIf, 0, 0),
        // endrep
        Inst(Opcode::EndRep, 0, 0),
        // call l2
        Inst(Opcode::Call, 0, 1),
        Src(RegType::Label, 2),
        // callnz l2, !b1
        Inst(Opcode::CallNZ, 0, 2),
        Src(RegType::Label, 2),
        Src(RegType::ConstBool, 1, Swz(0, 1, 2, 3), SrcMod::Not),
        Inst(Opcode::Ret, 0, 0),
        Inst(Opcode::Label, 0, 1),
        Src(RegType::Label, 2),
        Inst(Opcode::Ret, 0, 0),
        EndTok(),
    };

    rdcstr disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());

    rdcarray<rdcstr> expected = {
        "dcl_position v0",
        "dcl_blendindices v1",
        "defi i0, 4, 0, 1, 0",
        "defb b1, true",
        "mova a0.x, v1.x",
        "m4x4 oPos, v0, c4[a0.x]",
        "rep i0",
        "  sincos r0.xy, v0.x, c0, c1",
        "  if b1",
        "    mov_sat oD0, r0_abs",
        "  else",
        "    mul_pp oT3.zw, r0.yxzw, -r0_abs",
        "  endif",
        "endrep",
        "call l2",
        "callnz l2, !b1",
        "ret",
        "label l2",
        "ret",
    };
    CHECK(InstructionLines(disasm, true) == expected);

    // the same shader as vs_2_x, with predication and dynamic flow control
    tokens = {
        VersionToken(false, 2, 1),
        // setp_gt p0.x, c0.x, c1.x
        Inst(Opcode::SetP, 1, 3),
        Dst(RegType::Predicate, 0, 0x1),
        Src(RegType::Const, 0, Swz(0, 0, 0, 0)),
        Src(RegType::Const, 1, Swz(0, 0, 0, 0)),
        // (!p0.x) mov r0, c2
        Inst(Opcode::Mov, 0, 3) | PredBit,
        Dst(RegType::Temp, 0),
        Src(RegType::Predicate, 0, Swz(0, 0, 0, 0), SrcMod::Not),
        Src(RegType::Const, 2),
        // if_lt c0.y, c1.y
        Inst(Opcode::IfC, 4, 2),
        Src(RegType::Const, 0, Swz(1, 1, 1, 1)),
        Src(RegType::Const, 1, Swz(1, 1, 1, 1)),
        //   break_ne c0.x, c0.y (not in an if or loop, but checks the comparison)
        Inst(Opcode::BreakC, 5, 2),
        Src(RegType::Const, 0, Swz(0, 0, 0, 0)),
        Src(RegType::Const, 0, Swz(1, 1, 1, 1)),
        //   breakp p0.y
        Inst(Opcode::BreakP, 0, 1),
        Src(RegType::Predicate, 0, Swz(1, 1, 1, 1)),
        Inst(Opcode::EndIf, 0, 0),
        EndTok(),
    };

    disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());
    CHECK(disasm.beginsWith("    vs_2_x\n"));
    expected = {
        "setp_gt p0.x, c0.x, c1.x", "(!p0.x) mov r0, c2", "if_lt c0.y, c1.y",
        "  break_ne c0.x, c0.y",    "  breakp p0.y",      "endif",
    };
    CHECK(InstructionLines(disasm, true) == expected);
    REQUIRE(prog.instructions.size() == 6);
    CHECK(prog.instructions[1].predicated);
    CHECK(prog.instructions[1].srcs.size() == 1);
    CHECK(prog.instructions[1].predicate.type == RegType::Predicate);
  };

  SECTION("vs_3_0")
  {
    rdcarray<uint32_t> tokens = {
        VersionToken(false, 3, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Position, 0),
        Dst(RegType::Input, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Color, 0),
        Dst(RegType::Input, 1),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 0),
        Dst(RegType::Input, 2, 0x3),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Position, 0),
        Dst(RegType::TexCrdOutOrOutput, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Color, 0),
        Dst(RegType::TexCrdOutOrOutput, 1),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 2),
        Dst(RegType::TexCrdOutOrOutput, 2, 0x3),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Fog, 0),
        Dst(RegType::TexCrdOutOrOutput, 3, 0x1),
        // dcl_2d s0 (a vertex texture)
        Inst(Opcode::Dcl, 0, 2),
        DclSampler(2),
        Dst(RegType::Sampler, 0),
        // def c0, 1, 0, 0, 0
        Inst(Opcode::Def, 0, 5),
        Dst(RegType::Const, 0),
        FloatBits(1.0f),
        0,
        0,
        0,
        // loop aL, i0
        Inst(Opcode::Loop, 0, 2),
        Src(RegType::Loop, 0),
        Src(RegType::ConstInt, 0),
        //   mov o1[aL], v1  (outputs can be indexed by aL)
        Inst(Opcode::Mov, 0, 3),
        Dst(RegType::TexCrdOutOrOutput, 1) | RelBit,
        Src(RegType::Loop, 0),
        Src(RegType::Input, 1),
        Inst(Opcode::EndLoop, 0, 0),
        // texldl r0, v2, s0
        Inst(Opcode::TexLdl, 0, 3),
        Dst(RegType::Temp, 0),
        Src(RegType::Input, 2),
        Src(RegType::Sampler, 0),
        // mov o0, v0
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::TexCrdOutOrOutput, 0),
        Src(RegType::Input, 0),
        // mov o2.xy, v2
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::TexCrdOutOrOutput, 2, 0x3),
        Src(RegType::Input, 2),
        // sincos r1.xy, r0.x (3_0 sincos has one source)
        Inst(Opcode::SinCos, 0, 2),
        Dst(RegType::Temp, 1, 0x3),
        Src(RegType::Temp, 0, Swz(0, 0, 0, 0)),
        // mov o3.x, r1.y
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::TexCrdOutOrOutput, 3, 0x1),
        Src(RegType::Temp, 1, Swz(1, 1, 1, 1)),
        // nrm r2.xyz, c2049 (a constant from the second constant register type)
        Inst(Opcode::Nrm, 0, 2),
        Dst(RegType::Temp, 2, 0x7),
        Src(RegType::Const2, 1),
        EndTok(),
    };

    rdcstr disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());

    rdcarray<rdcstr> expected = {
        "dcl_position v0",    "dcl_color v1",        "dcl_texcoord v2.xy", "dcl_position o0",
        "dcl_color o1",       "dcl_texcoord2 o2.xy", "dcl_fog o3.x",       "dcl_2d s0",
        "def c0, 1, 0, 0, 0", "loop aL, i0",         "  mov o1[aL], v1",   "endloop",
        "texldl r0, v2, s0",  "mov o0, v0",          "mov o2.xy, v2",      "sincos r1.xy, r0.x",
        "mov o3.x, r1.y",     "nrm r2.xyz, c2049",
    };
    CHECK(InstructionLines(disasm, true) == expected);
    CHECK(prog.instructions[7].textureType == SamplerTextureType::Tex2D);
    CHECK(prog.instructions[10].dst.rel.present);
    CHECK(prog.instructions[10].dst.rel.type == RegType::Loop);
  };

  SECTION("ps_1_1 to ps_1_3")
  {
    rdcarray<uint32_t> tokens = {
        VersionToken(true, 1, 1),
        // def c0, 0.5, 0.5, 0.5, 1
        Inst(Opcode::Def, 0, 0),
        Dst(RegType::Const, 0),
        FloatBits(0.5f),
        FloatBits(0.5f),
        FloatBits(0.5f),
        FloatBits(1.0f),
        // tex t0
        Inst(Opcode::Tex, 0, 0),
        Dst(RegType::AddrOrTexture, 0),
        // texcoord t1
        Inst(Opcode::TexCoord, 0, 0),
        Dst(RegType::AddrOrTexture, 1),
        // texm3x2pad t2, t0_bx2
        Inst(Opcode::TexM3x2Pad, 0, 0),
        Dst(RegType::AddrOrTexture, 2),
        Src(RegType::AddrOrTexture, 0, Swz(0, 1, 2, 3), SrcMod::Sign),
        // texm3x2tex t3, t0_bx2
        Inst(Opcode::TexM3x2Tex, 0, 0),
        Dst(RegType::AddrOrTexture, 3),
        Src(RegType::AddrOrTexture, 0, Swz(0, 1, 2, 3), SrcMod::Sign),
        // texkill t1
        Inst(Opcode::TexKill, 0, 0),
        Dst(RegType::AddrOrTexture, 1),
        // mul_x2 r0.xyz, t0, v0_bias
        Inst(Opcode::Mul, 0, 0),
        Dst(RegType::Temp, 0, 0x7, 0, 1),
        Src(RegType::AddrOrTexture, 0),
        Src(RegType::Input, 0, Swz(0, 1, 2, 3), SrcMod::Bias),
        // + mov_d2 r0.w, 1-v1.w (co-issued)
        Inst(Opcode::Mov, 0, 0) | CoIssue,
        Dst(RegType::Temp, 0, 0x8, 0, -1),
        Src(RegType::Input, 1, Swz(3, 3, 3, 3), SrcMod::Comp),
        // cnd r0, r0.w, t3, -c0_bx2
        Inst(Opcode::Cnd, 0, 0),
        Dst(RegType::Temp, 0),
        Src(RegType::Temp, 0, Swz(3, 3, 3, 3)),
        Src(RegType::AddrOrTexture, 3),
        Src(RegType::Const, 0, Swz(0, 1, 2, 3), SrcMod::SignNeg),
        // lrp_sat r0, v0_x2, r0, -t0_x2 (ps_1_4's _x2, printed anyway)
        Inst(Opcode::Lrp, 0, 0),
        Dst(RegType::Temp, 0, 0xF, DstMod_Saturate),
        Src(RegType::Input, 0, Swz(0, 1, 2, 3), SrcMod::X2),
        Src(RegType::Temp, 0),
        Src(RegType::AddrOrTexture, 0, Swz(0, 1, 2, 3), SrcMod::X2Neg),
        EndTok(),
    };

    rdcstr disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());
    CHECK(disasm.beginsWith("    ps_1_1\n"));

    rdcarray<rdcstr> expected = {
        "def c0, 0.5, 0.5, 0.5, 1",
        "tex t0",
        "texcoord t1",
        "texm3x2pad t2, t0_bx2",
        "texm3x2tex t3, t0_bx2",
        "texkill t1",
        "mul_x2 r0.xyz, t0, v0_bias",
        "+ mov_d2 r0.w, 1-v1.w",
        "cnd r0, r0.w, t3, -c0_bx2",
        "lrp_sat r0, v0_x2, r0, -t0_x2",
    };
    CHECK(InstructionLines(disasm) == expected);
    CHECK(prog.instructions[7].coissue);

    tokens = {
        VersionToken(true, 1, 3),
        Inst(Opcode::Tex, 0, 0),
        Dst(RegType::AddrOrTexture, 0),
        // texbem t1, t0
        Inst(Opcode::TexBem, 0, 0),
        Dst(RegType::AddrOrTexture, 1),
        Src(RegType::AddrOrTexture, 0),
        // texm3x3pad t2, t0 / texm3x3pad t3, t0 / texm3x3spec t4, t0, c0
        Inst(Opcode::TexM3x3Pad, 0, 0),
        Dst(RegType::AddrOrTexture, 2),
        Src(RegType::AddrOrTexture, 0),
        Inst(Opcode::TexM3x3Pad, 0, 0),
        Dst(RegType::AddrOrTexture, 3),
        Src(RegType::AddrOrTexture, 0),
        Inst(Opcode::TexM3x3Spec, 0, 0),
        Dst(RegType::AddrOrTexture, 4),
        Src(RegType::AddrOrTexture, 0),
        Src(RegType::Const, 0),
        // cmp r0, t1, t4, c1
        Inst(Opcode::Cmp, 0, 0),
        Dst(RegType::Temp, 0),
        Src(RegType::AddrOrTexture, 1),
        Src(RegType::AddrOrTexture, 4),
        Src(RegType::Const, 1),
        EndTok(),
    };

    disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());
    expected = {
        "tex t0",
        "texbem t1, t0",
        "texm3x3pad t2, t0",
        "texm3x3pad t3, t0",
        "texm3x3spec t4, t0, c0",
        "cmp r0, t1, t4, c1",
    };
    CHECK(InstructionLines(disasm) == expected);
  };

  SECTION("ps_1_4")
  {
    rdcarray<uint32_t> tokens = {
        VersionToken(true, 1, 4),
        // texld r0, t0
        Inst(Opcode::Tex, 0, 0),
        Dst(RegType::Temp, 0),
        Src(RegType::AddrOrTexture, 0),
        // texcrd r1.xyz, t1_dw.xyw
        Inst(Opcode::TexCoord, 0, 0),
        Dst(RegType::Temp, 1, 0x7),
        Src(RegType::AddrOrTexture, 1, Swz(0, 1, 3, 3), SrcMod::DW),
        // bem r2.xy, r0, r1
        Inst(Opcode::Bem, 0, 0),
        Dst(RegType::Temp, 2, 0x3),
        Src(RegType::Temp, 0),
        Src(RegType::Temp, 1),
        Inst(Opcode::Phase, 0, 0),
        // texld r3, r2_dz
        Inst(Opcode::Tex, 0, 0),
        Dst(RegType::Temp, 3),
        Src(RegType::Temp, 2, Swz(0, 1, 2, 3), SrcMod::DZ),
        // texdepth r5
        Inst(Opcode::TexDepth, 0, 0),
        Dst(RegType::Temp, 5),
        // mul_x8 r0, r3, c7
        Inst(Opcode::Mul, 0, 0),
        Dst(RegType::Temp, 0, 0xF, 0, 3),
        Src(RegType::Temp, 3),
        Src(RegType::Const, 7),
        EndTok(),
    };

    rdcstr disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());
    rdcarray<rdcstr> expected = {
        "texld r0, t0", "texcrd r1.xyz, t1_dw.xyw", "bem r2.xy, r0, r1", "phase", "texld r3, r2_dz",
        "texdepth r5",  "mul_x8 r0, r3, c7",
    };
    CHECK(InstructionLines(disasm) == expected);
  };

  SECTION("ps_2_0 and ps_2_x")
  {
    rdcarray<uint32_t> tokens = {
        VersionToken(true, 2, 0),
        // a comment that isn't a constant table, skipped
        CommentTok(2),
        MakeFourCC('T', 'E', 'S', 'T'),
        0x12345678,
        Inst(Opcode::Def, 0, 5),
        Dst(RegType::Const, 3),
        FloatBits(0.1f),
        FloatBits(-0.0f),
        FloatBits(1e10f),
        FloatBits(3.14159274f),
        // dcl t0.xy
        Inst(Opcode::Dcl, 0, 2),
        DclNone(),
        Dst(RegType::AddrOrTexture, 0, 0x3),
        // dcl v0
        Inst(Opcode::Dcl, 0, 2),
        DclNone(),
        Dst(RegType::Input, 0),
        // dcl_2d s0, dcl_cube s1, dcl_volume s2
        Inst(Opcode::Dcl, 0, 2),
        DclSampler(2),
        Dst(RegType::Sampler, 0),
        Inst(Opcode::Dcl, 0, 2),
        DclSampler(3),
        Dst(RegType::Sampler, 1),
        Inst(Opcode::Dcl, 0, 2),
        DclSampler(4),
        Dst(RegType::Sampler, 2),
        // texld r0, t0, s0
        Inst(Opcode::Tex, 0, 3),
        Dst(RegType::Temp, 0),
        Src(RegType::AddrOrTexture, 0),
        Src(RegType::Sampler, 0),
        // texldp r1, t0, s1
        Inst(Opcode::Tex, 1, 3),
        Dst(RegType::Temp, 1),
        Src(RegType::AddrOrTexture, 0),
        Src(RegType::Sampler, 1),
        // texldb_pp r2, t0, s2
        Inst(Opcode::Tex, 2, 3),
        Dst(RegType::Temp, 2, 0xF, DstMod_PartialPrecision),
        Src(RegType::AddrOrTexture, 0),
        Src(RegType::Sampler, 2),
        // dp2add r0.x, r0, r1, c3.x
        Inst(Opcode::Dp2Add, 0, 4),
        Dst(RegType::Temp, 0, 0x1),
        Src(RegType::Temp, 0),
        Src(RegType::Temp, 1),
        Src(RegType::Const, 3, Swz(0, 0, 0, 0)),
        // texkill r0
        Inst(Opcode::TexKill, 0, 1),
        Dst(RegType::Temp, 0),
        // mov oC0, r0 / mov oC1, r1 / mov oDepth, r2.x
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::ColorOut, 0),
        Src(RegType::Temp, 0),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::ColorOut, 1),
        Src(RegType::Temp, 1),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::DepthOut, 0, 0x1),
        Src(RegType::Temp, 2, Swz(0, 0, 0, 0)),
        EndTok(),
    };

    rdcstr disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());
    CHECK(prog.comments.size() == 1);
    rdcarray<rdcstr> expected = {
        "def c3, 0.100000001, -0, 1e+10, 3.14159274",
        "dcl t0.xy",
        "dcl v0",
        "dcl_2d s0",
        "dcl_cube s1",
        "dcl_volume s2",
        "texld r0, t0, s0",
        "texldp r1, t0, s1",
        "texldb_pp r2, t0, s2",
        "dp2add r0.x, r0, r1, c3.x",
        "texkill r0",
        "mov oC0, r0",
        "mov oC1, r1",
        "mov oDepth.x, r2.x",
    };
    CHECK(InstructionLines(disasm) == expected);

    tokens = {
        VersionToken(true, 2, 1),
        Inst(Opcode::Dcl, 0, 2),
        DclNone(),
        Dst(RegType::AddrOrTexture, 0),
        // dsx r0, t0 / dsy r1, t0
        Inst(Opcode::Dsx, 0, 2),
        Dst(RegType::Temp, 0),
        Src(RegType::AddrOrTexture, 0),
        Inst(Opcode::Dsy, 0, 2),
        Dst(RegType::Temp, 1),
        Src(RegType::AddrOrTexture, 0),
        // texldd r2, t0, s0, r0, r1
        Inst(Opcode::TexLdd, 0, 5),
        Dst(RegType::Temp, 2),
        Src(RegType::AddrOrTexture, 0),
        Src(RegType::Sampler, 0),
        Src(RegType::Temp, 0),
        Src(RegType::Temp, 1),
        EndTok(),
    };

    disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());
    CHECK(disasm.beginsWith("    ps_2_x\n"));
    expected = {
        "dcl t0",
        "dsx r0, t0",
        "dsy r1, t0",
        "texldd r2, t0, s0, r0, r1",
    };
    CHECK(InstructionLines(disasm) == expected);
  };

  SECTION("ps_3_0")
  {
    rdcarray<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Color, 0),
        Dst(RegType::Input, 0),
        // dcl_texcoord1_centroid v1.xy
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 1),
        Dst(RegType::Input, 1, 0x3, DstMod_Centroid),
        // dcl vPos.xy / dcl vFace
        Inst(Opcode::Dcl, 0, 2),
        DclNone(),
        Dst(RegType::MiscType, 0, 0x3),
        Inst(Opcode::Dcl, 0, 2),
        DclNone(),
        Dst(RegType::MiscType, 1),
        Inst(Opcode::Dcl, 0, 2),
        DclSampler(2),
        Dst(RegType::Sampler, 3),
        // texld r0, v1, s3
        Inst(Opcode::Tex, 0, 3),
        Dst(RegType::Temp, 0),
        Src(RegType::Input, 1),
        Src(RegType::Sampler, 3),
        // if_ge vFace, c0.x
        Inst(Opcode::IfC, 3, 2),
        Src(RegType::MiscType, 1),
        Src(RegType::Const, 0, Swz(0, 0, 0, 0)),
        //   add r0.xy, r0, vPos
        Inst(Opcode::Add, 0, 3),
        Dst(RegType::Temp, 0, 0x3),
        Src(RegType::Temp, 0),
        Src(RegType::MiscType, 0),
        Inst(Opcode::EndIf, 0, 0),
        // loop aL, i0 / add r0, r0, v0[aL] / endloop
        Inst(Opcode::Loop, 0, 2),
        Src(RegType::Loop, 0),
        Src(RegType::ConstInt, 0),
        Inst(Opcode::Add, 0, 4),
        Dst(RegType::Temp, 0),
        Src(RegType::Temp, 0),
        Src(RegType::Input, 0) | RelBit,
        Src(RegType::Loop, 0),
        Inst(Opcode::EndLoop, 0, 0),
        // mov_sat_pp oC0, r0
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::ColorOut, 0, 0xF, DstMod_Saturate | DstMod_PartialPrecision),
        Src(RegType::Temp, 0),
        EndTok(),
    };

    rdcstr disasm = DisassembleTokens(tokens, prog);
    INFO(disasm.c_str());
    CHECK(prog.error.empty());
    rdcarray<rdcstr> expected = {
        "dcl_color v0",
        "dcl_texcoord1_centroid v1.xy",
        "dcl vPos.xy",
        "dcl vFace",
        "dcl_2d s3",
        "texld r0, v1, s3",
        "if_ge vFace, c0.x",
        "  add r0.xy, r0, vPos",
        "endif",
        "loop aL, i0",
        "  add r0, r0, v0[aL]",
        "endloop",
        "mov_sat_pp oC0, r0",
    };
    CHECK(InstructionLines(disasm, true) == expected);
  };

  SECTION("Malformed bytecode")
  {
    // nothing at all
    rdcstr disasm = DisassembleTokens({}, prog);
    CHECK(!prog.validVersion);
    CHECK(!prog.error.empty());
    CHECK(disasm.contains("Couldn't disassemble"));

    ParseProgram(NULL, 5, prog);
    CHECK(!prog.validVersion);

    // not a version token
    disasm = DisassembleTokens({0x12345678, EndTok()}, prog);
    CHECK(!prog.validVersion);
    CHECK(prog.error.contains("version token"));

    // a version we don't know
    disasm = DisassembleTokens({VersionToken(true, 4, 0), EndTok()}, prog);
    CHECK(!prog.validVersion);

    // no end token, the instructions are still there
    disasm = DisassembleTokens({VersionToken(true, 2, 0), Inst(Opcode::Mov, 0, 2),
                                Dst(RegType::ColorOut, 0), Src(RegType::Temp, 0)},
                               prog);
    CHECK(prog.validVersion);
    CHECK(prog.instructions.size() == 1);
    CHECK(prog.error.contains("end token"));
    CHECK(disasm.contains("mov oC0, r0"));
    CHECK(disasm.contains("Disassembly stopped"));

    // truncated instruction
    disasm = DisassembleTokens(
        {VersionToken(true, 2, 0), Inst(Opcode::Mov, 0, 2), Dst(RegType::ColorOut, 0)}, prog);
    CHECK(prog.instructions.empty());
    CHECK(prog.error.contains("past the end"));

    // truncated 1_x instruction
    disasm = DisassembleTokens({VersionToken(true, 1, 1), Inst(Opcode::Mad, 0, 0),
                                Dst(RegType::Temp, 0), Src(RegType::Temp, 0)},
                               prog);
    CHECK(prog.instructions.empty());
    CHECK(prog.error.contains("past the end"));

    // a comment longer than the shader
    disasm = DisassembleTokens({VersionToken(true, 2, 0), CommentTok(100), 0, 0}, prog);
    CHECK(prog.error.contains("comment"));

    // an unknown opcode
    disasm = DisassembleTokens({VersionToken(true, 2, 0), Inst(Opcode(200), 0, 0), EndTok()}, prog);
    CHECK(prog.error.contains("Unknown opcode"));

    // a 1_x shader with an opcode it can't have, and no length
    disasm = DisassembleTokens({VersionToken(false, 1, 1), Inst(Opcode::Pow, 0, 0), EndTok()}, prog);
    CHECK(prog.error.contains("isn't valid"));

    // a length that's too short leaves a parameter where the next instruction should be
    disasm = DisassembleTokens({VersionToken(true, 2, 0), Inst(Opcode::Mov, 0, 1),
                                Dst(RegType::ColorOut, 0), Src(RegType::Temp, 0), EndTok()},
                               prog);
    CHECK(prog.instructions.size() == 1);
    CHECK(prog.error.contains("Expected an instruction"));

    // a parameter missing its top bit
    disasm = DisassembleTokens(
        {VersionToken(true, 2, 0), Inst(Opcode::Mov, 0, 2),
         Dst(RegType::ColorOut, 0) & ~0x80000000U, Src(RegType::Temp, 0), EndTok()},
        prog);
    CHECK(prog.instructions.empty());
    CHECK(prog.error.contains("parameter token"));

    // relative addressing whose address token is missing
    disasm = DisassembleTokens({VersionToken(false, 2, 0), Inst(Opcode::Mov, 0, 2),
                                Dst(RegType::Temp, 0), Src(RegType::Const, 0) | RelBit, EndTok()},
                               prog);
    CHECK(prog.instructions.empty());
    CHECK(!prog.error.empty());

    // dcl with no parameters
    disasm = DisassembleTokens({VersionToken(false, 3, 0), Inst(Opcode::Dcl, 0, 0), EndTok()}, prog);
    CHECK(prog.instructions.empty());
    CHECK(!prog.error.empty());

    // unknown registers, modifiers and usages print rather than crash
    disasm =
        DisassembleTokens({VersionToken(false, 3, 0), Inst(Opcode::Dcl, 0, 2), Dcl(DeclUsage(15), 0),
                           Dst(RegType::Input, 0), Inst(Opcode::Mov, 0, 2), Dst(RegType(31), 0),
                           Src(RegType(25), 0, Swz(0, 1, 2, 3), SrcMod(15)), EndTok()},
                          prog);
    CHECK(prog.error.empty());
    CHECK(disasm.contains("unknownusage15"));
    CHECK(disasm.contains("unknownreg31_0"));
    CHECK(disasm.contains("_unknownmod15"));

    // random data never crashes, whatever it decodes to
    uint32_t seed = 12345;
    for(int iter = 0; iter < 2000; iter++)
    {
      rdcarray<uint32_t> random;
      random.push_back(VersionToken((iter & 1) != 0, uint8_t(1 + (iter % 3)), uint8_t(iter % 5)));
      int len = 1 + (iter % 40);
      for(int i = 0; i < len; i++)
      {
        seed = seed * 1103515245 + 12345;
        uint32_t tok = seed;
        // bias towards plausible opcodes, so more of the decoder is reached
        if((i % 3) == 0)
          tok = (tok & 0x1F00FF7F);
        random.push_back(tok);
      }
      disasm = DisassembleTokens(random, prog);
      CHECK(!disasm.empty());
    }
  };
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
