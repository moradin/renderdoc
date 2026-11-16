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

#include "d3d9_vs_debug.h"
#include "common/formatting.h"

// What the trace has:
// - inputs: the v# registers the input signature uses, as the vertex fetched them, with a source
//   variable per signature element (its semantic, e.g. TEXCOORD1).
// - constantBlocks: one per reflection constant block (c#, i#, b#), with a member for each register
//   the shader can read, the shader's literals applied. The registers are named by source
//   variables, which is what the viewer's constants list shows:
//   - each scalar, vector and matrix the constant table describes, by its HLSL name with struct
//     members and array elements spelled out (lights[1].dir), on the registers the compiler
//     allocated it. A variable the compiler only allocated part of is cut to that part: a
//     column_major float4x4 given three registers shows as the 4x3 the shader reads.
//   - those registers again by register, under "Registers" (Registers.c4), so the viewer finds the
//     disassembly's names when hovering over them.
//   - the shader's def, defi and defb literals under "Literals" (Literals.c10), so they're told
//     apart from what the application set.
//   - any other register by its name in the disassembly (c12).
// - the mutable registers, which the first state lists: the temporaries, a0, aL, p0 and the
//   outputs the shader uses. oFog and oPts are one component, aL is an integer and p0 booleans.
// - a source variable per output signature element, on its output register's components, which
//   is how the tests compare a debugged vertex with VS Out.
// - readOnlyResources and samplers: the vertex texture samplers texldl reads.
// - instInfo: each instruction's line in ShaderInfo::GetDisassembly.
//
// Each state is one instruction, except that declarations (dcl, def, defi, defb) run with the
// instruction before them, so stepping never stops on one. The last state points at the last
// instruction run, since there's no instruction after the end to point at.

namespace D3D9Emu
{
using namespace D3D9BC;

namespace
{
bool IsDeclaration(Opcode op)
{
  return op == Opcode::Dcl || op == Opcode::Def || op == Opcode::DefI || op == Opcode::DefB;
}

// what each register set's registers are called in the disassembly, and their type
const char *const SetPrefixes[] = {"c", "i", "b"};
const RegType SetRegTypes[] = {RegType::Const, RegType::ConstInt, RegType::ConstBool};

VarType SetVarType(ConstantSet set)
{
  return set == ConstantSet::Float ? VarType::Float
         : set == ConstantSet::Int ? VarType::SInt
                                   : VarType::Bool;
}

// Makes the source variables for a constant table: one for each scalar, vector and matrix in a
// constant's type, on the registers from reg up to end (the end of what the compiler allocated the
// constant). Struct members and array elements are in registers one after the other (see
// ConstantRegistersPerElement).
struct ConstantNamer
{
  ConstantSet set;
  // the constant block's variable name, e.g. "c"
  rdcstr block;
  rdcarray<SourceVariableMapping> *out;
  // the registers named
  std::set<uint32_t> *named;

  DebugVariableReference Ref(uint32_t reg, uint32_t comp) const
  {
    named->insert(reg);
    return DebugVariableReference(DebugVariableType::Constant,
                                  StringFormat::Fmt("%s[%u]", block.c_str(), reg), comp);
  }

