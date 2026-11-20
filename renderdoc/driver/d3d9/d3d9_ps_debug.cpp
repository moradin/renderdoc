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

#include "d3d9_ps_debug.h"
#include <map>
#include <set>
#include "common/formatting.h"
#include "d3d9_bytecode_build.h"

// What the pixel trace has, besides what the vertex trace has (d3d9_vs_debug.cpp):
// - inputs: the input registers the input signature uses, as the debugged pixel got them: v#, t#
//   (ps_2_0 and later's and ps_1_4's texture coordinates), vPos (x and y) and vFace (one
//   component). ps_1_1 to ps_1_3's texture coordinates are texcoord#, since their t# are the
//   registers the texture instructions write. A source variable per signature element.
// - the mutable registers: r#, ps_1_1 to ps_1_3's t#, aL, p0, and the outputs oC# and oDepth
//   (one component). ps_1_x's result is r0, copied to oC0 when the shader ends, which is the
//   last state's change.
// - a source variable per output signature element (COLOR#, DEPTH), on oC# or oDepth.
//
// The debugged pixel runs in a quad (PSQuad) with the neighbours QuadInputs extrapolated, so dsx,
// dsy and the derivatives of lookups are the GPU's. Only the debugged pixel's registers are in the
// states.

namespace D3D9Emu
{
using namespace D3D9BC;
using namespace D3D9BCBuild;

namespace
{
uint32_t RegKey(RegType type, uint32_t index)
{
  return (uint32_t(type) << 16) | index;
}

// the input field a readback item's register is read into
Vec4 *InputField(PSInputs &in, RegType type, uint32_t index)
{
  if(type == RegType::Input && index < MaxPSInputs)
    return &in.v[index];
  if(type == RegType::AddrOrTexture && index < MaxPSTexCoords)
    return &in.t[index];
  return NULL;
}
};    // namespace

bool BuildInputReadback(const Program &prog, bool gradients, uint32_t maxTargets,
                        rdcarray<ReadbackPass> &passes, rdcstr &error)
{
  passes.clear();

  const Version &ver = prog.version;
  if(!prog.validVersion || !ver.IsPS())
  {
    error = "it isn't a pixel shader";
    return false;
  }
  if(!prog.error.empty())
  {
    error = "the shader didn't parse: " + prog.error;
    return false;
  }

  maxTargets = RDCCLAMP(maxTargets, 1U, 4U);
  const bool sm3 = ver.major >= 3;

  // the input declarations, as tokens, and the registers read with their components
  rdcarray<uint32_t> decls;
  std::map<uint32_t, uint8_t> regs;
  bool face = false;

  if(ver.IsSM1())
  {
    // ps_1_x declares nothing: the colours and coordinates it reads, whole. ps_1_1 to ps_1_3's
    // texture instructions read the coordinate set their destination names (texreg2* don't, but
    // reading it too does no harm); ps_1_4 reads them as sources.
    const bool texRegisters = !ver.AtLeast(1, 4);
    for(const Instruction &inst : prog.instructions)
    {
      for(const SrcParam &src : inst.srcs)
      {
        if(src.type == RegType::Input && src.index < 2)
          regs[RegKey(RegType::Input, src.index)] = 0xF;
        else if(!texRegisters && src.type == RegType::AddrOrTexture && src.index < MaxPSTexCoords)
          regs[RegKey(RegType::AddrOrTexture, src.index)] = 0xF;
      }
      if(texRegisters && inst.hasDst && inst.dst.type == RegType::AddrOrTexture &&
         inst.dst.index < MaxPSTexCoords)
        regs[RegKey(RegType::AddrOrTexture, inst.dst.index)] = 0xF;
    }

    for(auto it = regs.begin(); it != regs.end(); ++it)
    {
      decls.push_back(Inst(Opcode::Dcl, 0, 2));
      decls.push_back(DclNone());
      decls.push_back(Dst(RegType(it->first >> 16), it->first & 0xFFFF, 0xF));
    }
  }
  else
  {
    for(const Instruction &inst : prog.instructions)
    {
      if(inst.op != Opcode::Dcl || !inst.hasDst)
        continue;

      const RegType type = inst.dst.type;
      const bool isFace = sm3 && type == RegType::MiscType && inst.dst.index == 1;
      const bool isInput = type == RegType::Input || (!sm3 && type == RegType::AddrOrTexture);
      if(!isFace && !isInput)
        continue;

      if(size_t(inst.offset) + 1 + inst.length > prog.tokens.size())
      {
        error = "a declaration runs past the end of the shader";
        return false;
      }

      // as the shader declares it: semantic, mask, centroid
      for(uint32_t t = 0; t <= inst.length; t++)
        decls.push_back(prog.tokens[inst.offset + t]);

      if(isFace)
        face = true;
      else
        regs[RegKey(type, inst.dst.index)] |= inst.dst.writeMask;
    }
  }

  const bool derivatives = !sm3 && gradients;

  rdcarray<ReadbackItem> items;
  for(auto it = regs.begin(); it != regs.end(); ++it)
  {
    ReadbackItem item;
    item.type = RegType(it->first >> 16);
    item.index = it->first & 0xFFFF;
    item.mask = it->second;
    item.what = ReadbackValue::Value;
    items.push_back(item);
    if(sm3)
    {
      for(ReadbackValue what :
          {ReadbackValue::TopLeft, ReadbackValue::TopRight, ReadbackValue::BottomLeft})
      {
        item.what = what;
        items.push_back(item);
      }
    }
    else if(derivatives)
    {
      item.what = ReadbackValue::Ddx;
      items.push_back(item);
      item.what = ReadbackValue::Ddy;
      items.push_back(item);
    }
  }
  if(face)
  {
    ReadbackItem item;
    item.what = ReadbackValue::Face;
    item.type = RegType::MiscType;
    item.index = 1;
    item.mask = 0x1;
    items.push_back(item);
  }

  // a shader that reads no inputs still gets a pass, writing 0 to oC0, which shows whether the
  // draw covers the pixel
  for(size_t first = 0; first < items.size() || (first == 0 && items.empty()); first += maxTargets)
  {
    ReadbackPass pass;

    // ps_2_x for dsx and dsy (ps_2_0 has neither)
    Assembler a(sm3 ? 3 : 2, sm3 ? 0 : (derivatives ? 1 : 0), true);
    a.def(0, 0.0f, 0.0f, 0.0f, 0.0f);
    a.def(1, 1.0f, -1.0f, 0.5f, 0.0f);
    a.tokens.append(decls);

    // ps_3_0 picks out the quad's pixels by their position: r4 is (x & 1, y & 1), from vPos, which
    // is the pixel's integer position
    if(sm3)
    {
      a.op(Opcode::Dcl, {DclNone(), Dst(RegType::MiscType, 0, 0x3)});
      a.op(Opcode::Mul, {Dst(RegType::Temp, 4, 0x3), Src(RegType::MiscType, 0),
                         Src(RegType::Const, 1, Swz(2, 2, 2, 2))});
      a.op(Opcode::Frc, {Dst(RegType::Temp, 4, 0x3), Src(RegType::Temp, 4)});
      a.op(Opcode::Add, {Dst(RegType::Temp, 4, 0x3), Src(RegType::Temp, 4), Src(RegType::Temp, 4)});
    }

    const uint32_t notRight = Src(RegType::Temp, 4, Swz(0, 0, 0, 0), SrcMod::Neg);
    const uint32_t notBottom = Src(RegType::Temp, 4, Swz(1, 1, 1, 1), SrcMod::Neg);
    const uint32_t zero = Src(RegType::Const, 0);

    for(uint32_t k = 0; k < maxTargets && first + k < items.size(); k++)
    {
      const ReadbackItem &item = items[first + k];
      pass.items.push_back(item);

      // the components not declared are 0
      a.op(Opcode::Mov, {Dst(RegType::Temp, k), zero});
      const uint32_t dst = Dst(RegType::Temp, k, item.mask);
      const uint32_t picked = Dst(RegType::Temp, 5, item.mask);
      const uint32_t src = Src(item.type, item.index);
      switch(item.what)
      {
        case ReadbackValue::Value: a.op(Opcode::Mov, {dst, src}); break;
        case ReadbackValue::Ddx: a.op(Opcode::Dsx, {dst, src}); break;
        case ReadbackValue::Ddy: a.op(Opcode::Dsy, {dst, src}); break;
        // dsx is the top row's difference and dsy the left column's, so with the input in one
        // pixel and 0 in the other they give that pixel's value exactly. cmp takes -0 as >= 0.
        case ReadbackValue::TopLeft:
          // -0 on the right and -input on the left: -0 - (-input) is the input, its zeros' signs
          // included
          a.op(Opcode::Cmp, {picked, notRight, Src(item.type, item.index, 0xE4, SrcMod::Neg),
                             Src(RegType::Const, 0, 0xE4, SrcMod::Neg)});
          a.op(Opcode::Dsx, {dst, Src(RegType::Temp, 5)});
          break;
        case ReadbackValue::TopRight:
          a.op(Opcode::Cmp, {picked, notRight, zero, src});
          a.op(Opcode::Dsx, {dst, Src(RegType::Temp, 5)});
          break;
        case ReadbackValue::BottomLeft:
          a.op(Opcode::Cmp, {picked, notBottom, zero, src});
          a.op(Opcode::Dsy, {dst, Src(RegType::Temp, 5)});
          break;
        case ReadbackValue::Face:
          // vFace can only be a condition: +1 where it's >= 0, -1 otherwise
          a.op(Opcode::Cmp,
               {Dst(RegType::Temp, k, 0x1), Src(RegType::MiscType, 1),
                Src(RegType::Const, 1, Swz(0, 0, 0, 0)), Src(RegType::Const, 1, Swz(1, 1, 1, 1))});
          break;
      }
      a.op(Opcode::Mov, {Dst(RegType::ColorOut, k), Src(RegType::Temp, k)});
    }

    if(items.empty())
      a.op(Opcode::Mov, {Dst(RegType::ColorOut, 0), Src(RegType::Const, 0)});

    pass.tokens = a.finish();
    passes.push_back(pass);
  }

  return true;
}

void QuadInputs(const rdcarray<ReadbackPass> &passes, const rdcarray<rdcarray<Vec4>> &results,
                const rdcarray<rdcarray<Vec4>> *corners, uint32_t x, uint32_t y,
                PSInputs quad[QuadPixels])
{
  const int32_t qx = int32_t(x & 1), qy = int32_t(y & 1);

  for(uint32_t i = 0; i < QuadPixels; i++)
  {
    quad[i] = PSInputs();
    quad[i].pos =
        Vec4(float(x - uint32_t(qx) + (i & 1)), float(y - uint32_t(qy) + (i >> 1)), 0.0f, 0.0f);
  }

  struct Values
  {
    Vec4 value, ddx, ddy;
    // ps_3_0's: the quad's top left, top right and bottom left pixels' values
    Vec4 corner[3];
    bool corners = false;
  };
  std::map<uint32_t, Values> regs;
  // ps_2_x's readback at the quad's top left, top right and bottom left pixels
  std::map<uint32_t, Values> cornerRegs[3];

  auto gather = [&passes](const rdcarray<rdcarray<Vec4>> &res, std::map<uint32_t, Values> &out,
                          PSInputs *faces) {
    for(size_t p = 0; p < passes.size() && p < res.size(); p++)
    {
      for(size_t k = 0; k < passes[p].items.size() && k < res[p].size(); k++)
      {
        const ReadbackItem &item = passes[p].items[k];
        const Vec4 &v = res[p][k];
        if(item.what == ReadbackValue::Face)
        {
          if(faces)
            for(uint32_t i = 0; i < QuadPixels; i++)
              faces[i].face = v[0] < 0.0f ? -1.0f : 1.0f;
          continue;
        }

        Values &vals = out[RegKey(item.type, item.index)];
        switch(item.what)
        {
          case ReadbackValue::Value: vals.value = v; break;
          case ReadbackValue::Ddx: vals.ddx = v; break;
          case ReadbackValue::Ddy: vals.ddy = v; break;
          case ReadbackValue::TopLeft:
          case ReadbackValue::TopRight:
          case ReadbackValue::BottomLeft:
            vals.corner[uint32_t(item.what) - uint32_t(ReadbackValue::TopLeft)] = v;
            vals.corners = true;
            break;
          case ReadbackValue::Face: break;
        }
      }
    }
  };

  gather(results, regs, quad);
  for(uint32_t c = 0; corners && c < 3; c++)
    gather(corners[c], cornerRegs[c], NULL);

  const uint32_t me = QuadPixelOf(x, y);

  for(auto it = regs.begin(); it != regs.end(); ++it)
  {
    const RegType type = RegType(it->first >> 16);
    const uint32_t index = it->first & 0xFFFF;
    const Values &vals = it->second;

    Vec4 values[QuadPixels];
    if(vals.corners)
    {
      // as the GPU had them. The bottom right pixel isn't in any derivative.
      for(uint32_t i = 0; i < 3; i++)
        values[i] = vals.corner[i];
      for(int c = 0; c < 4; c++)
        values[QuadBottomRight][c] =
            values[QuadTopRight][c] + (values[QuadBottomLeft][c] - values[QuadTopLeft][c]);
    }
    else
    {
      // The top left a derivative or two from the pixel, and the others a derivative from that,
      // as the GPU's derivatives are its differences. When the pixel is in the top row or the
      // left column that gets them back exactly as long as the derivatives were (subtracting
      // values within a factor of 2 of each other is). From the bottom right the top left is a
      // little off with perspective, which isn't linear across the quad.
      Vec4 topLeft = vals.value;
      for(int c = 0; c < 4; c++)
      {
        if(qx)
          topLeft[c] = topLeft[c] - vals.ddx[c];
        if(qy)
          topLeft[c] = topLeft[c] - vals.ddy[c];
      }
      values[QuadTopLeft] = topLeft;
      for(int c = 0; c < 4; c++)
      {
        values[QuadTopRight][c] = topLeft[c] + vals.ddx[c];
        values[QuadBottomLeft][c] = topLeft[c] + vals.ddy[c];
        values[QuadBottomRight][c] = values[QuadTopRight][c] + vals.ddy[c];
      }

      // a neighbour the same primitive covered, read there: its derivatives are the same quad's
      for(uint32_t c = 0; c < 3; c++)
      {
        auto corner = cornerRegs[c].find(it->first);
        if(corner == cornerRegs[c].end())
          continue;
        const Values &there = corner->second;
        if(memcmp(&there.ddx, &vals.ddx, sizeof(Vec4)) == 0 &&
           memcmp(&there.ddy, &vals.ddy, sizeof(Vec4)) == 0)
          values[c] = there.value;
      }
    }
    // the pixel's own as read
    values[me] = vals.value;

    for(uint32_t i = 0; i < QuadPixels; i++)
    {
      Vec4 *field = InputField(quad[i], type, index);
      if(field)
        *field = values[i];
    }
  }
}

bool PSInstructionSamples(const Instruction &inst)
{
  switch(inst.op)
  {
    // texld, texldp and texldb in ps_2_0 and later, tex and texld in ps_1_x
    case Opcode::Tex:
    case Opcode::TexLdd:
    case Opcode::TexLdl:
    case Opcode::TexBem:
    case Opcode::TexBemL:
    case Opcode::TexReg2AR:
    case Opcode::TexReg2GB:
    case Opcode::TexReg2RGB:
    case Opcode::TexM3x2Tex:
    case Opcode::TexM3x3Tex:
    case Opcode::TexM3x3Spec:
    case Opcode::TexM3x3VSpec:
    case Opcode::TexDP3Tex: return true;
    default: return false;
  }
}

bool PixelShaderDebuggable(const Program &prog, rdcstr &status)
{
  PSEmulator emu;
  if(!emu.Init(prog))
  {
    status = "The pixel shader can't be emulated: " + emu.GetError();
    return false;
  }
  if(prog.version.IsSM1())
  {
    status = "Debugging ps_1_x pixel shaders isn't supported yet";
    return false;
  }
  for(const Instruction &inst : prog.instructions)
  {
    if(PSInstructionSamples(inst))
    {
      status = "Debugging pixel shaders that sample textures isn't supported yet";
      return false;
    }
  }
  status.clear();
  return true;
}

rdcstr PSDebugger::InputName(RegType type, uint32_t index) const
{
  if(type == RegType::AddrOrTexture && m_TexRegisters)
    return StringFormat::Fmt("texcoord%u", index);
  return RegisterName(m_Version, type, index);
}

ShaderVariable PSDebugger::Variable(RegType type, uint32_t index, const Vec4 &f, const IVec4 &i) const
{
  const rdcstr name = RegisterName(m_Version, type, index);
  switch(type)
  {
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
    case RegType::DepthOut:
    {
      ShaderVariable ret(name, f[0], 0.0f, 0.0f, 0.0f);
      ret.columns = 1;
      return ret;
    }
    default: break;
  }
  return ShaderVariable(name, f[0], f[1], f[2], f[3]);
}

ShaderVariable PSDebugger::Current(RegType type, uint32_t index) const
{
  const PSRegisters &regs = Thread().Registers();
  Vec4 f;
  IVec4 i;
  switch(type)
  {
    case RegType::Temp:
      if(index < MaxPSTemps)
        f = regs.r[index];
      break;
    case RegType::AddrOrTexture:
      if(index < MaxPSTexCoords)
        f = regs.t[index];
      break;
    case RegType::Predicate:
      for(int c = 0; c < 4; c++)
        i.v[c] = regs.p[c] ? 1 : 0;
      break;
    case RegType::Loop: i.v[0] = regs.aL(); break;
    case RegType::ColorOut:
      if(index < MaxPSColorOutputs)
        f = regs.out.color[index];
      break;
    case RegType::DepthOut: f = regs.out.depth; break;
    default: break;
  }
  return Variable(type, index, f, i);
}

ShaderDebugTrace *PSDebugger::Begin(const PSEmulator &emu, const ShaderInfo &info,
                                    const rdcarray<uint32_t> &instLines,
                                    const PSInputs inputs[QuadPixels], uint32_t pixel,
                                    const VSConstants &consts, PSCallbacks *callbacks,
                                    const PSStageState *stages)
{
  ShaderDebugTrace *ret = new ShaderDebugTrace;
  ret->stage = ShaderStage::Pixel;
  ret->debugger = this;

  m_Pixel = pixel & 3;
  Reset(emu.GetProgram());
  m_TexRegisters = m_Version.IsSM1() && !m_Version.AtLeast(1, 4);
  m_Quad.Start(emu, inputs, consts, callbacks, stages);

  const Program &prog = emu.GetProgram();
  const ShaderReflection &refl = info.GetReflection();
  const PSInputs &in = inputs[m_Pixel];

  //////////////////////////////////////////////////////////////////////////
  // inputs

  // the register a signature element is in (see the reflection's signatures)
  auto inputRegister = [this](const SigParameter &sig, RegType &type, uint32_t &index) {
    index = sig.regIndex;
    if(m_Version.major >= 3)
    {
      if(sig.semanticName == "VPOS" || sig.semanticName == "VFACE")
      {
        type = RegType::MiscType;
        index = sig.semanticName == "VPOS" ? 0 : 1;
        return true;
      }
      type = RegType::Input;
      return index < MaxPSInputs;
    }
    if(sig.semanticName == "TEXCOORD")
    {
      type = RegType::AddrOrTexture;
      return index < MaxPSTexCoords;
    }
    type = RegType::Input;
    return index < MaxPSInputs;
  };

  std::set<uint32_t> inputRegs;
  for(const SigParameter &sig : refl.inputSignature)
  {
    RegType type;
    uint32_t index;
    if(inputRegister(sig, type, index))
      inputRegs.insert(RegKey(type, index));
  }

  for(uint32_t key : inputRegs)
  {
    const RegType type = RegType(key >> 16);
    const uint32_t index = key & 0xFFFF;
    const rdcstr name = InputName(type, index);
    if(type == RegType::MiscType)
    {
      ShaderVariable var = index == 0 ? ShaderVariable(name, in.pos[0], in.pos[1], 0.0f, 0.0f)
                                      : ShaderVariable(name, in.face, 0.0f, 0.0f, 0.0f);
      var.columns = index == 0 ? 2 : 1;
      ret->inputs.push_back(var);
      continue;
    }
    const Vec4 &v = type == RegType::Input ? in.v[index] : in.t[index];
    ret->inputs.push_back(ShaderVariable(name, v[0], v[1], v[2], v[3]));
  }

  for(int32_t s = 0; s < refl.inputSignature.count(); s++)
  {
    const SigParameter &sig = refl.inputSignature[s];
    RegType type;
    uint32_t index;
    if(!inputRegister(sig, type, index))
      continue;
    SourceVariableMapping map = DebugMapping(sig.semanticIdxName, VarType::Float, sig.compCount, s);
    const rdcstr reg = InputName(type, index);
    for(uint32_t c = 0; c < 4; c++)
      if(sig.regChannelMask & (1 << c))
        map.variables.push_back(DebugVariableReference(DebugVariableType::Input, reg, c));
    ret->sourceVars.push_back(map);
  }

  //////////////////////////////////////////////////////////////////////////
  // the registers the shader can change

  std::set<uint32_t> temps, texRegs, outputs;
  bool aL = false, p0 = false;

  auto use = [&](RegType type, uint32_t index, const RelativeAddress &rel) {
    if(type == RegType::Temp && index < MaxPSTemps)
      temps.insert(index);
    else if(type == RegType::AddrOrTexture && m_TexRegisters && index < MaxPSTexCoords)
      texRegs.insert(index);
    else if(type == RegType::Loop)
      aL = true;
    else if(type == RegType::Predicate)
      p0 = true;
    else if(type == RegType::ColorOut || type == RegType::DepthOut)
      outputs.insert(RegKey(type, index));

    if(rel.present && rel.type == RegType::Loop)
      aL = true;
  };

  for(const Instruction &inst : prog.instructions)
  {
    if(inst.op == Opcode::Loop)
      aL = true;
    if(IsDeclaration(inst.op))
      continue;
    if(inst.hasDst)
      use(inst.dst.type, inst.dst.index, inst.dst.rel);
    if(inst.predicated)
      use(inst.predicate.type, inst.predicate.index, inst.predicate.rel);
    for(const SrcParam &src : inst.srcs)
      use(src.type, src.index, src.rel);
  }

  // ps_1_x's r0 is copied to oC0 at the end
  if(m_Version.IsSM1())
    outputs.insert(RegKey(RegType::ColorOut, 0));

  // the register an output signature element is in
  auto outputRegister = [](const SigParameter &sig, RegType &type, uint32_t &index) {
    if(sig.semanticName == "DEPTH")
    {
      type = RegType::DepthOut;
      index = 0;
      return true;
    }
    type = RegType::ColorOut;
    index = sig.regIndex;
    return index < MaxPSColorOutputs;
  };

  for(const SigParameter &sig : refl.outputSignature)
  {
    RegType type;
    uint32_t index;
    if(outputRegister(sig, type, index))
      outputs.insert(RegKey(type, index));
  }

  for(uint32_t t : temps)
    AddRegister(RegType::Temp, t);
  for(uint32_t t : texRegs)
    AddRegister(RegType::AddrOrTexture, t);
  if(aL)
    AddRegister(RegType::Loop, 0);
  if(p0)
    AddRegister(RegType::Predicate, 0);
  // oC# sort before oDepth
  for(uint32_t key : outputs)
    AddRegister(RegType(key >> 16), key & 0xFFFF);

  //////////////////////////////////////////////////////////////////////////
  // outputs

  for(int32_t s = 0; s < refl.outputSignature.count(); s++)
  {
    const SigParameter &sig = refl.outputSignature[s];
    RegType type;
    uint32_t index;
    if(!outputRegister(sig, type, index))
      continue;
    SourceVariableMapping map = DebugMapping(sig.semanticIdxName, VarType::Float, sig.compCount, s);
    const rdcstr reg = RegisterName(m_Version, type, index);
    for(uint32_t c = 0; c < 4; c++)
      if(sig.regChannelMask & (1 << c))
        map.variables.push_back(DebugVariableReference(DebugVariableType::Variable, reg, c));
    ret->sourceVars.push_back(map);
  }

  DescribeConstants(ret, info, consts);
  DescribeInputRegisters(ret);
  DescribeSamplers(ret, refl);
  DescribeInstructions(ret, instLines);

  return ret;
}

};    // namespace D3D9Emu

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"
#include "d3d9_shader_reflect.h"

