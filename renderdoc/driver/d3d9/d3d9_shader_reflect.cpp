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

#include "d3d9_shader_reflect.h"
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include "common/common.h"
#include "common/formatting.h"
#include "replay/replay_driver.h"

namespace D3D9BC
{
namespace
{
// constant tables are nested structures of offsets, so limit how deep types can go in case a
// broken one refers back to itself
const int MaxTypeDepth = 16;

class CTABReader
{
public:
  CTABReader(const uint32_t *data, uint32_t numDWORDs)
      : m_Data((const byte *)data), m_Size(numDWORDs * sizeof(uint32_t))
  {
  }

  bool Read16(uint32_t offset, uint16_t &val) const
  {
    if(offset > m_Size || m_Size - offset < sizeof(uint16_t))
      return false;
    memcpy(&val, m_Data + offset, sizeof(uint16_t));
    return true;
  }

  bool Read32(uint32_t offset, uint32_t &val) const
  {
    if(offset > m_Size || m_Size - offset < sizeof(uint32_t))
      return false;
    memcpy(&val, m_Data + offset, sizeof(uint32_t));
    return true;
  }

  // strings are NULL-terminated, and must end inside the table
  bool ReadString(uint32_t offset, rdcstr &str) const
  {
    str.clear();
    if(offset >= m_Size)
      return false;
    const char *start = (const char *)m_Data + offset;
    const void *end = memchr(start, 0, m_Size - offset);
    if(!end)
      return false;
    str.assign(start, (const char *)end - start);
    return true;
  }