  void Name(const rdcstr &name, const CTABType &type, uint32_t reg, uint32_t end, int depth = 0)
  {
    if(reg >= end || depth > 16)
      return;

    const uint32_t elements = RDCMAX(1U, uint32_t(type.elements));
    if(elements > 1)
    {
      CTABType element = type;
      element.elements = 1;
      const uint64_t per = ConstantRegistersPerElement(element, set);
      for(uint32_t e = 0; e < elements && reg + e * per < end; e++)
        Name(StringFormat::Fmt("%s[%u]", name.c_str(), e), element, uint32_t(reg + e * per), end,
             depth + 1);
      return;
    }

    if(type.cls == ParamClass::Struct)
    {
      uint64_t r = reg;
      for(const CTABMember &m : type.members)
      {
        if(r >= end)
          break;
        Name(name + "." + m.name, m.type, uint32_t(r), end, depth + 1);
        r += uint64_t(ConstantRegistersPerElement(m.type, set)) *
             RDCMAX(1U, uint32_t(m.type.elements));
      }
      return;
    }

    const uint32_t avail = end - reg;
    const bool isBool = set == ConstantSet::Bool;
    uint32_t rows = RDCCLAMP(uint32_t(type.rows), 1U, 4U);
    uint32_t columns = RDCCLAMP(uint32_t(type.columns), 1U, 4U);
    rdcarray<rdcpair<uint32_t, uint32_t>> comps;

    switch(type.cls)
    {
      case ParamClass::Scalar:
        rows = columns = 1;
        comps.push_back({reg, 0});
        break;
      case ParamClass::Vector:
        rows = 1;
        // in the boolean set each component is a register
        if(isBool)
          columns = RDCMIN(columns, avail);
        for(uint32_t c = 0; c < columns; c++)
          comps.push_back(isBool ? rdcpair<uint32_t, uint32_t>(reg + c, 0)
                                 : rdcpair<uint32_t, uint32_t>(reg, c));
        break;
      case ParamClass::MatrixRows:
      case ParamClass::MatrixColumns:
      {
        const bool rowMajor = type.cls == ParamClass::MatrixRows;
        if(isBool)
        {
          // every component is a register, so only a whole matrix makes sense
          if(rows * columns > avail)
            return;
          for(uint32_t r = 0; r < rows; r++)
            for(uint32_t c = 0; c < columns; c++)
              comps.push_back({reg + (rowMajor ? r * columns + c : c * rows + r), 0});
          break;
        }
        // a register per row (row_major) or column, as many as were allocated
        if(rowMajor)
          rows = RDCMIN(rows, avail);
        else
          columns = RDCMIN(columns, avail);
        for(uint32_t r = 0; r < rows; r++)
          for(uint32_t c = 0; c < columns; c++)
            comps.push_back(rowMajor ? rdcpair<uint32_t, uint32_t>(reg + r, c)
                                     : rdcpair<uint32_t, uint32_t>(reg + c, r));
        break;
      }
      // samplers aren't in these sets
      default: return;
    }

    SourceVariableMapping map;
    map.name = name;
    map.type = SetVarType(set);
    map.rows = rows;
    map.columns = columns;
    map.offset = reg * (isBool ? 4 : 16);
    map.signatureIndex = -1;
    for(const rdcpair<uint32_t, uint32_t> &c : comps)
      map.variables.push_back(Ref(c.first, c.second));
    out->push_back(map);
  }
};

bool IsOutput(RegType type)
{
  return type == RegType::RastOut || type == RegType::AttrOut || type == RegType::TexCrdOutOrOutput;
}

// the output register a signature element is in, as OutputRegister finds it
bool OutputRegisterOf(const Version &version, const SigParameter &sig, RegType &type, uint32_t &index)
{
  index = sig.regIndex;
  type = RegType::TexCrdOutOrOutput;
  if(version.major >= 3)
    return index < 12;

  if(sig.semanticName == "POSITION" || sig.semanticName == "FOG" || sig.semanticName == "PSIZE")
  {
    type = RegType::RastOut;
    index = sig.semanticName == "POSITION" ? 0 : sig.semanticName == "FOG" ? 1 : 2;
    return true;
  }
  if(sig.semanticName == "COLOR")
  {
    type = RegType::AttrOut;
    return index < 2;
  }
  if(sig.semanticName == "TEXCOORD")
    return index < 8;
  return false;
}

SourceVariableMapping Mapping(const rdcstr &name, VarType type, uint32_t columns,
                              int32_t signatureIndex)
{
  SourceVariableMapping ret;
  ret.name = name;
  ret.type = type;
  ret.rows = columns > 0 ? 1 : 0;
  ret.columns = columns;
  ret.offset = 0;
  ret.signatureIndex = signatureIndex;
  return ret;
}
};    // namespace

ShaderVariable VSDebugger::Variable(RegType type, uint32_t index, const Vec4 &f, const IVec4 &i) const
{
  const rdcstr name = RegisterName(m_Version, type, index);
  switch(type)
  {
    case RegType::AddrOrTexture: return ShaderVariable(name, i.v[0], i.v[1], i.v[2], i.v[3]);
    case RegType::Predicate:
    {
      ShaderVariable ret(name, uint32_t(i.v[0]), uint32_t(i.v[1]), uint32_t(i.v[2]),
                         uint32_t(i.v[3]));
      ret.type = VarType::Bool;
      return ret;
    }
    case RegType::Loop:
    {
      ShaderVariable ret(name, i.v[0], 0, 0, 0);
      ret.columns = 1;
      return ret;
    }
    case RegType::RastOut:
      // oFog and oPts are scalars
      if(index > 0)
      {
        ShaderVariable ret(name, f[0], 0.0f, 0.0f, 0.0f);
        ret.columns = 1;
        return ret;
      }
      break;
    default: break;
  }
  return ShaderVariable(name, f[0], f[1], f[2], f[3]);
}

ShaderVariable VSDebugger::Current(RegType type, uint32_t index) const
{
  const VSRegisters &regs = m_Thread.Registers();
  Vec4 f;
  IVec4 i;
  switch(type)
  {
    case RegType::Temp:
      if(index < MaxVSTemps)
        f = regs.r[index];
      break;
    case RegType::AddrOrTexture: i = regs.a0; break;
    case RegType::Predicate:
      for(int c = 0; c < 4; c++)
        i.v[c] = regs.p[c] ? 1 : 0;
      break;
    case RegType::Loop: i.v[0] = regs.aL(); break;
    case RegType::RastOut:
      if(index < 3)
        f = regs.out.rast[index];
      break;
    case RegType::AttrOut:
      if(index < 2)
        f = regs.out.attr[index];
      break;
    case RegType::TexCrdOutOrOutput:
      if(index < 12)
        f = regs.out.tex[index];
      break;
    default: break;
  }
  return Variable(type, index, f, i);
}

ShaderDebugTrace *VSDebugger::Begin(const VSEmulator &emu, const ShaderInfo &info,
                                    const rdcarray<uint32_t> &instLines,
                                    const Vec4 inputs[MaxVSInputs], const VSConstants &consts,
                                    const VSTexture textures[NumVertexSamplers])
{
  ShaderDebugTrace *ret = new ShaderDebugTrace;
  ret->stage = ShaderStage::Vertex;
  ret->debugger = this;

  for(uint32_t i = 0; i < MaxVSInputs; i++)
    m_Inputs[i] = inputs[i];
  m_Emu = &emu;
  m_Version = emu.GetVersion();
  m_Steps = 0;
  m_LastInstruction = 0;
  m_Registers.clear();
  m_Known.clear();
  m_Thread.Start(emu, m_Inputs, consts, textures);

  const Program &prog = emu.GetProgram();
  const ShaderReflection &refl = info.GetReflection();

  //////////////////////////////////////////////////////////////////////////
  // inputs

  bool inputUsed[MaxVSInputs] = {};
  for(const SigParameter &sig : refl.inputSignature)
    if(sig.regIndex < MaxVSInputs)
      inputUsed[sig.regIndex] = true;

  for(uint32_t reg = 0; reg < MaxVSInputs; reg++)
  {
    if(!inputUsed[reg])
      continue;
    const Vec4 &v = m_Inputs[reg];
    ret->inputs.push_back(
        ShaderVariable(RegisterName(m_Version, RegType::Input, reg), v[0], v[1], v[2], v[3]));
  }

  for(int32_t s = 0; s < refl.inputSignature.count(); s++)
  {
    const SigParameter &sig = refl.inputSignature[s];
    if(sig.regIndex >= MaxVSInputs)
      continue;
    SourceVariableMapping map = Mapping(sig.semanticIdxName, VarType::Float, sig.compCount, s);
    const rdcstr reg = RegisterName(m_Version, RegType::Input, sig.regIndex);
    for(uint32_t c = 0; c < 4; c++)
      if(sig.regChannelMask & (1 << c))
        map.variables.push_back(DebugVariableReference(DebugVariableType::Input, reg, c));
    ret->sourceVars.push_back(map);
  }

  //////////////////////////////////////////////////////////////////////////
  // the registers the shader can change

  std::set<uint32_t> temps, outputs;
  bool a0 = false, aL = false, p0 = false, allOutputs = false;

  auto use = [&](RegType type, uint32_t index, const RelativeAddress &rel) {
    if(type == RegType::Temp && index < MaxVSTemps)
      temps.insert(index);
    else if(type == RegType::AddrOrTexture)
      a0 = true;
    else if(type == RegType::Loop)
      aL = true;
    else if(type == RegType::Predicate)
      p0 = true;
    else if(IsOutput(type))
    {
      outputs.insert(Key(type, index));
      // vs_3_0 can index the outputs with aL
      if(rel.present)
        allOutputs = true;
    }

    if(rel.present)
    {
      if(rel.type == RegType::Loop)
        aL = true;
      else
        a0 = true;
    }
  };

  for(const Instruction &inst : prog.instructions)
  {
    if(inst.op == Opcode::Loop)
      aL = true;
    if(IsDeclaration(inst.op))
    {
      // vs_3_0's declared outputs
      if(inst.op == Opcode::Dcl && inst.hasDst && IsOutput(inst.dst.type))
        outputs.insert(Key(inst.dst.type, inst.dst.index));
      continue;
    }
    if(inst.hasDst)
      use(inst.dst.type, inst.dst.index, inst.dst.rel);
    if(inst.predicated)
      use(inst.predicate.type, inst.predicate.index, inst.predicate.rel);
    for(const SrcParam &src : inst.srcs)
      use(src.type, src.index, src.rel);
  }

  if(allOutputs)
    for(uint32_t o = 0; o < 12; o++)
      outputs.insert(Key(RegType::TexCrdOutOrOutput, o));

  for(const SigParameter &sig : refl.outputSignature)
  {
    RegType type;
    uint32_t index;
    if(OutputRegisterOf(m_Version, sig, type, index))
      outputs.insert(Key(type, index));
  }

  auto add = [this](RegType type, uint32_t index) {
    if(m_Known.insert(Key(type, index)).second)
      m_Registers.push_back({type, index});
  };

  for(uint32_t t : temps)
    add(RegType::Temp, t);
  if(a0)
    add(RegType::AddrOrTexture, 0);
  if(aL)
    add(RegType::Loop, 0);
  if(p0)
    add(RegType::Predicate, 0);
  // the keys sort oPos, oFog, oPts, oD#, then oT# or o#
  for(uint32_t key : outputs)
    add(RegType(key >> 16), key & 0xFFFF);

  //////////////////////////////////////////////////////////////////////////
  // outputs

  for(int32_t s = 0; s < refl.outputSignature.count(); s++)
  {
    const SigParameter &sig = refl.outputSignature[s];
    RegType type;
    uint32_t index;
    if(!OutputRegisterOf(m_Version, sig, type, index))
      continue;
    SourceVariableMapping map = Mapping(sig.semanticIdxName, VarType::Float, sig.compCount, s);
    const rdcstr reg = RegisterName(m_Version, type, index);
    for(uint32_t c = 0; c < 4; c++)
      if(sig.regChannelMask & (1 << c))
        map.variables.push_back(DebugVariableReference(DebugVariableType::Variable, reg, c));
    ret->sourceVars.push_back(map);
  }

  //////////////////////////////////////////////////////////////////////////
  // constants

  const ConstantTable &ctab = info.GetConstantTable();

  // the registers each def, defi and defb sets
  std::set<uint32_t> literals[3];
  for(const Instruction &inst : prog.instructions)
  {
    if(!inst.hasDst)
      continue;
    if(inst.op == Opcode::Def && inst.dst.type == RegType::Const)
      literals[0].insert(inst.dst.index);
    else if(inst.op == Opcode::DefI && inst.dst.type == RegType::ConstInt)
      literals[1].insert(inst.dst.index);
    else if(inst.op == Opcode::DefB && inst.dst.type == RegType::ConstBool)
      literals[2].insert(inst.dst.index);
  }

  // the constant table's registers again by register, listed after everything else
  rdcarray<SourceVariableMapping> aliases;

  for(uint32_t k = 0; k < (uint32_t)refl.constantBlocks.size(); k++)
  {
    const ConstantBlock &block = refl.constantBlocks[k];

    // the variable's index matches the reflection's block, even for one that can't be shown
    ShaderVariable cb;
    cb.rows = cb.columns = 0;
    cb.SetBindIndex(ShaderBindIndex(DescriptorCategory::ConstantBlock, k, 0));

    const uint32_t set = block.fixedBindNumber;
    if(set > 2)
    {
      ret->constantBlocks.push_back(cb);
      continue;
    }

    cb.name = SetPrefixes[set];
    const uint32_t stride = set == 2 ? 4 : 16;

    // the constant table's names, on the registers the compiler allocated each constant
    std::set<uint32_t> named;
    ConstantNamer namer = {ConstantSet(set), cb.name, &ret->sourceVars, &named};
    std::set<rdcstr> tableNames;
    for(const CTABConstant &c : ctab.constants)
    {
      if(c.set == RegisterSet::Sampler || ConstantSetFor(c.set) != ConstantSet(set))
        continue;
      tableNames.insert(c.name);
      namer.Name(c.name, c.type, c.registerIndex,
                 RDCMIN(uint32_t(c.registerIndex) + c.registerCount, 8192U));
    }

    // the registers the shader can read: those named, and those of the reflection's other
    // variables (literals, and registers the code reads that the table doesn't describe)
    std::set<uint32_t> regs = named;
    for(const ShaderConstant &var : block.variables)
    {
      if(tableNames.count(var.name))
        continue;
      const uint32_t first = var.byteOffset / stride;
      const uint32_t size = var.type.arrayByteStride * RDCMAX(1U, var.type.elements);
      const uint32_t count = RDCMIN(RDCMAX(1U, (size + stride - 1) / stride), 8192U);
      for(uint32_t r = first; r < first + count && r < 8192U; r++)
        regs.insert(r);
    }

    for(uint32_t r : regs)
    {
      const rdcstr member = StringFormat::Fmt("[%u]", r);
      ShaderVariable m;
      if(set == 0)
      {
        const Vec4 v = r < consts.floats.size() ? consts.floats[r] : Vec4();
        m = ShaderVariable(member, v[0], v[1], v[2], v[3]);
      }
      else if(set == 1)
      {
        const IVec4 v = r < consts.ints.size() ? consts.ints[r] : IVec4();
        m = ShaderVariable(member, v.v[0], v.v[1], v.v[2], v.v[3]);
      }
      else
      {
        const uint32_t b = r < consts.bools.size() && consts.bools[r] ? 1U : 0U;
        m = ShaderVariable(member, b, 0U, 0U, 0U);
        m.type = VarType::Bool;
        m.columns = 1;
      }
      cb.members.push_back(m);

      const rdcstr regName = RegisterName(m_Version, SetRegTypes[set], r);
      const bool isNamed = named.count(r) > 0;
      rdcstr name = regName;
      if(isNamed)
        name = "Registers." + regName;
      else if(literals[set].count(r))
        name = "Literals." + regName;

      SourceVariableMapping map = Mapping(name, m.type, m.columns, -1);
      map.offset = r * stride;
      const rdcstr path = cb.name + member;
      for(uint32_t c = 0; c < m.columns; c++)
        map.variables.push_back(DebugVariableReference(DebugVariableType::Constant, path, c));
      if(isNamed)
        aliases.push_back(map);
      else
        ret->sourceVars.push_back(map);
    }

    // the block itself, so the viewer doesn't also list it as raw registers
    SourceVariableMapping blockMap = Mapping(block.name, VarType::Unknown, 0, -1);
    blockMap.variables.push_back(DebugVariableReference(DebugVariableType::Constant, cb.name));
    ret->sourceVars.push_back(blockMap);

    ret->constantBlocks.push_back(cb);
  }

  ret->sourceVars.append(aliases);

  //////////////////////////////////////////////////////////////////////////
  // textures

  for(uint32_t i = 0; i < (uint32_t)refl.readOnlyResources.size(); i++)
  {
    const ShaderResource &res = refl.readOnlyResources[i];
    ShaderVariable reg(RegisterName(m_Version, RegType::Sampler, res.fixedBindNumber), 0U, 0U, 0U,
                       0U);
    reg.rows = reg.columns = 1;
    reg.SetBindIndex(ShaderBindIndex(DescriptorCategory::ReadOnlyResource, i, 0));
    ret->readOnlyResources.push_back(reg);

    SourceVariableMapping map = Mapping(res.name, VarType::ReadOnlyResource, 1, -1);
    map.variables.push_back(DebugVariableReference(DebugVariableType::ReadOnlyResource, reg.name));
    ret->sourceVars.push_back(map);
  }

  for(uint32_t i = 0; i < (uint32_t)refl.samplers.size(); i++)
  {
    const ShaderSampler &samp = refl.samplers[i];
    ShaderVariable reg(RegisterName(m_Version, RegType::Sampler, samp.fixedBindNumber), 0U, 0U, 0U,
                       0U);
    reg.rows = reg.columns = 1;
    reg.SetBindIndex(ShaderBindIndex(DescriptorCategory::Sampler, i, 0));
    ret->samplers.push_back(reg);
  }

  //////////////////////////////////////////////////////////////////////////
  // disassembly lines

  ret->instInfo.resize(prog.instructions.size());
  for(uint32_t i = 0; i < (uint32_t)prog.instructions.size(); i++)
  {
    ret->instInfo[i].instruction = i;
    ret->instInfo[i].lineInfo.disassemblyLine = i < instLines.size() ? instLines[i] : 0;
  }

  return ret;
}

rdcarray<rdcstr> VSDebugger::Callstack() const
{
  // the oldest first, as the viewer wants them
  rdcarray<rdcstr> ret = {"main"};
  for(const VSCallFrame &f : m_Thread.Registers().calls)
    ret.push_back(StringFormat::Fmt("l%u", f.label));
  return ret;
}

void VSDebugger::SkipDeclarations(rdcarray<VSRegisterChange> &changes)
{
  const rdcarray<Instruction> &insts = m_Emu->GetProgram().instructions;
  while(!m_Thread.Finished() && m_Thread.NextInstruction() < insts.size() &&
        IsDeclaration(insts[m_Thread.NextInstruction()].op))
  {
    if(!m_Thread.Step(&changes))
      break;
  }
}

rdcarray<ShaderDebugState> VSDebugger::Continue(uint32_t maxSteps)
{
  rdcarray<ShaderDebugState> ret;

  rdcarray<VSRegisterChange> changes;

  if(m_Steps == 0)
  {
    // declarations change nothing
    SkipDeclarations(changes);
    changes.clear();

    ShaderDebugState initial;
    initial.stepIndex = 0;
    initial.nextInstruction = m_Thread.Finished() ? 0 : m_Thread.NextInstruction();
    initial.callstack = Callstack();
    for(const Register &r : m_Registers)
    {
      ShaderVariableChange change;
      change.after = Current(r.type, r.index);
      initial.changes.push_back(std::move(change));
    }
    ret.push_back(std::move(initial));
    m_Steps = 1;
  }

  std::set<uint32_t> seen;

  for(uint32_t n = 0; n < maxSteps && !m_Thread.Finished(); n++)
  {
    changes.clear();
    const uint32_t inst = m_Thread.NextInstruction();
    // a failed instruction changed nothing and finished the thread, so the last state stays on it
    if(!m_Thread.Step(&changes))
      break;
    m_LastInstruction = inst;
    SkipDeclarations(changes);

    const Instruction &ran = m_Emu->GetProgram().instructions[inst];

    ShaderDebugState state;
    state.stepIndex = m_Steps++;
    state.nextInstruction = m_Thread.Finished() ? m_LastInstruction : m_Thread.NextInstruction();
    state.callstack = Callstack();
    if(ran.op == Opcode::TexLdl)
      state.flags |= ShaderEvents::SampleLoadGather;

    // an instruction changes each register once, but declarations run with it
    seen.clear();
    for(const VSRegisterChange &c : changes)
    {
      const uint32_t key = Key(c.type, c.index);
      if(!seen.insert(key).second)
        continue;

      ShaderVariableChange change;
      // a register the shader wasn't expected to change appears here, with nothing before
      if(m_Known.insert(key).second)
        m_Registers.push_back({c.type, c.index});
      else
        change.before = Variable(c.type, c.index, c.f, c.i);
      change.after = Current(c.type, c.index);

      // a NaN or infinity in a component the instruction wrote
      if(ran.hasDst && c.type == ran.dst.type && change.after.type == VarType::Float)
      {
        for(uint32_t comp = 0; comp < change.after.columns; comp++)
        {
          const float f = change.after.value.f32v[comp];
          if((ran.dst.writeMask & (1 << comp)) && (RDCISNAN(f) || RDCISINF(f)))
            state.flags |= ShaderEvents::GeneratedNanOrInf;
        }
      }

      state.changes.push_back(std::move(change));
    }

    ret.push_back(std::move(state));
  }

  return ret;
}

};    // namespace D3D9Emu