using namespace D3D9BCBuild;
using namespace D3D9Emu;

namespace
{
// a shader run over a quad, the reference the readback and the debugger are checked against
struct QuadRun
{
  Program prog;
  PSEmulator emu;
  VSConstants consts;
  PSQuad quad;

  bool Run(const rdcarray<uint32_t> &tokens, const PSInputs inputs[QuadPixels])
  {
    ParseProgram(tokens.data(), tokens.size(), prog);
    if(!prog.error.empty() || !emu.Init(prog))
      return false;
    consts = emu.ApplyLiterals(VSConstants());
    quad.Start(emu, inputs, consts, NULL);
    return quad.Run();
  }
};

// the inputs of the quad whose top left is (x, y), linear in the position with exact values, so
// the neighbours worked out from derivatives are exact too (and none is -0, which a neighbour
// worked out to be 0 isn't)
void AffineQuad(uint32_t x, uint32_t y, PSInputs quad[QuadPixels], float face = 1.0f)
{
  for(uint32_t i = 0; i < QuadPixels; i++)
  {
    const float px = float(x + (i & 1)), py = float(y + (i >> 1));
    PSInputs &in = quad[i];
    in = PSInputs();
    in.pos = Vec4(px, py, 0.0f, 0.0f);
    in.face = face;
    in.v[0] = Vec4(0.25f * px, 0.5f * py, px + py, 1.0f);
    in.v[1] = Vec4(1.0f - 0.125f * px, 3.0f, 0.75f * py - px + 2.0f, 0.5f);
    in.t[0] = Vec4(2.0f * px - py, 0.125f * py, 7.0f, 9.0f);
    in.t[2] = Vec4(0.5f * px, -py, 3.0f, px - 4.0f * py);
  }
}

// the readback passes run on the quad, read at one of its pixels
rdcarray<rdcarray<Vec4>> RunReadback(const rdcarray<ReadbackPass> &passes,
                                     const PSInputs truth[QuadPixels], uint32_t pixel)
{
  rdcarray<rdcarray<Vec4>> results;
  for(const ReadbackPass &pass : passes)
  {
    QuadRun run;
    REQUIRE(run.Run(pass.tokens, truth));
    const PSOutputs &out = run.quad.Pixel(pixel).Registers().out;
    CHECK(out.written == (1U << pass.items.size()) - 1);
    rdcarray<Vec4> targets;
    for(size_t k = 0; k < pass.items.size(); k++)
      targets.push_back(out.color[k]);
    results.push_back(targets);
  }
  return results;
}

// runs the readback passes on the quad, reads them at pixel (x, y) and builds its quad again. With
// corners, they're read at the quad's top left, top right and bottom left pixels too, as ps_2_x's
// are, from cornerTruth (truth if NULL) as if another primitive covered them.
void ReadBack(const rdcarray<ReadbackPass> &passes, const PSInputs truth[QuadPixels], uint32_t x,
              uint32_t y, PSInputs rebuilt[QuadPixels], bool corners = false,
              const PSInputs *cornerTruth = NULL)
{
  const rdcarray<rdcarray<Vec4>> results = RunReadback(passes, truth, QuadPixelOf(x, y));
  rdcarray<rdcarray<Vec4>> cornerResults[3];
  if(corners)
  {
    for(uint32_t c = 0; c < 3; c++)
      if(c != QuadPixelOf(x, y))
        cornerResults[c] = RunReadback(passes, cornerTruth ? cornerTruth : truth, c);
  }
  QuadInputs(passes, results, corners ? cornerResults : NULL, x, y, rebuilt);
}

bool SameVec(const Vec4 &a, const Vec4 &b)
{
  return memcmp(&a, &b, sizeof(Vec4)) == 0;
}

Vec4 Masked(Vec4 v, uint8_t mask)
{
  for(int c = 0; c < 4; c++)
    if(!(mask & (1 << c)))
      v[c] = 0.0f;
  return v;
}

// a pixel debugged to the end, checking each change's value before is what the variable had, and
// that going back from the end undoes everything
struct PSDebugRun
{
  ShaderInfo info;
  PSEmulator emu;
  VSConstants consts;
  rdcarray<uint32_t> lines;

