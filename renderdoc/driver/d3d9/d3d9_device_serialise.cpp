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

#include "d3d9_device.h"
#include "d3d9_replay.h"
#include "d3d9_resources.h"
#include "d3d9_swapchain.h"

// Serialisation of every recorded call. Most of these were generated from a table of the API calls
// and only serialise their parameters: replaying them is the next step of D3D9 support (see
// STATUS.md in this folder).

namespace
{
template <typename T, size_t N>
rdcarray<T> ArrayOf(T (&arr)[N])
{
  return rdcarray<T>(arr, N);
}

UINT VertexCountForPrimitives(D3DPRIMITIVETYPE type, UINT primCount)
{
  switch(type)
  {
    case D3DPT_POINTLIST: return primCount;
    case D3DPT_LINELIST: return primCount * 2;
    case D3DPT_LINESTRIP: return primCount > 0 ? primCount + 1 : 0;
    case D3DPT_TRIANGLELIST: return primCount * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return primCount > 0 ? primCount + 2 : 0;
    default: return 0;
  }
}
};

rdcstr WrappedD3DDevice9::GetChunkName(uint32_t idx)
{
  if((SystemChunk)idx < SystemChunk::FirstDriverChunk)
    return ToStr((SystemChunk)idx);

  return ToStr((D3D9Chunk)idx);
}

///////////////////////////////////////////////////////////////////////////////
// generated

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetRenderTarget(SerialiserType &ser, DWORD RenderTargetIndex,
                                                  IDirect3DSurface9 *pRenderTarget)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, RenderTargetIndex);
  SERIALISE_ELEMENT(pRenderTarget);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetRenderTarget(RenderTargetIndex, pRenderTarget);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetRenderTarget,
                                DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetDepthStencilSurface(SerialiserType &ser,
                                                         IDirect3DSurface9 *pNewZStencil)
{
  SERIALISE_ELEMENT(pNewZStencil);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetDepthStencilSurface(pNewZStencil);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetDepthStencilSurface,
                                IDirect3DSurface9 *pNewZStencil);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_BeginScene(SerialiserType &ser)
{
  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayBeginScene();
  }

  return true;
}

template bool WrappedD3DDevice9::Serialise_BeginScene(ReadSerialiser &ser);
template bool WrappedD3DDevice9::Serialise_BeginScene(WriteSerialiser &ser);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_EndScene(SerialiserType &ser)
{
  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayBeginScene();
  }

  return true;
}

template bool WrappedD3DDevice9::Serialise_EndScene(ReadSerialiser &ser);
template bool WrappedD3DDevice9::Serialise_EndScene(WriteSerialiser &ser);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_Clear(SerialiserType &ser, DWORD Count, CONST D3DRECT *pRects,
                                        DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, Count);
  SERIALISE_ELEMENT_ARRAY(pRects, Count);
  SERIALISE_ELEMENT_TYPED(uint32_t, Flags);
  SERIALISE_ELEMENT_TYPED(uint32_t, Color);
  SERIALISE_ELEMENT(Z);
  SERIALISE_ELEMENT_TYPED(uint32_t, Stencil);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayEndScene();

    Clear(Count, pRects, Flags, Color, Z, Stencil);

    if(IsLoading(m_State))
    {
      AddEvent();

      // AddAction fills in the bound targets as the outputs. The copy destination is what the
      // texture viewer jumps to: render target 0 for a colour clear, otherwise the depth stencil.
      ActionDescription action;
      action.flags = ActionFlags::Clear;
      if(Flags & D3DCLEAR_TARGET)
      {
        action.flags |= ActionFlags::ClearColor;
        action.copyDestination = SurfaceTextureID(m_RenderTargets[0]);
      }
      if(Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))
      {
        action.flags |= ActionFlags::ClearDepthStencil;
        if(action.copyDestination == ResourceId())
          action.copyDestination = SurfaceTextureID(m_DepthStencil);
      }
      AddAction(action);
    }
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, Clear, DWORD Count, CONST D3DRECT *pRects,
                                DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetTransform(SerialiserType &ser, D3DTRANSFORMSTATETYPE State,
                                               CONST D3DMATRIX *pMatrix)
{
  SERIALISE_ELEMENT(State);
  SERIALISE_ELEMENT_OPT(pMatrix);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetTransform(State, pMatrix);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetTransform,
                                D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_MultiplyTransform(SerialiserType &ser, D3DTRANSFORMSTATETYPE State,
                                                    CONST D3DMATRIX *pMatrix)
{
  SERIALISE_ELEMENT(State);
  SERIALISE_ELEMENT_OPT(pMatrix);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    MultiplyTransform(State, pMatrix);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, MultiplyTransform,
                                D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetViewport(SerialiserType &ser, CONST D3DVIEWPORT9 *pViewport)
{
  SERIALISE_ELEMENT_OPT(pViewport);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetViewport(pViewport);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetViewport,
                                CONST D3DVIEWPORT9 *pViewport);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetMaterial(SerialiserType &ser, CONST D3DMATERIAL9 *pMaterial)
{
  SERIALISE_ELEMENT_OPT(pMaterial);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetMaterial(pMaterial);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetMaterial,
                                CONST D3DMATERIAL9 *pMaterial);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetLight(SerialiserType &ser, DWORD Index, CONST D3DLIGHT9 *pLight)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, Index);
  SERIALISE_ELEMENT_OPT(pLight);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetLight(Index, pLight);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetLight, DWORD Index,
                                CONST D3DLIGHT9 *pLight);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_LightEnable(SerialiserType &ser, DWORD Index, BOOL Enable)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, Index);
  SERIALISE_ELEMENT(Enable);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    LightEnable(Index, Enable);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, LightEnable, DWORD Index, BOOL Enable);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetClipPlane(SerialiserType &ser, DWORD Index, CONST float *pPlane)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, Index);
  SERIALISE_ELEMENT_ARRAY(pPlane, 4);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetClipPlane(Index, pPlane);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetClipPlane, DWORD Index,
                                CONST float *pPlane);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetRenderState(SerialiserType &ser, D3DRENDERSTATETYPE State,
                                                 DWORD Value)
{
  SERIALISE_ELEMENT(State);
  SERIALISE_ELEMENT_TYPED(uint32_t, Value);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetRenderState(State, Value);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetRenderState,
                                D3DRENDERSTATETYPE State, DWORD Value);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_BeginStateBlock(SerialiserType &ser)
{
  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    BeginStateBlock();
  }

  return true;
}

