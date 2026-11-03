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

///////////////////////////////////////////////////////////////////////////////
// Serialisation

#define IMPLEMENT_FUNCTION_SERIALISED(ret, func, ...) \
  ret func(__VA_ARGS__);                              \
  template <typename SerialiserType>                  \
  bool CONCAT(Serialise_, func(SerialiserType &ser, __VA_ARGS__));

#define INSTANTIATE_FUNCTION_SERIALISED(ret, parent, func, ...)                     \
  template bool parent::CONCAT(Serialise_, func(ReadSerialiser &ser, __VA_ARGS__)); \
  template bool parent::CONCAT(Serialise_, func(WriteSerialiser &ser, __VA_ARGS__));

#define USE_SCRATCH_SERIALISER() WriteSerialiser &ser = m_ScratchSerialiser;

#define SERIALISE_TIME_CALL(...)                                          \
  m_ScratchSerialiser.ChunkMetadata().timestampMicro = Timing::GetTick(); \
  __VA_ARGS__;                                                            \
  m_ScratchSerialiser.ChunkMetadata().durationMicro =                     \
      Timing::GetTick() - m_ScratchSerialiser.ChunkMetadata().timestampMicro;

// "is the serialiser reading and we're doing replay-mode stuff?" See the D3D11 driver for why both
// are checked.
#define IsReplayingAndReading() (ser.IsReading() && IsReplayMode(m_State))

// The parameters to device creation. Stored in the DriverInit chunk.
struct D3D9InitParams
{
  UINT Adapter = D3DADAPTER_DEFAULT;
  D3DDEVTYPE DeviceType = D3DDEVTYPE_HAL;
  DWORD BehaviorFlags = 0;
  bool Ex = false;

  // for information only, replay picks a device itself
  D3DADAPTER_IDENTIFIER9 AdapterIdentifier = {};

  // check if a frame capture section version is supported
  static const uint64_t CurrentVersion = 0x1;
  static bool IsSupportedVersion(uint64_t ver);
};

DECLARE_REFLECTION_STRUCT(D3D9InitParams);

// NOTE: Only ever add chunks at the end, existing values are stored in captures.
enum class D3D9Chunk : uint32_t
{
  // device-owned objects as they are when the capture starts
  DeviceInitialisation = (uint32_t)SystemChunk::FirstDriverChunk,

  // object creation
  CreateTexture,
  CreateVolumeTexture,
  CreateCubeTexture,
  CreateVertexBuffer,
  CreateIndexBuffer,
  CreateRenderTarget,
  CreateDepthStencilSurface,
  CreateOffscreenPlainSurface,
  CreateRenderTargetEx,
  CreateOffscreenPlainSurfaceEx,
  CreateDepthStencilSurfaceEx,
  CreateVertexDeclaration,
  CreateVertexShader,
  CreatePixelShader,
  CreateQuery,
  CreateStateBlock,
  CreateAdditionalSwapChain,
  // objects owned by other objects, created on first use
  Texture_GetSurfaceLevel,
  CubeTexture_GetCubeMapSurface,
  VolumeTexture_GetVolumeLevel,
  SwapChain_GetBackBuffer,

  // frame setup: the device's object bindings when the capture starts
  BeginCaptureFrame,

  // device calls
  SetRenderTarget,
  SetDepthStencilSurface,
  BeginScene,
  EndScene,
  Clear,
  SetTransform,
  MultiplyTransform,
  SetViewport,
  SetMaterial,
  SetLight,
  LightEnable,
  SetClipPlane,
  SetRenderState,
  BeginStateBlock,
  EndStateBlock,
  SetClipStatus,
  SetTexture,
  SetTextureStageState,
  SetSamplerState,
  SetPaletteEntries,
  SetCurrentTexturePalette,
  SetScissorRect,
  SetSoftwareVertexProcessing,
  SetNPatchMode,
  DrawPrimitive,
  DrawIndexedPrimitive,
  DrawPrimitiveUP,
  DrawIndexedPrimitiveUP,
  ProcessVertices,
  SetVertexDeclaration,
  SetFVF,
  SetVertexShader,
  SetVertexShaderConstantF,
  SetVertexShaderConstantI,
  SetVertexShaderConstantB,
  SetStreamSource,
  SetStreamSourceFreq,
  SetIndices,
  SetPixelShader,
  SetPixelShaderConstantF,
  SetPixelShaderConstantI,
  SetPixelShaderConstantB,
  DrawRectPatch,
  DrawTriPatch,
  DeletePatch,
  UpdateSurface,
  UpdateTexture,
  GetRenderTargetData,
  StretchRect,
  ColorFill,
  ComposeRects,
  SetConvolutionMonoKernel,
  Present,
  PresentEx,
  SwapChain_Present,

  // calls on objects
  StateBlock_Capture,
  StateBlock_Apply,
  Query_Issue,
  BaseTexture_GenerateMipSubLevels,
  BaseTexture_SetAutoGenFilterType,
  BaseTexture_SetLOD,

