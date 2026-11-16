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

// The snapshot of a draw that VS Out computes its vertices from (d3d9_postvs.cpp), which vertex
// shader debugging (d3d9_shaderdebug.cpp) runs a vertex from too.

#include "d3d9_ffp_vertex.h"
#include "d3d9_replay.h"
#include "d3d9_vs_emulate.h"

struct D3D9Replay::PostVSEvent
{
  // why there's no data, when there can't be any
  rdcstr status;

  D3D9Emu::DrawInputs draw;
  Topology topology = Topology::Unknown;
  uint32_t numInstances = 1;

  bool hasVS = false;
  // the vertex shader, in m_Shaders
  ResourceId vsId;
  D3D9Emu::VSEmulator vs;
  D3D9Emu::VSConstants consts;
  D3D9Emu::VSTexture textures[D3D9Emu::NumVertexSamplers];

  D3D9Emu::FFPState ffp;

  rdcarray<SigParameter> signature;

  // computed when they're first asked for, by instance
  std::map<uint32_t, D3D9Emu::PostVSResult> results;
  std::map<uint32_t, ResourceId> vertexBuffers;
  ResourceId indexBuffer;
};