  PSDebugger debugger;
  ShaderDebugTrace *trace = NULL;
  rdcarray<ShaderDebugState> states;
  std::map<rdcstr, ShaderVariable> vars;

  ~PSDebugRun() { delete trace; }

  void Debug(const rdcarray<uint32_t> &tokens, const PSInputs inputs[QuadPixels], uint32_t pixel)
  {
    info.Init(ResourceId(), tokens.data(), tokens.size());
    REQUIRE(emu.Init(info.GetProgram()));
    consts = emu.ApplyLiterals(consts);
    info.GetDisassembly(&lines);

    trace = debugger.Begin(emu, info, lines, inputs, pixel, consts, NULL, NULL);
    REQUIRE(trace->debugger == &debugger);
    CHECK(trace->stage == ShaderStage::Pixel);

    for(;;)
    {
      rdcarray<ShaderDebugState> next = debugger.Continue(3);
      if(next.empty())
        break;
      states.append(next);
    }
    CHECK(debugger.GetError().empty());

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
          const bool found = it != vars.end();
          REQUIRE(found);
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

  Vec4 Var(const rdcstr &name)
  {
    REQUIRE(vars.count(name) == 1);
    const ShaderVariable &v = vars[name];
    return Vec4(v.value.f32v[0], v.value.f32v[1], v.value.f32v[2], v.value.f32v[3]);
  }

  rdcarray<rdcstr> InitialNames() const
  {
    rdcarray<rdcstr> ret;
    if(!states.empty())
      for(const ShaderVariableChange &c : states[0].changes)
        ret.push_back(c.after.name);
    return ret;
  }

  const ShaderVariable *Input(const rdcstr &name) const
  {
    for(const ShaderVariable &v : trace->inputs)
      if(v.name == name)
        return &v;
    return NULL;
  }

  const SourceVariableMapping *SourceVar(const rdcstr &name) const
  {
    for(const SourceVariableMapping &m : trace->sourceVars)
      if(m.name == name)
        return &m;
    return NULL;
  }
};
};    // namespace

TEST_CASE("D3D9 pixel shader debugging - input readback", "[d3d9][psemu][psdebug]")
{
  SECTION("ps_2_0: each declared input and its derivatives")
  {
    Assembler a(2, 0, true);
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::AddrOrTexture, 0, 0x3)});
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::Input, 0)});
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::AddrOrTexture, 2)});
    a.op(Opcode::Mov, {Dst(RegType::ColorOut, 0), Src(RegType::Input, 0)});
    Program prog;
    const rdcarray<uint32_t> tokens = a.finish();
    ParseProgram(tokens.data(), tokens.size(), prog);

    rdcarray<ReadbackPass> passes;
    rdcstr error;
    REQUIRE(BuildInputReadback(prog, true, 4, passes, error));

    // v0, t0 and t2 with their derivatives, four a pass, in ps_2_x for dsx and dsy
    REQUIRE(passes.size() == 3);
    CHECK(passes[0].items.size() == 4);
    CHECK(passes[1].items.size() == 4);
    CHECK(passes[2].items.size() == 1);
    CHECK(passes[0].tokens[0] == VersionToken(true, 2, 1));
    CHECK((passes[0].items[0].type == RegType::Input && passes[0].items[0].index == 0));
    CHECK((passes[0].items[1].what == ReadbackValue::Ddx));
    CHECK((passes[0].items[2].what == ReadbackValue::Ddy));
    CHECK((passes[0].items[3].type == RegType::AddrOrTexture && passes[0].items[3].index == 0));
    CHECK(passes[0].items[3].mask == 0x3);
    CHECK((passes[2].items[0].type == RegType::AddrOrTexture && passes[2].items[0].index == 2 &&
           passes[2].items[0].what == ReadbackValue::Ddy));

    // read at each pixel of a quad, the quad is built again exactly: the pixel's values as they
    // were, the neighbours' from the derivatives (exact here, since the inputs are linear), the
    // undeclared components 0
    PSInputs truth[QuadPixels];
    AffineQuad(10, 20, truth);
    for(uint32_t p = 0; p < QuadPixels; p++)
    {
      const uint32_t x = 10 + (p & 1), y = 20 + (p >> 1);
      INFO("pixel " << x << ", " << y);
      CHECK(QuadPixelOf(x, y) == p);
      PSInputs rebuilt[QuadPixels];
      ReadBack(passes, truth, x, y, rebuilt);
      for(uint32_t i = 0; i < QuadPixels; i++)
      {
        INFO("quad pixel " << i);
        CHECK(SameVec(rebuilt[i].v[0], truth[i].v[0]));
        CHECK(SameVec(rebuilt[i].t[0], Masked(truth[i].t[0], 0x3)));
        CHECK(SameVec(rebuilt[i].t[2], truth[i].t[2]));
        CHECK(SameVec(rebuilt[i].pos, truth[i].pos));
        // not declared
        CHECK(SameVec(rebuilt[i].v[1], Vec4()));
      }
    }

    // a quad elsewhere: the positions are the pixel's quad's
    PSInputs rebuilt[QuadPixels];
    AffineQuad(6, 2, truth);
    ReadBack(passes, truth, 7, 3, rebuilt);
    CHECK(SameVec(rebuilt[0].pos, Vec4(6.0f, 2.0f, 0.0f, 0.0f)));
    CHECK(SameVec(rebuilt[3].pos, Vec4(7.0f, 3.0f, 0.0f, 0.0f)));
    CHECK(SameVec(rebuilt[0].t[2], truth[0].t[2]));

    // ps_2_x's derivatives give the quad back exactly from the top row and left column when
    // the inputs aren't linear, but not from the bottom right
    for(uint32_t i = 0; i < QuadPixels; i++)
      truth[i].t[2][3] = float(i * i) * 0.375f + 1.0f;
    for(uint32_t p = 0; p < QuadPixels; p++)
    {
      INFO("pixel " << p);
      PSInputs quad[QuadPixels];
      ReadBack(passes, truth, 6 + (p & 1), 2 + (p >> 1), quad);
      for(uint32_t i = 0; i < QuadPixels; i++)
      {
        INFO("quad pixel " << i);
        const bool exact = (p != QuadBottomRight && i != QuadBottomRight) || i == p;
        CHECK(SameVec(quad[i].t[2], truth[i].t[2]) == exact);
      }

      // read at the neighbours too, which the same primitive covers, they're exact from anywhere
      ReadBack(passes, truth, 6 + (p & 1), 2 + (p >> 1), quad, true);
      for(uint32_t i = 0; i < QuadPixels; i++)
      {
        INFO("quad pixel " << i << " with the neighbours read");
        if(i != QuadBottomRight || i == p)
          CHECK(SameVec(quad[i].t[2], truth[i].t[2]));
      }

      // another primitive at the neighbours has other derivatives, so they're worked out
      PSInputs other[QuadPixels];
      AffineQuad(6, 2, other);
      for(uint32_t i = 0; i < QuadPixels; i++)
        other[i].t[2][3] = float(i) * 2.0f;
      PSInputs fallback[QuadPixels];
      ReadBack(passes, truth, 6 + (p & 1), 2 + (p >> 1), fallback, true, other);
      ReadBack(passes, truth, 6 + (p & 1), 2 + (p >> 1), quad);
      for(uint32_t i = 0; i < QuadPixels; i++)
        CHECK(SameVec(fallback[i].t[2], quad[i].t[2]));
    }

    // without gradient instructions: ps_2_0, the values alone, the same in all four pixels
    REQUIRE(BuildInputReadback(prog, false, 4, passes, error));
    REQUIRE(passes.size() == 1);
    CHECK(passes[0].items.size() == 3);
    CHECK(passes[0].tokens[0] == VersionToken(true, 2, 0));
    ReadBack(passes, truth, 7, 3, rebuilt);
    for(uint32_t i = 0; i < QuadPixels; i++)
      CHECK(SameVec(rebuilt[i].v[0], truth[3].v[0]));

    // one target a pass
    REQUIRE(BuildInputReadback(prog, true, 1, passes, error));
    CHECK(passes.size() == 9);

    // a shader reading no inputs has one pass writing 0 to oC0, to see whether the draw covers
    // the pixel
    Assembler none(2, 0, true);
    none.def(0, 1.0f, 2.0f, 3.0f, 4.0f);
    none.op(Opcode::Mov, {Dst(RegType::ColorOut, 0), Src(RegType::Const, 0)});
    const rdcarray<uint32_t> noneTokens = none.finish();
    ParseProgram(noneTokens.data(), noneTokens.size(), prog);
    REQUIRE(BuildInputReadback(prog, true, 4, passes, error));
    REQUIRE(passes.size() == 1);
    CHECK(passes[0].items.empty());
    QuadRun run;
    REQUIRE(run.Run(passes[0].tokens, truth));
    CHECK(SameVec(run.quad.Pixel(0).Registers().out.color[0], Vec4()));
    CHECK(run.quad.Pixel(0).Registers().out.written == 1);
  }

  SECTION("ps_3_0: the declarations as they are, and vFace")
  {
    Assembler a(3, 0, true);
    a.dcl(DeclUsage::TexCoord, 0, 0, RegType::Input, 0x3);
    // two semantics in one register, one of them centroid
    a.op(Opcode::Dcl, {Dcl(DeclUsage::Color, 0), Dst(RegType::Input, 1, 0x3, DstMod_Centroid)});
    a.op(Opcode::Dcl, {Dcl(DeclUsage::TexCoord, 3), Dst(RegType::Input, 1, 0xC)});
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::MiscType, 0, 0x3)});
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::MiscType, 1)});
    a.op(Opcode::Mov, {Dst(RegType::ColorOut, 0), Src(RegType::Input, 0)});
    const rdcarray<uint32_t> tokens = a.finish();
    Program prog;
    ParseProgram(tokens.data(), tokens.size(), prog);

    rdcarray<ReadbackPass> passes;
    rdcstr error;
    // ps_3_0 reads the quad's pixels whether or not gradients are there for ps_2_x: each input,
    // then its top left, top right and bottom left values
    REQUIRE(BuildInputReadback(prog, false, 4, passes, error));
    REQUIRE(passes.size() == 3);
    CHECK(passes[0].tokens[0] == VersionToken(true, 3, 0));
    REQUIRE(passes[0].items.size() == 4);
    REQUIRE(passes[2].items.size() == 1);
    CHECK((passes[0].items[0].type == RegType::Input && passes[0].items[0].mask == 0x3));
    CHECK((passes[0].items[1].what == ReadbackValue::TopLeft));
    CHECK((passes[0].items[2].what == ReadbackValue::TopRight));
    CHECK((passes[0].items[3].what == ReadbackValue::BottomLeft));
    CHECK((passes[1].items[0].type == RegType::Input && passes[1].items[0].index == 1 &&
           passes[1].items[0].mask == 0xF));
    CHECK((passes[2].items[0].what == ReadbackValue::Face));

    // the four input declarations are copied (not vPos's), then vPos is declared for the quad
    Program readback;
    ParseProgram(passes[0].tokens.data(), passes[0].tokens.size(), readback);
    REQUIRE(readback.error.empty());
    rdcarray<const Instruction *> decls;
    for(const Instruction &inst : readback.instructions)
      if(inst.op == Opcode::Dcl)
        decls.push_back(&inst);
    REQUIRE(decls.size() == 5);
    CHECK((decls[1]->usage == DeclUsage::Color && (decls[1]->dst.mods & DstMod_Centroid)));
    CHECK((decls[2]->usage == DeclUsage::TexCoord && decls[2]->usageIndex == 3 &&
           decls[2]->dst.writeMask == 0xC));
    CHECK((decls[3]->dst.type == RegType::MiscType && decls[3]->dst.index == 1));
    CHECK((decls[4]->dst.type == RegType::MiscType && decls[4]->dst.index == 0));

    for(float face : {1.0f, -1.0f})
    {
      PSInputs truth[QuadPixels], rebuilt[QuadPixels];
      AffineQuad(0, 0, truth, face);
      ReadBack(passes, truth, 1, 0, rebuilt);
      for(uint32_t i = 0; i < QuadPixels; i++)
      {
        CHECK(rebuilt[i].face == face);
        CHECK(SameVec(rebuilt[i].v[0], Masked(truth[i].v[0], 0x3)));
        CHECK(SameVec(rebuilt[i].v[1], truth[i].v[1]));
      }
    }

    // inputs that aren't linear across the quad, as with perspective, and zeros of both signs:
    // the pixel and the three pixels the derivatives come from are exactly the GPU's from every
    // pixel of the quad. The bottom right is only the pixel's own.
    PSInputs truth[QuadPixels];
    AffineQuad(30, 40, truth);
    const float odd[QuadPixels][4] = {
        {0.1f, -0.0f, 7.25f, 1.0e-3f},
        {0.7f, 0.0f, -3.5f, 2.0e5f},
        {0.33f, -0.0f, 0.0f, -1.0f},
        {5.0f, 1.0f, 0.0f, 3.0f},
    };
    for(uint32_t i = 0; i < QuadPixels; i++)
      truth[i].v[1] = Vec4(odd[i][0], odd[i][1], odd[i][2], odd[i][3]);
    for(uint32_t p = 0; p < QuadPixels; p++)
    {
      INFO("pixel " << p);
      PSInputs rebuilt[QuadPixels];
      ReadBack(passes, truth, 30 + (p & 1), 40 + (p >> 1), rebuilt);
      for(uint32_t i = 0; i < QuadPixels; i++)
      {
        INFO("quad pixel " << i);
        if(i != QuadBottomRight || i == p)
          CHECK(SameVec(rebuilt[i].v[1], truth[i].v[1]));
      }
    }
  }

  SECTION("ps_1_x: the colours and coordinates read")
  {
    // ps_1_1: tex and texcoord read their own coordinate sets, arithmetic the colours
    Assembler a(1, 1, true);
    a.op(Opcode::Tex, {Dst(RegType::AddrOrTexture, 0)});
    a.op(Opcode::TexCoord, {Dst(RegType::AddrOrTexture, 2)});
    a.op(Opcode::Mul,
         {Dst(RegType::Temp, 0), Src(RegType::AddrOrTexture, 0), Src(RegType::Input, 1)});
    rdcarray<uint32_t> tokens = a.finish();
    Program prog;
    ParseProgram(tokens.data(), tokens.size(), prog);
    REQUIRE(prog.error.empty());

    rdcarray<ReadbackPass> passes;
    rdcstr error;
    REQUIRE(BuildInputReadback(prog, true, 4, passes, error));
    REQUIRE(passes.size() == 3);
    CHECK(passes[0].tokens[0] == VersionToken(true, 2, 1));
    CHECK((passes[0].items[0].type == RegType::Input && passes[0].items[0].index == 1));
    CHECK((passes[2].items[0].type == RegType::AddrOrTexture && passes[2].items[0].index == 2 &&
           passes[2].items[0].what == ReadbackValue::Ddy));

    PSInputs truth[QuadPixels], rebuilt[QuadPixels];
    AffineQuad(4, 4, truth);
    ReadBack(passes, truth, 5, 5, rebuilt);
    for(uint32_t i = 0; i < QuadPixels; i++)
    {
      CHECK(SameVec(rebuilt[i].v[1], truth[i].v[1]));
      CHECK(SameVec(rebuilt[i].t[0], truth[i].t[0]));
      CHECK(SameVec(rebuilt[i].t[2], truth[i].t[2]));
      CHECK(SameVec(rebuilt[i].v[0], Vec4()));
    }

    // ps_1_4: the coordinates its sources name
    Assembler b(1, 4, true);
    b.op(Opcode::TexCoord, {Dst(RegType::Temp, 0, 0x7), Src(RegType::AddrOrTexture, 2)});
    b.op(Opcode::Tex, {Dst(RegType::Temp, 1), Src(RegType::AddrOrTexture, 0)});
    b.op(Opcode::Add, {Dst(RegType::Temp, 0), Src(RegType::Temp, 0), Src(RegType::Input, 0)});
    tokens = b.finish();
    ParseProgram(tokens.data(), tokens.size(), prog);
    REQUIRE(prog.error.empty());
    REQUIRE(BuildInputReadback(prog, false, 4, passes, error));
    REQUIRE(passes.size() == 1);
    REQUIRE(passes[0].items.size() == 3);
    CHECK((passes[0].items[0].type == RegType::Input && passes[0].items[0].index == 0));
    CHECK((passes[0].items[1].type == RegType::AddrOrTexture && passes[0].items[1].index == 0));
    CHECK((passes[0].items[2].type == RegType::AddrOrTexture && passes[0].items[2].index == 2));
  }

  SECTION("what it refuses")
  {
    Assembler a(3, 0, false);
    a.op(Opcode::Mov, {Dst(RegType::TexCrdOutOrOutput, 0), Src(RegType::Input, 0)});
    const rdcarray<uint32_t> tokens = a.finish();
    Program prog;
    ParseProgram(tokens.data(), tokens.size(), prog);
    rdcarray<ReadbackPass> passes;
    rdcstr error;
    CHECK_FALSE(BuildInputReadback(prog, true, 4, passes, error));
    CHECK(error == "it isn't a pixel shader");
  }
}

