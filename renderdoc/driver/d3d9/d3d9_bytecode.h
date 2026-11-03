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

#include "api/replay/renderdoc_replay.h"

// Parsing and disassembly of D3D9 (shader model 1 to 3) shader bytecode.
//
// This is platform-neutral: it doesn't use the Windows headers, so it has its own definitions of
// the token layouts (named after d3d9types.h's D3DSIO_, D3DSPR_ and D3DSP_ values, which the
// Windows build has in scope as macros and enums, hence the different names here).
//
// A shader is a stream of DWORD tokens: a version token, then instructions and comments, then an
// end token. Each instruction is an instruction token followed by its parameters. Shader model 2
// and later encode each instruction's length in its token; 1_x shaders don't, so their lengths
// come from the opcode tables in d3d9_bytecode.cpp.
namespace D3D9BC
{
enum class ShaderType : uint8_t
{
  Vertex,
  Pixel,
};

struct Version
{
  ShaderType type = ShaderType::Vertex;
  uint8_t major = 0;
  uint8_t minor = 0;

  bool IsVS() const { return type == ShaderType::Vertex; }
  bool IsPS() const { return type == ShaderType::Pixel; }
  // vs_1_1, ps_1_1 to ps_1_4
  bool IsSM1() const { return major < 2; }
  // at least major.minor
  bool AtLeast(uint8_t ma, uint8_t mi) const { return major > ma || (major == ma && minor >= mi); }
};

// D3DSHADER_INSTRUCTION_OPCODE_TYPE
enum class Opcode : uint16_t
{
  Nop = 0,
  Mov,
  Add,
  Sub,
  Mad,
  Mul,
  Rcp,
  Rsq,
  Dp3,
  Dp4,
  Min,
  Max,
  Slt,
  Sge,
  Exp,
  Log,
  Lit,
  Dst,
  Lrp,
  Frc,
  M4x4,
  M4x3,
  M3x4,
  M3x3,
  M3x2,
  Call,
  CallNZ,
  Loop,
  Ret,
  EndLoop,
  Label,
  Dcl,
  Pow,
  Crs,
  Sgn,
  Abs,
  Nrm,
  SinCos,
  Rep,
  EndRep,
  If,
  IfC,
  Else,
  EndIf,
  Break,
  BreakC,
  MovA,
  DefB,
  DefI,

  TexCoord = 64,
  TexKill,
  Tex,
  TexBem,
  TexBemL,
  TexReg2AR,
  TexReg2GB,
  TexM3x2Pad,
  TexM3x2Tex,
  TexM3x3Pad,
  TexM3x3Tex,
  Reserved0,
  TexM3x3Spec,
  TexM3x3VSpec,
  ExpP,
  LogP,
  Cnd,
  Def,
  TexReg2RGB,
  TexDP3Tex,
  TexM3x2Depth,
  TexDP3,
  TexM3x3,
  TexDepth,
  Cmp,
  Bem,
  Dp2Add,
  Dsx,
  Dsy,
  TexLdd,
  SetP,
  TexLdl,
  BreakP,

  Phase = 0xFFFD,
  Comment = 0xFFFE,
  End = 0xFFFF,
};

// D3DSHADER_PARAM_REGISTER_TYPE. Some values mean different things in different shader types and
// versions: 3 is a0 in vertex shaders and t# in pixel shaders, 6 is oT# before vs_3_0 and o# in it.
enum class RegType : uint8_t
{
  Temp = 0,
  Input = 1,
  Const = 2,
  AddrOrTexture = 3,
  RastOut = 4,
  AttrOut = 5,
  TexCrdOutOrOutput = 6,
  ConstInt = 7,
  ColorOut = 8,
  DepthOut = 9,
  Sampler = 10,
  Const2 = 11,
  Const3 = 12,
  Const4 = 13,
  ConstBool = 14,
  Loop = 15,
  TempFloat16 = 16,
  MiscType = 17,
  Label = 18,
  Predicate = 19,
  Count,
};

// D3DSHADER_PARAM_SRCMOD_TYPE
enum class SrcMod : uint8_t
{
  None = 0,
  Neg,
  Bias,
  BiasNeg,
  Sign,
  SignNeg,
  Comp,
  X2,
  X2Neg,
  DZ,
  DW,
  Abs,
  AbsNeg,
  Not,
};

// D3DSPDM_* result modifier bits, as they are in the destination token shifted down
enum DstModFlags : uint8_t
{
  DstMod_None = 0,
  DstMod_Saturate = 0x1,
  DstMod_PartialPrecision = 0x2,
  DstMod_Centroid = 0x4,
};

// D3DDECLUSAGE
enum class DeclUsage : uint8_t
{
  Position = 0,
  BlendWeight,
  BlendIndices,
  Normal,
  PSize,
  TexCoord,
  Tangent,
  Binormal,
  TessFactor,
  PositionT,
  Color,
  Fog,
  Depth,
  Sample,
  Count,
};

// D3DSAMPLER_TEXTURE_TYPE, shifted down
enum class SamplerTextureType : uint8_t
{
  Unknown = 0,
  Tex1D = 1,
  Tex2D = 2,
  Cube = 3,
  Volume = 4,
};

// D3DSHADER_COMPARISON, for ifc, breakc and setp
enum class Comparison : uint8_t
{
  None = 0,
  GT,
  EQ,
  GE,
  LT,
  NE,
  LE,
};

// the register a relatively addressed parameter is indexed by (a0.x or aL)
struct RelativeAddress
{
  bool present = false;
  RegType type = RegType::AddrOrTexture;
  uint32_t index = 0;
  // the component of the address register used, 0-3
  uint8_t component = 0;
};

struct SrcParam
{
  RegType type = RegType::Temp;
  uint32_t index = 0;
  // the component each of x, y, z and w reads, 0-3
  uint8_t swizzle[4] = {0, 1, 2, 3};
  SrcMod mod = SrcMod::None;
  RelativeAddress rel;