#if ENABLED(ENABLE_UNIT_TESTS)

#include <map>
#include "catch/catch.hpp"
#include "d3d9_bytecode_test.h"
#include "d3d9_ctab_test_data.h"
#include "d3d9_shader_reflect.h"

using namespace D3D9BCTest;
using namespace D3D9Emu;

namespace
{
struct DebugRun
{
  ShaderInfo info;
  VSEmulator emu;
  VSConstants consts;
  Vec4 inputs[MaxVSInputs];
  VSTexture textures[NumVertexSamplers];
  rdcarray<uint32_t> lines;

  VSDebugger debugger;
  ShaderDebugTrace *trace = NULL;
  rdcarray<ShaderDebugState> states;
  // the variables after the last state
  std::map<rdcstr, ShaderVariable> vars;

  ~DebugRun() { delete trace; }

  void Init(const rdcarray<uint32_t> &tokens)
  {
    info.Init(ResourceId(), tokens.data(), tokens.size());
    REQUIRE(emu.Init(info.GetProgram()));
    consts = emu.ApplyLiterals(consts);
    info.GetDisassembly(&lines);
  }

  // debugs the vertex to the end, maxSteps at a time, checking each change's value before is what
  // the variable had, and that going back from the end undoes everything
  void Debug(uint32_t maxSteps)
  {
    trace = debugger.Begin(emu, info, lines, inputs, consts, textures);
    REQUIRE(trace->debugger == &debugger);

    for(;;)
    {
      rdcarray<ShaderDebugState> next = debugger.Continue(maxSteps);
      if(next.empty())
        break;
      // the first call adds the initial state
      CHECK(next.size() <= size_t(maxSteps) + (states.empty() ? 1 : 0));
      states.append(next);
    }
    // nothing more once it's finished
    CHECK(debugger.Continue(maxSteps).empty());

    for(size_t s = 0; s < states.size(); s++)
    {
      CHECK(states[s].stepIndex == s);
      for(const ShaderVariableChange &c : states[s].changes)
      {
        auto it = vars.find(c.after.name);
        if(c.before.name.empty())
        {
          const bool isNew = it == vars.end();
          CHECK(isNew);
        }
        else
        {
          REQUIRE(vars.count(c.after.name) == 1);
          const bool sameBefore = c.before == it->second;
          CHECK(sameBefore);
        }
        vars[c.after.name] = c.after;
      }
    }

    std::map<rdcstr, ShaderVariable> back = vars;
    for(size_t s = states.size(); s > 0; s--)
    {
      for(const ShaderVariableChange &c : states[s - 1].changes)
      {
        REQUIRE(back.count(c.after.name) == 1);
        const bool sameAfter = back[c.after.name] == c.after;
        CHECK(sameAfter);
        if(c.before.name.empty())
          back.erase(c.after.name);
        else
          back[c.before.name] = c.before;
      }
    }
    CHECK(back.empty());
  }