TEST_CASE("D3D9 pixel shader debugging - stepping a pixel", "[d3d9][psemu][psdebug]")
{
  SECTION("ps_3_0 with derivatives, vPos, vFace and oDepth")
  {
    Assembler a(3, 0, true);
    a.def(0, 0.5f, 2.0f, -1.0f, 0.25f);
    a.dcl(DeclUsage::TexCoord, 0, 0);
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::MiscType, 0, 0x3)});
    a.op(Opcode::Dcl, {DclNone(), Dst(RegType::MiscType, 1)});
    a.op(Opcode::Mul, {Dst(RegType::Temp, 0), Src(RegType::Input, 0), Src(RegType::Const, 0)});
    a.op(Opcode::Add, {Dst(RegType::Temp, 0, 0x3), Src(RegType::Temp, 0), Src(RegType::MiscType, 0)});
    a.op(Opcode::Dsx, {Dst(RegType::Temp, 1), Src(RegType::Temp, 0)});
    a.op(Opcode::Cmp,
         {Dst(RegType::Temp, 1, 0x8), Src(RegType::MiscType, 1),
          Src(RegType::Const, 0, Swz(1, 1, 1, 1)), Src(RegType::Const, 0, Swz(2, 2, 2, 2))});
    a.op(Opcode::Mov, {Dst(RegType::ColorOut, 0), Src(RegType::Temp, 0)});
    a.op(Opcode::Mov, {Dst(RegType::ColorOut, 1), Src(RegType::Temp, 1)});
    a.op(Opcode::Mov, {Dst(RegType::DepthOut, 0, 0x1), Src(RegType::Const, 0, Swz(3, 3, 3, 3))});
    const rdcarray<uint32_t> tokens = a.finish();

    PSInputs quad[QuadPixels];
    AffineQuad(8, 6, quad, -1.0f);
    QuadRun reference;
    REQUIRE(reference.Run(tokens, quad));

    for(uint32_t p = 0; p < QuadPixels; p++)
    {
      INFO("pixel " << p);
      PSDebugRun run;
      run.Debug(tokens, quad, p);

      // the outputs at the end are the quad's
      const PSOutputs &out = reference.quad.Pixel(p).Registers().out;
      CHECK(SameVec(run.Var("oC0"), out.color[0]));
      CHECK(SameVec(run.Var("oC1"), out.color[1]));
      CHECK(run.Var("oDepth")[0] == 0.25f);
      CHECK(run.vars["oDepth"].columns == 1);
      CHECK(run.debugger.Quad().Pixel(p).Finished());

      // the inputs, by register
      REQUIRE(run.Input("v0"));
      CHECK(run.Input("v0")->value.f32v[0] == quad[p].v[0][0]);
      REQUIRE(run.Input("vPos"));
      CHECK(run.Input("vPos")->columns == 2);
      CHECK(run.Input("vPos")->value.f32v[1] == quad[p].pos[1]);
      REQUIRE(run.Input("vFace"));
      CHECK(run.Input("vFace")->columns == 1);
      CHECK(run.Input("vFace")->value.f32v[0] == -1.0f);

      // the registers it changes, the temporaries first
      CHECK(run.InitialNames() == rdcarray<rdcstr>({"r0", "r1", "oC0", "oC1", "oDepth"}));

      // source variables for the signatures
      const SourceVariableMapping *tc = run.SourceVar("TEXCOORD");
      REQUIRE(tc);
      REQUIRE(tc->variables.size() == 4);
      CHECK(tc->variables[0].type == DebugVariableType::Input);
      CHECK(tc->variables[0].name == "v0");
      const SourceVariableMapping *face = run.SourceVar("VFACE");
      REQUIRE(face);
      CHECK(face->variables[0].name == "vFace");
      const SourceVariableMapping *col = run.SourceVar("COLOR1");
      REQUIRE(col);
      CHECK(col->variables[0].type == DebugVariableType::Variable);
      CHECK(col->variables[0].name == "oC1");
      // and by register, as the disassembly names them
      const SourceVariableMapping *face2 = run.SourceVar("Registers.vFace");
      REQUIRE(face2);
      CHECK(face2->variables.size() == 1);
      CHECK(face2->variables[0].type == DebugVariableType::Input);
      const SourceVariableMapping *pos = run.SourceVar("Registers.vPos");
      REQUIRE(pos);
      CHECK(pos->variables.size() == 2);
      const SourceVariableMapping *depth = run.SourceVar("DEPTH");
      REQUIRE(depth);
      CHECK(depth->variables.size() == 1);
      CHECK(depth->variables[0].name == "oDepth");

      // declarations aren't steps, and nothing sampled
      for(const ShaderDebugState &s : run.states)
      {
        CHECK((run.info.GetProgram().instructions[s.nextInstruction].op != Opcode::Dcl));
        CHECK((run.info.GetProgram().instructions[s.nextInstruction].op != Opcode::Def));
        CHECK(s.flags == ShaderEvents::NoEvent);
        CHECK(s.callstack == rdcarray<rdcstr>({"main"}));
      }
      // the initial state and the seven instructions
      CHECK(run.states.size() == 8);
    }
  }

  SECTION("ps_1_1: texture registers, coordinates and r0 written out")
  {
    Assembler a(1, 1, true);
    a.def(0, 0.5f, 0.5f, 0.5f, 1.0f);
    a.op(Opcode::TexCoord, {Dst(RegType::AddrOrTexture, 0)});
    a.op(Opcode::Mul,
         {Dst(RegType::Temp, 0), Src(RegType::AddrOrTexture, 0), Src(RegType::Input, 0)});
    a.op(Opcode::Add, {Dst(RegType::Temp, 0), Src(RegType::Temp, 0), Src(RegType::Const, 0)});
    const rdcarray<uint32_t> tokens = a.finish();

    PSInputs quad[QuadPixels];
    for(uint32_t i = 0; i < QuadPixels; i++)
    {
      quad[i].t[0] = Vec4(0.25f * float(i), 0.5f, 2.0f, -1.0f);
      quad[i].v[0] = Vec4(0.5f, 0.25f, 1.0f, 1.0f);
    }
    QuadRun reference;
    REQUIRE(reference.Run(tokens, quad));

    PSDebugRun run;
    run.Debug(tokens, quad, 3);
    CHECK(run.InitialNames() == rdcarray<rdcstr>({"r0", "t0", "oC0"}));
    CHECK(SameVec(run.Var("oC0"), reference.quad.Pixel(3).Registers().out.color[0]));
    // texcoord clamps, so t0 isn't the coordinate
    CHECK(SameVec(run.Var("t0"), Vec4(0.75f, 0.5f, 1.0f, 0.0f)));
    // the coordinates aren't t#
    REQUIRE(run.Input("texcoord0"));
    CHECK(run.Input("texcoord0")->value.f32v[2] == 2.0f);
    CHECK(run.Input("t0") == NULL);
    REQUIRE(run.Input("v0"));
    // r0 to oC0 is the last step's
    bool last = false;
    for(const ShaderVariableChange &c : run.states.back().changes)
      last |= c.after.name == "oC0";
    CHECK(last);
  }

  SECTION("which instructions sample")
  {
    Instruction inst;
    for(Opcode op : {Opcode::Tex, Opcode::TexLdd, Opcode::TexLdl, Opcode::TexBem,
                     Opcode::TexM3x3Spec, Opcode::TexDP3Tex, Opcode::TexReg2RGB})
    {
      inst.op = op;
      CHECK(PSInstructionSamples(inst));
    }
    for(Opcode op : {Opcode::TexCoord, Opcode::TexKill, Opcode::TexM3x2Pad, Opcode::TexDepth,
                     Opcode::TexDP3, Opcode::TexM3x3, Opcode::Mov})
    {
      inst.op = op;
      CHECK_FALSE(PSInstructionSamples(inst));
    }
  }
}

#endif    // ENABLED(ENABLE_UNIT_TESTS)