  bool ReadType(uint32_t offset, CTABType &type, int depth) const
  {
    if(depth > MaxTypeDepth)
      return false;

    // Class, Type, Rows, Columns, Elements and StructMembers, then the offset of the members
    uint16_t vals[6] = {};
    for(uint32_t i = 0; i < 6; i++)
      if(!Read16(offset + i * 2, vals[i]))
        return false;
    uint32_t memberOffset = 0;
    if(!Read32(offset + 12, memberOffset))
      return false;

    type.cls = ParamClass(vals[0]);
    type.type = ParamType(vals[1]);
    type.rows = vals[2];
    type.columns = vals[3];
    type.elements = vals[4];

    const uint16_t numMembers = vals[5];
    type.members.clear();
    if(numMembers > 0)
    {
      // a member is 8 bytes, check they all fit before allocating anything
      if(memberOffset > m_Size || (m_Size - memberOffset) / 8 < numMembers)
        return false;

      type.members.resize(numMembers);
      for(uint16_t m = 0; m < numMembers; m++)
      {
        uint32_t nameOffset = 0, typeOffset = 0;
        if(!Read32(memberOffset + m * 8, nameOffset) || !Read32(memberOffset + m * 8 + 4, typeOffset))
          return false;
        if(!ReadString(nameOffset, type.members[m].name))
          return false;
        if(!ReadType(typeOffset, type.members[m].type, depth + 1))
          return false;
      }
    }

    return true;
  }

private:
  const byte *m_Data;
  uint32_t m_Size;
};

ConstantSet SetFor(RegisterSet set)
{
  if(set == RegisterSet::Int4)
    return ConstantSet::Int;
  if(set == RegisterSet::Bool)
    return ConstantSet::Bool;
  return ConstantSet::Float;
}

uint32_t RegisterStride(ConstantSet set)
{
  return set == ConstantSet::Bool ? 4 : 16;
}

VarType BaseTypeFor(ConstantSet set)
{
  // what the registers hold, whatever the HLSL type is: an int or bool the compiler put in float
  // registers is a float there.
  if(set == ConstantSet::Int)
    return VarType::SInt;
  if(set == ConstantSet::Bool)
    return VarType::Bool;
  return VarType::Float;
}

// The registers one element of a type takes. In the float and integer sets a scalar or vector
// takes a register, and a matrix a register per row (row_major) or column. In the boolean set
// every component takes a register. Saturates rather than overflowing for broken tables.
const uint32_t MaxRegisters = 1U << 24;

uint32_t RegistersPerElement(const CTABType &type, ConstantSet set, int depth = 0)
{
  if(depth > MaxTypeDepth)
    return 1;

  const bool isBool = set == ConstantSet::Bool;
  switch(type.cls)
  {
    case ParamClass::Scalar: return 1;
    case ParamClass::Vector: return isBool ? RDCMAX(1U, uint32_t(type.columns)) : 1;
    case ParamClass::MatrixRows:
      return RDCMAX(1U, isBool ? uint32_t(type.rows) * uint32_t(type.columns) : uint32_t(type.rows));
    case ParamClass::MatrixColumns:
      return RDCMAX(1U,
                    isBool ? uint32_t(type.rows) * uint32_t(type.columns) : uint32_t(type.columns));
    case ParamClass::Struct:
    {
      uint64_t ret = 0;
      for(const CTABMember &m : type.members)
      {
        ret += uint64_t(RegistersPerElement(m.type, set, depth + 1)) *
               RDCMAX(1U, uint32_t(m.type.elements));
        if(ret >= MaxRegisters)
          return MaxRegisters;
      }
      return RDCMAX(1U, uint32_t(ret));
    }
    case ParamClass::Object: return 1;
    default: break;
  }
  return 1;
}

// budget is how many registers the type can cover. The compiler only allocates the registers an
// array uses up to the last element it reads, the registers after that can belong to something
// else, so arrays are cut short to fit. That also keeps broken tables from describing more
// variables than there are registers.
ShaderConstantType MakeConstantType(const CTABType &type, ConstantSet set, uint32_t budget,
                                    int depth = 0)
{
  ShaderConstantType ret;
  ret.name = CTABTypeName(type);

  const uint32_t stride = RegisterStride(set);
  const bool isBool = set == ConstantSet::Bool;

  ret.baseType = BaseTypeFor(set);

  budget = RDCMAX(1U, budget);
  const uint32_t perElement = RegistersPerElement(type, set);

  switch(type.cls)
  {
    case ParamClass::Scalar:
      ret.rows = 1;
      ret.columns = 1;
      break;
    case ParamClass::Vector:
      ret.rows = 1;
      ret.columns = uint8_t(RDCCLAMP(uint32_t(type.columns), 1U, 4U));
      break;
    case ParamClass::MatrixRows:
    case ParamClass::MatrixColumns:
    {
      ret.rows = uint8_t(RDCCLAMP(uint32_t(type.rows), 1U, 4U));
      ret.columns = uint8_t(RDCCLAMP(uint32_t(type.columns), 1U, 4U));
      const bool rowMajor = type.cls == ParamClass::MatrixRows;
      if(rowMajor)
        ret.flags |= ShaderVariableFlags::RowMajorMatrix;
      // each row (row_major) or column is a register, or in the boolean set each component is
      ret.matrixByteStride = uint8_t(isBool ? 4 * (rowMajor ? ret.columns : ret.rows) : 16);
      break;
    }
    case ParamClass::Struct:
    {
      ret.baseType = VarType::Struct;
      ret.rows = 0;
      ret.columns = 0;
      const uint32_t elementBudget = RDCMIN(perElement, budget);
      uint32_t reg = 0;
      for(const CTABMember &m : type.members)
      {
        // members past the budget would be in registers that aren't this variable's
        if(reg >= elementBudget || depth >= MaxTypeDepth)
          break;
        ShaderConstant member;
        member.name = m.name;
        member.byteOffset = reg * stride;
        member.type = MakeConstantType(m.type, set, elementBudget - reg, depth + 1);
        ret.members.push_back(member);
        const uint64_t memberRegs =
            uint64_t(RegistersPerElement(m.type, set)) * RDCMAX(1U, uint32_t(m.type.elements));
        reg = uint32_t(RDCMIN(uint64_t(reg) + memberRegs, uint64_t(MaxRegisters)));
      }
      break;
    }
    case ParamClass::Object:
    default:
      ret.rows = 1;
      ret.columns = 1;
      break;
  }

  uint32_t elements = RDCMAX(1U, uint32_t(type.elements));
  if(uint64_t(elements) * perElement > budget)
    elements = RDCMAX(1U, (budget + perElement - 1) / perElement);

  ret.elements = elements;
  ret.arrayByteStride = uint32_t(RDCMIN(uint64_t(perElement) * stride, uint64_t(MaxRegisters)));

  return ret;
}

// a register variable for registers the code uses that the constant table doesn't describe
// more registers than any set has, for limiting what broken tables describe
const uint32_t MaxSetRegisters = 8192;

ShaderConstant MakeRegisterConstant(ConstantSet set, uint32_t reg, uint32_t elements)
{
  static const char *const prefixes[] = {"c", "i", "b"};
  static const char *const typeNames[] = {"float4", "int4", "bool"};

  ShaderConstant ret;
  ret.name = StringFormat::Fmt("%s%u", prefixes[uint32_t(set)], reg);
  ret.byteOffset = reg * RegisterStride(set);
  ret.type.name = typeNames[uint32_t(set)];
  ret.type.baseType = BaseTypeFor(set);
  ret.type.rows = 1;
  ret.type.columns = set == ConstantSet::Bool ? 1 : 4;
  ret.type.elements = RDCMAX(1U, elements);
  ret.type.arrayByteStride = RegisterStride(set);
  return ret;
}

// how many registers a constant block variable covers
uint32_t VariableRegisters(const ShaderConstant &var, ConstantSet set)
{
  const uint32_t stride = RegisterStride(set);
  const uint32_t size = var.type.arrayByteStride * RDCMAX(1U, var.type.elements);
  return RDCMAX(1U, (size + stride - 1) / stride);
}

// registers are tracked by a key of their type and index, with the constant types above c2047
// folded into c#
uint32_t RegKey(RegType type, uint32_t index)
{
  if(type == RegType::Const2)
  {
    type = RegType::Const;
    index += 2048;
  }
  else if(type == RegType::Const3)
  {
    type = RegType::Const;
    index += 4096;
  }
  else if(type == RegType::Const4)
  {
    type = RegType::Const;
    index += 6144;
  }
  return (uint32_t(type) << 16) | (index & 0xFFFF);
}

RegType KeyType(uint32_t key)
{
  return RegType(key >> 16);
}

uint32_t KeyIndex(uint32_t key)
{
  return key & 0xFFFF;
}

// a usage declared for (some components of) a register
struct RegDecl
{
  uint8_t mask = 0;
  DeclUsage usage = DeclUsage::Position;
  uint32_t usageIndex = 0;
};

struct RegUsage
{
  uint8_t read = 0;
  uint8_t written = 0;
  bool declared = false;
  // the components of all of the register's declarations
  uint8_t declMask = 0;
  // vs_3_0 outputs and ps_3_0 inputs can have two or more usages in one register, each with its
  // own components (dcl_texcoord0 o1.xy, dcl_texcoord1 o1.zw), in the order they're declared
  rdcarray<RegDecl> decls;
  SamplerTextureType textureType = SamplerTextureType::Unknown;
};

// whether an instruction's sources are read per component of the destination
bool IsComponentwise(Opcode op)
{
  switch(op)
  {
    case Opcode::Mov:
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mad:
    case Opcode::Mul:
    case Opcode::Min:
    case Opcode::Max:
    case Opcode::Slt:
    case Opcode::Sge:
    case Opcode::Frc:
    case Opcode::Lrp:
    case Opcode::Cnd:
    case Opcode::Cmp:
    case Opcode::Abs:
    case Opcode::Sgn:
    case Opcode::MovA:
    case Opcode::Dsx:
    case Opcode::Dsy:
    case Opcode::SetP:
    // ps_1_4's texcrd
    case Opcode::TexCoord: return true;
    default: break;
  }
  return false;
}

uint8_t SourceReadMask(const Instruction &inst, size_t srcIdx)
{
  const SrcParam &src = inst.srcs[srcIdx];
  uint8_t used = 0xF;

  if(inst.hasDst && IsComponentwise(inst.op))
    used = inst.dst.writeMask;
  else if(inst.op == Opcode::Dp3 || inst.op == Opcode::M3x2 || inst.op == Opcode::M3x3 ||
          inst.op == Opcode::M3x4)
    used = 0x7;
  else if(inst.op == Opcode::Dp2Add && srcIdx < 2)
    used = 0x3;
  else if(inst.op == Opcode::Dp2Add)
    used = 0x1;

  uint8_t ret = 0;
  for(int c = 0; c < 4; c++)
    if(used & (1 << c))
      ret |= uint8_t(1 << src.swizzle[c]);
  return ret;
}

// ps_1_0 to ps_1_3's texture addressing instructions fill t# from the texture coordinates of the
// same number, and the ones that sample read the sampler of that number
bool IsSM1TexAddressOp(Opcode op)
{
  switch(op)
  {
    case Opcode::TexCoord:
    case Opcode::TexKill:
    case Opcode::Tex:
    case Opcode::TexBem:
    case Opcode::TexBemL:
    case Opcode::TexReg2AR:
    case Opcode::TexReg2GB:
    case Opcode::TexM3x2Pad:
    case Opcode::TexM3x2Tex:
    case Opcode::TexM3x3Pad:
    case Opcode::TexM3x3Tex:
    case Opcode::TexM3x3Spec:
    case Opcode::TexM3x3VSpec:
    case Opcode::TexReg2RGB:
    case Opcode::TexDP3Tex:
    case Opcode::TexM3x2Depth:
    case Opcode::TexDP3:
    case Opcode::TexM3x3: return true;
    default: break;
  }
  return false;
}

bool IsSM1SamplingOp(Opcode op)
{
  switch(op)
  {
    case Opcode::Tex:
    case Opcode::TexBem:
    case Opcode::TexBemL:
    case Opcode::TexReg2AR:
    case Opcode::TexReg2GB:
    case Opcode::TexM3x2Tex:
    case Opcode::TexM3x3Tex:
    case Opcode::TexM3x3Spec:
    case Opcode::TexM3x3VSpec:
    case Opcode::TexReg2RGB:
    case Opcode::TexDP3Tex: return true;
    default: break;
  }
  return false;
}

const char *SemanticName(DeclUsage usage)
{
  switch(usage)
  {
    case DeclUsage::Position: return "POSITION";
    case DeclUsage::BlendWeight: return "BLENDWEIGHT";
    case DeclUsage::BlendIndices: return "BLENDINDICES";
    case DeclUsage::Normal: return "NORMAL";
    case DeclUsage::PSize: return "PSIZE";
    case DeclUsage::TexCoord: return "TEXCOORD";
    case DeclUsage::Tangent: return "TANGENT";
    case DeclUsage::Binormal: return "BINORMAL";
    case DeclUsage::TessFactor: return "TESSFACTOR";
    case DeclUsage::PositionT: return "POSITIONT";
    case DeclUsage::Color: return "COLOR";
    case DeclUsage::Fog: return "FOG";
    case DeclUsage::Depth: return "DEPTH";
    case DeclUsage::Sample: return "SAMPLE";
    default: break;
  }
  return "UNKNOWN";
}

void AddSignature(rdcarray<SigParameter> &sig, const char *semantic, uint32_t semanticIndex,
                  uint32_t reg, uint8_t mask, uint8_t used, ShaderBuiltin builtin)
{
  SigParameter p;
  p.semanticName = semantic;
  p.semanticIndex = uint16_t(semanticIndex);
  p.regIndex = reg;
  p.systemValue = builtin;
  p.varType = VarType::Float;
  if(mask == 0)
    mask = 0xF;
  p.regChannelMask = mask;
  p.channelUsedMask = used ? uint8_t(used & mask) : mask;
  // the components the mask has, as DXBC counts them: dcl_texcoord1 o1.zw is 2 components,
  // o1.z and o1.w
  p.compCount =
      uint8_t(((mask >> 0) & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1) + ((mask >> 3) & 1));
  sig.push_back(p);
}

// an element for each of a register's declarations, in the order of their first components
void AddDeclarations(rdcarray<SigParameter> &sig, const RegUsage &u, uint32_t reg, uint8_t used,
                     bool vsOutput)
{
  auto first = [](uint8_t mask) {
    for(uint32_t c = 0; c < 4; c++)
      if(mask & (1 << c))
        return c;
    return 0U;
  };

  rdcarray<RegDecl> decls = u.decls;
  std::stable_sort(decls.begin(), decls.end(), [&first](const RegDecl &a, const RegDecl &b) {
    return first(a.mask) < first(b.mask);
  });

  for(const RegDecl &d : decls)
  {
    ShaderBuiltin builtin = ShaderBuiltin::Undefined;
    if(vsOutput && d.usage == DeclUsage::Position && d.usageIndex == 0)
      builtin = ShaderBuiltin::Position;
    else if(vsOutput && d.usage == DeclUsage::PSize)
      builtin = ShaderBuiltin::PointSize;
    AddSignature(sig, SemanticName(d.usage), d.usageIndex, reg, d.mask, used, builtin);
  }
}

void FinishSignature(rdcarray<SigParameter> &sig)
{
  // as DXBC does it, the semantic index is shown if it isn't 0 or another parameter has the same
  // semantic
  for(SigParameter &a : sig)
  {
    a.needSemanticIndex = a.semanticIndex != 0;
    for(const SigParameter &b : sig)
      if(&a != &b && a.semanticName == b.semanticName)
        a.needSemanticIndex = true;

    a.semanticIdxName = a.semanticName;
    if(a.needSemanticIndex)
      a.semanticIdxName += StringFormat::Fmt("%u", uint32_t(a.semanticIndex));
  }
}

TextureType TextureTypeFor(SamplerTextureType type)
{
  switch(type)
  {
    case SamplerTextureType::Tex1D: return TextureType::Texture1D;
    case SamplerTextureType::Tex2D: return TextureType::Texture2D;
    case SamplerTextureType::Cube: return TextureType::TextureCube;
    case SamplerTextureType::Volume: return TextureType::Texture3D;
    default: break;
  }
  return TextureType::Unknown;
}

TextureType TextureTypeFor(ParamType type)
{
  switch(type)
  {
    case ParamType::Sampler1D:
    case ParamType::Texture1D: return TextureType::Texture1D;
    case ParamType::Sampler2D:
    case ParamType::Texture2D: return TextureType::Texture2D;
    case ParamType::Sampler3D:
    case ParamType::Texture3D: return TextureType::Texture3D;
    case ParamType::SamplerCube:
    case ParamType::TextureCube: return TextureType::TextureCube;
    default: break;
  }
  return TextureType::Unknown;
}

void AppendParameter(rdcstr &ret, const rdcstr &indent, const rdcstr &name, const CTABType &type,
                     int depth)
{
  rdcstr arraySuffix;
  if(type.elements > 1)
    arraySuffix = StringFormat::Fmt("[%u]", uint32_t(type.elements));

  if(type.cls == ParamClass::Struct && depth < MaxTypeDepth)
  {
    ret += "//   " + indent + "struct\n";
    ret += "//   " + indent + "{\n";
    for(const CTABMember &m : type.members)
      AppendParameter(ret, indent + "    ", m.name, m.type, depth + 1);
    ret += "//\n";
    ret += "//   " + indent + "} " + name + arraySuffix + ";\n";
    return;
  }

  rdcstr typeName = CTABTypeName(type);
  if(type.cls == ParamClass::MatrixRows)
    typeName = "row_major " + typeName;
  ret += "//   " + indent + typeName + " " + name + arraySuffix + ";\n";
}

};    // anonymous namespace

const char *ConstantBlockName(ConstantSet set)
{
  switch(set)
  {
    case ConstantSet::Float: return "Float constants (c#)";
    case ConstantSet::Int: return "Integer constants (i#)";
    case ConstantSet::Bool: return "Boolean constants (b#)";
    default: break;
  }
  return "Unknown constants";
}

void ParseConstantTable(const uint32_t *data, uint32_t numDWORDs, ConstantTable &table)
{
  table = ConstantTable();
  table.present = true;

  CTABReader reader(data, numDWORDs);

  // Size, Creator, Version, Constants, ConstantInfo, Flags, Target
  uint32_t header[7] = {};
  for(uint32_t i = 0; i < 7; i++)
  {
    if(!reader.Read32(i * 4, header[i]))
    {
      table.error = "The constant table is truncated";
      return;
    }
  }

  // later versions of the structure might be bigger, but not smaller
  if(header[0] < 28)
  {
    table.error = StringFormat::Fmt("The constant table's size is %u, expected 28", header[0]);
    return;
  }

  // the creator and target are informational, so don't fail if they're broken
  if(header[1] != 0)
    reader.ReadString(header[1], table.creator);
  table.version = header[2];
  table.flags = header[5];
  if(header[6] != 0)
    reader.ReadString(header[6], table.target);

  const uint32_t numConstants = header[3];
  const uint32_t infoOffset = header[4];

  // a constant info is 20 bytes. Check they fit before allocating anything.
  if(numConstants > 0 && (infoOffset > numDWORDs * sizeof(uint32_t) ||
                          (numDWORDs * sizeof(uint32_t) - infoOffset) / 20 < numConstants))
  {
    table.error =
        StringFormat::Fmt("The constant table's %u constants run past its end", numConstants);
    return;
  }

  for(uint32_t c = 0; c < numConstants; c++)
  {
    const uint32_t offs = infoOffset + c * 20;

    uint32_t nameOffset = 0, typeOffset = 0, defaultOffset = 0;
    uint16_t set = 0, regIndex = 0, regCount = 0;
    reader.Read32(offs, nameOffset);
    reader.Read16(offs + 4, set);
    reader.Read16(offs + 6, regIndex);
    reader.Read16(offs + 8, regCount);
    reader.Read32(offs + 12, typeOffset);
    reader.Read32(offs + 16, defaultOffset);

    CTABConstant constant;
    constant.set = RegisterSet(set);
    constant.registerIndex = regIndex;
    constant.registerCount = regCount;

    if(!reader.ReadString(nameOffset, constant.name))
    {
      table.error = StringFormat::Fmt("Constant %u's name is outside the constant table", c);
      continue;
    }

    if(!reader.ReadType(typeOffset, constant.type, 0))
    {
      table.error = StringFormat::Fmt("Constant %s's type is broken", constant.name.c_str());
      continue;
    }

    if(constant.set > RegisterSet::Sampler)
    {
      table.error = StringFormat::Fmt("Constant %s has an unknown register set %u",
                                      constant.name.c_str(), uint32_t(set));
      continue;
    }

    // samplers have no default value
    if(defaultOffset != 0 && constant.set != RegisterSet::Sampler)
    {
      // the registers the whole type covers, including any the compiler didn't allocate
      const ConstantSet cset = SetFor(constant.set);
      const uint32_t registers = uint32_t(RDCMIN(uint64_t(RegistersPerElement(constant.type, cset)) *
                                                     RDCMAX(1U, uint32_t(constant.type.elements)),
                                                 uint64_t(MaxRegisters)));
      const uint32_t count = registers * (cset == ConstantSet::Bool ? 1 : 4);
      // only take default values that are all inside the table
      if(defaultOffset <= numDWORDs * sizeof(uint32_t) &&
         (numDWORDs * sizeof(uint32_t) - defaultOffset) / 4 >= count)
      {
        constant.defaultValue.resize(count);
        for(uint32_t i = 0; i < count; i++)
          reader.Read32(defaultOffset + i * 4, constant.defaultValue[i]);
      }
    }

    table.constants.push_back(constant);
  }
}

rdcstr CTABTypeName(const CTABType &type)
{
  rdcstr base;
  switch(type.type)
  {
    case ParamType::Void: base = "void"; break;
    case ParamType::Bool: base = "bool"; break;
    case ParamType::Int: base = "int"; break;
    case ParamType::Float: base = "float"; break;
    case ParamType::String: base = "string"; break;
    case ParamType::Texture: base = "texture"; break;
    case ParamType::Texture1D: base = "texture1D"; break;
    case ParamType::Texture2D: base = "texture2D"; break;
    case ParamType::Texture3D: base = "texture3D"; break;
    case ParamType::TextureCube: base = "textureCUBE"; break;
    case ParamType::Sampler: base = "sampler"; break;
    case ParamType::Sampler1D: base = "sampler1D"; break;
    case ParamType::Sampler2D: base = "sampler2D"; break;
    case ParamType::Sampler3D: base = "sampler3D"; break;
    case ParamType::SamplerCube: base = "samplerCUBE"; break;
    case ParamType::PixelShader: base = "pixelshader"; break;
    case ParamType::VertexShader: base = "vertexshader"; break;
    case ParamType::PixelFragment: base = "pixelfragment"; break;
    case ParamType::VertexFragment: base = "vertexfragment"; break;
    default: base = StringFormat::Fmt("unknowntype%u", uint32_t(type.type)); break;
  }

  switch(type.cls)
  {
    case ParamClass::Scalar:
    case ParamClass::Object: return base;
    case ParamClass::Vector: return base + StringFormat::Fmt("%u", uint32_t(type.columns));
    case ParamClass::MatrixRows:
    case ParamClass::MatrixColumns:
      return base + StringFormat::Fmt("%ux%u", uint32_t(type.rows), uint32_t(type.columns));
    case ParamClass::Struct: return "struct";
    default: break;
  }

  return base;
}

rdcstr ConstantTableComment(const ConstantTable &table)
{
  if(!table.present)
    return rdcstr();

  rdcstr ret = "//\n";
  if(!table.creator.empty())
    ret += "// Generated by " + table.creator + "\n//\n";

  if(!table.error.empty())
    ret += "// Couldn't read all of the constant table: " + table.error + "\n//\n";

  if(table.constants.empty())
    return ret + "\n";

  ret += "// Parameters:\n//\n";
  for(const CTABConstant &c : table.constants)
    AppendParameter(ret, rdcstr(), c.name, c.type, 0);
  ret += "//\n//\n";

  // registers are listed by register set, then by register
  rdcarray<const CTABConstant *> sorted;
  for(const CTABConstant &c : table.constants)
    sorted.push_back(&c);
  std::stable_sort(sorted.begin(), sorted.end(), [](const CTABConstant *a, const CTABConstant *b) {
    if(a->set != b->set)
      return a->set < b->set;
    return a->registerIndex < b->registerIndex;
  });

  size_t nameWidth = 4;
  for(const CTABConstant *c : sorted)
    nameWidth = RDCMAX(nameWidth, c->name.size());

  rdcstr pad;
  pad.fill(nameWidth, ' ');
  rdcstr dashes;
  dashes.fill(nameWidth, '-');

  ret += "// Registers:\n//\n";
  ret += "//   " + rdcstr("Name") + pad.substr(4) + " Reg   Size\n";
  ret += "//   " + dashes + " ----- ----\n";
  for(const CTABConstant *c : sorted)
  {
    static const char prefixes[] = {'b', 'i', 'c', 's'};
    rdcstr reg =
        StringFormat::Fmt("%c%u", prefixes[uint32_t(c->set) & 3], uint32_t(c->registerIndex));
    rdcstr line = "//   " + c->name + pad.substr(c->name.size()) + " " + reg;
    rdcstr regPad;
    regPad.fill(reg.size() < 5 ? 5 - reg.size() : 0, ' ');
    line += regPad + StringFormat::Fmt(" %4u\n", uint32_t(c->registerCount));
    ret += line;
  }
  ret += "//\n\n";

  return ret;
}

void ShaderInfo::Init(ResourceId id, const uint32_t *tokens, size_t numTokens)
{
  ParseProgram(tokens, numTokens, m_Program);

  m_ConstantTable = ConstantTable();
  uint32_t ctabLength = 0;
  const uint32_t *ctab = m_Program.FindComment(MakeFourCC('C', 'T', 'A', 'B'), ctabLength);
  if(ctab)
    ParseConstantTable(ctab, ctabLength, m_ConstantTable);

  BuildReflection(id);
}

rdcstr ShaderInfo::GetDisassembly() const
{
  return ConstantTableComment(m_ConstantTable) + Disassemble(m_Program);
}

void ShaderInfo::BuildReflection(ResourceId id)
{
  m_Reflection = ShaderReflection();
  m_Literals.clear();

  const Version &version = m_Program.version;

  m_Reflection.resourceId = id;
  m_Reflection.entryPoint = "main";
  m_Reflection.stage = version.IsPS() ? ShaderStage::Pixel : ShaderStage::Vertex;
  m_Reflection.encoding = ShaderEncoding::D3D9Bytecode;
  m_Reflection.rawBytes.assign((const byte *)m_Program.tokens.data(), m_Program.tokens.byteSize());
  m_Reflection.debugInfo.debuggable = false;
  m_Reflection.debugInfo.debugStatus = "Debugging D3D9 shaders isn't supported";

  if(!m_Program.validVersion)
    return;

  //////////////////////////////////////////////////////////////////////////
  // what the code uses

  std::map<uint32_t, RegUsage> regs;
  // relatively addressed float constants, by their base register
  std::set<uint32_t> relativeBases;
  // ps_1_x's texture coordinates and samplers, by number
  std::map<uint32_t, uint8_t> sm1TexCoords;
  std::map<uint32_t, SamplerTextureType> sm1Samplers;

  const bool ps1 = version.IsPS() && version.IsSM1();
  const bool ps14 = ps1 && version.AtLeast(1, 4);

  for(const Instruction &inst : m_Program.instructions)
  {
    if(inst.op == Opcode::Def || inst.op == Opcode::DefI || inst.op == Opcode::DefB)
    {
      Literal lit;
      lit.set = inst.op == Opcode::Def    ? ConstantSet::Float
                : inst.op == Opcode::DefI ? ConstantSet::Int
                                          : ConstantSet::Bool;
      lit.reg = KeyIndex(RegKey(inst.dst.type, inst.dst.index));
      memcpy(lit.value, inst.literal, sizeof(lit.value));
      if(lit.set == ConstantSet::Bool)
        lit.value[0] = lit.value[0] ? 1 : 0;
      m_Literals.push_back(lit);
      continue;
    }

    if(inst.op == Opcode::Dcl)
    {
      RegUsage &u = regs[RegKey(inst.dst.type, inst.dst.index)];
      u.declared = true;
      u.declMask |= inst.dst.writeMask;
      RegDecl d;
      d.mask = inst.dst.writeMask;
      d.usage = inst.usage;
      d.usageIndex = inst.usageIndex;
      u.decls.push_back(d);
      u.textureType = inst.textureType;
      continue;
    }

    if(inst.hasDst)
    {
      regs[RegKey(inst.dst.type, inst.dst.index)].written |= inst.dst.writeMask;

      if(ps1 && !ps14 && inst.dst.type == RegType::AddrOrTexture && IsSM1TexAddressOp(inst.op))
      {
        sm1TexCoords[inst.dst.index] |= 0xF;
        if(IsSM1SamplingOp(inst.op))
        {
          const bool cube = inst.op == Opcode::TexM3x3Tex || inst.op == Opcode::TexM3x3Spec ||
                            inst.op == Opcode::TexM3x3VSpec;
          sm1Samplers[inst.dst.index] = cube ? SamplerTextureType::Cube : SamplerTextureType::Tex2D;
        }
      }

      // ps_1_4's texld samples the sampler of its destination's number
      if(ps14 && inst.op == Opcode::Tex)
        sm1Samplers[inst.dst.index] = SamplerTextureType::Tex2D;
    }

    if(inst.predicated)
      regs[RegKey(inst.predicate.type, inst.predicate.index)].read |= inst.predicate.ReadMask();

    for(size_t s = 0; s < inst.srcs.size(); s++)
    {
      const SrcParam &src = inst.srcs[s];
      const uint32_t key = RegKey(src.type, src.index);

      if(src.rel.present && KeyType(key) == RegType::Const)
      {
        relativeBases.insert(KeyIndex(key));
      }
      else
      {
        regs[key].read |= SourceReadMask(inst, s);
      }

      if(src.rel.present)
        regs[RegKey(src.rel.type, src.rel.index)].read |= uint8_t(1 << src.rel.component);

      // ps_1_4 reads texture coordinates as sources of texld and texcrd
      if(ps14 && src.type == RegType::AddrOrTexture)
        sm1TexCoords[src.index] |= SourceReadMask(inst, s);
    }
  }

  //////////////////////////////////////////////////////////////////////////
  // signatures

  rdcarray<SigParameter> &inputs = m_Reflection.inputSignature;
  rdcarray<SigParameter> &outputs = m_Reflection.outputSignature;

  for(auto it = regs.begin(); it != regs.end(); ++it)
  {
    const RegType type = KeyType(it->first);
    const uint32_t index = KeyIndex(it->first);
    const RegUsage &u = it->second;

    if(version.IsVS())
    {
      if(type == RegType::Input)
      {
        if(u.declared)
          AddDeclarations(inputs, u, index, u.read, false);
        else
          AddSignature(inputs, "UNKNOWN", index, index, 0xF, u.read, ShaderBuiltin::Undefined);
      }
      else if(type == RegType::TexCrdOutOrOutput && version.major >= 3)
      {
        if(u.declared)
          AddDeclarations(outputs, u, index, u.written, true);
      }
      else if(version.major < 3 && u.written)
      {
        // before vs_3_0 the output registers are fixed
        if(type == RegType::RastOut && index == 0)
          AddSignature(outputs, "POSITION", 0, index, 0xF, u.written, ShaderBuiltin::Position);
        else if(type == RegType::RastOut && index == 1)
          AddSignature(outputs, "FOG", 0, index, 0x1, u.written, ShaderBuiltin::Undefined);
        else if(type == RegType::RastOut && index == 2)
          AddSignature(outputs, "PSIZE", 0, index, 0x1, u.written, ShaderBuiltin::PointSize);
        else if(type == RegType::AttrOut)
          AddSignature(outputs, "COLOR", index, index, 0xF, u.written, ShaderBuiltin::Undefined);
        else if(type == RegType::TexCrdOutOrOutput)
          AddSignature(outputs, "TEXCOORD", index, index, 0xF, u.written, ShaderBuiltin::Undefined);
      }
    }
    else
    {
      if(version.major >= 3)
      {
        if(type == RegType::Input && u.declared)
          AddDeclarations(inputs, u, index, u.read, false);
        else if(type == RegType::MiscType && index == 0)
          AddSignature(inputs, "VPOS", 0, index, u.declared ? u.declMask : 0x3, u.read,
                       ShaderBuiltin::Position);
        else if(type == RegType::MiscType && index == 1)
          AddSignature(inputs, "VFACE", 0, index, 0x1, u.read, ShaderBuiltin::IsFrontFace);
      }
      else if(version.major == 2)
      {
        // ps_2_0 and ps_2_x declare v# (colours) and t# (texture coordinates) without a usage
        if(type == RegType::Input && (u.declared || u.read))
          AddSignature(inputs, "COLOR", index, index, u.declared ? u.declMask : 0xF, u.read,
                       ShaderBuiltin::Undefined);
        else if(type == RegType::AddrOrTexture && (u.declared || u.read))
          AddSignature(inputs, "TEXCOORD", index, index, u.declared ? u.declMask : 0xF, u.read,
                       ShaderBuiltin::Undefined);
      }
      else if(type == RegType::Input && u.read)
      {
        AddSignature(inputs, "COLOR", index, index, 0xF, u.read, ShaderBuiltin::Undefined);
      }

      if(version.major >= 2 && u.written)
      {
        if(type == RegType::ColorOut)
          AddSignature(outputs, "COLOR", index, index, 0xF, u.written, ShaderBuiltin::ColorOutput);
        else if(type == RegType::DepthOut)
          AddSignature(outputs, "DEPTH", 0, index, 0x1, u.written, ShaderBuiltin::DepthOutput);
      }
    }
  }

  if(ps1)
  {
    for(auto it = sm1TexCoords.begin(); it != sm1TexCoords.end(); ++it)
      AddSignature(inputs, "TEXCOORD", it->first, it->first, 0xF, it->second,
                   ShaderBuiltin::Undefined);

    // ps_1_x's result is whatever is in r0 at the end
    AddSignature(outputs, "COLOR", 0, 0, 0xF, 0xF, ShaderBuiltin::ColorOutput);
  }

  FinishSignature(inputs);
  FinishSignature(outputs);

  //////////////////////////////////////////////////////////////////////////
  // constant blocks

  uint32_t relativeLimit = 256;
  if(version.IsPS())
    relativeLimit = version.major >= 3 ? 224 : version.major == 2 ? 32 : 8;

  for(uint32_t s = 0; s < uint32_t(ConstantSet::Count); s++)
  {
    const ConstantSet set = ConstantSet(s);
    const RegType regType = set == ConstantSet::Float ? RegType::Const
                            : set == ConstantSet::Int ? RegType::ConstInt
                                                      : RegType::ConstBool;

    rdcarray<ShaderConstant> vars;
    std::set<uint32_t> covered;

    for(const CTABConstant &c : m_ConstantTable.constants)
    {
      if(c.set == RegisterSet::Sampler || SetFor(c.set) != set)
        continue;

      ShaderConstant var;
      var.name = c.name;
      var.byteOffset = uint32_t(c.registerIndex) * RegisterStride(set);
      var.type = MakeConstantType(c.type, set, RDCMIN(uint32_t(c.registerCount), MaxSetRegisters));

      // a default value is only kept for a scalar or a vector of 2, which fit in 64 bits. Their
      // components are the first DWORDs of the default either way: in the float and integer sets
      // they're in one register, and in the boolean set each is a register of one DWORD.
      const uint32_t components = uint32_t(c.type.rows) * uint32_t(c.type.columns);
      if((c.type.cls == ParamClass::Scalar || c.type.cls == ParamClass::Vector) &&
         c.type.elements <= 1 && components >= 1 && components <= 2 &&
         c.defaultValue.size() >= components)
      {
        uint64_t def = c.defaultValue[0];
        if(components > 1)
          def |= uint64_t(c.defaultValue[1]) << 32;
        var.defaultValue = def;
      }

      for(uint32_t r = 0; r < RDCMIN(uint32_t(c.registerCount), MaxSetRegisters); r++)
        covered.insert(uint32_t(c.registerIndex) + r);

      vars.push_back(var);
    }

    for(const Literal &lit : m_Literals)
    {
      if(lit.set != set || covered.count(lit.reg))
        continue;
      vars.push_back(MakeRegisterConstant(set, lit.reg, 1));
      covered.insert(lit.reg);
    }

    // registers the code reads directly that aren't described above
    std::set<uint32_t> direct;
    for(auto it = regs.begin(); it != regs.end(); ++it)
      if(KeyType(it->first) == regType && it->second.read)
        direct.insert(KeyIndex(it->first));

    for(uint32_t reg : direct)
    {
      if(covered.count(reg))
        continue;
      vars.push_back(MakeRegisterConstant(set, reg, 1));
    }

    // relatively addressed registers can read anything from their base up. Without a constant
    // table to say how big the array is, assume it goes up to the next register used for
    // something else, or the last register.
    if(set == ConstantSet::Float)
    {
      for(uint32_t base : relativeBases)
      {
        if(covered.count(base) || direct.count(base))
          continue;
        uint32_t end = RDCMAX(relativeLimit, base + 1);
        for(uint32_t r = base + 1; r < end; r++)
        {
          if(covered.count(r) || direct.count(r) || relativeBases.count(r))
          {
            end = r;
            break;
          }
        }
        vars.push_back(MakeRegisterConstant(set, base, end - base));
      }
    }

    if(vars.empty())
      continue;

    std::stable_sort(vars.begin(), vars.end(), [](const ShaderConstant &a, const ShaderConstant &b) {
      return a.byteOffset < b.byteOffset;
    });

    ConstantBlock block;
    block.name = ConstantBlockName(set);
    block.fixedBindNumber = s;
    block.bufferBacked = false;
    block.variables = vars;
    for(const ShaderConstant &v : vars)
      block.byteSize =
          RDCMAX(block.byteSize, v.byteOffset + VariableRegisters(v, set) * RegisterStride(set));
    m_Reflection.constantBlocks.push_back(block);
  }

  //////////////////////////////////////////////////////////////////////////
  // samplers

  struct SamplerBind
  {
    rdcstr name;
    uint32_t count = 1;
    TextureType type = TextureType::Unknown;
  };
  std::map<uint32_t, SamplerBind> samplers;
  std::set<uint32_t> samplersCovered;

  // the type the code gives each sampler register
  std::map<uint32_t, SamplerTextureType> codeTypes = sm1Samplers;
  for(auto it = regs.begin(); it != regs.end(); ++it)
  {
    if(KeyType(it->first) != RegType::Sampler)
      continue;
    if(it->second.declared || it->second.read)
      codeTypes[KeyIndex(it->first)] = it->second.textureType;
  }

  for(const CTABConstant &c : m_ConstantTable.constants)
  {
    if(c.set != RegisterSet::Sampler)
      continue;
    SamplerBind bind;
    bind.name = c.name;
    bind.count = RDCMAX(1U, uint32_t(c.registerCount));
    bind.type = TextureTypeFor(c.type.type);
    if(bind.type == TextureType::Unknown && codeTypes.count(c.registerIndex))
      bind.type = TextureTypeFor(codeTypes[c.registerIndex]);
    samplers[c.registerIndex] = bind;
    for(uint32_t r = 0; r < bind.count; r++)
      samplersCovered.insert(uint32_t(c.registerIndex) + r);
  }

  for(auto it = codeTypes.begin(); it != codeTypes.end(); ++it)
  {
    if(samplersCovered.count(it->first))
      continue;
    SamplerBind bind;
    bind.name = StringFormat::Fmt("s%u", it->first);
    bind.type = TextureTypeFor(it->second);
    // 1_x shaders don't say, 2D is the likeliest
    if(bind.type == TextureType::Unknown)
      bind.type = TextureType::Texture2D;
    samplers[it->first] = bind;
  }

  for(auto it = samplers.begin(); it != samplers.end(); ++it)
  {
    ShaderSampler samp;
    samp.name = it->second.name;
    samp.fixedBindNumber = it->first;
    samp.bindArraySize = it->second.count;
    m_Reflection.samplers.push_back(samp);

    ShaderResource res;
    res.name = it->second.name;
    res.textureType = it->second.type;
    res.descriptorType = DescriptorType::ImageSampler;
    res.isTexture = true;
    res.isReadOnly = true;
    res.hasSampler = true;
    res.fixedBindNumber = it->first;
    res.bindArraySize = it->second.count;
    res.variableType.name = "float4";
    res.variableType.baseType = VarType::Float;
    res.variableType.rows = 1;
    res.variableType.columns = 4;
    m_Reflection.readOnlyResources.push_back(res);
  }
}

bytebuf ShaderInfo::ApplyLiterals(ConstantSet set, const bytebuf &registerData) const
{
  bytebuf ret = registerData;
  const uint32_t stride = RegisterStride(set);

  for(const Literal &lit : m_Literals)
  {
    if(lit.set != set)
      continue;

    const size_t offset = size_t(lit.reg) * stride;
    if(ret.size() < offset + stride)
      ret.resize(offset + stride);
    memcpy(ret.data() + offset, lit.value, stride);
  }

  return ret;
}

void ShaderInfo::FillConstants(uint32_t cbufSlot, const bytebuf &registerData,
                               rdcarray<ShaderVariable> &outvars) const
{
  outvars.clear();

  if(cbufSlot >= m_Reflection.constantBlocks.size())
    return;

  const ConstantBlock &block = m_Reflection.constantBlocks[cbufSlot];
  if(block.fixedBindNumber >= uint32_t(ConstantSet::Count))
    return;

  bytebuf data = ApplyLiterals(ConstantSet(block.fixedBindNumber), registerData);
  StandardFillCBufferVariables(m_Reflection.resourceId, block.variables, outvars, data);
}

};    // namespace D3D9BC

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"
#include "d3d9_bytecode_test.h"
#include "d3d9_ctab_test_data.h"