  bool IdentitySwizzle() const
  {
    return swizzle[0] == 0 && swizzle[1] == 1 && swizzle[2] == 2 && swizzle[3] == 3;
  }
  // the components read, as a mask, assuming all four results are used
  uint8_t ReadMask() const
  {
    return uint8_t((1 << swizzle[0]) | (1 << swizzle[1]) | (1 << swizzle[2]) | (1 << swizzle[3]));
  }
};

struct DstParam
{
  RegType type = RegType::Temp;
  uint32_t index = 0;
  // bit n set writes component n
  uint8_t writeMask = 0xF;
  // DstModFlags
  uint8_t mods = DstMod_None;
  // the result is multiplied by 2^shift, -3 to 3
  int8_t shift = 0;
  RelativeAddress rel;
};

struct Instruction
{
  Opcode op = Opcode::Nop;
  // the offset of the instruction token, in DWORDs from the version token
  uint32_t offset = 0;
  // the number of DWORDs after the instruction token
  uint32_t length = 0;
  // bits 16-23 of the instruction token, the opcode specific controls
  uint8_t control = 0;
  // ps_1_x, executes alongside the previous instruction ('+' in the disassembly)
  bool coissue = false;

  bool hasDst = false;
  DstParam dst;

  bool predicated = false;
  SrcParam predicate;

  rdcarray<SrcParam> srcs;

  // dcl
  DeclUsage usage = DeclUsage::Position;
  uint32_t usageIndex = 0;
  SamplerTextureType textureType = SamplerTextureType::Unknown;

  // def, defi and defb: the float bits, integers or the bool (in [0]) given
  uint32_t literal[4] = {};

  Comparison GetComparison() const { return Comparison(control & 0x7); }
};

struct CommentBlock
{
  // the offset of the first DWORD after the comment token, in DWORDs from the version token
  uint32_t offset = 0;
  // the comment's length in DWORDs, not including the comment token
  uint32_t length = 0;
};

struct Program
{
  Version version;
  // whether there was a valid version token at all
  bool validVersion = false;
  rdcarray<Instruction> instructions;
  rdcarray<CommentBlock> comments;
  // the tokens, as given
  rdcarray<uint32_t> tokens;
  // empty if the whole shader parsed, otherwise what went wrong. What parsed before it is kept.
  rdcstr error;

  // a comment whose first DWORD is the given four character code, or NULL. Returns the DWORDs
  // after the code and their count.
  const uint32_t *FindComment(uint32_t fourcc, uint32_t &length) const;
};

// Parses a shader. Never fails outright: on bad bytecode it stops at the first problem, fills
// Program::error and keeps what it parsed up to there.
void ParseProgram(const uint32_t *tokens, size_t numTokens, Program &prog);

// The name of a shader version, e.g. "vs_3_0", "ps_2_x".
rdcstr VersionString(const Version &version);

// The name of an opcode as the disassembly shows it, including texld/texldp/texldb and the
// comparison of ifc, breakc and setp.
rdcstr OpcodeName(const Version &version, const Instruction &inst);

// A register's name, e.g. "r0", "oPos", "c2050", "vFace".
rdcstr RegisterName(const Version &version, RegType type, uint32_t index);

// The disassembly of the instructions, in the style of fxc and D3DXDisassembleShader. It starts at
// the version line; the constant table comment is generated separately (see d3d9_shader_reflect.h).
// If the shader didn't parse completely, the error is shown after the instructions that did.
rdcstr Disassemble(const Program &prog);

// The DWORD for a four character code, as comments store them
constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
{
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
         (uint32_t(uint8_t(d)) << 24);
}

};    // namespace D3D9BC