  Max,
};

DECLARE_REFLECTION_ENUM(D3D9Chunk);

// Interfaces are serialised as the ResourceId of their wrapper
#define SERIALISE_D3D9_INTERFACES()                 \
  SERIALISE_INTERFACE(IDirect3DSwapChain9);         \
  SERIALISE_INTERFACE(IDirect3DResource9);          \
  SERIALISE_INTERFACE(IDirect3DBaseTexture9);       \
  SERIALISE_INTERFACE(IDirect3DTexture9);           \
  SERIALISE_INTERFACE(IDirect3DCubeTexture9);       \
  SERIALISE_INTERFACE(IDirect3DVolumeTexture9);     \
  SERIALISE_INTERFACE(IDirect3DSurface9);           \
  SERIALISE_INTERFACE(IDirect3DVolume9);            \
  SERIALISE_INTERFACE(IDirect3DVertexBuffer9);      \
  SERIALISE_INTERFACE(IDirect3DIndexBuffer9);       \
  SERIALISE_INTERFACE(IDirect3DVertexDeclaration9); \
  SERIALISE_INTERFACE(IDirect3DVertexShader9);      \
  SERIALISE_INTERFACE(IDirect3DPixelShader9);       \
  SERIALISE_INTERFACE(IDirect3DStateBlock9);        \
  SERIALISE_INTERFACE(IDirect3DQuery9);

#define SERIALISE_INTERFACE(iface) DECLARE_REFLECTION_STRUCT(iface *)

SERIALISE_D3D9_INTERFACES();

// DWORD and LONG members are serialised as uint32_t/int32_t, since 'long' is reserved for HRESULT
DECLARE_REFLECTION_STRUCT(D3DPRESENT_PARAMETERS);
DECLARE_REFLECTION_STRUCT(D3DDISPLAYMODEEX);
DECLARE_REFLECTION_STRUCT(D3DADAPTER_IDENTIFIER9);
DECLARE_REFLECTION_STRUCT(D3DVIEWPORT9);
DECLARE_REFLECTION_STRUCT(D3DMATRIX);
DECLARE_REFLECTION_STRUCT(D3DCOLORVALUE);
DECLARE_REFLECTION_STRUCT(D3DVECTOR);
DECLARE_REFLECTION_STRUCT(D3DMATERIAL9);
DECLARE_REFLECTION_STRUCT(D3DLIGHT9);
DECLARE_REFLECTION_STRUCT(D3DCLIPSTATUS9);
DECLARE_REFLECTION_STRUCT(D3DVERTEXELEMENT9);
DECLARE_REFLECTION_STRUCT(D3DRECT);
DECLARE_REFLECTION_STRUCT(D3DBOX);
DECLARE_REFLECTION_STRUCT(D3DRECTPATCH_INFO);
DECLARE_REFLECTION_STRUCT(D3DTRIPATCH_INFO);
DECLARE_REFLECTION_STRUCT(PALETTEENTRY);
// RECT is also D3D11_RECT, and the D3D11 driver's TypeName for it wins in the shared DLL, so the
// D3D9 serialise code renames each RECT element with TypedAs("RECT").
DECLARE_REFLECTION_STRUCT(RECT);
DECLARE_REFLECTION_STRUCT(POINT);

DECLARE_REFLECTION_ENUM(D3DFORMAT);
DECLARE_REFLECTION_ENUM(D3DPOOL);
DECLARE_REFLECTION_ENUM(D3DRESOURCETYPE);
DECLARE_REFLECTION_ENUM(D3DMULTISAMPLE_TYPE);
DECLARE_REFLECTION_ENUM(D3DSWAPEFFECT);
DECLARE_REFLECTION_ENUM(D3DDEVTYPE);
DECLARE_REFLECTION_ENUM(D3DPRIMITIVETYPE);
DECLARE_REFLECTION_ENUM(D3DRENDERSTATETYPE);
DECLARE_REFLECTION_ENUM(D3DSAMPLERSTATETYPE);
DECLARE_REFLECTION_ENUM(D3DTEXTURESTAGESTATETYPE);
DECLARE_REFLECTION_ENUM(D3DTRANSFORMSTATETYPE);
DECLARE_REFLECTION_ENUM(D3DSTATEBLOCKTYPE);
DECLARE_REFLECTION_ENUM(D3DQUERYTYPE);
DECLARE_REFLECTION_ENUM(D3DTEXTUREFILTERTYPE);
DECLARE_REFLECTION_ENUM(D3DBACKBUFFER_TYPE);
DECLARE_REFLECTION_ENUM(D3DCUBEMAP_FACES);
DECLARE_REFLECTION_ENUM(D3DLIGHTTYPE);
DECLARE_REFLECTION_ENUM(D3DCOMPOSERECTSOP);
DECLARE_REFLECTION_ENUM(D3DSCANLINEORDERING);
DECLARE_REFLECTION_ENUM(D3DBASISTYPE);
DECLARE_REFLECTION_ENUM(D3DDEGREETYPE);