using namespace D3D9BC;
using namespace D3D9BCTest;

namespace
{
// Builds a constant table the way D3DX lays it out: the header, the constant infos, then types,
// members, names and default values wherever they're put.
struct CTABBuilder
{
  struct Type
  {
    uint16_t cls, type, rows, columns, elements;
    rdcarray<rdcpair<rdcstr, uint32_t>> members;    // name and type index
  };
  struct Constant
  {
    rdcstr name;
    uint16_t set, reg, count;
    uint32_t type;
    rdcarray<float> def;
  };

  rdcarray<Type> types;
  rdcarray<Constant> constants;
  rdcstr creator = "Microsoft (R) HLSL Shader Compiler 9.29.952.3111";
  rdcstr target = "vs_3_0";

  uint32_t AddType(ParamClass cls, ParamType type, uint16_t rows, uint16_t columns,
                   uint16_t elements = 0, rdcarray<rdcpair<rdcstr, uint32_t>> members = {})
  {
    types.push_back({uint16_t(cls), uint16_t(type), rows, columns, elements, members});
    return uint32_t(types.size() - 1);
  }

  void AddConstant(const rdcstr &name, RegisterSet set, uint16_t reg, uint16_t count, uint32_t type,
                   rdcarray<float> def = {})
  {
    constants.push_back({name, uint16_t(set), reg, count, type, def});
  }

