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

// Vertex shader debugging (steps 5.2 and 5.3): a ShaderDebugger that steps one vertex through the
// interpreter (VSThread in d3d9_vs_emulate.h) and describes it as the replay controller's
// ShaderDebugTrace and ShaderDebugStates. The pixel debugger (d3d9_ps_debug.h) shares its stepping
// and description (StepDebugger). Like the interpreter it doesn't need any Windows
// headers, so it's unit tested on every platform. The replay side, which takes the vertex's inputs,
// constants and textures from VS Out's snapshot of the draw, is d3d9_shaderdebug.cpp.

#include <memory>
#include <set>
#include "d3d9_shader_reflect.h"
#include "d3d9_vs_emulate.h"

namespace D3D9Emu
{
// a declaration (dcl, def, defi, defb), which the debuggers run with the instruction before it
bool IsDeclaration(D3D9BC::Opcode op);

// a source variable with no variables yet, of one row of columns (none for 0)
SourceVariableMapping DebugMapping(const rdcstr &name, VarType type, uint32_t columns,
                                   int32_t signatureIndex);

// What the vertex and pixel debuggers (d3d9_ps_debug.h) share: stepping a thread and describing
// its registers as ShaderDebugStates, and the parts of the trace that only depend on the shader
// (constants, samplers, disassembly lines). Each debugger runs its own kind of thread.
class StepDebugger : public ShaderDebugger
{
public:
  // The next states: on the first call the initial one, with every register the shader can change,
  // then up to maxSteps instructions, each with the registers it changed. Empty once the shader has
  // finished. Declarations (dcl, def, defi, defb) aren't steps. Each state has the call stack at
  // its next instruction ("main", then a label such as "l2" per call), and its flags say whether
  // its instruction sampled a texture or wrote a NaN or infinity.
  rdcarray<ShaderDebugState> Continue(uint32_t maxSteps = 100);

  // why the shader stopped before its end, if it did
  virtual const rdcstr &GetError() const = 0;

  // what the emulator, constants and textures belong to, kept alive with the debugger
  std::shared_ptr<void> owner;

protected:
  struct Register
  {
    D3D9BC::RegType type;
    uint32_t index;
  };

  static uint32_t Key(D3D9BC::RegType type, uint32_t index)
  {
    return (uint32_t(type) << 16) | index;
  }

  // starts describing prog, which must outlive the debugger
  void Reset(const D3D9BC::Program &prog);
  // adds a register the shader can change to the initial state's list, once
  void AddRegister(D3D9BC::RegType type, uint32_t index);

  // the constant blocks (consts with the literals applied), the samplers and each instruction's
  // line in the disassembly, into ret
  void DescribeConstants(ShaderDebugTrace *ret, const D3D9BC::ShaderInfo &info,
                         const VSConstants &consts) const;
  void DescribeSamplers(ShaderDebugTrace *ret, const ShaderReflection &refl) const;
  void DescribeInstructions(ShaderDebugTrace *ret, const rdcarray<uint32_t> &instLines) const;
  // each of ret's inputs again by register under "Registers" (Registers.v0), as the constant
  // table's registers are, since the viewer only lists the inputs by their semantics
  void DescribeInputRegisters(ShaderDebugTrace *ret) const;

  // the thread being debugged
  virtual bool ThreadFinished() const = 0;
  virtual uint32_t ThreadNextInstruction() const = 0;
  // runs the next instruction, false if it failed (the thread is then finished)
  virtual bool ThreadStep(rdcarray<VSRegisterChange> &changes) = 0;
  virtual const FlowRegisters &Flow() const = 0;
  // a register as a variable, with the value given or the thread's current one
  virtual ShaderVariable Variable(D3D9BC::RegType type, uint32_t index, const Vec4 &f,
                                  const IVec4 &i) const = 0;
  virtual ShaderVariable Current(D3D9BC::RegType type, uint32_t index) const = 0;
  // whether inst samples a texture
  virtual bool Samples(const D3D9BC::Instruction &inst) const = 0;

  const D3D9BC::Program *m_Prog = NULL;
  D3D9BC::Version m_Version;

private:
  void SkipDeclarations(rdcarray<VSRegisterChange> &changes);
  rdcarray<rdcstr> Callstack() const;

  uint32_t m_Steps = 0;
  // the instruction last run, which the final state points at
  uint32_t m_LastInstruction = 0;

  // the registers the shader can change, in the order the initial state lists them, and their keys
  rdcarray<Register> m_Registers;
  std::set<uint32_t> m_Known;
};

class VSDebugger : public StepDebugger
{
public:
  // Starts debugging one vertex, returning its trace with debugger set to this. inputs are copied.
  // emu (initialised without an error), consts (from emu.ApplyLiterals) and textures (or NULL)
  // must outlive the debugger: owner can keep what they belong to alive. info is the shader's
  // reflection and constant table (only used here), and instLines each instruction's line in its
  // disassembly (ShaderInfo::GetDisassembly).
  ShaderDebugTrace *Begin(const VSEmulator &emu, const D3D9BC::ShaderInfo &info,
                          const rdcarray<uint32_t> &instLines, const Vec4 inputs[MaxVSInputs],
                          const VSConstants &consts, const VSTexture textures[NumVertexSamplers]);

  const rdcstr &GetError() const override { return m_Thread.GetError(); }

private:
  bool ThreadFinished() const override { return m_Thread.Finished(); }
  uint32_t ThreadNextInstruction() const override { return m_Thread.NextInstruction(); }
  bool ThreadStep(rdcarray<VSRegisterChange> &changes) override { return m_Thread.Step(&changes); }
  const FlowRegisters &Flow() const override { return m_Thread.Registers(); }
  ShaderVariable Variable(D3D9BC::RegType type, uint32_t index, const Vec4 &f,
                          const IVec4 &i) const override;
  ShaderVariable Current(D3D9BC::RegType type, uint32_t index) const override;
  // texldl
  bool Samples(const D3D9BC::Instruction &inst) const override
  {
    return inst.op == D3D9BC::Opcode::TexLdl;
  }

  VSThread m_Thread;
  Vec4 m_Inputs[MaxVSInputs];
};

};    // namespace D3D9Emu