template bool WrappedD3DDevice9::Serialise_BeginStateBlock(ReadSerialiser &ser);
template bool WrappedD3DDevice9::Serialise_BeginStateBlock(WriteSerialiser &ser);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetClipStatus(SerialiserType &ser, CONST D3DCLIPSTATUS9 *pClipStatus)
{
  SERIALISE_ELEMENT_OPT(pClipStatus);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    BeginStateBlock();
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetClipStatus,
                                CONST D3DCLIPSTATUS9 *pClipStatus);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetTexture(SerialiserType &ser, DWORD Stage,
                                             IDirect3DBaseTexture9 *pTexture)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, Stage);
  SERIALISE_ELEMENT(pTexture);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetTexture(Stage, pTexture);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetTexture, DWORD Stage,
                                IDirect3DBaseTexture9 *pTexture);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetTextureStageState(SerialiserType &ser, DWORD Stage,
                                                       D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, Stage);
  SERIALISE_ELEMENT(Type);
  SERIALISE_ELEMENT_TYPED(uint32_t, Value);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetTextureStageState(Stage, Type, Value);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetTextureStageState, DWORD Stage,
                                D3DTEXTURESTAGESTATETYPE Type, DWORD Value);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetSamplerState(SerialiserType &ser, DWORD Sampler,
                                                  D3DSAMPLERSTATETYPE Type, DWORD Value)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, Sampler);
  SERIALISE_ELEMENT(Type);
  SERIALISE_ELEMENT_TYPED(uint32_t, Value);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetSamplerState(Sampler, Type, Value);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetSamplerState, DWORD Sampler,
                                D3DSAMPLERSTATETYPE Type, DWORD Value);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetPaletteEntries(SerialiserType &ser, UINT PaletteNumber,
                                                    CONST PALETTEENTRY *pEntries)
{
  SERIALISE_ELEMENT(PaletteNumber);
  SERIALISE_ELEMENT_ARRAY(pEntries, 256);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetPaletteEntries(PaletteNumber, pEntries);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetPaletteEntries, UINT PaletteNumber,
                                CONST PALETTEENTRY *pEntries);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetCurrentTexturePalette(SerialiserType &ser, UINT PaletteNumber)
{
  SERIALISE_ELEMENT(PaletteNumber);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetCurrentTexturePalette(PaletteNumber);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetCurrentTexturePalette,
                                UINT PaletteNumber);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetScissorRect(SerialiserType &ser, CONST RECT *pRect)
{
  SERIALISE_ELEMENT_OPT(pRect).TypedAs("RECT"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetScissorRect(pRect);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetScissorRect, CONST RECT *pRect);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetSoftwareVertexProcessing(SerialiserType &ser, BOOL bSoftware)
{
  SERIALISE_ELEMENT(bSoftware);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetSoftwareVertexProcessing(bSoftware);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetSoftwareVertexProcessing,
                                BOOL bSoftware);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetNPatchMode(SerialiserType &ser, float nSegments)
{
  SERIALISE_ELEMENT(nSegments);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetNPatchMode(nSegments);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetNPatchMode, float nSegments);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_DrawPrimitive(SerialiserType &ser, D3DPRIMITIVETYPE PrimitiveType,
                                                UINT StartVertex, UINT PrimitiveCount)
{
  SERIALISE_ELEMENT(PrimitiveType);
  SERIALISE_ELEMENT(StartVertex);
  SERIALISE_ELEMENT(PrimitiveCount);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayBeginScene();
    DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);

    AddDrawAction(ActionFlags::NoFlags, VertexCountForPrimitives(PrimitiveType, PrimitiveCount), 0,
                  0, StartVertex);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, DrawPrimitive,
                                D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                UINT PrimitiveCount);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_DrawIndexedPrimitive(SerialiserType &ser,
                                                       D3DPRIMITIVETYPE PrimitiveType,
                                                       INT BaseVertexIndex, UINT MinVertexIndex,
                                                       UINT NumVertices, UINT startIndex,
                                                       UINT primCount)
{
  SERIALISE_ELEMENT(PrimitiveType);
  SERIALISE_ELEMENT(BaseVertexIndex);
  SERIALISE_ELEMENT(MinVertexIndex);
  SERIALISE_ELEMENT(NumVertices);
  SERIALISE_ELEMENT(startIndex);
  SERIALISE_ELEMENT(primCount);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayBeginScene();
    DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex,
                         primCount);

    AddDrawAction(ActionFlags::Indexed, VertexCountForPrimitives(PrimitiveType, primCount),
                  startIndex, BaseVertexIndex, 0);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, DrawIndexedPrimitive,
                                D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
                                UINT MinVertexIndex, UINT NumVertices, UINT startIndex,
                                UINT primCount);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_ProcessVertices(SerialiserType &ser, UINT SrcStartIndex,
                                                  UINT DestIndex, UINT VertexCount,
                                                  IDirect3DVertexBuffer9 *pDestBuffer,
                                                  IDirect3DVertexDeclaration9 *pVertexDecl,
                                                  DWORD Flags)
{
  SERIALISE_ELEMENT(SrcStartIndex);
  SERIALISE_ELEMENT(DestIndex);
  SERIALISE_ELEMENT(VertexCount);
  SERIALISE_ELEMENT(pDestBuffer);
  SERIALISE_ELEMENT(pVertexDecl);
  SERIALISE_ELEMENT_TYPED(uint32_t, Flags);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayBeginScene();
    ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);

    if(IsLoading(m_State))
    {
      AddEvent();

      ActionDescription action;
      action.flags = ActionFlags::Drawcall;
      action.numIndices = VertexCount;
      action.vertexOffset = SrcStartIndex;
      action.numInstances = 1;
      action.copyDestination = GetResID(pDestBuffer);
      AddAction(action);
    }
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, ProcessVertices, UINT SrcStartIndex,
                                UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer,
                                IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetVertexDeclaration(SerialiserType &ser,
                                                       IDirect3DVertexDeclaration9 *pDecl)
{
  SERIALISE_ELEMENT(pDecl);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetVertexDeclaration(pDecl);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetVertexDeclaration,
                                IDirect3DVertexDeclaration9 *pDecl);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetFVF(SerialiserType &ser, DWORD FVF)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, FVF);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetFVF(FVF);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetFVF, DWORD FVF);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetVertexShader(SerialiserType &ser, IDirect3DVertexShader9 *pShader)
{
  SERIALISE_ELEMENT(pShader);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetVertexShader(pShader);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetVertexShader,
                                IDirect3DVertexShader9 *pShader);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetVertexShaderConstantF(SerialiserType &ser, UINT StartRegister,
                                                           CONST float *pConstantData,
                                                           UINT Vector4fCount)
{
  SERIALISE_ELEMENT(StartRegister);
  SERIALISE_ELEMENT_ARRAY(pConstantData, Vector4fCount * 4);
  SERIALISE_ELEMENT(Vector4fCount);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetVertexShaderConstantF,
                                UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetVertexShaderConstantI(SerialiserType &ser, UINT StartRegister,
                                                           CONST int *pConstantData,
                                                           UINT Vector4iCount)
{
  SERIALISE_ELEMENT(StartRegister);
  SERIALISE_ELEMENT_ARRAY(pConstantData, Vector4iCount * 4);
  SERIALISE_ELEMENT(Vector4iCount);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetVertexShaderConstantI,
                                UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetVertexShaderConstantB(SerialiserType &ser, UINT StartRegister,
                                                           CONST BOOL *pConstantData, UINT BoolCount)
{
  SERIALISE_ELEMENT(StartRegister);
  SERIALISE_ELEMENT_ARRAY(pConstantData, BoolCount);
  SERIALISE_ELEMENT(BoolCount);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetVertexShaderConstantB,
                                UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetStreamSource(SerialiserType &ser, UINT StreamNumber,
                                                  IDirect3DVertexBuffer9 *pStreamData,
                                                  UINT OffsetInBytes, UINT Stride)
{
  SERIALISE_ELEMENT(StreamNumber);
  SERIALISE_ELEMENT(pStreamData);
  SERIALISE_ELEMENT(OffsetInBytes);
  SERIALISE_ELEMENT(Stride);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetStreamSource, UINT StreamNumber,
                                IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetStreamSourceFreq(SerialiserType &ser, UINT StreamNumber,
                                                      UINT Setting)
{
  SERIALISE_ELEMENT(StreamNumber);
  SERIALISE_ELEMENT(Setting);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetStreamSourceFreq(StreamNumber, Setting);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetStreamSourceFreq, UINT StreamNumber,
                                UINT Setting);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetIndices(SerialiserType &ser, IDirect3DIndexBuffer9 *pIndexData)
{
  SERIALISE_ELEMENT(pIndexData);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetIndices(pIndexData);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetIndices,
                                IDirect3DIndexBuffer9 *pIndexData);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetPixelShader(SerialiserType &ser, IDirect3DPixelShader9 *pShader)
{
  SERIALISE_ELEMENT(pShader);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetPixelShader(pShader);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetPixelShader,
                                IDirect3DPixelShader9 *pShader);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetPixelShaderConstantF(SerialiserType &ser, UINT StartRegister,
                                                          CONST float *pConstantData,
                                                          UINT Vector4fCount)
{
  SERIALISE_ELEMENT(StartRegister);
  SERIALISE_ELEMENT_ARRAY(pConstantData, Vector4fCount * 4);
  SERIALISE_ELEMENT(Vector4fCount);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetPixelShaderConstantF,
                                UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetPixelShaderConstantI(SerialiserType &ser, UINT StartRegister,
                                                          CONST int *pConstantData,
                                                          UINT Vector4iCount)
{
  SERIALISE_ELEMENT(StartRegister);
  SERIALISE_ELEMENT_ARRAY(pConstantData, Vector4iCount * 4);
  SERIALISE_ELEMENT(Vector4iCount);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetPixelShaderConstantI,
                                UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetPixelShaderConstantB(SerialiserType &ser, UINT StartRegister,
                                                          CONST BOOL *pConstantData, UINT BoolCount)
{
  SERIALISE_ELEMENT(StartRegister);
  SERIALISE_ELEMENT_ARRAY(pConstantData, BoolCount);
  SERIALISE_ELEMENT(BoolCount);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetPixelShaderConstantB,
                                UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_DrawRectPatch(SerialiserType &ser, UINT Handle,
                                                CONST float *pNumSegs,
                                                CONST D3DRECTPATCH_INFO *pRectPatchInfo)
{
  SERIALISE_ELEMENT(Handle);
  SERIALISE_ELEMENT_ARRAY(pNumSegs, 4);
  SERIALISE_ELEMENT_OPT(pRectPatchInfo);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayBeginScene();
    DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);

    AddDrawAction(ActionFlags::NoFlags, 0, 0, 0, 0);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, DrawRectPatch, UINT Handle,
                                CONST float *pNumSegs, CONST D3DRECTPATCH_INFO *pRectPatchInfo);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_DrawTriPatch(SerialiserType &ser, UINT Handle,
                                               CONST float *pNumSegs,
                                               CONST D3DTRIPATCH_INFO *pTriPatchInfo)
{
  SERIALISE_ELEMENT(Handle);
  SERIALISE_ELEMENT_ARRAY(pNumSegs, 3);
  SERIALISE_ELEMENT_OPT(pTriPatchInfo);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayBeginScene();
    DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);

    AddDrawAction(ActionFlags::NoFlags, 0, 0, 0, 0);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, DrawTriPatch, UINT Handle,
                                CONST float *pNumSegs, CONST D3DTRIPATCH_INFO *pTriPatchInfo);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_DeletePatch(SerialiserType &ser, UINT Handle)
{
  SERIALISE_ELEMENT(Handle);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    DeletePatch(Handle);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, DeletePatch, UINT Handle);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_UpdateSurface(SerialiserType &ser,
                                                IDirect3DSurface9 *pSourceSurface,
                                                CONST RECT *pSourceRect,
                                                IDirect3DSurface9 *pDestinationSurface,
                                                CONST POINT *pDestPoint)
{
  SERIALISE_ELEMENT(pSourceSurface);
  SERIALISE_ELEMENT_OPT(pSourceRect).TypedAs("RECT"_lit);
  SERIALISE_ELEMENT(pDestinationSurface);
  SERIALISE_ELEMENT_OPT(pDestPoint);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);

    AddCopyAction(ActionFlags::Copy, pSourceSurface, pDestinationSurface);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, UpdateSurface,
                                IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect,
                                IDirect3DSurface9 *pDestinationSurface, CONST POINT *pDestPoint);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_UpdateTexture(SerialiserType &ser,
                                                IDirect3DBaseTexture9 *pSourceTexture,
                                                IDirect3DBaseTexture9 *pDestinationTexture)
{
  SERIALISE_ELEMENT(pSourceTexture);
  SERIALISE_ELEMENT(pDestinationTexture);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    UpdateTexture(pSourceTexture, pDestinationTexture);

    AddCopyAction(ActionFlags::Copy, pSourceTexture, pDestinationTexture);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, UpdateTexture,
                                IDirect3DBaseTexture9 *pSourceTexture,
                                IDirect3DBaseTexture9 *pDestinationTexture);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_GetRenderTargetData(SerialiserType &ser,
                                                      IDirect3DSurface9 *pRenderTarget,
                                                      IDirect3DSurface9 *pDestSurface)
{
  SERIALISE_ELEMENT(pRenderTarget);
  SERIALISE_ELEMENT(pDestSurface);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GetRenderTargetData(pRenderTarget, pDestSurface);

    AddCopyAction(ActionFlags::Copy, pRenderTarget, pDestSurface);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, GetRenderTargetData,
                                IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_StretchRect(SerialiserType &ser, IDirect3DSurface9 *pSourceSurface,
                                              CONST RECT *pSourceRect,
                                              IDirect3DSurface9 *pDestSurface,
                                              CONST RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter)
{
  SERIALISE_ELEMENT(pSourceSurface);
  SERIALISE_ELEMENT_OPT(pSourceRect).TypedAs("RECT"_lit);
  SERIALISE_ELEMENT(pDestSurface);
  SERIALISE_ELEMENT_OPT(pDestRect).TypedAs("RECT"_lit);
  SERIALISE_ELEMENT(Filter);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);

    D3DSURFACE_DESC srcDesc = {};
    if(pSourceSurface)
      pSourceSurface->GetDesc(&srcDesc);
    AddCopyAction(
        srcDesc.MultiSampleType != D3DMULTISAMPLE_NONE ? ActionFlags::Resolve : ActionFlags::Copy,
        pSourceSurface, pDestSurface);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, StretchRect,
                                IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect,
                                IDirect3DSurface9 *pDestSurface, CONST RECT *pDestRect,
                                D3DTEXTUREFILTERTYPE Filter);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_ColorFill(SerialiserType &ser, IDirect3DSurface9 *pSurface,
                                            CONST RECT *pRect, D3DCOLOR color)
{
  SERIALISE_ELEMENT(pSurface);
  SERIALISE_ELEMENT_OPT(pRect).TypedAs("RECT"_lit);
  SERIALISE_ELEMENT_TYPED(uint32_t, color);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ColorFill(pSurface, pRect, color);

    if(IsLoading(m_State))
    {
      AddEvent();

      ActionDescription action;
      action.flags = ActionFlags::Clear | ActionFlags::ClearColor;
      action.copyDestination = ContentsTextureID(pSurface);
      AddAction(action);
    }
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, ColorFill, IDirect3DSurface9 *pSurface,
                                CONST RECT *pRect, D3DCOLOR color);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_ComposeRects(SerialiserType &ser, IDirect3DSurface9 *pSrc,
                                               IDirect3DSurface9 *pDst,
                                               IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects,
                                               IDirect3DVertexBuffer9 *pDstRectDescs,
                                               D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset)
{
  SERIALISE_ELEMENT(pSrc);
  SERIALISE_ELEMENT(pDst);
  SERIALISE_ELEMENT(pSrcRectDescs);
  SERIALISE_ELEMENT(NumRects);
  SERIALISE_ELEMENT(pDstRectDescs);
  SERIALISE_ELEMENT(Operation);
  SERIALISE_ELEMENT(Xoffset);
  SERIALISE_ELEMENT(Yoffset);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(m_deviceEx)
      ComposeRects(pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs, Operation, Xoffset, Yoffset);

    AddCopyAction(ActionFlags::Copy, pSrc, pDst);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, ComposeRects, IDirect3DSurface9 *pSrc,
                                IDirect3DSurface9 *pDst, IDirect3DVertexBuffer9 *pSrcRectDescs,
                                UINT NumRects, IDirect3DVertexBuffer9 *pDstRectDescs,
                                D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SetConvolutionMonoKernel(SerialiserType &ser, UINT width,
                                                           UINT height, float *rows, float *columns)
{
  SERIALISE_ELEMENT(width);
  SERIALISE_ELEMENT(height);
  SERIALISE_ELEMENT_ARRAY(rows, width);
  SERIALISE_ELEMENT_ARRAY(columns, height);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(m_deviceEx)
      SetConvolutionMonoKernel(width, height, (float *)rows, (float *)columns);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SetConvolutionMonoKernel, UINT width,
                                UINT height, float *rows, float *columns);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_StateBlock_Capture(SerialiserType &ser,
                                                     IDirect3DStateBlock9 *pStateBlock)
{
  SERIALISE_ELEMENT(pStateBlock);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pStateBlock)
      pStateBlock->Capture();
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, StateBlock_Capture,
                                IDirect3DStateBlock9 *pStateBlock);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_StateBlock_Apply(SerialiserType &ser,
                                                   IDirect3DStateBlock9 *pStateBlock)
{
  SERIALISE_ELEMENT(pStateBlock);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pStateBlock)
      pStateBlock->Apply();
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, StateBlock_Apply,
                                IDirect3DStateBlock9 *pStateBlock);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_Query_Issue(SerialiserType &ser, IDirect3DQuery9 *pQuery,
                                              DWORD dwIssueFlags)
{
  SERIALISE_ELEMENT(pQuery);
  SERIALISE_ELEMENT_TYPED(uint32_t, dwIssueFlags);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pQuery)
      pQuery->Issue(dwIssueFlags);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, Query_Issue, IDirect3DQuery9 *pQuery,
                                DWORD dwIssueFlags);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_BaseTexture_GenerateMipSubLevels(SerialiserType &ser,
                                                                   IDirect3DBaseTexture9 *pTexture)
{
  SERIALISE_ELEMENT(pTexture);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pTexture)
      pTexture->GenerateMipSubLevels();

    if(IsLoading(m_State))
    {
      AddEvent();

      ActionDescription action;
      action.flags = ActionFlags::GenMips;
      action.copyDestination = GetResID(pTexture);
      AddAction(action);
    }
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, BaseTexture_GenerateMipSubLevels,
                                IDirect3DBaseTexture9 *pTexture);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_BaseTexture_SetAutoGenFilterType(SerialiserType &ser,
                                                                   IDirect3DBaseTexture9 *pTexture,
                                                                   D3DTEXTUREFILTERTYPE FilterType)
{
  SERIALISE_ELEMENT(pTexture);
  SERIALISE_ELEMENT(FilterType);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pTexture)
      pTexture->SetAutoGenFilterType(FilterType);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, BaseTexture_SetAutoGenFilterType,
                                IDirect3DBaseTexture9 *pTexture, D3DTEXTUREFILTERTYPE FilterType);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_BaseTexture_SetLOD(SerialiserType &ser,
                                                     IDirect3DBaseTexture9 *pTexture, DWORD LODNew)
{
  SERIALISE_ELEMENT(pTexture);
  SERIALISE_ELEMENT_TYPED(uint32_t, LODNew);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pTexture)
      pTexture->SetLOD(LODNew);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, BaseTexture_SetLOD,
                                IDirect3DBaseTexture9 *pTexture, DWORD LODNew);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateTexture(SerialiserType &ser, UINT Width, UINT Height,
                                                UINT Levels, DWORD Usage, D3DFORMAT Format,
                                                D3DPOOL Pool, IDirect3DTexture9 **ppTexture,
                                                HANDLE *pSharedHandle)
{
  SERIALISE_ELEMENT(Width);
  SERIALISE_ELEMENT(Height);
  SERIALISE_ELEMENT(Levels);
  SERIALISE_ELEMENT_TYPED(uint32_t, Usage);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(Pool);
  SERIALISE_ELEMENT_LOCAL(pTexture, GetResID(*ppTexture)).TypedAs("IDirect3DTexture9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DTexture9 *obj = NULL;
    HRESULT hr = ReplayCreate(pTexture, Pool, Usage, true, [&](D3DPOOL pool, DWORD usage) {
      return CreateTexture(Width, Height, Levels, usage, Format, pool, &obj, NULL);
    });
    if(!CheckReplayCreate(hr, obj, pTexture, "texture"))
      return false;

    AddReplayResource(pTexture, ResourceType::Texture,
                      (Usage & D3DUSAGE_RENDERTARGET)   ? "Render Target Texture"
                      : (Usage & D3DUSAGE_DEPTHSTENCIL) ? "Depth Texture"
                                                        : "Texture");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateTexture, UINT Width, UINT Height,
                                UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateVolumeTexture(SerialiserType &ser, UINT Width, UINT Height,
                                                      UINT Depth, UINT Levels, DWORD Usage,
                                                      D3DFORMAT Format, D3DPOOL Pool,
                                                      IDirect3DVolumeTexture9 **ppVolumeTexture,
                                                      HANDLE *pSharedHandle)
{
  SERIALISE_ELEMENT(Width);
  SERIALISE_ELEMENT(Height);
  SERIALISE_ELEMENT(Depth);
  SERIALISE_ELEMENT(Levels);
  SERIALISE_ELEMENT_TYPED(uint32_t, Usage);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(Pool);
  SERIALISE_ELEMENT_LOCAL(pVolumeTexture, GetResID(*ppVolumeTexture))
      .TypedAs("IDirect3DVolumeTexture9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DVolumeTexture9 *obj = NULL;
    HRESULT hr = ReplayCreate(pVolumeTexture, Pool, Usage, true, [&](D3DPOOL pool, DWORD usage) {
      return CreateVolumeTexture(Width, Height, Depth, Levels, usage, Format, pool, &obj, NULL);
    });
    if(!CheckReplayCreate(hr, obj, pVolumeTexture, "volume texture"))
      return false;

    AddReplayResource(pVolumeTexture, ResourceType::Texture, "Volume Texture");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateVolumeTexture, UINT Width,
                                UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture,
                                HANDLE *pSharedHandle);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateCubeTexture(SerialiserType &ser, UINT EdgeLength, UINT Levels,
                                                    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                    IDirect3DCubeTexture9 **ppCubeTexture,
                                                    HANDLE *pSharedHandle)
{
  SERIALISE_ELEMENT(EdgeLength);
  SERIALISE_ELEMENT(Levels);
  SERIALISE_ELEMENT_TYPED(uint32_t, Usage);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(Pool);
  SERIALISE_ELEMENT_LOCAL(pCubeTexture, GetResID(*ppCubeTexture))
      .TypedAs("IDirect3DCubeTexture9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DCubeTexture9 *obj = NULL;
    HRESULT hr = ReplayCreate(pCubeTexture, Pool, Usage, true, [&](D3DPOOL pool, DWORD usage) {
      return CreateCubeTexture(EdgeLength, Levels, usage, Format, pool, &obj, NULL);
    });
    if(!CheckReplayCreate(hr, obj, pCubeTexture, "cube texture"))
      return false;

    AddReplayResource(
        pCubeTexture, ResourceType::Texture,
        (Usage & D3DUSAGE_RENDERTARGET) ? "Render Target Cube Texture" : "Cube Texture");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateCubeTexture, UINT EdgeLength,
                                UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateVertexBuffer(SerialiserType &ser, UINT Length, DWORD Usage,
                                                     DWORD FVF, D3DPOOL Pool,
                                                     IDirect3DVertexBuffer9 **ppVertexBuffer,
                                                     HANDLE *pSharedHandle)
{
  SERIALISE_ELEMENT(Length);
  SERIALISE_ELEMENT_TYPED(uint32_t, Usage);
  SERIALISE_ELEMENT_TYPED(uint32_t, FVF);
  SERIALISE_ELEMENT(Pool);
  SERIALISE_ELEMENT_LOCAL(pVertexBuffer, GetResID(*ppVertexBuffer))
      .TypedAs("IDirect3DVertexBuffer9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DVertexBuffer9 *obj = NULL;
    HRESULT hr = ReplayCreate(pVertexBuffer, Pool, Usage, false, [&](D3DPOOL pool, DWORD usage) {
      return CreateVertexBuffer(Length, usage & ~D3DUSAGE_WRITEONLY, FVF, pool, &obj, NULL);
    });
    if(!CheckReplayCreate(hr, obj, pVertexBuffer, "vertex buffer"))
      return false;

    AddReplayResource(pVertexBuffer, ResourceType::Buffer, "Vertex Buffer");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateVertexBuffer, UINT Length,
                                DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateIndexBuffer(SerialiserType &ser, UINT Length, DWORD Usage,
                                                    D3DFORMAT Format, D3DPOOL Pool,
                                                    IDirect3DIndexBuffer9 **ppIndexBuffer,
                                                    HANDLE *pSharedHandle)
{
  SERIALISE_ELEMENT(Length);
  SERIALISE_ELEMENT_TYPED(uint32_t, Usage);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(Pool);
  SERIALISE_ELEMENT_LOCAL(pIndexBuffer, GetResID(*ppIndexBuffer))
      .TypedAs("IDirect3DIndexBuffer9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DIndexBuffer9 *obj = NULL;
    HRESULT hr = ReplayCreate(pIndexBuffer, Pool, Usage, false, [&](D3DPOOL pool, DWORD usage) {
      return CreateIndexBuffer(Length, usage & ~D3DUSAGE_WRITEONLY, Format, pool, &obj, NULL);
    });
    if(!CheckReplayCreate(hr, obj, pIndexBuffer, "index buffer"))
      return false;

    AddReplayResource(pIndexBuffer, ResourceType::Buffer, "Index Buffer");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateIndexBuffer, UINT Length,
                                DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateRenderTarget(
    SerialiserType &ser, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
  SERIALISE_ELEMENT(Width);
  SERIALISE_ELEMENT(Height);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(MultiSample);
  SERIALISE_ELEMENT_TYPED(uint32_t, MultisampleQuality);
  SERIALISE_ELEMENT(Lockable);
  SERIALISE_ELEMENT_LOCAL(pSurface, GetResID(*ppSurface)).TypedAs("IDirect3DSurface9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DSurface9 *obj = NULL;
    m_NextWrapperID = pSurface;
    HRESULT hr = CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality,
                                    Lockable, &obj, NULL);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pSurface, "render target"))
      return false;

    AddReplayResource(pSurface, ResourceType::Texture, "Render Target");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateRenderTarget, UINT Width,
                                UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                DWORD MultisampleQuality, BOOL Lockable,
                                IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateDepthStencilSurface(
    SerialiserType &ser, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
  SERIALISE_ELEMENT(Width);
  SERIALISE_ELEMENT(Height);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(MultiSample);
  SERIALISE_ELEMENT_TYPED(uint32_t, MultisampleQuality);
  SERIALISE_ELEMENT(Discard);
  SERIALISE_ELEMENT_LOCAL(pSurface, GetResID(*ppSurface)).TypedAs("IDirect3DSurface9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DSurface9 *obj = NULL;
    m_NextWrapperID = pSurface;
    HRESULT hr = CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality,
                                           Discard, &obj, NULL);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pSurface, "depth stencil"))
      return false;

    AddReplayResource(pSurface, ResourceType::Texture, "Depth Stencil");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateDepthStencilSurface, UINT Width,
                                UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                DWORD MultisampleQuality, BOOL Discard,
                                IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateOffscreenPlainSurface(SerialiserType &ser, UINT Width,
                                                              UINT Height, D3DFORMAT Format,
                                                              D3DPOOL Pool,
                                                              IDirect3DSurface9 **ppSurface,
                                                              HANDLE *pSharedHandle)
{
  SERIALISE_ELEMENT(Width);
  SERIALISE_ELEMENT(Height);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(Pool);
  SERIALISE_ELEMENT_LOCAL(pSurface, GetResID(*ppSurface)).TypedAs("IDirect3DSurface9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(Pool == D3DPOOL_MANAGED)
      Pool = D3DPOOL_DEFAULT;

    IDirect3DSurface9 *obj = NULL;
    m_NextWrapperID = pSurface;
    HRESULT hr = CreateOffscreenPlainSurface(Width, Height, Format, Pool, &obj, NULL);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pSurface, "offscreen plain surface"))
      return false;

    AddReplayResource(pSurface, ResourceType::Texture, "Offscreen Surface");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateOffscreenPlainSurface, UINT Width,
                                UINT Height, D3DFORMAT Format, D3DPOOL Pool,
                                IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateRenderTargetEx(SerialiserType &ser, UINT Width, UINT Height,
                                                       D3DFORMAT Format,
                                                       D3DMULTISAMPLE_TYPE MultiSample,
                                                       DWORD MultisampleQuality, BOOL Lockable,
                                                       IDirect3DSurface9 **ppSurface,
                                                       HANDLE *pSharedHandle, DWORD Usage)
{
  SERIALISE_ELEMENT(Width);
  SERIALISE_ELEMENT(Height);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(MultiSample);
  SERIALISE_ELEMENT_TYPED(uint32_t, MultisampleQuality);
  SERIALISE_ELEMENT(Lockable);
  SERIALISE_ELEMENT_LOCAL(pSurface, GetResID(*ppSurface)).TypedAs("IDirect3DSurface9 *"_lit);
  SERIALISE_ELEMENT_TYPED(uint32_t, Usage);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DSurface9 *obj = NULL;
    m_NextWrapperID = pSurface;
    HRESULT hr = CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality,
                                      Lockable, &obj, NULL, Usage);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pSurface, "render target"))
      return false;

    AddReplayResource(pSurface, ResourceType::Texture, "Render Target");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateRenderTargetEx, UINT Width,
                                UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                DWORD MultisampleQuality, BOOL Lockable,
                                IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateOffscreenPlainSurfaceEx(SerialiserType &ser, UINT Width,
                                                                UINT Height, D3DFORMAT Format,
                                                                D3DPOOL Pool,
                                                                IDirect3DSurface9 **ppSurface,
                                                                HANDLE *pSharedHandle, DWORD Usage)
{
  SERIALISE_ELEMENT(Width);
  SERIALISE_ELEMENT(Height);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(Pool);
  SERIALISE_ELEMENT_LOCAL(pSurface, GetResID(*ppSurface)).TypedAs("IDirect3DSurface9 *"_lit);
  SERIALISE_ELEMENT_TYPED(uint32_t, Usage);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DSurface9 *obj = NULL;
    m_NextWrapperID = pSurface;
    HRESULT hr = CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, &obj, NULL, Usage);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pSurface, "offscreen plain surface"))
      return false;

    AddReplayResource(pSurface, ResourceType::Texture, "Offscreen Surface");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateOffscreenPlainSurfaceEx,
                                UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
                                IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateDepthStencilSurfaceEx(SerialiserType &ser, UINT Width,
                                                              UINT Height, D3DFORMAT Format,
                                                              D3DMULTISAMPLE_TYPE MultiSample,
                                                              DWORD MultisampleQuality, BOOL Discard,
                                                              IDirect3DSurface9 **ppSurface,
                                                              HANDLE *pSharedHandle, DWORD Usage)
{
  SERIALISE_ELEMENT(Width);
  SERIALISE_ELEMENT(Height);
  SERIALISE_ELEMENT(Format);
  SERIALISE_ELEMENT(MultiSample);
  SERIALISE_ELEMENT_TYPED(uint32_t, MultisampleQuality);
  SERIALISE_ELEMENT(Discard);
  SERIALISE_ELEMENT_LOCAL(pSurface, GetResID(*ppSurface)).TypedAs("IDirect3DSurface9 *"_lit);
  SERIALISE_ELEMENT_TYPED(uint32_t, Usage);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DSurface9 *obj = NULL;
    m_NextWrapperID = pSurface;
    HRESULT hr = CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality,
                                             Discard, &obj, NULL, Usage);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pSurface, "depth stencil"))
      return false;

    AddReplayResource(pSurface, ResourceType::Texture, "Depth Stencil");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateDepthStencilSurfaceEx, UINT Width,
                                UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                DWORD MultisampleQuality, BOOL Discard,
                                IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateQuery(SerialiserType &ser, D3DQUERYTYPE Type,
                                              IDirect3DQuery9 **ppQuery)
{
  SERIALISE_ELEMENT(Type);
  SERIALISE_ELEMENT_LOCAL(pQuery, GetResID(*ppQuery)).TypedAs("IDirect3DQuery9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DQuery9 *obj = NULL;
    m_NextWrapperID = pQuery;
    HRESULT hr = CreateQuery(Type, &obj);
    m_NextWrapperID = ResourceId();

    // queries the replay GPU doesn't support aren't needed to replay the frame
    if(FAILED(hr) || obj == NULL)
    {
      RDCWARN("Couldn't create query %s of type %s: %s", ToStr(pQuery).c_str(), ToStr(Type).c_str(),
              ToStr(hr).c_str());
    }
    else
    {
      m_ReplayObjects.push_back(obj);
      AddReplayResource(pQuery, ResourceType::Query, "Query");
    }
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateQuery, D3DQUERYTYPE Type,
                                IDirect3DQuery9 **ppQuery);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateStateBlock(SerialiserType &ser, D3DSTATEBLOCKTYPE Type,
                                                   IDirect3DStateBlock9 **ppSB)
{
  SERIALISE_ELEMENT(Type);
  SERIALISE_ELEMENT_LOCAL(pSB, GetResID(*ppSB)).TypedAs("IDirect3DStateBlock9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DStateBlock9 *obj = NULL;
    m_NextWrapperID = pSB;
    HRESULT hr = CreateStateBlock(Type, &obj);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pSB, "state block"))
      return false;

    AddReplayResource(pSB, ResourceType::StateObject, "State Block");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateStateBlock,
                                D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_EndStateBlock(SerialiserType &ser, IDirect3DStateBlock9 **ppSB)
{
  SERIALISE_ELEMENT_LOCAL(pSB, GetResID(*ppSB)).TypedAs("IDirect3DStateBlock9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DStateBlock9 *obj = NULL;

    // during the frame, the state block already exists: the recording only keeps the calls in it
    // from changing the device
    const bool exists = m_ResourceManager->HasResource(pSB);

    if(!exists)
      m_NextWrapperID = pSB;
    HRESULT hr = EndStateBlock(&obj);
    m_NextWrapperID = ResourceId();

    if(exists)
    {
      SAFE_RELEASE(obj);
    }
    else
    {
      if(!CheckReplayCreate(hr, obj, pSB, "state block"))
        return false;

      AddReplayResource(pSB, ResourceType::StateObject, "State Block");
    }
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, EndStateBlock,
                                IDirect3DStateBlock9 **ppSB);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateAdditionalSwapChain(
    SerialiserType &ser, D3DPRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DSwapChain9 **pSwapChain)
{
  SERIALISE_ELEMENT_OPT(pPresentationParameters);
  SERIALISE_ELEMENT_LOCAL(SwapChain, GetResID(*pSwapChain)).TypedAs("IDirect3DSwapChain9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    D3DPRESENT_PARAMETERS params = {};
    if(pPresentationParameters)
      params = *pPresentationParameters;
    PatchReplayPresentParams(params);

    IDirect3DSwapChain9 *obj = NULL;
    m_NextWrapperID = SwapChain;
    HRESULT hr = CreateAdditionalSwapChain(&params, &obj);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, SwapChain, "swapchain"))
      return false;

    AddReplayResource(SwapChain, ResourceType::Unknown, "Swapchain");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateAdditionalSwapChain,
                                D3DPRESENT_PARAMETERS *pPresentationParameters,
                                IDirect3DSwapChain9 **pSwapChain);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_Texture_GetSurfaceLevel(SerialiserType &ser,
                                                          IDirect3DTexture9 *pTexture, UINT Level,
                                                          IDirect3DSurface9 *_pSurfaceLevel)
{
  SERIALISE_ELEMENT(pTexture);
  SERIALISE_ELEMENT(Level);
  SERIALISE_ELEMENT_LOCAL(pSurfaceLevel, GetResID(_pSurfaceLevel))
      .TypedAs("IDirect3DSurface9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pTexture)
      ReplayGetChild(GetResID(pTexture), pSurfaceLevel, ResourceType::Texture, "Surface",
                     [&]() -> IUnknown * {
                       IDirect3DSurface9 *obj = NULL;
                       pTexture->GetSurfaceLevel(Level, &obj);
                       return obj;
                     });
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, Texture_GetSurfaceLevel,
                                IDirect3DTexture9 *pTexture, UINT Level,
                                IDirect3DSurface9 *_pSurfaceLevel);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CubeTexture_GetCubeMapSurface(SerialiserType &ser,
                                                                IDirect3DCubeTexture9 *pCubeTexture,
                                                                D3DCUBEMAP_FACES FaceType, UINT Level,
                                                                IDirect3DSurface9 *_pCubeMapSurface)
{
  SERIALISE_ELEMENT(pCubeTexture);
  SERIALISE_ELEMENT(FaceType);
  SERIALISE_ELEMENT(Level);
  SERIALISE_ELEMENT_LOCAL(pCubeMapSurface, GetResID(_pCubeMapSurface))
      .TypedAs("IDirect3DSurface9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pCubeTexture)
      ReplayGetChild(GetResID(pCubeTexture), pCubeMapSurface, ResourceType::Texture, "Surface",
                     [&]() -> IUnknown * {
                       IDirect3DSurface9 *obj = NULL;
                       pCubeTexture->GetCubeMapSurface(FaceType, Level, &obj);
                       return obj;
                     });
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CubeTexture_GetCubeMapSurface,
                                IDirect3DCubeTexture9 *pCubeTexture, D3DCUBEMAP_FACES FaceType,
                                UINT Level, IDirect3DSurface9 *_pCubeMapSurface);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_VolumeTexture_GetVolumeLevel(SerialiserType &ser,
                                                               IDirect3DVolumeTexture9 *pVolumeTexture,
                                                               UINT Level,
                                                               IDirect3DVolume9 *_pVolumeLevel)
{
  SERIALISE_ELEMENT(pVolumeTexture);
  SERIALISE_ELEMENT(Level);
  SERIALISE_ELEMENT_LOCAL(pVolumeLevel, GetResID(_pVolumeLevel)).TypedAs("IDirect3DVolume9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pVolumeTexture)
      ReplayGetChild(GetResID(pVolumeTexture), pVolumeLevel, ResourceType::Texture, "Volume",
                     [&]() -> IUnknown * {
                       IDirect3DVolume9 *obj = NULL;
                       pVolumeTexture->GetVolumeLevel(Level, &obj);
                       return obj;
                     });
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, VolumeTexture_GetVolumeLevel,
                                IDirect3DVolumeTexture9 *pVolumeTexture, UINT Level,
                                IDirect3DVolume9 *_pVolumeLevel);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SwapChain_GetBackBuffer(SerialiserType &ser,
                                                          IDirect3DSwapChain9 *pSwapChain,
                                                          UINT iBackBuffer,
                                                          IDirect3DSurface9 *_pBackBuffer)
{
  SERIALISE_ELEMENT(pSwapChain);
  SERIALISE_ELEMENT(iBackBuffer);
  SERIALISE_ELEMENT_LOCAL(pBackBuffer, GetResID(_pBackBuffer)).TypedAs("IDirect3DSurface9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(pSwapChain)
      ReplayGetChild(GetResID(pSwapChain), pBackBuffer, ResourceType::SwapchainImage, "Backbuffer",
                     [&]() -> IUnknown * {
                       IDirect3DSurface9 *obj = NULL;
                       pSwapChain->GetBackBuffer(iBackBuffer, D3DBACKBUFFER_TYPE_MONO, &obj);
                       return obj;
                     });
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SwapChain_GetBackBuffer,
                                IDirect3DSwapChain9 *pSwapChain, UINT iBackBuffer,
                                IDirect3DSurface9 *_pBackBuffer);

///////////////////////////////////////////////////////////////////////////////
// hand-written

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CaptureScope(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(frameNumber, m_CapturedFrames.back().frameNumber);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading() && m_Replay)
  {
    m_Replay->WriteFrameRecord().frameInfo.frameNumber = frameNumber;
    RDCEraseEl(m_Replay->WriteFrameRecord().frameInfo.stats);
  }

  return true;
}

template bool WrappedD3DDevice9::Serialise_CaptureScope(ReadSerialiser &ser);
template bool WrappedD3DDevice9::Serialise_CaptureScope(WriteSerialiser &ser);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_DeviceInitialisation(SerialiserType &ser)
{
  SERIALISE_ELEMENT_LOCAL(Device, m_DeviceResourceID).TypedAs("IDirect3DDevice9 *"_lit);
  // the implicit swapchains and their presentation parameters, as they are when the capture starts
  // (they may have changed since device creation with Reset)
  SERIALISE_ELEMENT_LOCAL(ImplicitSwapChains, GetImplicitSwapChainIDs());
  SERIALISE_ELEMENT_LOCAL(PresentParameters, GetImplicitPresentParams());
  SERIALISE_ELEMENT_LOCAL(AutoDepthStencil, GetResID((IDirect3DSurface9 *)m_AutoDepthStencil))
      .TypedAs("IDirect3DSurface9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    AddReplayResource(Device, ResourceType::Device, "Device");

    // the implicit swapchains become additional swapchains on the hidden window
    for(size_t i = 0; i < ImplicitSwapChains.size() && i < PresentParameters.size(); i++)
    {
      D3DPRESENT_PARAMETERS params = PresentParameters[i];
      PatchReplayPresentParams(params);

      IDirect3DSwapChain9 *obj = NULL;
      m_NextWrapperID = ImplicitSwapChains[i];
      HRESULT hr = CreateAdditionalSwapChain(&params, &obj);
      m_NextWrapperID = ResourceId();
      if(!CheckReplayCreate(hr, obj, ImplicitSwapChains[i], "implicit swapchain"))
        return false;

      m_ReplaySwapChains.push_back(obj);
      AddReplayResource(ImplicitSwapChains[i], ResourceType::Unknown, "Swapchain");
      DerivedReplayResource(Device, ImplicitSwapChains[i]);
    }

    // and the automatic depth stencil a standalone one
    if(AutoDepthStencil != ResourceId() && !PresentParameters.empty())
    {
      const D3DPRESENT_PARAMETERS &params = PresentParameters[0];

      IDirect3DSurface9 *obj = NULL;
      m_NextWrapperID = AutoDepthStencil;
      HRESULT hr = CreateDepthStencilSurface(
          params.BackBufferWidth, params.BackBufferHeight, params.AutoDepthStencilFormat,
          params.MultiSampleType, params.MultiSampleQuality,
          (params.Flags & D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL) ? TRUE : FALSE, &obj, NULL);
      m_NextWrapperID = ResourceId();
      if(!CheckReplayCreate(hr, obj, AutoDepthStencil, "automatic depth stencil"))
        return false;

      AddReplayResource(AutoDepthStencil, ResourceType::Texture, "Auto Depth Stencil");
      DerivedReplayResource(Device, AutoDepthStencil);
    }
  }

  return true;
}

template bool WrappedD3DDevice9::Serialise_DeviceInitialisation(ReadSerialiser &ser);
template bool WrappedD3DDevice9::Serialise_DeviceInitialisation(WriteSerialiser &ser);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_BeginCaptureFrame(SerialiserType &ser)
{
  // the objects bound when the frame starts. Textures are indexed by packed slot: pixel samplers
  // 0-15, then the displacement map sampler, then vertex samplers 0-3.
  SERIALISE_ELEMENT_LOCAL(Textures, ArrayOf(m_Bindings.textures));

  rdcarray<IDirect3DVertexBuffer9 *> buffers;
  rdcarray<uint32_t> offsets, strides;
  if(ser.IsWriting())
  {
    for(const D3D9StreamBinding &s : m_Bindings.streams)
    {
      buffers.push_back(s.buffer);
      offsets.push_back(s.offset);
      strides.push_back(s.stride);
    }
  }
  SERIALISE_ELEMENT_LOCAL(StreamSources, buffers);
  SERIALISE_ELEMENT_LOCAL(StreamOffsets, offsets);
  SERIALISE_ELEMENT_LOCAL(StreamStrides, strides);

  SERIALISE_ELEMENT_LOCAL(Indices, m_Bindings.indices);
  SERIALISE_ELEMENT_LOCAL(VertexShader, m_Bindings.vs);
  SERIALISE_ELEMENT_LOCAL(PixelShader, m_Bindings.ps);
  SERIALISE_ELEMENT_LOCAL(VertexDeclaration, m_Bindings.decl);
  SERIALISE_ELEMENT_LOCAL(DeclarationFromFVF, m_Bindings.declFromFVF);
  SERIALISE_ELEMENT_LOCAL(RenderTargets, ArrayOf(m_RenderTargets));
  SERIALISE_ELEMENT_LOCAL(DepthStencil, m_DepthStencil);

  // everything else: render states, transforms, lights, shader constants...
  SERIALISE_ELEMENT_LOCAL(State, m_FrameStartState);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    // targets first, since setting render target 0 resets the viewport and scissor rect. It can't
    // be NULL.
    for(size_t i = 0; i < RenderTargets.size() && i < D3D9_NUM_RENDER_TARGETS; i++)
      if(i > 0 || RenderTargets[i] != NULL)
        SetRenderTarget((DWORD)i, RenderTargets[i]);
    SetDepthStencilSurface(DepthStencil);

    ApplyReplayDeviceState(State);

    for(size_t slot = 0; slot < Textures.size() && slot < D3D9_NUM_TEXTURE_SLOTS; slot++)
      SetTexture(D3D9SlotToSampler((uint32_t)slot), Textures[slot]);
    for(size_t i = 0; i < StreamSources.size() && i < D3D9_NUM_STREAMS; i++)
      SetStreamSource((UINT)i, StreamSources[i], i < StreamOffsets.size() ? StreamOffsets[i] : 0,
                      i < StreamStrides.size() ? StreamStrides[i] : 0);
    SetIndices(Indices);
    SetVertexShader(VertexShader);
    SetPixelShader(PixelShader);
    if(DeclarationFromFVF)
      SetFVF(State.FVF);
    else
      SetVertexDeclaration(VertexDeclaration);
  }

  return true;
}

template bool WrappedD3DDevice9::Serialise_BeginCaptureFrame(ReadSerialiser &ser);
template bool WrappedD3DDevice9::Serialise_BeginCaptureFrame(WriteSerialiser &ser);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateVertexDeclaration(SerialiserType &ser,
                                                          CONST D3DVERTEXELEMENT9 *pVertexElements,
                                                          IDirect3DVertexDeclaration9 **ppDecl)
{
  // the elements including the terminating D3DDECL_END()
  uint32_t numElements = 0;
  if(ser.IsWriting() && pVertexElements)
  {
    while(numElements < MAXD3DDECLLENGTH && pVertexElements[numElements].Stream != 0xFF)
      numElements++;
    numElements++;
  }

  SERIALISE_ELEMENT_ARRAY(pVertexElements, numElements);
  SERIALISE_ELEMENT_LOCAL(pDecl, GetResID(*ppDecl)).TypedAs("IDirect3DVertexDeclaration9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DVertexDeclaration9 *obj = NULL;
    m_NextWrapperID = pDecl;
    HRESULT hr = CreateVertexDeclaration(pVertexElements, &obj);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pDecl, "vertex declaration"))
      return false;

    AddReplayResource(pDecl, ResourceType::StateObject, "Vertex Declaration");
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateVertexDeclaration,
                                CONST D3DVERTEXELEMENT9 *pVertexElements,
                                IDirect3DVertexDeclaration9 **ppDecl);

// shader bytecode has no explicit length, so take it back from the created shader. The length is
// serialised on its own because SERIALISE_ELEMENT_ARRAY takes the count by value, and the replay
// needs it back.
template <typename SerialiserType, typename ShaderType>
static void SerialiseShaderBytecode(SerialiserType &ser, ShaderType *shader, rdcarray<DWORD> &read)
{
  rdcarray<byte> storage;
  const void *Bytecode = NULL;
  uint64_t BytecodeLength = 0;

  if(ser.IsWriting() && shader)
  {
    UINT size = 0;
    shader->GetFunction(NULL, &size);
    storage.resize(size);
    if(size > 0)
      shader->GetFunction(storage.data(), &size);
    Bytecode = storage.data();
    BytecodeLength = size;
  }

  SERIALISE_ELEMENT(BytecodeLength).Hidden();
  SERIALISE_ELEMENT_ARRAY(Bytecode, BytecodeLength).Important();

  if(ser.IsReading() && Bytecode && BytecodeLength >= sizeof(DWORD))
  {
    read.resize(size_t(BytecodeLength / sizeof(DWORD)));
    memcpy(read.data(), Bytecode, read.byteSize());
  }
}

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreateVertexShader(SerialiserType &ser, CONST DWORD *pFunction,
                                                     IDirect3DVertexShader9 **ppShader)
{
  rdcarray<DWORD> code;
  SerialiseShaderBytecode(ser, ser.IsWriting() ? *ppShader : (IDirect3DVertexShader9 *)NULL, code);
  SERIALISE_ELEMENT_LOCAL(pShader, GetResID(*ppShader)).TypedAs("IDirect3DVertexShader9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DVertexShader9 *obj = NULL;
    m_NextWrapperID = pShader;
    HRESULT hr = code.empty() ? E_INVALIDARG : CreateVertexShader(code.data(), &obj);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pShader, "vertex shader"))
      return false;

    AddReplayResource(pShader, ResourceType::Shader, "Vertex Shader");
    if(m_Replay)
      m_Replay->AddShader(pShader, (const uint32_t *)code.data(), code.size());
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreateVertexShader,
                                CONST DWORD *pFunction, IDirect3DVertexShader9 **ppShader);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CreatePixelShader(SerialiserType &ser, CONST DWORD *pFunction,
                                                    IDirect3DPixelShader9 **ppShader)
{
  rdcarray<DWORD> code;
  SerialiseShaderBytecode(ser, ser.IsWriting() ? *ppShader : (IDirect3DPixelShader9 *)NULL, code);
  SERIALISE_ELEMENT_LOCAL(pShader, GetResID(*ppShader)).TypedAs("IDirect3DPixelShader9 *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    IDirect3DPixelShader9 *obj = NULL;
    m_NextWrapperID = pShader;
    HRESULT hr = code.empty() ? E_INVALIDARG : CreatePixelShader(code.data(), &obj);
    m_NextWrapperID = ResourceId();
    if(!CheckReplayCreate(hr, obj, pShader, "pixel shader"))
      return false;

    AddReplayResource(pShader, ResourceType::Shader, "Pixel Shader");
    if(m_Replay)
      m_Replay->AddShader(pShader, (const uint32_t *)code.data(), code.size());
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, CreatePixelShader,
                                CONST DWORD *pFunction, IDirect3DPixelShader9 **ppShader);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_DrawPrimitiveUP(SerialiserType &ser,
                                                  D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                                  CONST void *pVertexStreamZeroData,
                                                  UINT VertexStreamZeroStride)
{
  SERIALISE_ELEMENT(PrimitiveType);
  SERIALISE_ELEMENT(PrimitiveCount).Important();
  SERIALISE_ELEMENT(VertexStreamZeroStride);

  uint64_t VertexDataLength = 0;
  if(ser.IsWriting())
    VertexDataLength =
        uint64_t(VertexCountForPrimitives(PrimitiveType, PrimitiveCount)) * VertexStreamZeroStride;

  SERIALISE_ELEMENT_ARRAY(pVertexStreamZeroData, VertexDataLength);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayBeginScene();
    if(pVertexStreamZeroData)
      DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);

    AddDrawAction(ActionFlags::NoFlags, VertexCountForPrimitives(PrimitiveType, PrimitiveCount), 0,
                  0, 0);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, DrawPrimitiveUP,
                                D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_DrawIndexedPrimitiveUP(
    SerialiserType &ser, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
    UINT PrimitiveCount, CONST void *pIndexData, D3DFORMAT IndexDataFormat,
    CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
  SERIALISE_ELEMENT(PrimitiveType);
  SERIALISE_ELEMENT(MinVertexIndex);
  SERIALISE_ELEMENT(NumVertices);
  SERIALISE_ELEMENT(PrimitiveCount).Important();
  SERIALISE_ELEMENT(IndexDataFormat);
  SERIALISE_ELEMENT(VertexStreamZeroStride);

  uint64_t IndexDataLength = 0;
  uint64_t VertexDataLength = 0;
  if(ser.IsWriting())
  {
    IndexDataLength = uint64_t(VertexCountForPrimitives(PrimitiveType, PrimitiveCount)) *
                      (IndexDataFormat == D3DFMT_INDEX32 ? 4 : 2);
    // indices are relative to the start of the vertex data, so include everything up to the last
    // vertex used
    VertexDataLength = uint64_t(MinVertexIndex + NumVertices) * VertexStreamZeroStride;
  }

  SERIALISE_ELEMENT_ARRAY(pIndexData, IndexDataLength);
  SERIALISE_ELEMENT_ARRAY(pVertexStreamZeroData, VertexDataLength);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ReplayBeginScene();
    if(pIndexData && pVertexStreamZeroData)
      DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData,
                             IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);

    AddDrawAction(ActionFlags::Indexed, VertexCountForPrimitives(PrimitiveType, PrimitiveCount), 0,
                  0, 0);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, DrawIndexedPrimitiveUP,
                                D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
                                UINT NumVertices, UINT PrimitiveCount, CONST void *pIndexData,
                                D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData,
                                UINT VertexStreamZeroStride);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_Present(SerialiserType &ser, CONST RECT *pSourceRect,
                                          CONST RECT *pDestRect, HWND hDestWindowOverride,
                                          CONST RGNDATA *pDirtyRegion)
{
  SERIALISE_ELEMENT_OPT(pSourceRect).TypedAs("RECT"_lit);
  SERIALISE_ELEMENT_OPT(pDestRect).TypedAs("RECT"_lit);
  // for information only
  SERIALISE_ELEMENT_LOCAL(DestWindowOverride, (uint64_t)(uintptr_t)hDestWindowOverride);
  SERIALISE_ELEMENT_LOCAL(HasDirtyRegion, pDirtyRegion != NULL);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading() && IsLoading(m_State))
  {
    AddEvent();

    ActionDescription action;
    action.flags = ActionFlags::Present;
    action.copyDestination =
        ReplayBackBufferID(m_ReplaySwapChains.empty() ? NULL : m_ReplaySwapChains[0]);
    AddAction(action);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, Present, CONST RECT *pSourceRect,
                                CONST RECT *pDestRect, HWND hDestWindowOverride,
                                CONST RGNDATA *pDirtyRegion);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_PresentEx(SerialiserType &ser, CONST RECT *pSourceRect,
                                            CONST RECT *pDestRect, HWND hDestWindowOverride,
                                            CONST RGNDATA *pDirtyRegion, DWORD dwFlags)
{
  SERIALISE_ELEMENT_OPT(pSourceRect).TypedAs("RECT"_lit);
  SERIALISE_ELEMENT_OPT(pDestRect).TypedAs("RECT"_lit);
  SERIALISE_ELEMENT_LOCAL(DestWindowOverride, (uint64_t)(uintptr_t)hDestWindowOverride);
  SERIALISE_ELEMENT_LOCAL(HasDirtyRegion, pDirtyRegion != NULL);
  SERIALISE_ELEMENT_TYPED(uint32_t, dwFlags);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading() && IsLoading(m_State))
  {
    AddEvent();

    ActionDescription action;
    action.flags = ActionFlags::Present;
    action.copyDestination =
        ReplayBackBufferID(m_ReplaySwapChains.empty() ? NULL : m_ReplaySwapChains[0]);
    AddAction(action);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, PresentEx, CONST RECT *pSourceRect,
                                CONST RECT *pDestRect, HWND hDestWindowOverride,
                                CONST RGNDATA *pDirtyRegion, DWORD dwFlags);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SwapChain_Present(SerialiserType &ser,
                                                    IDirect3DSwapChain9 *pSwapChain,
                                                    CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                                    HWND hDestWindowOverride,
                                                    CONST RGNDATA *pDirtyRegion, DWORD dwFlags)
{
  SERIALISE_ELEMENT(pSwapChain);
  SERIALISE_ELEMENT_OPT(pSourceRect).TypedAs("RECT"_lit);
  SERIALISE_ELEMENT_OPT(pDestRect).TypedAs("RECT"_lit);
  SERIALISE_ELEMENT_LOCAL(DestWindowOverride, (uint64_t)(uintptr_t)hDestWindowOverride);
  SERIALISE_ELEMENT_LOCAL(HasDirtyRegion, pDirtyRegion != NULL);
  SERIALISE_ELEMENT_TYPED(uint32_t, dwFlags);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading() && IsLoading(m_State))
  {
    AddEvent();

    ActionDescription action;
    action.flags = ActionFlags::Present;
    action.copyDestination = ReplayBackBufferID(pSwapChain);
    AddAction(action);
  }

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(HRESULT, WrappedD3DDevice9, SwapChain_Present,
                                IDirect3DSwapChain9 *pSwapChain, CONST RECT *pSourceRect,
                                CONST RECT *pDestRect, HWND hDestWindowOverride,
                                CONST RGNDATA *pDirtyRegion, DWORD dwFlags);

///////////////////////////////////////////////////////////////////////////////
// reading captures

bool WrappedD3DDevice9::ProcessChunk(ReadSerialiser &ser, D3D9Chunk chunk)
{
  switch(chunk)
  {
    case D3D9Chunk::SetRenderTarget: return Serialise_SetRenderTarget(ser, {}, {});
    case D3D9Chunk::SetDepthStencilSurface: return Serialise_SetDepthStencilSurface(ser, {});
    case D3D9Chunk::BeginScene: return Serialise_BeginScene(ser);
    case D3D9Chunk::EndScene: return Serialise_EndScene(ser);
    case D3D9Chunk::Clear: return Serialise_Clear(ser, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::SetTransform: return Serialise_SetTransform(ser, {}, {});
    case D3D9Chunk::MultiplyTransform: return Serialise_MultiplyTransform(ser, {}, {});
    case D3D9Chunk::SetViewport: return Serialise_SetViewport(ser, {});
    case D3D9Chunk::SetMaterial: return Serialise_SetMaterial(ser, {});
    case D3D9Chunk::SetLight: return Serialise_SetLight(ser, {}, {});
    case D3D9Chunk::LightEnable: return Serialise_LightEnable(ser, {}, {});
    case D3D9Chunk::SetClipPlane: return Serialise_SetClipPlane(ser, {}, {});
    case D3D9Chunk::SetRenderState: return Serialise_SetRenderState(ser, {}, {});
    case D3D9Chunk::BeginStateBlock: return Serialise_BeginStateBlock(ser);
    case D3D9Chunk::SetClipStatus: return Serialise_SetClipStatus(ser, {});
    case D3D9Chunk::SetTexture: return Serialise_SetTexture(ser, {}, {});
    case D3D9Chunk::SetTextureStageState: return Serialise_SetTextureStageState(ser, {}, {}, {});
    case D3D9Chunk::SetSamplerState: return Serialise_SetSamplerState(ser, {}, {}, {});
    case D3D9Chunk::SetPaletteEntries: return Serialise_SetPaletteEntries(ser, {}, {});
    case D3D9Chunk::SetCurrentTexturePalette: return Serialise_SetCurrentTexturePalette(ser, {});
    case D3D9Chunk::SetScissorRect: return Serialise_SetScissorRect(ser, {});
    case D3D9Chunk::SetSoftwareVertexProcessing:
      return Serialise_SetSoftwareVertexProcessing(ser, {});
    case D3D9Chunk::SetNPatchMode: return Serialise_SetNPatchMode(ser, {});
    case D3D9Chunk::DrawPrimitive: return Serialise_DrawPrimitive(ser, {}, {}, {});
    case D3D9Chunk::DrawIndexedPrimitive:
      return Serialise_DrawIndexedPrimitive(ser, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::ProcessVertices: return Serialise_ProcessVertices(ser, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::SetVertexDeclaration: return Serialise_SetVertexDeclaration(ser, {});
    case D3D9Chunk::SetFVF: return Serialise_SetFVF(ser, {});
    case D3D9Chunk::SetVertexShader: return Serialise_SetVertexShader(ser, {});
    case D3D9Chunk::SetVertexShaderConstantF:
      return Serialise_SetVertexShaderConstantF(ser, {}, {}, {});
    case D3D9Chunk::SetVertexShaderConstantI:
      return Serialise_SetVertexShaderConstantI(ser, {}, {}, {});
    case D3D9Chunk::SetVertexShaderConstantB:
      return Serialise_SetVertexShaderConstantB(ser, {}, {}, {});
    case D3D9Chunk::SetStreamSource: return Serialise_SetStreamSource(ser, {}, {}, {}, {});
    case D3D9Chunk::SetStreamSourceFreq: return Serialise_SetStreamSourceFreq(ser, {}, {});
    case D3D9Chunk::SetIndices: return Serialise_SetIndices(ser, {});
    case D3D9Chunk::SetPixelShader: return Serialise_SetPixelShader(ser, {});
    case D3D9Chunk::SetPixelShaderConstantF:
      return Serialise_SetPixelShaderConstantF(ser, {}, {}, {});
    case D3D9Chunk::SetPixelShaderConstantI:
      return Serialise_SetPixelShaderConstantI(ser, {}, {}, {});
    case D3D9Chunk::SetPixelShaderConstantB:
      return Serialise_SetPixelShaderConstantB(ser, {}, {}, {});
    case D3D9Chunk::DrawRectPatch: return Serialise_DrawRectPatch(ser, {}, {}, {});
    case D3D9Chunk::DrawTriPatch: return Serialise_DrawTriPatch(ser, {}, {}, {});
    case D3D9Chunk::DeletePatch: return Serialise_DeletePatch(ser, {});
    case D3D9Chunk::UpdateSurface: return Serialise_UpdateSurface(ser, {}, {}, {}, {});
    case D3D9Chunk::UpdateTexture: return Serialise_UpdateTexture(ser, {}, {});
    case D3D9Chunk::GetRenderTargetData: return Serialise_GetRenderTargetData(ser, {}, {});
    case D3D9Chunk::StretchRect: return Serialise_StretchRect(ser, {}, {}, {}, {}, {});
    case D3D9Chunk::ColorFill: return Serialise_ColorFill(ser, {}, {}, {});
    case D3D9Chunk::ComposeRects:
      return Serialise_ComposeRects(ser, {}, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::SetConvolutionMonoKernel:
      return Serialise_SetConvolutionMonoKernel(ser, {}, {}, {}, {});
    case D3D9Chunk::StateBlock_Capture: return Serialise_StateBlock_Capture(ser, {});
    case D3D9Chunk::StateBlock_Apply: return Serialise_StateBlock_Apply(ser, {});
    case D3D9Chunk::Query_Issue: return Serialise_Query_Issue(ser, {}, {});
    case D3D9Chunk::BaseTexture_GenerateMipSubLevels:
      return Serialise_BaseTexture_GenerateMipSubLevels(ser, {});
    case D3D9Chunk::BaseTexture_SetAutoGenFilterType:
      return Serialise_BaseTexture_SetAutoGenFilterType(ser, {}, {});
    case D3D9Chunk::BaseTexture_SetLOD: return Serialise_BaseTexture_SetLOD(ser, {}, {});
    case D3D9Chunk::VertexBuffer_Unlock:
    case D3D9Chunk::IndexBuffer_Unlock:
    case D3D9Chunk::Texture_UnlockRect:
    case D3D9Chunk::CubeTexture_UnlockRect:
    case D3D9Chunk::VolumeTexture_UnlockBox:
    case D3D9Chunk::Surface_UnlockRect:
    case D3D9Chunk::Volume_UnlockBox: return ProcessUnlockChunk(ser, chunk);
    case D3D9Chunk::Texture_AddDirtyRect: return Serialise_Texture_AddDirtyRect(ser, {}, {});
    case D3D9Chunk::CubeTexture_AddDirtyRect:
      return Serialise_CubeTexture_AddDirtyRect(ser, {}, {}, {});
    case D3D9Chunk::VolumeTexture_AddDirtyBox:
      return Serialise_VolumeTexture_AddDirtyBox(ser, {}, {});
    case D3D9Chunk::Surface_ReleaseDC:
    case D3D9Chunk::GetFrontBufferData:
    case D3D9Chunk::SwapChain_GetFrontBufferData: return ProcessUnlockChunk(ser, chunk);
    case D3D9Chunk::D3DPERF_BeginEvent: return Serialise_D3DPERF_BeginEvent(ser, {}, {});
    case D3D9Chunk::D3DPERF_EndEvent: return Serialise_D3DPERF_EndEvent(ser);
    case D3D9Chunk::D3DPERF_SetMarker: return Serialise_D3DPERF_SetMarker(ser, {}, {});
    case D3D9Chunk::CreateTexture:
      return Serialise_CreateTexture(ser, {}, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateVolumeTexture:
      return Serialise_CreateVolumeTexture(ser, {}, {}, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateCubeTexture:
      return Serialise_CreateCubeTexture(ser, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateVertexBuffer:
      return Serialise_CreateVertexBuffer(ser, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateIndexBuffer:
      return Serialise_CreateIndexBuffer(ser, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateRenderTarget:
      return Serialise_CreateRenderTarget(ser, {}, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateDepthStencilSurface:
      return Serialise_CreateDepthStencilSurface(ser, {}, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateOffscreenPlainSurface:
      return Serialise_CreateOffscreenPlainSurface(ser, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateRenderTargetEx:
      return Serialise_CreateRenderTargetEx(ser, {}, {}, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateOffscreenPlainSurfaceEx:
      return Serialise_CreateOffscreenPlainSurfaceEx(ser, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateDepthStencilSurfaceEx:
      return Serialise_CreateDepthStencilSurfaceEx(ser, {}, {}, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::CreateQuery: return Serialise_CreateQuery(ser, {}, {});
    case D3D9Chunk::CreateStateBlock: return Serialise_CreateStateBlock(ser, {}, {});
    case D3D9Chunk::EndStateBlock: return Serialise_EndStateBlock(ser, {});
    case D3D9Chunk::CreateAdditionalSwapChain:
      return Serialise_CreateAdditionalSwapChain(ser, {}, {});
    case D3D9Chunk::Texture_GetSurfaceLevel:
      return Serialise_Texture_GetSurfaceLevel(ser, {}, {}, {});
    case D3D9Chunk::CubeTexture_GetCubeMapSurface:
      return Serialise_CubeTexture_GetCubeMapSurface(ser, {}, {}, {}, {});
    case D3D9Chunk::VolumeTexture_GetVolumeLevel:
      return Serialise_VolumeTexture_GetVolumeLevel(ser, {}, {}, {});
    case D3D9Chunk::SwapChain_GetBackBuffer:
      return Serialise_SwapChain_GetBackBuffer(ser, {}, {}, {});
    case D3D9Chunk::DeviceInitialisation: return Serialise_DeviceInitialisation(ser);
    case D3D9Chunk::BeginCaptureFrame: return Serialise_BeginCaptureFrame(ser);
    case D3D9Chunk::CreateVertexDeclaration: return Serialise_CreateVertexDeclaration(ser, {}, {});
    case D3D9Chunk::CreateVertexShader: return Serialise_CreateVertexShader(ser, {}, {});
    case D3D9Chunk::CreatePixelShader: return Serialise_CreatePixelShader(ser, {}, {});
    case D3D9Chunk::DrawPrimitiveUP: return Serialise_DrawPrimitiveUP(ser, {}, {}, {}, {});
    case D3D9Chunk::DrawIndexedPrimitiveUP:
      return Serialise_DrawIndexedPrimitiveUP(ser, {}, {}, {}, {}, {}, {}, {}, {});
    case D3D9Chunk::Present: return Serialise_Present(ser, {}, {}, {}, {});
    case D3D9Chunk::PresentEx: return Serialise_PresentEx(ser, {}, {}, {}, {}, {});
    case D3D9Chunk::SwapChain_Present:
      return Serialise_SwapChain_Present(ser, {}, {}, {}, {}, {}, {});

    // no explicit default so that we have compiler warnings if a chunk isn't explicitly handled.
    case D3D9Chunk::Max: break;
  }

  SystemChunk system = (SystemChunk)chunk;
  if(system == SystemChunk::DriverInit)
  {
    D3D9InitParams InitParams;
    SERIALISE_ELEMENT(InitParams);

    SERIALISE_CHECK_READ_ERRORS();
  }
  else if(system == SystemChunk::InitialContents)
  {
    return m_ResourceManager->ReadInitialState(ser);
  }
  else if(system == SystemChunk::InitialContentsList)
  {
    m_ResourceManager->CreateInitialContents(ser);

    SERIALISE_CHECK_READ_ERRORS();
  }
  else if(system == SystemChunk::CaptureScope)
  {
    return Serialise_CaptureScope(ser);
  }
  else if(system < SystemChunk::FirstDriverChunk)
  {
    RDCERR("Unexpected system chunk in capture data: %u", system);
    ser.SkipCurrentChunk();

    SERIALISE_CHECK_READ_ERRORS();
  }
  else
  {
    RDCERR("Unrecognised chunk type %u", chunk);
    return false;
  }

  return true;
}

bool WrappedD3DDevice9::ProcessUnlockChunk(ReadSerialiser &ser, D3D9Chunk chunk)
{
  D3D9SubresourceData Contents;

  switch(chunk)
  {
    case D3D9Chunk::VertexBuffer_Unlock:
      return Serialise_Unlock(ser, (IDirect3DVertexBuffer9 *)NULL, 0, Contents);
    case D3D9Chunk::IndexBuffer_Unlock:
      return Serialise_Unlock(ser, (IDirect3DIndexBuffer9 *)NULL, 0, Contents);
    case D3D9Chunk::Texture_UnlockRect:
      return Serialise_Unlock(ser, (IDirect3DTexture9 *)NULL, 0, Contents);
    case D3D9Chunk::CubeTexture_UnlockRect:
      return Serialise_Unlock(ser, (IDirect3DCubeTexture9 *)NULL, 0, Contents);
    case D3D9Chunk::VolumeTexture_UnlockBox:
      return Serialise_Unlock(ser, (IDirect3DVolumeTexture9 *)NULL, 0, Contents);
    case D3D9Chunk::Surface_UnlockRect:
      return Serialise_Unlock(ser, (IDirect3DSurface9 *)NULL, 0, Contents);
    case D3D9Chunk::Volume_UnlockBox:
      return Serialise_Unlock(ser, (IDirect3DVolume9 *)NULL, 0, Contents);
    case D3D9Chunk::Surface_ReleaseDC: return Serialise_Surface_ReleaseDC(ser, NULL, Contents);
    case D3D9Chunk::GetFrontBufferData: return Serialise_GetFrontBufferData(ser, 0, NULL, Contents);
    case D3D9Chunk::SwapChain_GetFrontBufferData:
      return Serialise_SwapChain_GetFrontBufferData(ser, NULL, NULL, Contents);
    default: return false;
  }
}