  static void Put32(bytebuf &b, uint32_t offs, uint32_t v) { memcpy(b.data() + offs, &v, 4); }
  static void Put16(bytebuf &b, uint32_t offs, uint16_t v) { memcpy(b.data() + offs, &v, 2); }
  static uint32_t AddString(bytebuf &b, const rdcstr &s)
  {
    uint32_t ret = (uint32_t)b.size();
    b.append((const byte *)s.c_str(), s.size() + 1);
    while(b.size() % 4)
      b.push_back(0);
    return ret;
  }

  // the comment's DWORDs, including the comment token and 'CTAB'
  rdcarray<uint32_t> Build()
  {
    bytebuf b;
    b.resize(28 + constants.size() * 20);

    rdcarray<uint32_t> typeOffsets;
    for(size_t t = 0; t < types.size(); t++)
    {
      typeOffsets.push_back((uint32_t)b.size());
      b.resize(b.size() + 16);
    }
    for(size_t t = 0; t < types.size(); t++)
    {
      const Type &ty = types[t];
      uint32_t o = typeOffsets[t];
      Put16(b, o, ty.cls);
      Put16(b, o + 2, ty.type);
      Put16(b, o + 4, ty.rows);
      Put16(b, o + 6, ty.columns);
      Put16(b, o + 8, ty.elements);
      Put16(b, o + 10, uint16_t(ty.members.size()));
      if(!ty.members.empty())
      {
        uint32_t memberOffset = (uint32_t)b.size();
        b.resize(b.size() + ty.members.size() * 8);
        for(size_t m = 0; m < ty.members.size(); m++)
        {
          uint32_t name = AddString(b, ty.members[m].first);
          Put32(b, memberOffset + uint32_t(m) * 8, name);
          Put32(b, memberOffset + uint32_t(m) * 8 + 4, typeOffsets[ty.members[m].second]);
        }
        Put32(b, o + 12, memberOffset);
      }
    }

    for(size_t c = 0; c < constants.size(); c++)
    {
      const Constant &con = constants[c];
      uint32_t o = 28 + uint32_t(c) * 20;
      Put32(b, o, AddString(b, con.name));
      Put16(b, o + 4, con.set);
      Put16(b, o + 6, con.reg);
      Put16(b, o + 8, con.count);
      Put32(b, o + 12, typeOffsets[con.type]);
      if(!con.def.empty())
      {
        uint32_t d = (uint32_t)b.size();
        b.append((const byte *)con.def.data(), con.def.byteSize());
        Put32(b, o + 16, d);
      }
    }

    Put32(b, 0, 28);
    Put32(b, 4, AddString(b, creator));
    Put32(b, 8, 0xFFFE0300);
    Put32(b, 12, (uint32_t)constants.size());
    Put32(b, 16, 28);
    Put32(b, 20, 0);
    Put32(b, 24, AddString(b, target));

    rdcarray<uint32_t> ret;
    ret.push_back(CommentTok(uint32_t(b.size() / 4) + 1));
    ret.push_back(MakeFourCC('C', 'T', 'A', 'B'));
    for(size_t i = 0; i < b.size(); i += 4)
    {
      uint32_t v;
      memcpy(&v, b.data() + i, 4);
      ret.push_back(v);
    }
    return ret;
  }
};

bytebuf Floats(const rdcarray<float> &f)
{
  bytebuf ret;
  ret.assign((const byte *)f.data(), f.byteSize());
  return ret;
}

const ConstantBlock *FindBlock(const ShaderReflection &refl, ConstantSet set)
{
  for(const ConstantBlock &b : refl.constantBlocks)
    if(b.fixedBindNumber == uint32_t(set))
      return &b;
  return NULL;
}

const ShaderConstant *FindVar(const ConstantBlock *block, const rdcstr &name)
{
  if(!block)
    return NULL;
  for(const ShaderConstant &v : block->variables)
    if(v.name == name)
      return &v;
  return NULL;
}

// Finds a constant, struct member or array element by D3DX's path for it ("s.b", "sarr[1].c"),
// and the register it starts at. An element is described by its array's ShaderConstant.
const ShaderConstant *FindPath(const ConstantBlock *block, ConstantSet set, const rdcstr &path,
                               uint32_t &reg)
{
  if(!block)
    return NULL;

  const uint32_t stride = set == ConstantSet::Bool ? 4 : 16;
  const ShaderConstant *cur = NULL;
  uint32_t offset = 0;
  int32_t i = 0;
  while(i < path.count())
  {
    if(path[i] == '[')
    {
      const int32_t end = path.indexOf(']', i);
      if(cur == NULL || end < 0)
        return NULL;
      const uint32_t idx = (uint32_t)atoi(path.substr(i + 1, end - i - 1).c_str());
      if(idx >= RDCMAX(1U, cur->type.elements))
        return NULL;
      offset += idx * cur->type.arrayByteStride;
      i = end + 1;
      continue;
    }

    if(path[i] == '.')
      i++;
    int32_t end = i;
    while(end < path.count() && path[end] != '.' && path[end] != '[')
      end++;
    const rdcstr name = path.substr(i, end - i);

    const rdcarray<ShaderConstant> &vars = cur ? cur->type.members : block->variables;
    const ShaderConstant *next = NULL;
    for(const ShaderConstant &v : vars)
      if(v.name == name)
        next = &v;
    if(next == NULL)
      return NULL;
    cur = next;
    offset += cur->byteOffset;
    i = end;
  }

  reg = offset / stride;
  return cur;
}
};

