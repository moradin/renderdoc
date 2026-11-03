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

#include "d3d9_common.h"

// The objects bound to the device that are part of state blocks. All pointers are to *wrapped*
// objects, and each non-NULL pointer holds an internal reference on its wrapper. That mirrors the
// runtime, where bindings keep objects alive without counting as application references.
//
// We track these ourselves rather than asking the real device, because:
//  - Get* calls must return our wrappers, with the same pointer every time.
//  - Get* calls on state-block state fail on D3DCREATE_PUREDEVICE devices.
//  - capture needs the full binding state at the start of a frame.
struct D3D9StreamBinding
{
  IDirect3DVertexBuffer9 *buffer = NULL;
  UINT offset = 0;
  UINT stride = 0;
};

// Which bindings a state block covers.
struct D3D9BindingMask
{
  uint32_t textures = 0;    // one bit per texture slot, see D3D9SamplerToSlot
  uint32_t streams = 0;     // one bit per stream
  bool indices = false;
  bool vs = false;
  bool ps = false;
  bool decl = false;    // vertex declaration, including one set implicitly with SetFVF

  static D3D9BindingMask All();
  static D3D9BindingMask ForStateBlockType(D3DSTATEBLOCKTYPE type);
};

struct D3D9ObjectBindings
{
  D3D9ObjectBindings() = default;
  ~D3D9ObjectBindings() { ReleaseAll(); }

  // bindings own references, so they can't be implicitly copied
  D3D9ObjectBindings(const D3D9ObjectBindings &) = delete;
  D3D9ObjectBindings &operator=(const D3D9ObjectBindings &) = delete;

  IDirect3DBaseTexture9 *textures[D3D9_NUM_TEXTURE_SLOTS] = {};
  D3D9StreamBinding streams[D3D9_NUM_STREAMS];
  IDirect3DIndexBuffer9 *indices = NULL;
  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  IDirect3DVertexDeclaration9 *decl = NULL;
  // set when the declaration came from SetFVF. The real device owns that declaration so decl is
  // NULL, and GetVertexDeclaration has to ask the real device for it.
  bool declFromFVF = false;

  void SetTexture(uint32_t slot, IDirect3DBaseTexture9 *tex);
  void SetStream(uint32_t stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride);
  void SetIndices(IDirect3DIndexBuffer9 *ib);
  void SetVertexShader(IDirect3DVertexShader9 *shader);
  void SetPixelShader(IDirect3DPixelShader9 *shader);
  void SetDecl(IDirect3DVertexDeclaration9 *decl, bool fromFVF);

  // copy the bindings selected by mask from src
  void CopyFrom(const D3D9ObjectBindings &src, const D3D9BindingMask &mask);

  // drop every binding, releasing the internal references
  void ReleaseAll();
};

// internal reference helpers that accept any wrapped D3D9 interface pointer (or NULL)
void D3D9IntAddRef(IUnknown *wrapped);
void D3D9IntRelease(IUnknown *wrapped);

// replace a binding slot holding a wrapped object, managing internal references
template <typename T>
void D3D9SetBound(T *&slot, T *val)
{
  if(slot == val)
    return;
  D3D9IntAddRef(val);
  T *old = slot;
  slot = val;
  D3D9IntRelease(old);
}
