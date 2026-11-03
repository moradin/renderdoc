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

#include "core/core.h"
#include "driver/dx/official/d3d9.h"
#include "driver/dxgi/dxgi_common.h"

class WrappedD3D9;
class WrappedD3DDevice9;
class WrappedD3DSwapChain9;
class WrappedD3DStateBlock9;
class WrappedD3D9Object;

// Texture binding slots. D3D9 has 16 pixel samplers, the displacement map sampler and 4 vertex
// texture samplers, addressed by sparse sampler indices. Internally we pack them into one array.
enum
{
  D3D9_NUM_PIXEL_SAMPLERS = 16,
  D3D9_NUM_VERTEX_SAMPLERS = 4,
  D3D9_NUM_TEXTURE_SLOTS = D3D9_NUM_PIXEL_SAMPLERS + 1 + D3D9_NUM_VERTEX_SAMPLERS,
  D3D9_NUM_STREAMS = 16,
  D3D9_NUM_RENDER_TARGETS = 4,
};

// returns ~0U for an invalid sampler index
inline uint32_t D3D9SamplerToSlot(DWORD sampler)
{
  if(sampler < D3D9_NUM_PIXEL_SAMPLERS)
    return sampler;
  if(sampler == D3DDMAPSAMPLER)
    return D3D9_NUM_PIXEL_SAMPLERS;
  if(sampler >= D3DVERTEXTEXTURESAMPLER0 && sampler <= D3DVERTEXTEXTURESAMPLER3)
    return D3D9_NUM_PIXEL_SAMPLERS + 1 + (sampler - D3DVERTEXTEXTURESAMPLER0);
  return ~0U;
}

inline DWORD D3D9SlotToSampler(uint32_t slot)
{
  if(slot < D3D9_NUM_PIXEL_SAMPLERS)
    return slot;
  if(slot == D3D9_NUM_PIXEL_SAMPLERS)
    return D3DDMAPSAMPLER;
  return D3DVERTEXTEXTURESAMPLER0 + (slot - D3D9_NUM_PIXEL_SAMPLERS - 1);
}