TEST_CASE("D3D9 shader reflection", "[d3d9][shader]")
{
  SECTION("Constant table")
  {
    CTABBuilder ctab;
    uint32_t f4x4 = ctab.AddType(ParamClass::MatrixColumns, ParamType::Float, 4, 4);
    uint32_t f4x3rm = ctab.AddType(ParamClass::MatrixRows, ParamType::Float, 4, 3);
    uint32_t f4arr = ctab.AddType(ParamClass::Vector, ParamType::Float, 1, 4, 8);
    uint32_t f1 = ctab.AddType(ParamClass::Scalar, ParamType::Float, 1, 1);
    uint32_t f3 = ctab.AddType(ParamClass::Vector, ParamType::Float, 1, 3);
    uint32_t light = ctab.AddType(ParamClass::Struct, ParamType::Void, 1, 7, 2,
                                  {{"dir", f3}, {"intensity", f1}, {"xform", f4x4}});
    uint32_t i4 = ctab.AddType(ParamClass::Vector, ParamType::Int, 1, 4);
    uint32_t b1 = ctab.AddType(ParamClass::Scalar, ParamType::Bool, 1, 1);
    uint32_t b3arr = ctab.AddType(ParamClass::Scalar, ParamType::Bool, 1, 1, 3);
    uint32_t samp2d = ctab.AddType(ParamClass::Object, ParamType::Sampler2D, 1, 1);
    uint32_t sampcube = ctab.AddType(ParamClass::Object, ParamType::SamplerCube, 1, 1, 2);
    uint32_t intInFloat = ctab.AddType(ParamClass::Scalar, ParamType::Int, 1, 1);

    ctab.AddConstant("WorldViewProj", RegisterSet::Float4, 0, 4, f4x4);
    ctab.AddConstant("World", RegisterSet::Float4, 4, 4, f4x3rm);
    // an array whose last 3 elements aren't used, so only 5 registers were allocated
    ctab.AddConstant("Bones", RegisterSet::Float4, 8, 5, f4arr);
    ctab.AddConstant("Lights", RegisterSet::Float4, 13, 10, light);
    // defaults are stored as whole registers
    ctab.AddConstant("Scale", RegisterSet::Float4, 23, 1, f1, {2.5f, 0.0f, 0.0f, 0.0f});
    ctab.AddConstant("Count", RegisterSet::Float4, 24, 1, intInFloat);
    ctab.AddConstant("Loops", RegisterSet::Int4, 1, 1, i4);
    ctab.AddConstant("Enable", RegisterSet::Bool, 0, 1, b1);
    ctab.AddConstant("Flags", RegisterSet::Bool, 2, 3, b3arr);
    ctab.AddConstant("Diffuse", RegisterSet::Sampler, 1, 1, samp2d);
    ctab.AddConstant("Env", RegisterSet::Sampler, 2, 2, sampcube);

    rdcarray<uint32_t> tokens = {VersionToken(false, 3, 0)};
    tokens.append(ctab.Build());
    rdcarray<uint32_t> code = {
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Position, 0),
        Dst(RegType::Input, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Position, 0),
        Dst(RegType::TexCrdOutOrOutput, 0),
        Inst(Opcode::Dcl, 0, 2),
        DclSampler(2),
        Dst(RegType::Sampler, 1),
        Inst(Opcode::Dcl, 0, 2),
        DclSampler(3),
        Dst(RegType::Sampler, 2),
        // a literal the compiler made, which isn't in the constant table
        Inst(Opcode::Def, 0, 5),
        Dst(RegType::Const, 30),
        FloatBits(1.0f),
        FloatBits(2.0f),
        FloatBits(3.0f),
        FloatBits(4.0f),
        Inst(Opcode::DefB, 0, 2),
        Dst(RegType::ConstBool, 7),
        5,
        Inst(Opcode::M4x4, 0, 3),
        Dst(RegType::TexCrdOutOrOutput, 0),
        Src(RegType::Input, 0),
        Src(RegType::Const, 0),
        EndTok(),
    };
    tokens.append(code);

    ShaderInfo info;
    info.Init(ResourceId(), tokens.data(), tokens.size());
    const ShaderReflection &refl = info.GetReflection();
    const ConstantTable &table = info.GetConstantTable();

    INFO(info.GetDisassembly().c_str());
    CHECK(info.GetProgram().error.empty());
    CHECK(table.present);
    CHECK(table.error.empty());
    CHECK(table.creator == "Microsoft (R) HLSL Shader Compiler 9.29.952.3111");
    CHECK(table.target == "vs_3_0");
    REQUIRE(table.constants.size() == 11);
    CHECK(table.constants[3].type.members.size() == 3);
    CHECK(table.constants[4].defaultValue.size() == 4);

    CHECK(refl.stage == ShaderStage::Vertex);
    CHECK(refl.entryPoint == "main");
    CHECK(refl.encoding == ShaderEncoding::D3D9Bytecode);
    CHECK(refl.rawBytes.size() == tokens.byteSize());

    REQUIRE(refl.constantBlocks.size() == 3);
    CHECK(refl.constantBlocks[0].name == "Float constants (c#)");
    CHECK(refl.constantBlocks[1].name == "Integer constants (i#)");
    CHECK(refl.constantBlocks[2].name == "Boolean constants (b#)");
    for(uint32_t i = 0; i < 3; i++)
    {
      CHECK(refl.constantBlocks[i].fixedBindNumber == i);
      CHECK(!refl.constantBlocks[i].bufferBacked);
    }

    const ConstantBlock *floats = FindBlock(refl, ConstantSet::Float);
    const ShaderConstant *wvp = FindVar(floats, "WorldViewProj");
    REQUIRE(wvp);
    CHECK(wvp->byteOffset == 0);
    CHECK(wvp->type.rows == 4);
    CHECK(wvp->type.columns == 4);
    CHECK(wvp->type.ColMajor());
    CHECK(wvp->type.matrixByteStride == 16);
    CHECK(wvp->type.name == "float4x4");

    const ShaderConstant *world = FindVar(floats, "World");
    REQUIRE(world);
    CHECK(world->byteOffset == 64);
    CHECK(world->type.RowMajor());
    CHECK(world->type.rows == 4);
    CHECK(world->type.columns == 3);

    const ShaderConstant *bones = FindVar(floats, "Bones");
    REQUIRE(bones);
    CHECK(bones->byteOffset == 8 * 16);
    CHECK(bones->type.elements == 5);
    CHECK(bones->type.arrayByteStride == 16);

    const ShaderConstant *lights = FindVar(floats, "Lights");
    REQUIRE(lights);
    CHECK(lights->byteOffset == 13 * 16);
    CHECK(lights->type.baseType == VarType::Struct);
    CHECK(lights->type.elements == 2);
    CHECK(lights->type.arrayByteStride == 6 * 16);
    REQUIRE(lights->type.members.size() == 3);
    CHECK(lights->type.members[0].byteOffset == 0);
    CHECK(lights->type.members[1].byteOffset == 16);
    CHECK(lights->type.members[2].byteOffset == 32);

    const ShaderConstant *scale = FindVar(floats, "Scale");
    REQUIRE(scale);
    float defScale = 0.0f;
    uint32_t defBits = uint32_t(scale->defaultValue);
    memcpy(&defScale, &defBits, 4);
    CHECK(defScale == 2.5f);

    // ints the compiler put in float registers are floats there
    const ShaderConstant *count = FindVar(floats, "Count");
    REQUIRE(count);
    CHECK(count->type.baseType == VarType::Float);
    CHECK(count->type.name == "int");

    const ShaderConstant *lit = FindVar(floats, "c30");
    REQUIRE(lit);
    CHECK(lit->byteOffset == 30 * 16);
    CHECK(floats->byteSize == 31 * 16);

    const ShaderConstant *loops = FindVar(FindBlock(refl, ConstantSet::Int), "Loops");
    REQUIRE(loops);
    CHECK(loops->byteOffset == 16);
    CHECK(loops->type.baseType == VarType::SInt);
    CHECK(loops->type.columns == 4);

    const ConstantBlock *bools = FindBlock(refl, ConstantSet::Bool);
    const ShaderConstant *flags = FindVar(bools, "Flags");
    REQUIRE(flags);
    CHECK(flags->byteOffset == 8);
    CHECK(flags->type.elements == 3);
    CHECK(flags->type.arrayByteStride == 4);
    CHECK(flags->type.baseType == VarType::Bool);
    CHECK(FindVar(bools, "b7"));

    REQUIRE(refl.samplers.size() == 2);
    CHECK(refl.samplers[0].name == "Diffuse");
    CHECK(refl.samplers[0].fixedBindNumber == 1);
    CHECK(refl.samplers[1].name == "Env");
    CHECK(refl.samplers[1].fixedBindNumber == 2);
    CHECK(refl.samplers[1].bindArraySize == 2);
    REQUIRE(refl.readOnlyResources.size() == 2);
    CHECK(refl.readOnlyResources[0].textureType == TextureType::Texture2D);
    CHECK(refl.readOnlyResources[1].textureType == TextureType::TextureCube);
    CHECK(refl.readOnlyResources[1].fixedBindNumber == 2);
    CHECK(refl.readOnlyResources[1].descriptorType == DescriptorType::ImageSampler);

    rdcstr disasm = info.GetDisassembly();
    CHECK(disasm.contains("// Generated by Microsoft (R) HLSL Shader Compiler 9.29.952.3111\n"));
    CHECK(disasm.contains("//   float4x4 WorldViewProj;\n"));
    CHECK(disasm.contains("//   row_major float4x3 World;\n"));
    CHECK(disasm.contains("//   float4 Bones[8];\n"));
    CHECK(disasm.contains("//   struct\n//   {\n//       float3 dir;\n"));
    CHECK(disasm.contains("//   } Lights[2];\n"));
    CHECK(disasm.contains("//   samplerCUBE Env[2];\n"));
    CHECK(disasm.contains("//   Name          Reg   Size\n"));
    CHECK(disasm.contains("//   ------------- ----- ----\n"));
    CHECK(disasm.contains("//   WorldViewProj c0       4\n"));
    CHECK(disasm.contains("//   Enable        b0       1\n"));
    CHECK(disasm.contains("//   Lights        c13     10\n"));
    // registers are in set order: bool, int, float, sampler
    CHECK(disasm.find("Enable        b0") < disasm.find("Loops         i1"));
    CHECK(disasm.find("Loops         i1") < disasm.find("WorldViewProj c0"));
    CHECK(disasm.find("WorldViewProj c0") < disasm.find("Diffuse       s1"));
    CHECK(disasm.contains("    vs_3_0\n"));

    // filling: the matrix is column major, so each register is a column
    rdcarray<float> regs;
    regs.resize(32 * 4);
    for(size_t i = 0; i < regs.size(); i++)
      regs[i] = float(i);

    rdcarray<ShaderVariable> vars;
    info.FillConstants(0, Floats(regs), vars);
    REQUIRE(vars.size() == floats->variables.size());
    CHECK(vars[0].name == "WorldViewProj");
    CHECK(vars[0].rows == 4);
    CHECK(vars[0].columns == 4);
    // row 0 is the x of each column register
    CHECK(vars[0].value.f32v[0] == 0.0f);
    CHECK(vars[0].value.f32v[1] == 4.0f);
    CHECK(vars[0].value.f32v[2] == 8.0f);
    CHECK(vars[0].value.f32v[3] == 12.0f);
    CHECK(vars[0].value.f32v[4] == 1.0f);
    // the row major one's rows are registers c4-c7
    CHECK(vars[1].name == "World");
    CHECK(vars[1].value.f32v[0] == 16.0f);
    CHECK(vars[1].value.f32v[1] == 17.0f);
    CHECK(vars[1].value.f32v[2] == 18.0f);
    CHECK(vars[1].value.f32v[3] == 20.0f);

    // the literal replaces the device's value
    const ShaderVariable *litVar = NULL;
    for(const ShaderVariable &v : vars)
      if(v.name == "c30")
        litVar = &v;
    REQUIRE(litVar);
    CHECK(litVar->value.f32v[0] == 1.0f);
    CHECK(litVar->value.f32v[3] == 4.0f);

    // bools: one uint32 per register, and the defb literal applies (as 1)
    rdcarray<uint32_t> boolRegs = {1, 0, 0, 1, 1, 0, 0, 0};
    bytebuf boolData;
    boolData.assign((const byte *)boolRegs.data(), boolRegs.byteSize());
    info.FillConstants(2, boolData, vars);
    REQUIRE(vars.size() == 3);
    CHECK(vars[0].name == "Enable");
    CHECK(vars[0].value.u32v[0] == 1);
    CHECK(vars[1].name == "Flags");
    REQUIRE(vars[1].members.size() == 3);
    CHECK(vars[1].members[0].value.u32v[0] == 0);
    CHECK(vars[1].members[1].value.u32v[0] == 1);
    CHECK(vars[1].members[2].value.u32v[0] == 1);
    CHECK(vars[2].name == "b7");
    CHECK(vars[2].value.u32v[0] == 1);

    // a slot that doesn't exist
    info.FillConstants(5, boolData, vars);
    CHECK(vars.empty());
  };

  SECTION("No constant table")
  {
    rdcarray<uint32_t> tokens = {
        VersionToken(false, 2, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Position, 0),
        Dst(RegType::Input, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::BlendIndices, 0),
        Dst(RegType::Input, 1, 0x1),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 3),
        Dst(RegType::Input, 2, 0x3),
        Inst(Opcode::Def, 0, 5),
        Dst(RegType::Const, 3),
        FloatBits(5.0f),
        FloatBits(6.0f),
        FloatBits(7.0f),
        FloatBits(8.0f),
        Inst(Opcode::DefI, 0, 5),
        Dst(RegType::ConstInt, 2),
        10,
        0,
        1,
        0,
        Inst(Opcode::MovA, 0, 2),
        Dst(RegType::AddrOrTexture, 0, 0x1),
        Src(RegType::Input, 1, Swz(0, 0, 0, 0)),
        // m4x3 r0.xyz, v0, c10[a0.x]: an array from c10, up to c40 which is used directly
        Inst(Opcode::M4x3, 0, 4),
        Dst(RegType::Temp, 0, 0x7),
        Src(RegType::Input, 0),
        Src(RegType::Const, 10) | RelBit,
        Src(RegType::AddrOrTexture, 0),
        Inst(Opcode::Mad, 0, 4),
        Dst(RegType::RastOut, 0),
        Src(RegType::Temp, 0),
        Src(RegType::Const, 40),
        Src(RegType::Const, 3),
        Inst(Opcode::Rep, 0, 1),
        Src(RegType::ConstInt, 2),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::AttrOut, 1, 0x7),
        Src(RegType::Const, 5),
        Inst(Opcode::EndRep, 0, 0),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::TexCrdOutOrOutput, 2, 0x3),
        Src(RegType::Input, 2),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::RastOut, 1, 0x1),
        Src(RegType::Const, 3, Swz(0, 0, 0, 0)),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::RastOut, 2, 0x1),
        Src(RegType::Const, 3, Swz(1, 1, 1, 1)),
        EndTok(),
    };

    ShaderInfo info;
    info.Init(ResourceId(), tokens.data(), tokens.size());
    const ShaderReflection &refl = info.GetReflection();
    INFO(info.GetDisassembly().c_str());
    CHECK(info.GetProgram().error.empty());
    CHECK(!info.GetConstantTable().present);

    REQUIRE(refl.inputSignature.size() == 3);
    CHECK(refl.inputSignature[0].semanticName == "POSITION");
    CHECK(refl.inputSignature[0].regIndex == 0);
    CHECK(refl.inputSignature[0].semanticIdxName == "POSITION");
    CHECK(refl.inputSignature[1].semanticName == "BLENDINDICES");
    CHECK(refl.inputSignature[1].compCount == 1);
    CHECK(refl.inputSignature[2].semanticName == "TEXCOORD");
    CHECK(refl.inputSignature[2].semanticIndex == 3);
    CHECK(refl.inputSignature[2].semanticIdxName == "TEXCOORD3");
    CHECK(refl.inputSignature[2].regChannelMask == 0x3);
    CHECK(refl.inputSignature[2].compCount == 2);

    REQUIRE(refl.outputSignature.size() == 5);
    CHECK(refl.outputSignature[0].semanticName == "POSITION");
    CHECK(refl.outputSignature[0].systemValue == ShaderBuiltin::Position);
    CHECK(refl.outputSignature[1].semanticName == "FOG");
    CHECK(refl.outputSignature[2].semanticName == "PSIZE");
    CHECK(refl.outputSignature[2].systemValue == ShaderBuiltin::PointSize);
    CHECK(refl.outputSignature[3].semanticIdxName == "COLOR1");
    CHECK(refl.outputSignature[3].channelUsedMask == 0x7);
    CHECK(refl.outputSignature[4].semanticIdxName == "TEXCOORD2");

    // float: c3 (literal), c5, c10-c39 (relative), c40. int: i2. No bools.
    REQUIRE(refl.constantBlocks.size() == 2);
    const ConstantBlock *floats = FindBlock(refl, ConstantSet::Float);
    REQUIRE(floats);
    REQUIRE(floats->variables.size() == 4);
    CHECK(floats->variables[0].name == "c3");
    CHECK(floats->variables[1].name == "c5");
    CHECK(floats->variables[2].name == "c10");
    CHECK(floats->variables[2].type.elements == 30);
    CHECK(floats->variables[3].name == "c40");
    CHECK(floats->byteSize == 41 * 16);
    const ConstantBlock *ints = FindBlock(refl, ConstantSet::Int);
    REQUIRE(ints);
    REQUIRE(ints->variables.size() == 1);
    CHECK(ints->variables[0].name == "i2");
    CHECK(ints->variables[0].byteOffset == 32);
    CHECK(ints->variables[0].type.baseType == VarType::SInt);
    CHECK(!FindBlock(refl, ConstantSet::Bool));

    // the literals apply on top of the given data, even when it's too short
    rdcarray<ShaderVariable> vars;
    info.FillConstants(1, bytebuf(), vars);
    REQUIRE(vars.size() == 1);
    CHECK(vars[0].value.s32v[0] == 10);
    CHECK(vars[0].value.s32v[2] == 1);

    rdcarray<float> regs;
    regs.resize(41 * 4);
    for(float &f : regs)
      f = 9.0f;
    info.FillConstants(0, Floats(regs), vars);
    REQUIRE(vars.size() == 4);
    CHECK(vars[0].value.f32v[0] == 5.0f);
    CHECK(vars[0].value.f32v[3] == 8.0f);
    CHECK(vars[1].value.f32v[0] == 9.0f);
    REQUIRE(vars[2].members.size() == 30);
    CHECK(vars[2].members[29].value.f32v[0] == 9.0f);

    bytebuf patched = info.ApplyLiterals(ConstantSet::Float, Floats(regs));
    float c3x = 0.0f;
    memcpy(&c3x, patched.data() + 3 * 16, 4);
    CHECK(c3x == 5.0f);
  };

  SECTION("Pixel shader signatures and samplers")
  {
    // ps_3_0
    rdcarray<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Color, 1),
        Dst(RegType::Input, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 0),
        Dst(RegType::Input, 1, 0x3),
        Inst(Opcode::Dcl, 0, 2),
        DclNone(),
        Dst(RegType::MiscType, 1),
        Inst(Opcode::Dcl, 0, 2),
        DclSampler(4),
        Dst(RegType::Sampler, 5),
        Inst(Opcode::Tex, 0, 3),
        Dst(RegType::Temp, 0),
        Src(RegType::Input, 1),
        Src(RegType::Sampler, 5),
        Inst(Opcode::If, 0, 1),
        Src(RegType::ConstBool, 3),
        Inst(Opcode::Mul, 0, 3),
        Dst(RegType::Temp, 0),
        Src(RegType::Temp, 0),
        Src(RegType::MiscType, 1, Swz(0, 0, 0, 0)),
        Inst(Opcode::EndIf, 0, 0),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::ColorOut, 0),
        Src(RegType::Temp, 0),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::DepthOut, 0, 0x1),
        Src(RegType::Input, 0, Swz(3, 3, 3, 3)),
        EndTok(),
    };

    ShaderInfo info;
    info.Init(ResourceId(), tokens.data(), tokens.size());
    const ShaderReflection &refl = info.GetReflection();
    INFO(info.GetDisassembly().c_str());
    CHECK(refl.stage == ShaderStage::Pixel);
    REQUIRE(refl.inputSignature.size() == 3);
    CHECK(refl.inputSignature[0].semanticIdxName == "COLOR1");
    CHECK(refl.inputSignature[0].channelUsedMask == 0x8);
    CHECK(refl.inputSignature[1].semanticIdxName == "TEXCOORD");
    CHECK(refl.inputSignature[2].semanticName == "VFACE");
    CHECK(refl.inputSignature[2].systemValue == ShaderBuiltin::IsFrontFace);
    REQUIRE(refl.outputSignature.size() == 2);
    CHECK(refl.outputSignature[0].systemValue == ShaderBuiltin::ColorOutput);
    CHECK(refl.outputSignature[1].systemValue == ShaderBuiltin::DepthOutput);
    REQUIRE(refl.samplers.size() == 1);
    CHECK(refl.samplers[0].name == "s5");
    CHECK(refl.samplers[0].fixedBindNumber == 5);
    CHECK(refl.readOnlyResources[0].textureType == TextureType::Texture3D);
    REQUIRE(refl.constantBlocks.size() == 1);
    CHECK(refl.constantBlocks[0].fixedBindNumber == uint32_t(ConstantSet::Bool));
    CHECK(refl.constantBlocks[0].variables[0].name == "b3");
    CHECK(refl.constantBlocks[0].variables[0].byteOffset == 12);

    // ps_3_0 with two semantics in v0, declared in reverse order, and three in v1:
    // dcl_texcoord1 v0.zw, dcl_texcoord0 v0.xy, dcl_texcoord2_centroid v1.x,
    // dcl_texcoord3_centroid v1.y, dcl_color1_centroid v1.zw. Reads v0.xyz and v1.
    tokens = {
        VersionToken(true, 3, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 1),
        Dst(RegType::Input, 0, 0xC),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 0),
        Dst(RegType::Input, 0, 0x3),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 2),
        Dst(RegType::Input, 1, 0x1, 0x4),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 3),
        Dst(RegType::Input, 1, 0x2, 0x4),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Color, 1),
        Dst(RegType::Input, 1, 0xC, 0x4),
        Inst(Opcode::Add, 0, 3),
        Dst(RegType::Temp, 0, 0x7),
        Src(RegType::Input, 0),
        Src(RegType::Input, 0),
        Inst(Opcode::Add, 0, 3),
        Dst(RegType::Temp, 1),
        Src(RegType::Temp, 0, Swz(0, 1, 2, 2)),
        Src(RegType::Input, 1),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::ColorOut, 0),
        Src(RegType::Temp, 1),
        EndTok(),
    };
    {
      ShaderInfo packed;
      packed.Init(ResourceId(), tokens.data(), tokens.size());
      const ShaderReflection &p = packed.GetReflection();
      INFO(packed.GetDisassembly().c_str());
      CHECK(packed.GetProgram().error.empty());
      REQUIRE(p.inputSignature.size() == 5);
      CHECK(p.inputSignature[0].semanticIdxName == "TEXCOORD0");
      CHECK(p.inputSignature[0].regIndex == 0);
      CHECK(p.inputSignature[0].regChannelMask == 0x3);
      CHECK(p.inputSignature[0].channelUsedMask == 0x3);
      CHECK(p.inputSignature[0].compCount == 2);
      CHECK(p.inputSignature[1].semanticIdxName == "TEXCOORD1");
      CHECK(p.inputSignature[1].regIndex == 0);
      CHECK(p.inputSignature[1].regChannelMask == 0xC);
      // only .z is read
      CHECK(p.inputSignature[1].channelUsedMask == 0x4);
      CHECK(p.inputSignature[1].compCount == 2);
      CHECK(p.inputSignature[2].semanticIdxName == "TEXCOORD2");
      CHECK(p.inputSignature[2].regIndex == 1);
      CHECK(p.inputSignature[2].compCount == 1);
      CHECK(p.inputSignature[3].semanticIdxName == "TEXCOORD3");
      CHECK(p.inputSignature[3].regChannelMask == 0x2);
      CHECK(p.inputSignature[4].semanticIdxName == "COLOR1");
      CHECK(p.inputSignature[4].regChannelMask == 0xC);
      CHECK(p.inputSignature[4].compCount == 2);
      // the disassembly keeps every declaration
      CHECK(packed.GetDisassembly().find("dcl_texcoord1 v0.zw") >= 0);
      CHECK(packed.GetDisassembly().find("dcl_texcoord v0.xy") >= 0);
      CHECK(packed.GetDisassembly().find("dcl_color1_centroid v1.zw") >= 0);
    }

    // ps_1_1: texture coordinates from the texture instructions, r0 is the output
    tokens = {
        VersionToken(true, 1, 1),
        Inst(Opcode::Tex, 0, 0),
        Dst(RegType::AddrOrTexture, 0),
        Inst(Opcode::TexCoord, 0, 0),
        Dst(RegType::AddrOrTexture, 1),
        Inst(Opcode::TexM3x3Pad, 0, 0),
        Dst(RegType::AddrOrTexture, 2),
        Src(RegType::AddrOrTexture, 0),
        Inst(Opcode::TexM3x3Pad, 0, 0),
        Dst(RegType::AddrOrTexture, 3),
        Src(RegType::AddrOrTexture, 0),
        Inst(Opcode::TexM3x3Tex, 0, 0),
        Dst(RegType::AddrOrTexture, 4),
        Src(RegType::AddrOrTexture, 0),
        Inst(Opcode::Mul, 0, 0),
        Dst(RegType::Temp, 0),
        Src(RegType::AddrOrTexture, 4),
        Src(RegType::Input, 1),
        Inst(Opcode::Add, 0, 0),
        Dst(RegType::Temp, 0),
        Src(RegType::Temp, 0),
        Src(RegType::Const, 2),
        EndTok(),
    };
    info.Init(ResourceId(), tokens.data(), tokens.size());
    CHECK(info.GetProgram().error.empty());
    {
      const ShaderReflection &r = info.GetReflection();
      // v1, then t0-t4
      REQUIRE(r.inputSignature.size() == 6);
      CHECK(r.inputSignature[0].semanticIdxName == "COLOR1");
      CHECK(r.inputSignature[1].semanticIdxName == "TEXCOORD0");
      CHECK(r.inputSignature[5].semanticIdxName == "TEXCOORD4");
      REQUIRE(r.outputSignature.size() == 1);
      CHECK(r.outputSignature[0].semanticName == "COLOR");
      CHECK(r.outputSignature[0].systemValue == ShaderBuiltin::ColorOutput);
      // tex t0 samples s0, texm3x3tex t4 samples a cube from s4
      REQUIRE(r.samplers.size() == 2);
      CHECK(r.samplers[0].fixedBindNumber == 0);
      CHECK(r.samplers[1].fixedBindNumber == 4);
      CHECK(r.readOnlyResources[0].textureType == TextureType::Texture2D);
      CHECK(r.readOnlyResources[1].textureType == TextureType::TextureCube);
      REQUIRE(r.constantBlocks.size() == 1);
      CHECK(r.constantBlocks[0].variables[0].name == "c2");
    }

    // ps_1_4: texture coordinates are read, samplers are the destination's number
    tokens = {
        VersionToken(true, 1, 4),       Inst(Opcode::Tex, 0, 0),      Dst(RegType::Temp, 1),
        Src(RegType::AddrOrTexture, 2), Inst(Opcode::TexCoord, 0, 0), Dst(RegType::Temp, 0, 0x7),
        Src(RegType::AddrOrTexture, 0), Inst(Opcode::Mul, 0, 0),      Dst(RegType::Temp, 0),
        Src(RegType::Temp, 0),          Src(RegType::Temp, 1),        EndTok(),
    };
    info.Init(ResourceId(), tokens.data(), tokens.size());
    CHECK(info.GetProgram().error.empty());
    {
      const ShaderReflection &r = info.GetReflection();
      REQUIRE(r.inputSignature.size() == 2);
      CHECK(r.inputSignature[0].semanticIdxName == "TEXCOORD0");
      CHECK(r.inputSignature[0].channelUsedMask == 0x7);
      CHECK(r.inputSignature[1].semanticIdxName == "TEXCOORD2");
      REQUIRE(r.samplers.size() == 1);
      CHECK(r.samplers[0].fixedBindNumber == 1);
    }

    // ps_2_0: dcl without usages
    tokens = {
        VersionToken(true, 2, 0),
        Inst(Opcode::Dcl, 0, 2),
        DclNone(),
        Dst(RegType::Input, 0),
        Inst(Opcode::Dcl, 0, 2),
        DclNone(),
        Dst(RegType::AddrOrTexture, 3, 0x3),
        Inst(Opcode::Dcl, 0, 2),
        DclSampler(2),
        Dst(RegType::Sampler, 0),
        Inst(Opcode::Tex, 0, 3),
        Dst(RegType::Temp, 0),
        Src(RegType::AddrOrTexture, 3),
        Src(RegType::Sampler, 0),
        Inst(Opcode::Mul, 0, 3),
        Dst(RegType::ColorOut, 0),
        Src(RegType::Temp, 0),
        Src(RegType::Input, 0),
        EndTok(),
    };
    info.Init(ResourceId(), tokens.data(), tokens.size());
    {
      const ShaderReflection &r = info.GetReflection();
      REQUIRE(r.inputSignature.size() == 2);
      CHECK(r.inputSignature[0].semanticIdxName == "COLOR");
      CHECK(r.inputSignature[1].semanticIdxName == "TEXCOORD3");
      CHECK(r.inputSignature[1].compCount == 2);
      REQUIRE(r.outputSignature.size() == 1);
      REQUIRE(r.samplers.size() == 1);
      CHECK(r.readOnlyResources[0].textureType == TextureType::Texture2D);
      CHECK(r.constantBlocks.empty());
    }
  };

  SECTION("vs_3_0 outputs")
  {
    rdcarray<uint32_t> tokens = {
        VersionToken(false, 3, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Position, 0),
        Dst(RegType::Input, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::Position, 0),
        Dst(RegType::TexCrdOutOrOutput, 0),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 0),
        Dst(RegType::TexCrdOutOrOutput, 1, 0x3),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::TexCoord, 1),
        Dst(RegType::TexCrdOutOrOutput, 1, 0xC),
        Inst(Opcode::Dcl, 0, 2),
        Dcl(DeclUsage::PSize, 0),
        Dst(RegType::TexCrdOutOrOutput, 2, 0x1),
        Inst(Opcode::Mov, 0, 2),
        Dst(RegType::TexCrdOutOrOutput, 0),
        Src(RegType::Input, 0),
        EndTok(),
    };

    ShaderInfo info;
    info.Init(ResourceId(), tokens.data(), tokens.size());
    const ShaderReflection &refl = info.GetReflection();
    REQUIRE(refl.inputSignature.size() == 1);
    REQUIRE(refl.outputSignature.size() >= 2);
    CHECK(refl.outputSignature[0].systemValue == ShaderBuiltin::Position);
    CHECK(refl.outputSignature.back().systemValue == ShaderBuiltin::PointSize);
  };

  SECTION("Compiled constant tables")
  {
    // Tables from D3DCompile and the legacy D3DX9 compiler, with what D3DX9_43 reads from them:
    // see d3d9_ctab_test_data.h. Each table is put in a shader with no instructions.
    for(const CTABTestData::Shader &sh : CTABTestData::shaders)
    {
      INFO(sh.name);

      rdcarray<uint32_t> tokens = {sh.version, CommentTok(sh.ctabDWORDs + 1),
                                   MakeFourCC('C', 'T', 'A', 'B')};
      tokens.append(sh.ctab, sh.ctabDWORDs);
      tokens.push_back(EndTok());

      ShaderInfo info;
      info.Init(ResourceId(), tokens.data(), tokens.size());
      const ShaderReflection &refl = info.GetReflection();
      const ConstantTable &table = info.GetConstantTable();

      CHECK(info.GetProgram().error.empty());
      CHECK(table.present);
      CHECK(table.error.empty());

      // the registers our default values fill, as SetDefaults does: only the allocated ones
      std::map<rdcpair<uint16_t, uint16_t>, rdcarray<uint32_t>> image;
      for(const CTABConstant &c : table.constants)
      {
        if(c.defaultValue.empty())
          continue;
        const uint32_t per = c.set == RegisterSet::Bool ? 1 : 4;
        INFO(c.name.c_str());
        REQUIRE(c.defaultValue.size() >= c.registerCount * per);
        CHECK(c.defaultValue.size() % per == 0);
        for(uint32_t r = 0; r < c.registerCount; r++)
          image[{uint16_t(c.set), uint16_t(c.registerIndex + r)}] =
              rdcarray<uint32_t>(c.defaultValue.data() + r * per, per);
      }

      CHECK(image.size() == sh.numRegs);
      for(size_t r = 0; r < sh.numRegs; r++)
      {
        const CTABTestData::Register &e = sh.regs[r];
        INFO("register set " << e.set << " register " << e.reg);
        auto it = image.find({e.set, e.reg});
        const bool filled = it != image.end();
        REQUIRE(filled);
        for(size_t k = 0; k < it->second.size(); k++)
          CHECK(it->second[k] == e.value[k]);
      }

      for(size_t n = 0; n < sh.numNodes; n++)
      {
        const CTABTestData::Node &node = sh.nodes[n];
        const rdcstr path = node.path;
        INFO(node.path);

        const bool top = !path.contains('.') && !path.contains('[');
        if(top)
        {
          const CTABConstant *c = NULL;
          for(const CTABConstant &tc : table.constants)
            if(tc.name == path)
              c = &tc;
          REQUIRE(c);
          CHECK(uint16_t(c->set) == node.set);
          CHECK(c->registerIndex == node.reg);
          CHECK(c->registerCount == node.count);
          CHECK(uint16_t(c->type.cls) == node.cls);
          CHECK(uint16_t(c->type.type) == node.type);
          CHECK(c->type.rows == node.rows);
          CHECK(c->type.columns == node.columns);
          CHECK(RDCMAX(uint16_t(1), c->type.elements) == node.elements);
          CHECK(c->type.members.size() == node.members);
        }

        if(node.set == uint16_t(RegisterSet::Sampler))
        {
          if(!top)
            continue;
          const ShaderSampler *samp = NULL;
          for(const ShaderSampler &ss : refl.samplers)
            if(ss.name == path)
              samp = &ss;
          REQUIRE(samp);
          CHECK(samp->fixedBindNumber == node.reg);
          CHECK(samp->bindArraySize == node.count);
          continue;
        }

        // registers the compiler didn't allocate aren't part of the variable
        if(node.count == 0)
          continue;

        const ConstantSet set = node.set == uint16_t(RegisterSet::Bool)   ? ConstantSet::Bool
                                : node.set == uint16_t(RegisterSet::Int4) ? ConstantSet::Int
                                                                          : ConstantSet::Float;
        uint32_t reg = ~0U;
        const ShaderConstant *var = FindPath(FindBlock(refl, set), set, path, reg);
        REQUIRE(var);
        CHECK(reg == node.reg);

        if(node.cls <= uint16_t(ParamClass::MatrixColumns))
        {
          CHECK(var->type.rows == node.rows);
          CHECK(var->type.columns == node.columns);
          CHECK(bool(var->type.flags & ShaderVariableFlags::RowMajorMatrix) ==
                (node.cls == uint16_t(ParamClass::MatrixRows)));
        }

        // a scalar's or a 2 component vector's default value, from SetDefaults' registers
        if(top && node.cls <= uint16_t(ParamClass::Vector) && node.elements == 1 &&
           node.rows * node.columns <= 2)
        {
          const uint32_t components = uint32_t(node.rows) * uint32_t(node.columns);
          uint32_t expected[2] = {};
          bool found = false;
          for(size_t r = 0; r < sh.numRegs; r++)
          {
            const CTABTestData::Register &e = sh.regs[r];
            if(e.set != node.set)
              continue;
            if(set == ConstantSet::Bool)
            {
              if(e.reg >= node.reg && e.reg < node.reg + components)
              {
                expected[e.reg - node.reg] = e.value[0];
                found = true;
              }
            }
            else if(e.reg == node.reg)
            {
              expected[0] = e.value[0];
              expected[1] = components > 1 ? e.value[1] : 0;
              found = true;
            }
          }
          // a constant without a default is 0
          CHECK(var->defaultValue == (found ? (uint64_t(expected[1]) << 32) | expected[0] : 0));
        }
      }

      // the compiler stores the default of every register the type covers, allocated or not
      if(rdcstr(sh.name) == "partial_vs3")
      {
        for(const CTABConstant &c : table.constants)
        {
          INFO(c.name.c_str());
          if(c.name == "part")
          {
            REQUIRE(c.defaultValue.size() == 16);
            CHECK(c.defaultValue[8] == FloatBits(3.0f));
            CHECK(c.defaultValue[15] == FloatBits(4.0f));
          }
          else if(c.name == "bpart")
          {
            CHECK(c.defaultValue == rdcarray<uint32_t>({~0U, ~0U, 0U, ~0U}));
          }
          else if(c.name == "tpart")
          {
            REQUIRE(c.defaultValue.size() == 12);
            CHECK(c.defaultValue[8] == FloatBits(7.0f));
          }
          else if(c.name == "mpart")
          {
            // a column per register
            REQUIRE(c.defaultValue.size() == 16);
            CHECK(c.defaultValue[4] == FloatBits(2.0f));
            CHECK(c.defaultValue[15] == FloatBits(16.0f));
          }
        }
      }
    }
  }

  SECTION("Malformed constant tables")
  {
    CTABBuilder good;
    uint32_t f4 = good.AddType(ParamClass::Vector, ParamType::Float, 1, 4);
    good.AddConstant("Colour", RegisterSet::Float4, 0, 1, f4);
    rdcarray<uint32_t> blob = good.Build();
    // the DWORDs after the comment token and the 'CTAB'
    rdcarray<uint32_t> table;
    table.assign(blob.data() + 2, blob.size() - 2);

    ConstantTable ct;

    // every truncation fails cleanly
    for(uint32_t len = 0; len < table.size(); len++)
    {
      ParseConstantTable(table.data(), len, ct);
      CHECK(ct.present);
    }

    ParseConstantTable(table.data(), (uint32_t)table.size(), ct);
    CHECK(ct.error.empty());
    CHECK(ct.constants.size() == 1);

    // a huge constant count
    rdcarray<uint32_t> broken = table;
    broken[3] = 0x7FFFFFFF;
    ParseConstantTable(broken.data(), (uint32_t)broken.size(), ct);
    CHECK(!ct.error.empty());
    CHECK(ct.constants.empty());

    // a name offset outside the table
    broken = table;
    broken[7] = 0xFFFFFFF0;
    ParseConstantTable(broken.data(), (uint32_t)broken.size(), ct);
    CHECK(!ct.error.empty());

    // a type that contains itself
    broken = table;
    uint32_t typeOffset = broken[7 + 3];
    broken[typeOffset / 4 + 2] = (1 << 16) | 0;    // elements 0, one member
    // the member is in the constant info, whose register count DWORD becomes the member's type
    broken[typeOffset / 4 + 3] = 28 + 4;
    broken[(28 + 4) / 4 + 1] = typeOffset;
    ParseConstantTable(broken.data(), (uint32_t)broken.size(), ct);
    CHECK(!ct.error.empty());

    // a small header size
    broken = table;
    broken[0] = 4;
    ParseConstantTable(broken.data(), (uint32_t)broken.size(), ct);
    CHECK(!ct.error.empty());

    // random tables never crash, and neither does reflecting a shader with one
    uint32_t seed = 999;
    for(int iter = 0; iter < 3000; iter++)
    {
      broken = table;
      for(int i = 0; i < 4; i++)
      {
        seed = seed * 1103515245 + 12345;
        uint32_t idx = (seed >> 8) % broken.size();
        seed = seed * 1103515245 + 12345;
        // small values are more likely to be valid offsets and counts
        broken[idx] = (iter & 1) ? seed : (seed % 64);
      }
      ParseConstantTable(broken.data(), (uint32_t)broken.size(), ct);
      ConstantTableComment(ct);

      rdcarray<uint32_t> shader = {VersionToken(true, 2, 0), CommentTok(uint32_t(broken.size()) + 1),
                                   MakeFourCC('C', 'T', 'A', 'B')};
      shader.append(broken);
      shader.push_back(EndTok());
      ShaderInfo info;
      info.Init(ResourceId(), shader.data(), shader.size());
      rdcarray<ShaderVariable> vars;
      for(uint32_t slot = 0; slot < 3; slot++)
        info.FillConstants(slot, bytebuf(), vars);
      CHECK(!info.GetDisassembly().empty());
    }
  };
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