  rdcarray<uint32_t> NextInstructions() const
  {
    rdcarray<uint32_t> ret;
    for(const ShaderDebugState &s : states)
      ret.push_back(s.nextInstruction);
    return ret;
  }

  rdcarray<rdcstr> InitialNames() const
  {
    rdcarray<rdcstr> ret;
    for(const ShaderVariableChange &c : states[0].changes)
      ret.push_back(c.after.name);
    return ret;
  }

  const SourceVariableMapping *SourceVar(const rdcstr &name) const
  {
    for(const SourceVariableMapping &m : trace->sourceVars)
      if(m.name == name)
        return &m;
    return NULL;
  }

  // checks each output signature element's source variable ends with what Execute outputs
  void CheckOutputs()
  {
    VSOutputs out;
    rdcstr error;
    REQUIRE(emu.Execute(inputs, consts, textures, out, error));

    const rdcarray<SigParameter> &sig = info.GetReflection().outputSignature;
    REQUIRE(!sig.empty());
    for(int32_t s = 0; s < sig.count(); s++)
    {
      const SourceVariableMapping *map = NULL;
      for(const SourceVariableMapping &m : trace->sourceVars)
        if(m.signatureIndex == s && !m.variables.empty() &&
           m.variables[0].type == DebugVariableType::Variable)
          map = &m;
      REQUIRE(map != NULL);
      CHECK(map->name == sig[s].semanticIdxName);
      CHECK(map->columns == sig[s].compCount);

      const Vec4 *reg = OutputRegister(out, emu.GetVersion(), sig[s]);
      REQUIRE(reg != NULL);
      uint32_t c = 0;
      for(uint32_t comp = 0; comp < 4; comp++)
      {
        if(!(sig[s].regChannelMask & (1 << comp)))
          continue;
        REQUIRE(c < map->variables.size());
        const DebugVariableReference &ref = map->variables[c++];
        CHECK(ref.component == comp);
        REQUIRE(vars.count(ref.name) == 1);
        const ShaderVariable &v = vars[ref.name];
        uint32_t got, expect;
        memcpy(&got, &v.value.f32v[comp], 4);
        memcpy(&expect, &reg->v[comp], 4);
        CHECK(got == expect);
      }
    }
  }
};

const uint32_t X = Swz(0, 0, 0, 0), Y = Swz(1, 1, 1, 1), Z = Swz(2, 2, 2, 2);

uint32_t R(uint32_t i, uint32_t swz = 0xE4)
{
  return Src(RegType::Temp, i, swz);
}
uint32_t C(uint32_t i, uint32_t swz = 0xE4)
{
  return Src(RegType::Const, i, swz);
}
uint32_t DR(uint32_t i, uint32_t mask = 0xF)
{
  return Dst(RegType::Temp, i, mask);
}
};    // namespace

