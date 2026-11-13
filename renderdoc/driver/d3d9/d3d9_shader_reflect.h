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
#include "d3d9_bytecode.h"

// Reflection for D3D9 shaders: the constant table D3DX and fxc put in a comment (CTAB), and
// building a ShaderReflection from it and the bytecode. Platform-neutral, like d3d9_bytecode.h.
//
// The ShaderReflection describes D3D9's constant registers as up to three constant blocks, one for
// each register set the shader uses, in this order: float (c#), integer (i#) and boolean (b#). A
// block's fixedBindNumber is its ConstantSet. Its variables' byteOffsets are register * 16 for
// float and integer registers (4 components each) and register * 4 for booleans, so the data
// FillConstants takes for a block is that whole register file from register 0: float4s, int4s, or
// one uint32 (0 or 1) per boolean register.
//
// Samplers and their textures are bound by register: each one is in samplers and in
// readOnlyResources with fixedBindNumber the sampler register (s#). For vs_3_0's vertex texture
// samplers that's 0-3, which the device calls D3DVERTEXTEXTURESAMPLER0-3.
namespace D3D9BC
{
// D3DXREGISTER_SET
enum class RegisterSet : uint16_t
{
  Bool = 0,
  Int4,
  Float4,
  Sampler,
};

// D3DXPARAMETER_CLASS
enum class ParamClass : uint16_t
{
  Scalar = 0,
  Vector,
  MatrixRows,
  MatrixColumns,
  Object,
  Struct,
};

// D3DXPARAMETER_TYPE
enum class ParamType : uint16_t
{
  Void = 0,
  Bool,
  Int,
  Float,
  String,
  Texture,
  Texture1D,
  Texture2D,
  Texture3D,
  TextureCube,
  Sampler,
  Sampler1D,
  Sampler2D,
  Sampler3D,
  SamplerCube,
  PixelShader,
  VertexShader,
  PixelFragment,
  VertexFragment,
  Unsupported,
};

struct CTABMember;

// D3DXSHADER_TYPEINFO
struct CTABType
{
  ParamClass cls = ParamClass::Scalar;
  ParamType type = ParamType::Float;
  uint16_t rows = 1;
  uint16_t columns = 1;
  // 0 or 1 for a single value
  uint16_t elements = 1;
  rdcarray<CTABMember> members;
};

// D3DXSHADER_STRUCTMEMBERINFO
struct CTABMember
{
  rdcstr name;
  CTABType type;
};

// D3DXSHADER_CONSTANTINFO
struct CTABConstant
{
  rdcstr name;
  RegisterSet set = RegisterSet::Float4;
  uint16_t registerIndex = 0;
  // how many registers the compiler allocated, which can be fewer than the type needs if the end
  // of it isn't used
  uint16_t registerCount = 0;
  CTABType type;
  // the default value, as the compiler stores it: the registers' contents for every register the
  // type covers, including any past registerCount. That's 4 DWORDs per register in the float and
  // integer sets (a matrix's rows or columns, or a vector, padded with zeros; an int scalar the
  // compiler puts in an integer register is stored as a loop's (count, 0, 1, 0)), and 1 DWORD per
  // register in the boolean set (0 or 0xFFFFFFFF). Checked against D3DX9_43's SetDefaults.
  rdcarray<uint32_t> defaultValue;
};

// D3DXSHADER_CONSTANTTABLE
struct ConstantTable
{
  // whether the shader has a constant table that could be read
  bool present = false;
  rdcstr creator;
  rdcstr target;
  uint32_t version = 0;
  uint32_t flags = 0;
  rdcarray<CTABConstant> constants;
  // if the table is there but couldn't be read, why
  rdcstr error;
};

// The register sets as constant blocks, see the top of this file
enum class ConstantSet : uint32_t
{
  Float = 0,
  Int = 1,
  Bool = 2,
  Count,
};

const char *ConstantBlockName(ConstantSet set);

// Reads a constant table from the DWORDs after a comment's 'CTAB' code.
void ParseConstantTable(const uint32_t *data, uint32_t numDWORDs, ConstantTable &table);

// The HLSL name of a type, without the array size, e.g. "float4x4", "sampler2D".
rdcstr CTABTypeName(const CTABType &type);

// The "Parameters" and "Registers" comment fxc prints before the disassembly.
rdcstr ConstantTableComment(const ConstantTable &table);

// A parsed shader and everything known about it, which replay keeps for each shader.
class ShaderInfo
{
public:
  // tokens is the shader's bytecode. Never fails: if the bytecode can't be fully parsed, the
  // reflection and disassembly have what could be.
  void Init(ResourceId id, const uint32_t *tokens, size_t numTokens);

  const Program &GetProgram() const { return m_Program; }
  const ConstantTable &GetConstantTable() const { return m_ConstantTable; }
  const ShaderReflection &GetReflection() const { return m_Reflection; }
  ShaderStage GetStage() const { return m_Reflection.stage; }

  // the constant table comment and the disassembly
  rdcstr GetDisassembly() const;

  // Fills the variables of a constant block from that set's register file (see the top of this
  // file), with the shader's def, defi and defb literals replacing the registers they define,
  // since the shader reads those rather than the device's constants.
  void FillConstants(uint32_t cbufSlot, const bytebuf &registerData,
                     rdcarray<ShaderVariable> &outvars) const;

  // The register file with the literals applied, as FillConstants uses it.
  bytebuf ApplyLiterals(ConstantSet set, const bytebuf &registerData) const;

private:
  struct Literal
  {
    ConstantSet set;
    uint32_t reg;
    uint32_t value[4];
  };

  void BuildReflection(ResourceId id);

  Program m_Program;
  ConstantTable m_ConstantTable;
  ShaderReflection m_Reflection;
  rdcarray<Literal> m_Literals;
};

};    // namespace D3D9BC
