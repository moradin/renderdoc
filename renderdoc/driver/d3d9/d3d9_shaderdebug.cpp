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

// Shader debugging (step 5). A vertex is debugged on the CPU from the same snapshot of the draw
// that VS Out computes its vertices from (InitPostVSBuffers, d3d9_postvs.cpp): the vertex's inputs
// are fetched as RunPostVS fetches them, and the shader steps through the same interpreter with
// the same constants and vertex textures, so the debugged outputs are VS Out's exactly. The trace
// itself is made by D3D9Emu::VSDebugger (d3d9_vs_debug.cpp).
//
// Pixel shaders can't be debugged yet (DebugPixel, step 5.7), and fixed function vertex processing
// isn't a shader. Both say so in their reflection's debugStatus, which the UI shows.

#include "d3d9_postvs.h"
#include "d3d9_vs_debug.h"

namespace
{
ShaderDebugTrace *NoDebug(uint32_t eventId, const rdcstr &why)
{
  RDCWARN("Can't debug a vertex at event %u: %s", eventId, why.c_str());
  return new ShaderDebugTrace;
}
};    // namespace

ShaderDebugTrace *D3D9Replay::DebugVertex(uint32_t eventId, uint32_t vertid, uint32_t instid,
                                          uint32_t idx, uint32_t view)
{
  // the vertex is found from idx, as D3D11 does. There's no multiview.
  (void)vertid;
  (void)view;

  // the replay is at the event, as for GetPostVSData
  InitPostVSBuffers(eventId);

  auto it = m_PostVS.find(eventId);
  if(it == m_PostVS.end())
    return NoDebug(eventId, "it isn't a draw");

  std::shared_ptr<PostVSEvent> ev = it->second;
  if(!ev->status.empty())
    return NoDebug(eventId, ev->status);
  if(!ev->hasVS)
    return NoDebug(eventId, "fixed function vertex processing isn't a shader");

  auto sh = m_Shaders.find(ev->vsId);
  if(sh == m_Shaders.end())
    return NoDebug(eventId, "the vertex shader's bytecode wasn't kept");

  const D3D9Emu::DrawInputs &draw = ev->draw;

  // idx is the mesh viewer's IDX column: an indexed draw's index with the base vertex added, or a
  // non-indexed draw's vertex counted from its first
  const uint32_t vertex = draw.indexed ? idx : draw.firstVertex + idx;
  const uint32_t instance = RDCMIN(instid, ev->numInstances - 1);

  D3D9Emu::Vec4 inputs[D3D9Emu::MaxVSInputs];
  D3D9Emu::FetchVSInputs(draw, D3D9Emu::BindVSInputs(ev->vs.GetProgram(), draw.elements), vertex,
                         instance, inputs);

  rdcarray<uint32_t> lines;
  sh->second.GetDisassembly(&lines);

  D3D9Emu::VSDebugger *debugger = new D3D9Emu::VSDebugger;
  // the emulator, constants and textures are the event's
  debugger->owner = ev;
  return debugger->Begin(ev->vs, sh->second.GetReflection(), lines, inputs, ev->consts, ev->textures);
}

rdcarray<ShaderDebugState> D3D9Replay::ContinueDebug(ShaderDebugger *debugger)
{
  if(debugger == NULL)
    return {};

  D3D9Emu::VSDebugger *vs = (D3D9Emu::VSDebugger *)debugger;
  rdcarray<ShaderDebugState> ret = vs->Continue();
  if(ret.empty() && !vs->GetError().empty())
    RDCWARN("The debugged vertex shader stopped early: %s", vs->GetError().c_str());
  return ret;
}

void D3D9Replay::FreeDebugger(ShaderDebugger *debugger)
{
  delete debugger;
}