TEST_CASE("D3D9 vertex shader debugging", "[d3d9][vsdebug]")
{
  DebugRun run;

  SECTION("vs_3_0: registers, flow control, packed outputs")
  {
    Assembler a(3, 0);
    a.dcl(DeclUsage::Position, 0, 0);
    a.dcl(DeclUsage::TexCoord, 0, 1);
    a.dcl(DeclUsage::Position, 0, 0, RegType::TexCrdOutOrOutput);
    a.dcl(DeclUsage::TexCoord, 0, 1, RegType::TexCrdOutOrOutput, 0x3);
    a.dcl(DeclUsage::TexCoord, 1, 1, RegType::TexCrdOutOrOutput, 0xC);
    a.defi(0, 3, 0, 1, 0);
    a.def(10, 0.5f, 1.0f, 2.0f, 0.0f);
    // 7
    a.op(Opcode::Mov, {DR(0), Src(RegType::Input, 0)});
    a.op(Opcode::MovA, {Dst(RegType::AddrOrTexture, 0, 0x1), C(10, Z)});
    a.op(Opcode::SetP, {Dst(RegType::Predicate, 0), Src(RegType::Input, 1), C(10, X)},
         uint32_t(Comparison::GT));
    // 10: r1 = c[a0.x]
    a.op(Opcode::Mov, {DR(1), C(0) | RelBit, Src(RegType::AddrOrTexture, 0, X)});
    a.op(Opcode::Loop, {Src(RegType::Loop, 0), Src(RegType::ConstInt, 0)});
    a.op(Opcode::Add, {DR(0), R(0), C(0) | RelBit, Src(RegType::Loop, 0)});
    a.op(Opcode::EndLoop, {});
    // 14
    a.op(Opcode::Mov, {DR(2), Src(RegType::Predicate, 0), C(10, Y)}, 0, PredBit);
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 0), R(0)});
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 1, 0x3), Src(RegType::Input, 1)});
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 1, 0xC), R(1, Swz(0, 1, 0, 1))});

    run.consts.floats.resize(10);
    for(uint32_t i = 0; i < 10; i++)
      run.consts.floats[i] = Vec4(float(i), float(i * 2), 0.0f, 1.0f);
    run.inputs[0] = Vec4(1.0f, 2.0f, 3.0f, 1.0f);
    run.inputs[1] = Vec4(0.75f, 0.25f, 0.5f, 1.0f);
    run.Init(a.finish());
    // a few at a time, to check stopping and carrying on
    run.Debug(3);

    CHECK(run.debugger.GetError().empty());
    CHECK(run.trace->stage == ShaderStage::Vertex);

    // declarations aren't steps, and the last state stays on the last instruction
    CHECK(run.NextInstructions() ==
          rdcarray<uint32_t>({7, 8, 9, 10, 11, 12, 13, 12, 13, 12, 13, 14, 15, 16, 17, 17}));

    CHECK(run.InitialNames() == rdcarray<rdcstr>({"r0", "r1", "r2", "a0", "aL", "p0", "o0", "o1"}));
    for(const ShaderVariableChange &c : run.states[0].changes)
    {
      CHECK(c.before.name.empty());
      CHECK(c.after.rows == 1);
    }

    // the types and sizes
    CHECK(run.vars["r0"].type == VarType::Float);
    CHECK(run.vars["r0"].columns == 4);
    CHECK(run.vars["a0"].type == VarType::SInt);
    CHECK(run.vars["a0"].value.s32v[0] == 2);
    CHECK(run.vars["aL"].type == VarType::SInt);
    CHECK(run.vars["aL"].columns == 1);
    CHECK(run.vars["p0"].type == VarType::Bool);
    CHECK(run.vars["p0"].value.u32v[0] == 1);
    CHECK(run.vars["p0"].value.u32v[1] == 0);
    CHECK(run.vars["r1"].value.f32v[0] == 2.0f);
    CHECK(run.vars["r1"].value.f32v[1] == 4.0f);
    // the predicated write only wrote x and w
    CHECK(run.vars["r2"].value.f32v[0] == 1.0f);
    CHECK(run.vars["r2"].value.f32v[1] == 0.0f);
    CHECK(run.vars["r2"].value.f32v[3] == 1.0f);

    // state n is after the nth step: the add in the loop changes r0, and endloop aL
    REQUIRE(run.states[6].changes.size() == 1);
    CHECK(run.states[6].changes[0].after.name == "r0");
    REQUIRE(run.states[7].changes.size() == 1);
    CHECK(run.states[7].changes[0].after.name == "aL");
    CHECK(run.states[7].changes[0].before.value.s32v[0] == 0);
    CHECK(run.states[7].changes[0].after.value.s32v[0] == 1);

    run.CheckOutputs();

    // inputs, as fetched, and their signature elements
    REQUIRE(run.trace->inputs.size() == 2);
    CHECK(run.trace->inputs[0].name == "v0");
    CHECK(run.trace->inputs[1].name == "v1");
    CHECK(run.trace->inputs[1].value.f32v[0] == 0.75f);
    const SourceVariableMapping *tc = run.SourceVar("TEXCOORD");
    REQUIRE(tc != NULL);
    CHECK(tc->signatureIndex == 1);
    REQUIRE(tc->variables.size() == 4);
    CHECK(tc->variables[0].type == DebugVariableType::Input);
    CHECK(tc->variables[0].name == "v1");

    // packed outputs: two elements in o1
    const SourceVariableMapping *tc1 = run.SourceVar("TEXCOORD1");
    REQUIRE(tc1 != NULL);
    REQUIRE(tc1->variables.size() == 2);
    CHECK(tc1->variables[0].name == "o1");
    CHECK(tc1->variables[0].component == 2);
    CHECK(tc1->variables[1].component == 3);

    // constants by register: c0-c9 for the relative reads, c10 for the def, and i0
    REQUIRE(run.trace->constantBlocks.size() == 2);
    const ShaderVariable &floats = run.trace->constantBlocks[0];
    CHECK(floats.name == "c");
    CHECK(floats.GetBindIndex().category == DescriptorCategory::ConstantBlock);
    CHECK(floats.GetBindIndex().index == 0);
    REQUIRE(floats.members.size() == 11);
    CHECK(floats.members[3].name == "[3]");
    CHECK(floats.members[3].value.f32v[1] == 6.0f);
    CHECK(floats.members[10].value.f32v[2] == 2.0f);
    const ShaderVariable &ints = run.trace->constantBlocks[1];
    CHECK(ints.name == "i");
    REQUIRE(ints.members.size() == 1);
    CHECK(ints.members[0].type == VarType::SInt);
    CHECK(ints.members[0].value.s32v[0] == 3);

    // without a constant table, by register, and the literals marked
    const SourceVariableMapping *c3 = run.SourceVar("c3");
    REQUIRE(c3 != NULL);
    CHECK(c3->type == VarType::Float);
    REQUIRE(c3->variables.size() == 4);
    CHECK(c3->variables[1].name == "c[3]");
    CHECK(c3->variables[1].component == 1);
    CHECK(run.SourceVar("c10") == NULL);
    const SourceVariableMapping *c10 = run.SourceVar("Literals.c10");
    REQUIRE(c10 != NULL);
    CHECK(c10->type == VarType::Float);
    REQUIRE(c10->variables.size() == 4);
    CHECK(c10->variables[2].type == DebugVariableType::Constant);
    CHECK(c10->variables[2].name == "c[10]");
    CHECK(c10->variables[2].component == 2);
    CHECK(run.SourceVar("Literals.i0") != NULL);
    CHECK(run.SourceVar("i0") == NULL);
    const SourceVariableMapping *block = run.SourceVar("Float constants (c#)");
    REQUIRE(block != NULL);
    CHECK(block->columns == 0);
    REQUIRE(block->variables.size() == 1);
    CHECK(block->variables[0].name == "c");

    // each instruction's disassembly line
    REQUIRE(run.trace->instInfo.size() == run.info.GetProgram().instructions.size());
    for(uint32_t i = 0; i < run.trace->instInfo.size(); i++)
    {
      CHECK(run.trace->instInfo[i].instruction == i);
      CHECK(run.trace->instInfo[i].lineInfo.disassemblyLine == run.lines[i]);
    }
  }

  SECTION("vs_1_1: the fixed output registers")
  {
    Assembler a(1, 1);
    a.dcl(DeclUsage::Position, 0, 0);
    a.dcl(DeclUsage::Color, 0, 1);
    a.def(0, 0.25f, 3.0f, 1.5f, 0.0f);
    a.op(Opcode::Mov, {DR(0), Src(RegType::Input, 0)});
    a.op(Opcode::Mov, {Dst(RegType::AddrOrTexture, 0, 0x1), C(0, Z)});
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 0), R(0)});
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 1, 0x1), C(0, X)});
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 2, 0x1), C(0, Y)});
    a.op(Opcode::Mov, {Dst(RegType::AttrOut, 0), Src(RegType::Input, 1)});
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 3), R(0, Swz(2, 1, 0, 3))});

    run.inputs[0] = Vec4(-1.0f, 0.5f, 0.25f, 1.0f);
    run.inputs[1] = Vec4(0.1f, 0.2f, 0.3f, 0.4f);
    run.Init(a.finish());
    run.Debug(100);

    CHECK(run.NextInstructions() == rdcarray<uint32_t>({3, 4, 5, 6, 7, 8, 9, 9}));
    CHECK(run.InitialNames() ==
          rdcarray<rdcstr>({"r0", "a0", "oPos", "oFog", "oPts", "oD0", "oT3"}));
    CHECK(run.vars["oFog"].columns == 1);
    CHECK(run.vars["oFog"].value.f32v[0] == 0.25f);
    CHECK(run.vars["oPts"].columns == 1);
    CHECK(run.vars["oPts"].value.f32v[0] == 3.0f);
    // vs_1_1's mov to a0 rounds down
    CHECK(run.vars["a0"].value.s32v[0] == 1);
    run.CheckOutputs();

    const SourceVariableMapping *fog = run.SourceVar("FOG");
    REQUIRE(fog != NULL);
    REQUIRE(fog->variables.size() == 1);
    CHECK(fog->variables[0].name == "oFog");
    CHECK(fog->variables[0].component == 0);
  }

  SECTION("a def between instructions isn't a step")
  {
    Assembler a(2, 0);
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 0), C(0)});
    a.def(1, 1.0f, 2.0f, 3.0f, 4.0f);
    a.op(Opcode::Mov, {DR(0), C(1)});
    run.consts.floats.resize(1);
    run.consts.floats[0] = Vec4(5.0f, 6.0f, 7.0f, 8.0f);
    run.Init(a.finish());
    run.Debug(1);

    CHECK(run.NextInstructions() == rdcarray<uint32_t>({0, 2, 2}));
    CHECK(run.vars["r0"].value.f32v[3] == 4.0f);
    run.CheckOutputs();
  }

  SECTION("a failed instruction ends the trace on it")
  {
    // l0 calls itself until the calls nest too deeply
    Assembler a(2, 0);
    a.op(Opcode::Mov, {Dst(RegType::RastOut, 0), Src(RegType::Input, 0)});
    a.op(Opcode::Call, {Src(RegType::Label, 0)});
    a.op(Opcode::Ret, {});
    a.op(Opcode::Label, {Src(RegType::Label, 0)});
    a.op(Opcode::Call, {Src(RegType::Label, 0)});
    a.op(Opcode::Ret, {});
    run.Init(a.finish());
    run.Debug(4);

    CHECK(run.debugger.GetError().contains("Calls nested too deeply at instruction 4"));
    // the initial state, the mov, the first call, then 15 more calls before the 17th fails
    CHECK(run.states.size() == 18);
    CHECK(run.states.back().nextInstruction == 4);
    // stopped inside the 16 calls that ran
    CHECK(run.states.back().callstack.size() == 17);
    CHECK(run.states.back().callstack[0] == "main");
    CHECK(run.states.back().callstack[16] == "l0");
  }

  SECTION("call stacks and flags")
  {
    Assembler a(3, 0);
    a.dcl(DeclUsage::Position, 0, 0);
    a.dcl(DeclUsage::Position, 0, 0, RegType::TexCrdOutOrOutput);
    a.dcl(DeclUsage::TexCoord, 0, 1, RegType::TexCrdOutOrOutput);
    a.op(Opcode::Dcl, {DclSampler(2), Dst(RegType::Sampler, 0)});
    a.defi(0, 2, 0, 0, 0);
    a.def(0, 0.0f, 1.0f, 0.5f, 0.0f);
    // 6
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 0), Src(RegType::Input, 0)});
    a.op(Opcode::Mov, {DR(0), C(0, X)});
    a.op(Opcode::Rep, {Src(RegType::ConstInt, 0)});
    a.op(Opcode::Call, {Src(RegType::Label, 1)});
    a.op(Opcode::EndRep, {});
    // 11: a sample, then 1 / 0 into r2.x and 0 * 0 into r2.y
    a.op(Opcode::TexLdl, {DR(1), C(0, Z), Src(RegType::Sampler, 0)});
    a.op(Opcode::Rcp, {DR(2, 0x1), C(0, X)});
    a.op(Opcode::Mul, {DR(2, 0x2), C(0, X), C(0, X)});
    a.op(Opcode::Add, {Dst(RegType::TexCrdOutOrOutput, 1), R(0), R(1)});
    a.op(Opcode::Ret, {});
    // 16: l1 calls l2
    a.op(Opcode::Label, {Src(RegType::Label, 1)});
    a.op(Opcode::Add, {DR(0), R(0), C(0, Y)});
    a.op(Opcode::Call, {Src(RegType::Label, 2)});
    a.op(Opcode::Ret, {});
    // 20
    a.op(Opcode::Label, {Src(RegType::Label, 2)});
    a.op(Opcode::Mul, {DR(0, 0x1), R(0, X), C(0, Z)});
    a.op(Opcode::Ret, {});

    run.inputs[0] = Vec4(0.5f, 0.25f, 0.0f, 1.0f);
    run.Init(a.finish());
    run.Debug(5);
    CHECK(run.debugger.GetError().empty());

    // each state's call stack is where its next instruction is
    rdcarray<rdcstr> mainOnly = {"main"}, inL1 = {"main", "l1"}, inL2 = {"main", "l1", "l2"};
    const rdcarray<Instruction> &insts = run.info.GetProgram().instructions;
    uint32_t calls = 0, inner = 0;
    for(const ShaderDebugState &s : run.states)
    {
      const uint32_t next = s.nextInstruction;
      INFO("step " << s.stepIndex << ", instruction " << next);
      if(next >= 20)
      {
        CHECK(s.callstack == inL2);
        inner++;
      }
      else if(next >= 16)
      {
        CHECK(s.callstack == inL1);
      }
      else
      {
        CHECK(s.callstack == mainOnly);
      }
      if(insts[next].op == Opcode::Call)
        calls++;

      // only the texldl samples, and only the rcp makes an infinity (0 * 0 is 0)
      const bool sample = s.stepIndex > 0 && run.states[s.stepIndex - 1].nextInstruction == 11;
      const bool inf = s.stepIndex > 0 && run.states[s.stepIndex - 1].nextInstruction == 12;
      CHECK(bool(s.flags & ShaderEvents::SampleLoadGather) == sample);
      CHECK(bool(s.flags & ShaderEvents::GeneratedNanOrInf) == inf);
    }
    // two calls from main and two from l1, and l2's two instructions run twice
    CHECK(calls == 4);
    CHECK(inner == 4);
    CHECK(run.states.back().callstack == mainOnly);
    CHECK(run.vars["r0"].value.f32v[0] == 0.75f);
    CHECK(RDCISINF(run.vars["r2"].value.f32v[0]));
    run.CheckOutputs();
  }

  SECTION("constants named from compiled constant tables")
  {
    // the tables from D3DCompile in d3d9_ctab_test_data.h, with what D3DX9_43 says each constant,
    // struct member and array element is allocated
    for(const CTABTestData::Shader &sh : CTABTestData::shaders)
    {
      if((sh.version >> 16) != 0xFFFE || ((sh.version >> 8) & 0xFF) < 2)
        continue;
      INFO(sh.name);

      // the table, a def, and reads of c90 (which isn't in any table) and the def
      Assembler a(3, 0);
      a.tokens.push_back(CommentTok(sh.ctabDWORDs + 1));
      a.tokens.push_back(MakeFourCC('C', 'T', 'A', 'B'));
      a.tokens.append(sh.ctab, sh.ctabDWORDs);
      a.dcl(DeclUsage::Position, 0, 0, RegType::TexCrdOutOrOutput);
      a.def(95, 1.0f, 2.0f, 3.0f, 4.0f);
      a.op(Opcode::Add, {Dst(RegType::TexCrdOutOrOutput, 0), C(90), C(95)});

      DebugRun one;
      one.consts.floats.resize(96);
      for(uint32_t r = 0; r < 96; r++)
        one.consts.floats[r] = Vec4(float(r), float(r) + 0.25f, float(r) + 0.5f, float(r) + 0.75f);
      one.consts.ints.resize(16);
      for(uint32_t r = 0; r < 16; r++)
        one.consts.ints[r].v[0] = int32_t(r) * 10;
      one.consts.bools.resize(16);
      for(uint32_t r = 0; r < 16; r++)
        one.consts.bools[r] = (r % 2) == 1;
      one.Init(a.finish());
      one.Debug(100);
      REQUIRE(one.info.GetConstantTable().present);

      // the constant blocks' registers, by the variable names the source variables use
      std::map<rdcstr, const ShaderVariable *> regs;
      for(const ShaderVariable &cb : one.trace->constantBlocks)
        for(const ShaderVariable &m : cb.members)
          regs[cb.name + m.name] = &m;

      uint32_t leaves = 0;
      for(size_t n = 0; n < sh.numNodes; n++)
      {
        const CTABTestData::Node &node = sh.nodes[n];
        // scalars, vectors and matrices, not structs (5), objects (4) or whole arrays
        if(node.cls >= 4 || node.elements > 1 || node.set > 2)
          continue;
        INFO(node.path);

        const SourceVariableMapping *map = one.SourceVar(node.path);
        if(node.count == 0)
        {
          // nothing allocated, so there's nothing to show
          CHECK(map == NULL);
          continue;
        }
        REQUIRE(map != NULL);
        leaves++;

        // exactly the registers D3DX says it's allocated
        static const char prefixes[] = {'b', 'i', 'c'};
        std::set<rdcstr> used, expected;
        for(uint32_t r = node.reg; r < uint32_t(node.reg + node.count); r++)
          expected.insert(StringFormat::Fmt("%c[%u]", prefixes[node.set], r));
        for(const DebugVariableReference &v : map->variables)
        {
          CHECK(v.type == DebugVariableType::Constant);
          used.insert(v.name);
          REQUIRE(regs.count(v.name) == 1);
        }
        CHECK(used == expected);
        CHECK(map->variables.size() == size_t(map->rows) * map->columns);
        CHECK(map->type == (node.set == 2   ? VarType::Float
                            : node.set == 1 ? VarType::SInt
                                            : VarType::Bool));

        // the alias for each register
        for(const rdcstr &r : expected)
        {
          const rdcstr alias =
              StringFormat::Fmt("Registers.%c%s", r[0], r.substr(2, r.size() - 3).c_str());
          CHECK(one.SourceVar(alias) != NULL);
        }
      }
      CHECK(leaves > 0);

      // c90 isn't in the table and c95 is the def
      CHECK(one.SourceVar("c90") != NULL);
      CHECK(one.SourceVar("Literals.c95") != NULL);
      CHECK(one.SourceVar("Registers.c90") == NULL);
      one.CheckOutputs();
    }

    // a few by hand: a column_major and a row_major matrix, a struct in an array, and an int and
    // bool vector
    auto component = [](DebugRun &r, const char *name, size_t i) {
      const SourceVariableMapping *map = r.SourceVar(name);
      REQUIRE(map != NULL);
      REQUIRE(i < map->variables.size());
      return StringFormat::Fmt("%s.%u", map->variables[i].name.c_str(),
                               (uint32_t)map->variables[i].component);
    };
    for(const CTABTestData::Shader &sh : CTABTestData::shaders)
    {
      const rdcstr name = sh.name;
      if(name != "floats_vs3" && name != "intbool_vs3" && name != "partial_vs3")
        continue;
      Assembler a(3, 0);
      a.tokens.push_back(CommentTok(sh.ctabDWORDs + 1));
      a.tokens.push_back(MakeFourCC('C', 'T', 'A', 'B'));
      a.tokens.append(sh.ctab, sh.ctabDWORDs);
      a.dcl(DeclUsage::Position, 0, 0, RegType::TexCrdOutOrOutput);
      a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 0), C(0)});
      DebugRun one;
      one.Init(a.finish());
      one.Debug(100);

      if(name == "floats_vs3")
      {
        // m44 is column_major at c13: row 1, column 2 is c15.y
        const SourceVariableMapping *m44 = one.SourceVar("m44");
        REQUIRE(m44 != NULL);
        CHECK(m44->rows == 4);
        CHECK(m44->columns == 4);
        CHECK(component(one, "m44", 1 * 4 + 2) == "c[15].1");
        // rm44 is row_major at c17: row 1, column 2 is c18.z
        CHECK(component(one, "rm44", 1 * 4 + 2) == "c[18].2");
        // m23, a column_major float2x3 at c32: row 1, column 2 is c34.y
        CHECK(component(one, "m23", 1 * 3 + 2) == "c[34].1");
        // sarr[1].c, a float2x2 in the second element of a struct array at c4
        CHECK(component(one, "sarr[1].c", 1 * 2 + 0) == "c[6].1");
        CHECK(component(one, "o.i[1].q", 0) == "c[11].0");
        CHECK(one.SourceVar("sarr") == NULL);
      }
      else if(name == "intbool_vs3")
      {
        // bones is float4x3[4], column_major: three registers each
        const SourceVariableMapping *bone = one.SourceVar("bones[1]");
        REQUIRE(bone != NULL);
        CHECK(bone->rows == 4);
        CHECK(bone->columns == 3);
        CHECK(component(one, "bones[1]", 3 * 3 + 1) == "c[4].3");
        // rbones is row_major float4x3[2]: four registers each
        CHECK(component(one, "rbones[1]", 3 * 3 + 1) == "c[19].1");
        // a bool2 takes a boolean register per component
        CHECK(component(one, "bb", 1) == "b[1].0");
        CHECK(component(one, "count", 0) == "i[0].0");
        CHECK(one.SourceVar("count")->type == VarType::SInt);
      }
      else
      {
        // mpart is a float4x4 the compiler gave one register, its first column
        const SourceVariableMapping *mpart = one.SourceVar("mpart");
        REQUIRE(mpart != NULL);
        CHECK(mpart->rows == 4);
        CHECK(mpart->columns == 1);
        CHECK(component(one, "mpart", 2) == "c[3].2");
        // tpart.b and part[2] weren't given a register
        CHECK(one.SourceVar("tpart.a") != NULL);
        CHECK(one.SourceVar("tpart.b") == NULL);
        CHECK(one.SourceVar("part[1]") != NULL);
        CHECK(one.SourceVar("part[2]") == NULL);
      }
    }
  }
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
