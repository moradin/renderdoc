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
// ShaderDebugTrace and ShaderDebugStates. Like the interpreter it doesn't need any Windows
// headers, so it's unit tested on every platform. The replay side, which takes the vertex's inputs,
// constants and textures from VS Out's snapshot of the draw, is d3d9_shaderdebug.cpp.

#include <memory>
#include <set>
#include "d3d9_shader_reflect.h"
#include "d3d9_vs_emulate.h"

namespace D3D9Emu
{
class VSDebugger : public ShaderDebugger
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

  // The next states: on the first call the initial one, with every register the shader can change,
  // then up to maxSteps instructions, each with the registers it changed. Empty once the shader has
  // finished. Declarations (dcl, def, defi, defb) aren't steps. Each state has the call stack at
  // its next instruction ("main", then a label such as "l2" per call), and its flags say whether
  // its instruction sampled a texture (texldl) or wrote a NaN or infinity.
  rdcarray<ShaderDebugState> Continue(uint32_t maxSteps = 100);

  // why the shader stopped before its end, if it did
  const rdcstr &GetError() const { return m_Thread.GetError(); }

  // what emu, consts and textures belong to, kept alive with the debugger
  std::shared_ptr<void> owner;

private:
  struct Register
  {
    D3D9BC::RegType type;
    uint32_t index;
  };

  static uint32_t Key(D3D9BC::RegType type, uint32_t index)
  {
    return (uint32_t(type) << 16) | index;
  }
  ShaderVariable Variable(D3D9BC::RegType type, uint32_t index, const Vec4 &f, const IVec4 &i) const;
  ShaderVariable Current(D3D9BC::RegType type, uint32_t index) const;
  void SkipDeclarations(rdcarray<VSRegisterChange> &changes);
  rdcarray<rdcstr> Callstack() const;

  const VSEmulator *m_Emu = NULL;
  VSThread m_Thread;
  D3D9BC::Version m_Version;
  Vec4 m_Inputs[MaxVSInputs];
  uint32_t m_Steps = 0;
  // the instruction last run, which the final state points at
  uint32_t m_LastInstruction = 0;

  // the registers the shader can change, in the order the initial state lists them, and their keys
  rdcarray<Register> m_Registers;
  std::set<uint32_t> m_Known;
};

};    // namespace D3D9Emu
