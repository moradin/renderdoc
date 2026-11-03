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
//  - Get* calls on state-block state aren't guaranteed to work on D3DCREATE_PUREDEVICE devices.
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

// The rest of the device state: everything a frame can depend on that isn't an object binding.
// Captures store this as it was when the frame started.
//
// It's read back from the real device with Get* calls rather than tracked as calls are made, so
// it's right however it was set: directly, by applying a state block, or by a Reset putting
// everything back to defaults. Get* calls on this state aren't guaranteed to work on
// D3DCREATE_PUREDEVICE devices, so the real device is never created with that flag (see
// WrappedD3D9::CreateDevice).
struct D3D9RenderStateValue
{
  D3DRENDERSTATETYPE State = D3DRS_ZENABLE;
  DWORD Value = 0;
};

struct D3D9SamplerStateValue
{
  D3DSAMPLERSTATETYPE State = D3DSAMP_ADDRESSU;
  DWORD Value = 0;
};

struct D3D9TextureStageStateValue
{
  D3DTEXTURESTAGESTATETYPE State = D3DTSS_COLOROP;
  DWORD Value = 0;
};

struct D3D9SamplerStates
{
  // the sampler as the API numbers it: 0-15, D3DDMAPSAMPLER, or D3DVERTEXTEXTURESAMPLER0-3
  DWORD Sampler = 0;
  rdcarray<D3D9SamplerStateValue> States;
};

struct D3D9TextureStageStates
{
  DWORD Stage = 0;
  rdcarray<D3D9TextureStageStateValue> States;
};

struct D3D9TransformValue
{
  D3DTRANSFORMSTATETYPE State = D3DTS_VIEW;
  D3DMATRIX Matrix = {};
};

struct D3D9LightState
{
  DWORD Index = 0;
  D3DLIGHT9 Light = {};
  BOOL Enabled = FALSE;
};

struct D3D9Palette
{
  UINT Number = 0;
  rdcfixedarray<PALETTEENTRY, 256> Entries;
};

struct D3D9Vec4f
{
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

struct D3D9Vec4i
{
  int32_t x = 0, y = 0, z = 0, w = 0;
};

struct D3D9ShaderConstants
{
  rdcarray<D3D9Vec4f> Float;
  rdcarray<D3D9Vec4i> Int;
  rdcarray<BOOL> Bool;
};

struct D3D9DeviceState
{
  rdcarray<D3D9RenderStateValue> RenderStates;
  rdcarray<D3D9SamplerStates> SamplerStates;
  rdcarray<D3D9TextureStageStates> TextureStageStates;
  // view, projection and texture transforms, and the world matrices. World matrices that are the
  // identity (the default) are left out, except D3DTS_WORLD itself.
  rdcarray<D3D9TransformValue> Transforms;
  D3DVIEWPORT9 Viewport = {};
  RECT ScissorRect = {};
  D3DMATERIAL9 Material = {};
  // only the lights that have been set or enabled, see D3D9IndexSet
  rdcarray<D3D9LightState> Lights;
  rdcarray<D3D9Vec4f> ClipPlanes;
  D3D9ShaderConstants VertexShaderConstants;
  D3D9ShaderConstants PixelShaderConstants;
  // the FVF, when the vertex declaration came from SetFVF
  DWORD FVF = 0;
  rdcarray<UINT> StreamFrequencies;
  float NPatchMode = 0.0f;
  BOOL SoftwareVertexProcessing = FALSE;
  UINT CurrentTexturePalette = 0;
  rdcarray<D3D9Palette> Palettes;
  D3DCLIPSTATUS9 ClipStatus = {};
};

// Lights and palettes are indexed by any number the application likes, and the device can't
// enumerate them, so the device remembers which indices have been used.
struct D3D9IndexSet
{
  void Add(uint32_t idx)
  {
    if(!m_Indices.contains(idx))
      m_Indices.push_back(idx);
  }
  const rdcarray<uint32_t> &Indices() const { return m_Indices; }

private:
  rdcarray<uint32_t> m_Indices;
};

// read the device state from the real device. behaviorFlags are the flags the application created
// the device with, which decide how many vertex shader constants there are.
void D3D9FetchDeviceState(IDirect3DDevice9 *realDevice, DWORD behaviorFlags,
                          const D3D9IndexSet &lights, const D3D9IndexSet &palettes,
                          D3D9DeviceState &state);

// set the real device's state back to target, only setting what differs from current. Both must
// have been read from the same device with D3D9FetchDeviceState. The FVF isn't set, it's part of
// the vertex declaration binding.
void D3D9RestoreDeviceState(IDirect3DDevice9 *realDevice, const D3D9DeviceState &target,
                            const D3D9DeviceState &current);

DECLARE_REFLECTION_STRUCT(D3D9RenderStateValue);
DECLARE_REFLECTION_STRUCT(D3D9SamplerStateValue);
DECLARE_REFLECTION_STRUCT(D3D9TextureStageStateValue);
DECLARE_REFLECTION_STRUCT(D3D9SamplerStates);
DECLARE_REFLECTION_STRUCT(D3D9TextureStageStates);
DECLARE_REFLECTION_STRUCT(D3D9TransformValue);
DECLARE_REFLECTION_STRUCT(D3D9LightState);
DECLARE_REFLECTION_STRUCT(D3D9Palette);
DECLARE_REFLECTION_STRUCT(D3D9Vec4f);
DECLARE_REFLECTION_STRUCT(D3D9Vec4i);
DECLARE_REFLECTION_STRUCT(D3D9ShaderConstants);
DECLARE_REFLECTION_STRUCT(D3D9DeviceState);
