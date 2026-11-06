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
#include "d3d9_resources.h"
#include "d3d9_swapchain.h"

// add a chunk for the call that was just made to the frame being captured. The caller checks
// that a capture is active and holds the lock.
#define RECORD_FRAME_CHUNK(chunk, ...)           \
  {                                              \
    USE_SCRATCH_SERIALISER();                    \
    SCOPED_SERIALISE_CHUNK(D3D9Chunk::chunk);    \
    CONCAT(Serialise_, chunk)(ser, __VA_ARGS__); \
    m_FrameRecord->AddChunk(scope.Get());        \
  }

// the same for state calls, which are also recorded while a state block is being recorded. The
// caller checks ShouldRecordState() and holds the lock.
#define RECORD_STATE_CHUNK(chunk, ...)           \
  {                                              \
    USE_SCRATCH_SERIALISER();                    \
    SCOPED_SERIALISE_CHUNK(D3D9Chunk::chunk);    \
    CONCAT(Serialise_, chunk)(ser, __VA_ARGS__); \
    AddStateChunk(scope.Get());                  \
  }

#define RECORD_FRAME_CHUNK0(chunk)            \
  {                                           \
    USE_SCRATCH_SERIALISER();                 \
    SCOPED_SERIALISE_CHUNK(D3D9Chunk::chunk); \
    CONCAT(Serialise_, chunk)(ser);           \
    m_FrameRecord->AddChunk(scope.Get());     \
  }

// a call that failed is recorded as a FailedCall chunk, with the parameters its own chunk would
// have had (see RecordFailedCall). Arrays whose size comes from the call's counts are left out,
// passed as NULL: the runtime might have rejected the call because a count was wrong, and reading
// that much of the application's memory could crash. The caller doesn't hold the lock.
#define RECORD_FAILED_CALL(hr, chunk, ...)                             \
  if(FAILED(hr))                                                       \
    RecordFailedCall(hr, D3D9Chunk::chunk, [&](WriteSerialiser &ser) { \
      CONCAT(Serialise_, chunk)(ser, __VA_ARGS__);                     \
    });

#define RECORD_FAILED_CALL0(hr, chunk)                                 \
  if(FAILED(hr))                                                       \
    RecordFailedCall(hr, D3D9Chunk::chunk, [&](WriteSerialiser &ser) { \
      CONCAT(Serialise_, chunk)(ser);                                  \
    });

///////////////////////////////////////////////////////////////////////////////
// Methods passed straight through to the real device, or that only record themselves

HRESULT __stdcall WrappedD3DDevice9::TestCooperativeLevel()
{
  return m_device->TestCooperativeLevel();
}

UINT __stdcall WrappedD3DDevice9::GetAvailableTextureMem()
{
  return m_device->GetAvailableTextureMem();
}

HRESULT __stdcall WrappedD3DDevice9::EvictManagedResources()
{
  return m_device->EvictManagedResources();
}

HRESULT __stdcall WrappedD3DDevice9::GetDeviceCaps(D3DCAPS9 *pCaps)
{
  return m_device->GetDeviceCaps(pCaps);
}

HRESULT __stdcall WrappedD3DDevice9::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode)
{
  return m_device->GetDisplayMode(iSwapChain, pMode);
}

HRESULT __stdcall WrappedD3DDevice9::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
{
  HRESULT ret = m_device->GetCreationParameters(pParameters);
  // the real device was created without D3DCREATE_PUREDEVICE, report the flags the application
  // asked for
  if(SUCCEEDED(ret) && pParameters)
    pParameters->BehaviorFlags = m_InitParams.BehaviorFlags;
  return ret;
}

void __stdcall WrappedD3DDevice9::SetCursorPosition(int X, int Y, DWORD Flags)
{
  m_device->SetCursorPosition(X, Y, Flags);
}

BOOL __stdcall WrappedD3DDevice9::ShowCursor(BOOL bShow)
{
  return m_device->ShowCursor(bShow);
}

UINT __stdcall WrappedD3DDevice9::GetNumberOfSwapChains()
{
  return m_device->GetNumberOfSwapChains();
}

HRESULT __stdcall WrappedD3DDevice9::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus)
{
  return m_device->GetRasterStatus(iSwapChain, pRasterStatus);
}

HRESULT __stdcall WrappedD3DDevice9::SetDialogBoxMode(BOOL bEnableDialogs)
{
  return m_device->SetDialogBoxMode(bEnableDialogs);
}

void __stdcall WrappedD3DDevice9::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP *pRamp)
{
  m_device->SetGammaRamp(iSwapChain, Flags, pRamp);
}

void __stdcall WrappedD3DDevice9::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp)
{
  m_device->GetGammaRamp(iSwapChain, pRamp);
}

HRESULT __stdcall WrappedD3DDevice9::BeginScene()
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->BeginScene());

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK0(BeginScene);
  }

  RECORD_FAILED_CALL0(ret, BeginScene);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::EndScene()
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->EndScene());

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK0(EndScene);
  }

  RECORD_FAILED_CALL0(ret, EndScene);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::Clear(DWORD Count, CONST D3DRECT *pRects, DWORD Flags,
                                           D3DCOLOR Color, float Z, DWORD Stencil)
{
  HRESULT ret;
  PrepareRenderTargetsForWrite();
  SERIALISE_TIME_CALL(ret = m_device->Clear(Count, pRects, Flags, Color, Z, Stencil));

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(Clear, Count, pRects, Flags, Color, Z, Stencil);
    MarkRenderTargetsReferenced();
  }

  RECORD_FAILED_CALL(ret, Clear, Count, NULL, Flags, Color, Z, Stencil);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::SetTransform(D3DTRANSFORMSTATETYPE State,
                                                  CONST D3DMATRIX *pMatrix)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetTransform(State, pMatrix));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetTransform, State, pMatrix);
  }

  RECORD_FAILED_CALL(ret, SetTransform, State, pMatrix);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
  return m_device->GetTransform(State, pMatrix);
}

HRESULT __stdcall WrappedD3DDevice9::MultiplyTransform(D3DTRANSFORMSTATETYPE State,
                                                       CONST D3DMATRIX *pMatrix)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->MultiplyTransform(State, pMatrix));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(MultiplyTransform, State, pMatrix);
  }

  RECORD_FAILED_CALL(ret, MultiplyTransform, State, pMatrix);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::SetViewport(CONST D3DVIEWPORT9 *pViewport)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetViewport(pViewport));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetViewport, pViewport);
  }

  RECORD_FAILED_CALL(ret, SetViewport, pViewport);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetViewport(D3DVIEWPORT9 *pViewport)
{
  return m_device->GetViewport(pViewport);
}

HRESULT __stdcall WrappedD3DDevice9::SetMaterial(CONST D3DMATERIAL9 *pMaterial)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetMaterial(pMaterial));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetMaterial, pMaterial);
  }

  RECORD_FAILED_CALL(ret, SetMaterial, pMaterial);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetMaterial(D3DMATERIAL9 *pMaterial)
{
  return m_device->GetMaterial(pMaterial);
}

HRESULT __stdcall WrappedD3DDevice9::SetLight(DWORD Index, CONST D3DLIGHT9 *pLight)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetLight(Index, pLight));

  if(SUCCEEDED(ret))
  {
    SCOPED_LOCK(m_D3DLock);
    m_LightIndices.Add(Index);
    if(ShouldRecordState())
    {
      RECORD_STATE_CHUNK(SetLight, Index, pLight);
    }
  }

  RECORD_FAILED_CALL(ret, SetLight, Index, pLight);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetLight(DWORD Index, D3DLIGHT9 *_arg2)
{
  return m_device->GetLight(Index, _arg2);
}

HRESULT __stdcall WrappedD3DDevice9::LightEnable(DWORD Index, BOOL Enable)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->LightEnable(Index, Enable));

  if(SUCCEEDED(ret))
  {
    SCOPED_LOCK(m_D3DLock);
    m_LightIndices.Add(Index);
    if(ShouldRecordState())
    {
      RECORD_STATE_CHUNK(LightEnable, Index, Enable);
    }
  }

  RECORD_FAILED_CALL(ret, LightEnable, Index, Enable);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetLightEnable(DWORD Index, BOOL *pEnable)
{
  return m_device->GetLightEnable(Index, pEnable);
}

HRESULT __stdcall WrappedD3DDevice9::SetClipPlane(DWORD Index, CONST float *pPlane)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetClipPlane(Index, pPlane));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetClipPlane, Index, pPlane);
  }

  RECORD_FAILED_CALL(ret, SetClipPlane, Index, pPlane);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetClipPlane(DWORD Index, float *pPlane)
{
  return m_device->GetClipPlane(Index, pPlane);
}

HRESULT __stdcall WrappedD3DDevice9::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetRenderState(State, Value));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetRenderState, State, Value);
  }

  RECORD_FAILED_CALL(ret, SetRenderState, State, Value);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue)
{
  return m_device->GetRenderState(State, pValue);
}

HRESULT __stdcall WrappedD3DDevice9::SetClipStatus(CONST D3DCLIPSTATUS9 *pClipStatus)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetClipStatus(pClipStatus));

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(SetClipStatus, pClipStatus);
  }

  RECORD_FAILED_CALL(ret, SetClipStatus, pClipStatus);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetClipStatus(D3DCLIPSTATUS9 *pClipStatus)
{
  return m_device->GetClipStatus(pClipStatus);
}

HRESULT __stdcall WrappedD3DDevice9::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                                          DWORD *pValue)
{
  return m_device->GetTextureStageState(Stage, Type, pValue);
}

HRESULT __stdcall WrappedD3DDevice9::SetTextureStageState(DWORD Stage,
                                                          D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetTextureStageState(Stage, Type, Value));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetTextureStageState, Stage, Type, Value);
  }

  RECORD_FAILED_CALL(ret, SetTextureStageState, Stage, Type, Value);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type,
                                                     DWORD *pValue)
{
  return m_device->GetSamplerState(Sampler, Type, pValue);
}

HRESULT __stdcall WrappedD3DDevice9::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type,
                                                     DWORD Value)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetSamplerState(Sampler, Type, Value));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetSamplerState, Sampler, Type, Value);
  }

  RECORD_FAILED_CALL(ret, SetSamplerState, Sampler, Type, Value);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::ValidateDevice(DWORD *pNumPasses)
{
  return m_device->ValidateDevice(pNumPasses);
}

HRESULT __stdcall WrappedD3DDevice9::SetPaletteEntries(UINT PaletteNumber,
                                                       CONST PALETTEENTRY *pEntries)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetPaletteEntries(PaletteNumber, pEntries));

  if(SUCCEEDED(ret))
  {
    SCOPED_LOCK(m_D3DLock);
    m_PaletteNumbers.Add(PaletteNumber);
    if(IsActiveCapturing(m_State))
    {
      RECORD_FRAME_CHUNK(SetPaletteEntries, PaletteNumber, pEntries);
    }
  }

  RECORD_FAILED_CALL(ret, SetPaletteEntries, PaletteNumber, pEntries);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries)
{
  return m_device->GetPaletteEntries(PaletteNumber, pEntries);
}

HRESULT __stdcall WrappedD3DDevice9::SetCurrentTexturePalette(UINT PaletteNumber)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetCurrentTexturePalette(PaletteNumber));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetCurrentTexturePalette, PaletteNumber);
  }

  RECORD_FAILED_CALL(ret, SetCurrentTexturePalette, PaletteNumber);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetCurrentTexturePalette(UINT *PaletteNumber)
{
  return m_device->GetCurrentTexturePalette(PaletteNumber);
}

HRESULT __stdcall WrappedD3DDevice9::SetScissorRect(CONST RECT *pRect)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetScissorRect(pRect));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetScissorRect, pRect);
  }

  RECORD_FAILED_CALL(ret, SetScissorRect, pRect);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetScissorRect(RECT *pRect)
{
  return m_device->GetScissorRect(pRect);
}

HRESULT __stdcall WrappedD3DDevice9::SetSoftwareVertexProcessing(BOOL bSoftware)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetSoftwareVertexProcessing(bSoftware));

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(SetSoftwareVertexProcessing, bSoftware);
  }

  RECORD_FAILED_CALL(ret, SetSoftwareVertexProcessing, bSoftware);

  return ret;
}

BOOL __stdcall WrappedD3DDevice9::GetSoftwareVertexProcessing()
{
  return m_device->GetSoftwareVertexProcessing();
}

HRESULT __stdcall WrappedD3DDevice9::SetNPatchMode(float nSegments)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetNPatchMode(nSegments));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetNPatchMode, nSegments);
  }

  RECORD_FAILED_CALL(ret, SetNPatchMode, nSegments);

  return ret;
}

float __stdcall WrappedD3DDevice9::GetNPatchMode()
{
  return m_device->GetNPatchMode();
}

HRESULT __stdcall WrappedD3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                                   UINT PrimitiveCount)
{
  HRESULT ret;
  PrepareRenderTargetsForWrite();
  SERIALISE_TIME_CALL(ret = m_device->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount));

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(DrawPrimitive, PrimitiveType, StartVertex, PrimitiveCount);
    MarkDrawReferenced();
  }

  RECORD_FAILED_CALL(ret, DrawPrimitive, PrimitiveType, StartVertex, PrimitiveCount);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType,
                                                          INT BaseVertexIndex, UINT MinVertexIndex,
                                                          UINT NumVertices, UINT startIndex,
                                                          UINT primCount)
{
  HRESULT ret;
  PrepareRenderTargetsForWrite();
  SERIALISE_TIME_CALL(ret = m_device->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex,
                                                           MinVertexIndex, NumVertices, startIndex,
                                                           primCount));

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(DrawIndexedPrimitive, PrimitiveType, BaseVertexIndex, MinVertexIndex,
                       NumVertices, startIndex, primCount);
    MarkDrawReferenced();
  }

  RECORD_FAILED_CALL(ret, DrawIndexedPrimitive, PrimitiveType, BaseVertexIndex, MinVertexIndex,
                     NumVertices, startIndex, primCount);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetFVF(DWORD *pFVF)
{
  return m_device->GetFVF(pFVF);
}

HRESULT __stdcall WrappedD3DDevice9::SetVertexShaderConstantF(UINT StartRegister,
                                                              CONST float *pConstantData,
                                                              UINT Vector4fCount)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(
      ret = m_device->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetVertexShaderConstantF, StartRegister, pConstantData, Vector4fCount);
  }

  RECORD_FAILED_CALL(ret, SetVertexShaderConstantF, StartRegister, NULL, Vector4fCount);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetVertexShaderConstantF(UINT StartRegister,
                                                              float *pConstantData,
                                                              UINT Vector4fCount)
{
  return m_device->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT __stdcall WrappedD3DDevice9::SetVertexShaderConstantI(UINT StartRegister,
                                                              CONST int *pConstantData,
                                                              UINT Vector4iCount)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(
      ret = m_device->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetVertexShaderConstantI, StartRegister, pConstantData, Vector4iCount);
  }

  RECORD_FAILED_CALL(ret, SetVertexShaderConstantI, StartRegister, NULL, Vector4iCount);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetVertexShaderConstantI(UINT StartRegister,
                                                              int *pConstantData, UINT Vector4iCount)
{
  return m_device->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT __stdcall WrappedD3DDevice9::SetVertexShaderConstantB(UINT StartRegister,
                                                              CONST BOOL *pConstantData,
                                                              UINT BoolCount)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(
      ret = m_device->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetVertexShaderConstantB, StartRegister, pConstantData, BoolCount);
  }

  RECORD_FAILED_CALL(ret, SetVertexShaderConstantB, StartRegister, NULL, BoolCount);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetVertexShaderConstantB(UINT StartRegister,
                                                              BOOL *pConstantData, UINT BoolCount)
{
  return m_device->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
}

HRESULT __stdcall WrappedD3DDevice9::SetStreamSourceFreq(UINT StreamNumber, UINT Setting)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->SetStreamSourceFreq(StreamNumber, Setting));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetStreamSourceFreq, StreamNumber, Setting);
  }

  RECORD_FAILED_CALL(ret, SetStreamSourceFreq, StreamNumber, Setting);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting)
{
  return m_device->GetStreamSourceFreq(StreamNumber, pSetting);
}

HRESULT __stdcall WrappedD3DDevice9::SetPixelShaderConstantF(UINT StartRegister,
                                                             CONST float *pConstantData,
                                                             UINT Vector4fCount)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(
      ret = m_device->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetPixelShaderConstantF, StartRegister, pConstantData, Vector4fCount);
  }

  RECORD_FAILED_CALL(ret, SetPixelShaderConstantF, StartRegister, NULL, Vector4fCount);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetPixelShaderConstantF(UINT StartRegister,
                                                             float *pConstantData, UINT Vector4fCount)
{
  return m_device->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT __stdcall WrappedD3DDevice9::SetPixelShaderConstantI(UINT StartRegister,
                                                             CONST int *pConstantData,
                                                             UINT Vector4iCount)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(
      ret = m_device->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetPixelShaderConstantI, StartRegister, pConstantData, Vector4iCount);
  }

  RECORD_FAILED_CALL(ret, SetPixelShaderConstantI, StartRegister, NULL, Vector4iCount);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetPixelShaderConstantI(UINT StartRegister, int *pConstantData,
                                                             UINT Vector4iCount)
{
  return m_device->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT __stdcall WrappedD3DDevice9::SetPixelShaderConstantB(UINT StartRegister,
                                                             CONST BOOL *pConstantData,
                                                             UINT BoolCount)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(
      ret = m_device->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount));

  if(SUCCEEDED(ret) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetPixelShaderConstantB, StartRegister, pConstantData, BoolCount);
  }

  RECORD_FAILED_CALL(ret, SetPixelShaderConstantB, StartRegister, NULL, BoolCount);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::GetPixelShaderConstantB(UINT StartRegister,
                                                             BOOL *pConstantData, UINT BoolCount)
{
  return m_device->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
}

HRESULT __stdcall WrappedD3DDevice9::DrawRectPatch(UINT Handle, CONST float *pNumSegs,
                                                   CONST D3DRECTPATCH_INFO *pRectPatchInfo)
{
  HRESULT ret;
  PrepareRenderTargetsForWrite();
  SERIALISE_TIME_CALL(ret = m_device->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo));

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(DrawRectPatch, Handle, pNumSegs, pRectPatchInfo);
    MarkDrawReferenced();
  }

  RECORD_FAILED_CALL(ret, DrawRectPatch, Handle, pNumSegs, pRectPatchInfo);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::DrawTriPatch(UINT Handle, CONST float *pNumSegs,
                                                  CONST D3DTRIPATCH_INFO *pTriPatchInfo)
{
  HRESULT ret;
  PrepareRenderTargetsForWrite();
  SERIALISE_TIME_CALL(ret = m_device->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo));

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(DrawTriPatch, Handle, pNumSegs, pTriPatchInfo);
    MarkDrawReferenced();
  }

  RECORD_FAILED_CALL(ret, DrawTriPatch, Handle, pNumSegs, pTriPatchInfo);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::DeletePatch(UINT Handle)
{
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_device->DeletePatch(Handle));

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(DeletePatch, Handle);
  }

  RECORD_FAILED_CALL(ret, DeletePatch, Handle);

  return ret;
}

HRESULT __stdcall WrappedD3DDevice9::SetConvolutionMonoKernel(UINT width, UINT height, float *rows,
                                                              float *columns)
{
  RDCASSERT(m_deviceEx != NULL);

  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_deviceEx->SetConvolutionMonoKernel(width, height, rows, columns));

  if(SUCCEEDED(ret) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(SetConvolutionMonoKernel, width, height, rows, columns);
  }

  RECORD_FAILED_CALL(ret, SetConvolutionMonoKernel, width, height, NULL, NULL);

  return ret;
}

HRESULT WrappedD3DDevice9::GetGPUThreadPriority(INT *pPriority)
{
  RDCASSERT(m_deviceEx != NULL);
  return m_deviceEx->GetGPUThreadPriority(pPriority);
}

HRESULT WrappedD3DDevice9::SetGPUThreadPriority(INT Priority)
{
  RDCASSERT(m_deviceEx != NULL);
  return m_deviceEx->SetGPUThreadPriority(Priority);
}

HRESULT WrappedD3DDevice9::WaitForVBlank(UINT iSwapChain)
{
  RDCASSERT(m_deviceEx != NULL);
  return m_deviceEx->WaitForVBlank(iSwapChain);
}

HRESULT WrappedD3DDevice9::SetMaximumFrameLatency(UINT MaxLatency)
{
  RDCASSERT(m_deviceEx != NULL);
  return m_deviceEx->SetMaximumFrameLatency(MaxLatency);
}

HRESULT WrappedD3DDevice9::GetMaximumFrameLatency(UINT *pMaxLatency)
{
  RDCASSERT(m_deviceEx != NULL);
  return m_deviceEx->GetMaximumFrameLatency(pMaxLatency);
}

HRESULT WrappedD3DDevice9::CheckDeviceState(HWND hDestinationWindow)
{
  RDCASSERT(m_deviceEx != NULL);
  return m_deviceEx->CheckDeviceState(hDestinationWindow);
}

HRESULT WrappedD3DDevice9::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX *pMode,
                                            D3DDISPLAYROTATION *pRotation)
{
  RDCASSERT(m_deviceEx != NULL);
  return m_deviceEx->GetDisplayModeEx(iSwapChain, pMode, pRotation);
}

HRESULT __stdcall WrappedD3DDevice9::GetDirect3D(IDirect3D9 **ppD3D9)
{
  // make sure the underlying object is actually there
  IDirect3D9 *direct3D = NULL;
  HRESULT res = m_device->GetDirect3D(&direct3D);
  if(direct3D != NULL)
  {
    direct3D->Release();
  }

  if(res == S_OK)
  {
    m_D3D->AddRef();
    *ppD3D9 = m_D3D;
  }
  else
  {
    *ppD3D9 = NULL;
  }

  return res;
}

///////////////////////////////////////////////////////////////////////////////
// Methods that create, take or return objects

namespace
{
// wrap a newly created object. The wrapper takes over the real reference.
template <typename WrapType, typename RealType>
HRESULT WrapCreated(HRESULT hr, RealType *real, RealType **out, WrappedD3DDevice9 *device)
{
  if(SUCCEEDED(hr) && real != NULL)
    *out = new WrapType(real, device);
  else
    *out = NULL;
  return hr;
}

// return a tracked binding to the application, with a new external reference
template <typename T>
T *AddRefBound(T *obj)
{
  if(obj)
    obj->AddRef();
  return obj;
}
};

HRESULT __stdcall WrappedD3DDevice9::SetCursorProperties(UINT XHotSpot, UINT YHotSpot,
                                                         IDirect3DSurface9 *pCursorBitmap)
{
  return m_device->SetCursorProperties(XHotSpot, YHotSpot, Unwrap(pCursorBitmap));
}

HRESULT __stdcall WrappedD3DDevice9::CreateAdditionalSwapChain(
    D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **pSwapChain)
{
  if(pSwapChain == NULL)
    return D3DERR_INVALIDCALL;

  IDirect3DSwapChain9 *real = NULL;
  HRESULT hr = m_device->CreateAdditionalSwapChain(pPresentationParameters, &real);

  if(SUCCEEDED(hr) && real != NULL)
  {
    *pSwapChain = new WrappedD3DSwapChain9(real, this, false);
    RecordCreation(GetWrappedObject(*pSwapChain), D3D9Chunk::CreateAdditionalSwapChain,
                   [&](WriteSerialiser &ser) {
                     Serialise_CreateAdditionalSwapChain(ser, pPresentationParameters, pSwapChain);
                   });
  }
  else
  {
    *pSwapChain = NULL;
  }

  if(FAILED(hr))
    RecordFailedCall(hr, ToStr(D3D9Chunk::CreateAdditionalSwapChain));
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain)
{
  if(pSwapChain == NULL)
    return D3DERR_INVALIDCALL;

  // only implicit swapchains are accessible through the device
  if(iSwapChain >= m_ImplicitSwapChains.size())
  {
    *pSwapChain = NULL;
    return D3DERR_INVALIDCALL;
  }

  *pSwapChain = AddRefBound(m_ImplicitSwapChains[iSwapChain]);
  return S_OK;
}

HRESULT __stdcall WrappedD3DDevice9::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters)
{
  bool appControlled = false;
  bool discarded = DiscardCaptureForReset(appControlled);
  HRESULT hr = m_device->Reset(pPresentationParameters);
  if(SUCCEEDED(hr))
    OnReset();
  if(discarded)
    AfterResetDiscard(appControlled, SUCCEEDED(hr));
  return hr;
}

HRESULT WrappedD3DDevice9::ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters,
                                   D3DDISPLAYMODEEX *pFullscreenDisplayMode)
{
  RDCASSERT(m_deviceEx != NULL);
  bool appControlled = false;
  bool discarded = DiscardCaptureForReset(appControlled);
  HRESULT hr = m_deviceEx->ResetEx(pPresentationParameters, pFullscreenDisplayMode);
  if(SUCCEEDED(hr))
    OnReset();
  if(discarded)
    AfterResetDiscard(appControlled, SUCCEEDED(hr));
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::Present(CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                             HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion)
{
  if(IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(Present, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
  }

  HandlePresent(m_ImplicitSwapChains.empty() ? NULL : m_ImplicitSwapChains[0]->GetReal(),
                hDestWindowOverride);

  return m_device->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT WrappedD3DDevice9::PresentEx(CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                     HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion,
                                     DWORD dwFlags)
{
  RDCASSERT(m_deviceEx != NULL);

  if(IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(PresentEx, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
  }

  HandlePresent(m_ImplicitSwapChains.empty() ? NULL : m_ImplicitSwapChains[0]->GetReal(),
                hDestWindowOverride);

  return m_deviceEx->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

HRESULT __stdcall WrappedD3DDevice9::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer,
                                                   D3DBACKBUFFER_TYPE Type,
                                                   IDirect3DSurface9 **ppBackBuffer)
{
  if(ppBackBuffer == NULL)
    return D3DERR_INVALIDCALL;

  if(iSwapChain >= m_ImplicitSwapChains.size())
  {
    *ppBackBuffer = NULL;
    return D3DERR_INVALIDCALL;
  }

  return m_ImplicitSwapChains[iSwapChain]->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
}

HRESULT __stdcall WrappedD3DDevice9::CreateTexture(UINT Width, UINT Height, UINT Levels,
                                                   DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                   IDirect3DTexture9 **ppTexture,
                                                   HANDLE *pSharedHandle)
{
  if(ppTexture == NULL)
    return m_device->CreateTexture(Width, Height, Levels, Usage, Format, Pool, NULL, pSharedHandle);

  IDirect3DTexture9 *real = NULL;
  HRESULT hr =
      m_device->CreateTexture(Width, Height, Levels, Usage, Format, Pool, &real, pSharedHandle);
  WrapCreated<WrappedD3DTexture9>(hr, real, ppTexture, this);
  RecordCreated(hr, GetWrappedObject(*ppTexture), D3D9Chunk::CreateTexture, [&](WriteSerialiser &ser) {
    Serialise_CreateTexture(ser, Width, Height, Levels, Usage, Format, Pool, ppTexture,
                            pSharedHandle);
  });
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth,
                                                         UINT Levels, DWORD Usage, D3DFORMAT Format,
                                                         D3DPOOL Pool,
                                                         IDirect3DVolumeTexture9 **ppVolumeTexture,
                                                         HANDLE *pSharedHandle)
{
  if(ppVolumeTexture == NULL)
    return m_device->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, NULL,
                                         pSharedHandle);

  IDirect3DVolumeTexture9 *real = NULL;
  HRESULT hr = m_device->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool,
                                             &real, pSharedHandle);
  WrapCreated<WrappedD3DVolumeTexture9>(hr, real, ppVolumeTexture, this);
  RecordCreated(hr, GetWrappedObject(*ppVolumeTexture), D3D9Chunk::CreateVolumeTexture,
                [&](WriteSerialiser &ser) {
                  Serialise_CreateVolumeTexture(ser, Width, Height, Depth, Levels, Usage, Format,
                                                Pool, ppVolumeTexture, pSharedHandle);
                });
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage,
                                                       D3DFORMAT Format, D3DPOOL Pool,
                                                       IDirect3DCubeTexture9 **ppCubeTexture,
                                                       HANDLE *pSharedHandle)
{
  if(ppCubeTexture == NULL)
    return m_device->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, NULL, pSharedHandle);

  IDirect3DCubeTexture9 *real = NULL;
  HRESULT hr =
      m_device->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, &real, pSharedHandle);
  WrapCreated<WrappedD3DCubeTexture9>(hr, real, ppCubeTexture, this);
  RecordCreated(hr, GetWrappedObject(*ppCubeTexture), D3D9Chunk::CreateCubeTexture,
                [&](WriteSerialiser &ser) {
                  Serialise_CreateCubeTexture(ser, EdgeLength, Levels, Usage, Format, Pool,
                                              ppCubeTexture, pSharedHandle);
                });
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF,
                                                        D3DPOOL Pool,
                                                        IDirect3DVertexBuffer9 **ppVertexBuffer,
                                                        HANDLE *pSharedHandle)
{
  if(ppVertexBuffer == NULL)
    return m_device->CreateVertexBuffer(Length, Usage, FVF, Pool, NULL, pSharedHandle);

  IDirect3DVertexBuffer9 *real = NULL;
  HRESULT hr = m_device->CreateVertexBuffer(Length, Usage, FVF, Pool, &real, pSharedHandle);
  WrapCreated<WrappedD3DVertexBuffer9>(hr, real, ppVertexBuffer, this);
  RecordCreated(
      hr, GetWrappedObject(*ppVertexBuffer), D3D9Chunk::CreateVertexBuffer, [&](WriteSerialiser &ser) {
        Serialise_CreateVertexBuffer(ser, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
      });
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format,
                                                       D3DPOOL Pool,
                                                       IDirect3DIndexBuffer9 **ppIndexBuffer,
                                                       HANDLE *pSharedHandle)
{
  if(ppIndexBuffer == NULL)
    return m_device->CreateIndexBuffer(Length, Usage, Format, Pool, NULL, pSharedHandle);

  IDirect3DIndexBuffer9 *real = NULL;
  HRESULT hr = m_device->CreateIndexBuffer(Length, Usage, Format, Pool, &real, pSharedHandle);
  WrapCreated<WrappedD3DIndexBuffer9>(hr, real, ppIndexBuffer, this);
  RecordCreated(
      hr, GetWrappedObject(*ppIndexBuffer), D3D9Chunk::CreateIndexBuffer, [&](WriteSerialiser &ser) {
        Serialise_CreateIndexBuffer(ser, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
      });
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                                        D3DMULTISAMPLE_TYPE MultiSample,
                                                        DWORD MultisampleQuality, BOOL Lockable,
                                                        IDirect3DSurface9 **ppSurface,
                                                        HANDLE *pSharedHandle)
{
  if(ppSurface == NULL)
    return m_device->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality,
                                        Lockable, NULL, pSharedHandle);

  IDirect3DSurface9 *real = NULL;
  HRESULT hr = m_device->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality,
                                            Lockable, &real, pSharedHandle);
  WrapCreated<WrappedD3DSurface9>(hr, real, ppSurface, this);
  RecordCreated(
      hr, GetWrappedObject(*ppSurface), D3D9Chunk::CreateRenderTarget, [&](WriteSerialiser &ser) {
        Serialise_CreateRenderTarget(ser, Width, Height, Format, MultiSample, MultisampleQuality,
                                     Lockable, ppSurface, pSharedHandle);
      });
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
  if(ppSurface == NULL)
    return m_device->CreateDepthStencilSurface(Width, Height, Format, MultiSample,
                                               MultisampleQuality, Discard, NULL, pSharedHandle);

  IDirect3DSurface9 *real = NULL;
  HRESULT hr = m_device->CreateDepthStencilSurface(
      Width, Height, Format, MultiSample, MultisampleQuality, Discard, &real, pSharedHandle);
  WrapCreated<WrappedD3DSurface9>(hr, real, ppSurface, this);
  RecordCreated(hr, GetWrappedObject(*ppSurface), D3D9Chunk::CreateDepthStencilSurface,
                [&](WriteSerialiser &ser) {
                  Serialise_CreateDepthStencilSurface(ser, Width, Height, Format, MultiSample,
                                                      MultisampleQuality, Discard, ppSurface,
                                                      pSharedHandle);
                });
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::CreateOffscreenPlainSurface(UINT Width, UINT Height,
                                                                 D3DFORMAT Format, D3DPOOL Pool,
                                                                 IDirect3DSurface9 **ppSurface,
                                                                 HANDLE *pSharedHandle)
{
  if(ppSurface == NULL)
    return m_device->CreateOffscreenPlainSurface(Width, Height, Format, Pool, NULL, pSharedHandle);

  IDirect3DSurface9 *real = NULL;
  HRESULT hr =
      m_device->CreateOffscreenPlainSurface(Width, Height, Format, Pool, &real, pSharedHandle);
  WrapCreated<WrappedD3DSurface9>(hr, real, ppSurface, this);
  RecordCreated(hr, GetWrappedObject(*ppSurface), D3D9Chunk::CreateOffscreenPlainSurface,
                [&](WriteSerialiser &ser) {
                  Serialise_CreateOffscreenPlainSurface(ser, Width, Height, Format, Pool, ppSurface,
                                                        pSharedHandle);
                });
  return hr;
}

HRESULT WrappedD3DDevice9::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format,
                                                D3DMULTISAMPLE_TYPE MultiSample,
                                                DWORD MultisampleQuality, BOOL Lockable,
                                                IDirect3DSurface9 **ppSurface,
                                                HANDLE *pSharedHandle, DWORD Usage)
{
  RDCASSERT(m_deviceEx != NULL);

  if(ppSurface == NULL)
    return m_deviceEx->CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality,
                                            Lockable, NULL, pSharedHandle, Usage);

  IDirect3DSurface9 *real = NULL;
  HRESULT hr = m_deviceEx->CreateRenderTargetEx(
      Width, Height, Format, MultiSample, MultisampleQuality, Lockable, &real, pSharedHandle, Usage);
  WrapCreated<WrappedD3DSurface9>(hr, real, ppSurface, this);
  RecordCreated(
      hr, GetWrappedObject(*ppSurface), D3D9Chunk::CreateRenderTargetEx, [&](WriteSerialiser &ser) {
        Serialise_CreateRenderTargetEx(ser, Width, Height, Format, MultiSample, MultisampleQuality,
                                       Lockable, ppSurface, pSharedHandle, Usage);
      });
  return hr;
}

HRESULT WrappedD3DDevice9::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format,
                                                         D3DPOOL Pool, IDirect3DSurface9 **ppSurface,
                                                         HANDLE *pSharedHandle, DWORD Usage)
{
  RDCASSERT(m_deviceEx != NULL);

  if(ppSurface == NULL)
    return m_deviceEx->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, NULL,
                                                     pSharedHandle, Usage);

  IDirect3DSurface9 *real = NULL;
  HRESULT hr = m_deviceEx->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, &real,
                                                         pSharedHandle, Usage);
  WrapCreated<WrappedD3DSurface9>(hr, real, ppSurface, this);
  RecordCreated(hr, GetWrappedObject(*ppSurface), D3D9Chunk::CreateOffscreenPlainSurfaceEx,
                [&](WriteSerialiser &ser) {
                  Serialise_CreateOffscreenPlainSurfaceEx(ser, Width, Height, Format, Pool,
                                                          ppSurface, pSharedHandle, Usage);
                });
  return hr;
}

HRESULT WrappedD3DDevice9::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format,
                                                       D3DMULTISAMPLE_TYPE MultiSample,
                                                       DWORD MultisampleQuality, BOOL Discard,
                                                       IDirect3DSurface9 **ppSurface,
                                                       HANDLE *pSharedHandle, DWORD Usage)
{
  RDCASSERT(m_deviceEx != NULL);

  if(ppSurface == NULL)
    return m_deviceEx->CreateDepthStencilSurfaceEx(
        Width, Height, Format, MultiSample, MultisampleQuality, Discard, NULL, pSharedHandle, Usage);

  IDirect3DSurface9 *real = NULL;
  HRESULT hr = m_deviceEx->CreateDepthStencilSurfaceEx(
      Width, Height, Format, MultiSample, MultisampleQuality, Discard, &real, pSharedHandle, Usage);
  WrapCreated<WrappedD3DSurface9>(hr, real, ppSurface, this);
  RecordCreated(hr, GetWrappedObject(*ppSurface), D3D9Chunk::CreateDepthStencilSurfaceEx,
                [&](WriteSerialiser &ser) {
                  Serialise_CreateDepthStencilSurfaceEx(ser, Width, Height, Format, MultiSample,
                                                        MultisampleQuality, Discard, ppSurface,
                                                        pSharedHandle, Usage);
                });
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::UpdateSurface(IDirect3DSurface9 *pSourceSurface,
                                                   CONST RECT *pSourceRect,
                                                   IDirect3DSurface9 *pDestinationSurface,
                                                   CONST POINT *pDestPoint)
{
  PrepareForWrite(pDestinationSurface);
  HRESULT hr = m_device->UpdateSurface(Unwrap(pSourceSurface), pSourceRect,
                                       Unwrap(pDestinationSurface), pDestPoint);

  if(SUCCEEDED(hr))
    UpdateShadowFromSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(UpdateSurface, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
    MarkReferenced(pSourceSurface, eFrameRef_Read);
    MarkReferenced(pDestinationSurface, eFrameRef_PartialWrite);
  }

  RECORD_FAILED_CALL(hr, UpdateSurface, pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture,
                                                   IDirect3DBaseTexture9 *pDestinationTexture)
{
  PrepareForWrite(pDestinationTexture);
  HRESULT hr = m_device->UpdateTexture(Unwrap(pSourceTexture), Unwrap(pDestinationTexture));

  if(SUCCEEDED(hr))
    UpdateShadowFromTexture(pSourceTexture, pDestinationTexture);

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(UpdateTexture, pSourceTexture, pDestinationTexture);
    MarkReferenced(pSourceTexture, eFrameRef_Read);
    MarkReferenced(pDestinationTexture, eFrameRef_PartialWrite);
  }

  RECORD_FAILED_CALL(hr, UpdateTexture, pSourceTexture, pDestinationTexture);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget,
                                                         IDirect3DSurface9 *pDestSurface)
{
  PrepareForWrite(pDestSurface);
  HRESULT hr = m_device->GetRenderTargetData(Unwrap(pRenderTarget), Unwrap(pDestSurface));

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(GetRenderTargetData, pRenderTarget, pDestSurface);
    MarkReferenced(pRenderTarget, eFrameRef_Read);
    MarkReferenced(pDestSurface, eFrameRef_PartialWrite);
  }

  RECORD_FAILED_CALL(hr, GetRenderTargetData, pRenderTarget, pDestSurface);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetFrontBufferData(UINT iSwapChain,
                                                        IDirect3DSurface9 *pDestSurface)
{
  PrepareForWrite(pDestSurface);
  HRESULT hr = m_device->GetFrontBufferData(iSwapChain, Unwrap(pDestSurface));
  if(SUCCEEDED(hr))
    OnGetFrontBufferData(iSwapChain, pDestSurface);
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::StretchRect(IDirect3DSurface9 *pSourceSurface,
                                                 CONST RECT *pSourceRect,
                                                 IDirect3DSurface9 *pDestSurface,
                                                 CONST RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter)
{
  PrepareForWrite(pDestSurface);
  HRESULT hr = m_device->StretchRect(Unwrap(pSourceSurface), pSourceRect, Unwrap(pDestSurface),
                                     pDestRect, Filter);

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(StretchRect, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
    MarkReferenced(pSourceSurface, eFrameRef_Read);
    MarkReferenced(pDestSurface, eFrameRef_PartialWrite);
  }

  RECORD_FAILED_CALL(hr, StretchRect, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::ColorFill(IDirect3DSurface9 *pSurface, CONST RECT *pRect,
                                               D3DCOLOR color)
{
  PrepareForWrite(pSurface);
  HRESULT hr = m_device->ColorFill(Unwrap(pSurface), pRect, color);

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(ColorFill, pSurface, pRect, color);
    MarkReferenced(pSurface, eFrameRef_PartialWrite);
  }

  RECORD_FAILED_CALL(hr, ColorFill, pSurface, pRect, color);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::SetRenderTarget(DWORD RenderTargetIndex,
                                                     IDirect3DSurface9 *pRenderTarget)
{
  HRESULT hr = m_device->SetRenderTarget(RenderTargetIndex, Unwrap(pRenderTarget));

  // render targets aren't part of state blocks, so this always applies to the device
  if(SUCCEEDED(hr) && RenderTargetIndex < D3D9_NUM_RENDER_TARGETS)
  {
    SCOPED_LOCK(m_D3DLock);
    D3D9SetBound(m_RenderTargets[RenderTargetIndex], pRenderTarget);
  }

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(SetRenderTarget, RenderTargetIndex, pRenderTarget);
    MarkReferenced(pRenderTarget, eFrameRef_PartialWrite);
  }

  RECORD_FAILED_CALL(hr, SetRenderTarget, RenderTargetIndex, pRenderTarget);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetRenderTarget(DWORD RenderTargetIndex,
                                                     IDirect3DSurface9 **ppRenderTarget)
{
  if(ppRenderTarget == NULL || RenderTargetIndex >= D3D9_NUM_RENDER_TARGETS)
    return D3DERR_INVALIDCALL;

  SCOPED_LOCK(m_D3DLock);
  *ppRenderTarget = AddRefBound(m_RenderTargets[RenderTargetIndex]);
  return *ppRenderTarget ? S_OK : D3DERR_NOTFOUND;
}

HRESULT __stdcall WrappedD3DDevice9::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil)
{
  HRESULT hr = m_device->SetDepthStencilSurface(Unwrap(pNewZStencil));

  if(SUCCEEDED(hr))
  {
    SCOPED_LOCK(m_D3DLock);
    D3D9SetBound(m_DepthStencil, pNewZStencil);
  }

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(SetDepthStencilSurface, pNewZStencil);
    MarkReferenced(pNewZStencil, eFrameRef_PartialWrite);
  }

  RECORD_FAILED_CALL(hr, SetDepthStencilSurface, pNewZStencil);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface)
{
  if(ppZStencilSurface == NULL)
    return D3DERR_INVALIDCALL;

  SCOPED_LOCK(m_D3DLock);
  *ppZStencilSurface = AddRefBound(m_DepthStencil);
  return *ppZStencilSurface ? S_OK : D3DERR_NOTFOUND;
}

HRESULT __stdcall WrappedD3DDevice9::CreateStateBlock(D3DSTATEBLOCKTYPE Type,
                                                      IDirect3DStateBlock9 **ppSB)
{
  if(ppSB == NULL)
    return m_device->CreateStateBlock(Type, NULL);

  IDirect3DStateBlock9 *real = NULL;
  HRESULT hr = m_device->CreateStateBlock(Type, &real);

  if(SUCCEEDED(hr) && real != NULL)
  {
    SCOPED_LOCK(m_D3DLock);
    *ppSB =
        new WrappedD3DStateBlock9(real, this, D3D9BindingMask::ForStateBlockType(Type), m_Bindings);
    RecordCreation(GetWrappedObject(*ppSB), D3D9Chunk::CreateStateBlock,
                   [&](WriteSerialiser &ser) { Serialise_CreateStateBlock(ser, Type, ppSB); });
  }
  else
  {
    *ppSB = NULL;
  }

  if(FAILED(hr))
    RecordFailedCall(hr, ToStr(D3D9Chunk::CreateStateBlock));
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::BeginStateBlock()
{
  HRESULT hr = m_device->BeginStateBlock();

  if(SUCCEEDED(hr))
  {
    SCOPED_LOCK(m_D3DLock);
    SAFE_DELETE(m_Recording);
    m_Recording = new D3D9ObjectBindings();
    m_RecordingMask = D3D9BindingMask();

    FreeRecordingRecord();
    if(IsCaptureMode(m_State))
    {
      m_RecordingRecord = m_ResourceManager->AddResourceRecord(ResourceIDGen::GetNewUniqueID());
      m_RecordingRecord->DataInSerialiser = false;
      m_RecordingRecord->Length = 0;
      m_RecordingRecord->InternalResource = true;

      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(D3D9Chunk::BeginStateBlock);
      Serialise_BeginStateBlock(ser);
      AddStateChunk(scope.Get());
    }
  }

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::EndStateBlock(IDirect3DStateBlock9 **ppSB)
{
  IDirect3DStateBlock9 *real = NULL;
  HRESULT hr = m_device->EndStateBlock(ppSB ? &real : NULL);

  SCOPED_LOCK(m_D3DLock);

  if(SUCCEEDED(hr) && real != NULL)
  {
    if(m_Recording)
    {
      *ppSB = new WrappedD3DStateBlock9(real, this, m_RecordingMask, *m_Recording);
    }
    else
    {
      RDCERR("EndStateBlock succeeded but we weren't tracking a recording");
      *ppSB = new WrappedD3DStateBlock9(real, this, D3D9BindingMask(), m_Bindings);
    }
  }
  else if(ppSB)
  {
    *ppSB = NULL;
  }

  if(SUCCEEDED(hr) && ppSB && *ppSB)
  {
    // The state block's contents come from the calls recorded between BeginStateBlock and here,
    // which are kept in m_RecordingRecord. Mid-frame, those calls are in the frame too.
    RecordCreation(GetWrappedObject(*ppSB), D3D9Chunk::EndStateBlock,
                   [&](WriteSerialiser &ser) { Serialise_EndStateBlock(ser, ppSB); });

    WrappedD3DStateBlock9 *sb = (WrappedD3DStateBlock9 *)GetWrappedObject(*ppSB);
    D3D9ResourceRecord *record = sb->GetResourceRecord();
    if(record && m_RecordingRecord)
    {
      // the objects the recorded calls set
      const D3D9ObjectBindings &b = sb->GetBindings();
      IUnknown *objs[D3D9_NUM_TEXTURE_SLOTS + D3D9_NUM_STREAMS + 4] = {};
      size_t n = 0;
      for(IDirect3DBaseTexture9 *tex : b.textures)
        objs[n++] = tex;
      for(const D3D9StreamBinding &stream : b.streams)
        objs[n++] = stream.buffer;
      objs[n++] = b.indices;
      objs[n++] = b.vs;
      objs[n++] = b.ps;
      objs[n++] = b.decl;
      for(IUnknown *o : objs)
      {
        WrappedD3D9Object *w = GetWrappedObject(o);
        if(w && w->GetResourceRecord())
          m_RecordingRecord->AddParent(w->GetResourceRecord());
      }

      record->AddParent(m_RecordingRecord);
    }
  }

  // recording ends whether or not the state block was returned
  if(SUCCEEDED(hr))
  {
    SAFE_DELETE(m_Recording);
    // the state block's record holds a reference if it took the chunks
    FreeRecordingRecord();
  }

  if(SUCCEEDED(hr) && ppSB && *ppSB && IsActiveCapturing(m_State))
  {
    RECORD_FRAME_CHUNK(EndStateBlock, ppSB);
    MarkReferenced(*ppSB, eFrameRef_PartialWrite);
  }

  if(FAILED(hr))
    RecordFailedCall(hr, ToStr(D3D9Chunk::EndStateBlock));
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture)
{
  const uint32_t slot = D3D9SamplerToSlot(Stage);

  if(ppTexture == NULL || slot == ~0U)
  {
    if(ppTexture)
      *ppTexture = NULL;
    return D3DERR_INVALIDCALL;
  }

  SCOPED_LOCK(m_D3DLock);
  *ppTexture = AddRefBound(m_Bindings.textures[slot]);
  return S_OK;
}

HRESULT __stdcall WrappedD3DDevice9::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture)
{
  HRESULT hr = m_device->SetTexture(Stage, Unwrap(pTexture));

  const uint32_t slot = D3D9SamplerToSlot(Stage);

  if(SUCCEEDED(hr) && slot != ~0U)
  {
    SCOPED_LOCK(m_D3DLock);
    BindingTarget().SetTexture(slot, pTexture);
    if(m_Recording)
      m_RecordingMask.textures |= (1U << slot);
  }

  if(SUCCEEDED(hr) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetTexture, Stage, pTexture);
    MarkReferenced(pTexture, eFrameRef_Read);
  }

  RECORD_FAILED_CALL(hr, SetTexture, Stage, pTexture);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType,
                                                     UINT PrimitiveCount,
                                                     CONST void *pVertexStreamZeroData,
                                                     UINT VertexStreamZeroStride)
{
  PrepareRenderTargetsForWrite();
  HRESULT hr = m_device->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData,
                                         VertexStreamZeroStride);

  // the runtime unbinds stream 0 after a UP draw
  if(SUCCEEDED(hr))
  {
    SCOPED_LOCK(m_D3DLock);
    m_Bindings.SetStream(0, NULL, 0, 0);
  }

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(DrawPrimitiveUP, PrimitiveType, PrimitiveCount, pVertexStreamZeroData,
                       VertexStreamZeroStride);
    MarkDrawReferenced();
  }

  RECORD_FAILED_CALL(hr, DrawPrimitiveUP, PrimitiveType, PrimitiveCount, NULL,
                     VertexStreamZeroStride);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount,
    CONST void *pIndexData, D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData,
    UINT VertexStreamZeroStride)
{
  PrepareRenderTargetsForWrite();
  HRESULT hr = m_device->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices,
                                                PrimitiveCount, pIndexData, IndexDataFormat,
                                                pVertexStreamZeroData, VertexStreamZeroStride);

  // the runtime unbinds stream 0 and the index buffer after an indexed UP draw
  if(SUCCEEDED(hr))
  {
    SCOPED_LOCK(m_D3DLock);
    m_Bindings.SetStream(0, NULL, 0, 0);
    m_Bindings.SetIndices(NULL);
  }

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(DrawIndexedPrimitiveUP, PrimitiveType, MinVertexIndex, NumVertices,
                       PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData,
                       VertexStreamZeroStride);
    MarkDrawReferenced();
  }

  RECORD_FAILED_CALL(hr, DrawIndexedPrimitiveUP, PrimitiveType, MinVertexIndex, NumVertices,
                     PrimitiveCount, NULL, IndexDataFormat, NULL, VertexStreamZeroStride);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::ProcessVertices(UINT SrcStartIndex, UINT DestIndex,
                                                     UINT VertexCount,
                                                     IDirect3DVertexBuffer9 *pDestBuffer,
                                                     IDirect3DVertexDeclaration9 *pVertexDecl,
                                                     DWORD Flags)
{
  PrepareForWrite(pDestBuffer);
  HRESULT hr = m_device->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, Unwrap(pDestBuffer),
                                         Unwrap(pVertexDecl), Flags);

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(ProcessVertices, SrcStartIndex, DestIndex, VertexCount, pDestBuffer,
                       pVertexDecl, Flags);
    MarkReferenced(pDestBuffer, eFrameRef_PartialWrite);
    MarkReferenced(pVertexDecl, eFrameRef_Read);
    MarkDrawReferenced();
  }

  RECORD_FAILED_CALL(hr, ProcessVertices, SrcStartIndex, DestIndex, VertexCount, pDestBuffer,
                     pVertexDecl, Flags);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9 *pVertexElements,
                                                             IDirect3DVertexDeclaration9 **ppDecl)
{
  if(ppDecl == NULL)
    return m_device->CreateVertexDeclaration(pVertexElements, NULL);

  IDirect3DVertexDeclaration9 *real = NULL;
  HRESULT hr = m_device->CreateVertexDeclaration(pVertexElements, &real);
  WrapCreated<WrappedD3DVertexDeclaration9>(hr, real, ppDecl, this);
  RecordCreated(hr, GetWrappedObject(*ppDecl), D3D9Chunk::CreateVertexDeclaration,
                [&](WriteSerialiser &ser) {
                  Serialise_CreateVertexDeclaration(ser, pVertexElements, ppDecl);
                });
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl)
{
  HRESULT hr = m_device->SetVertexDeclaration(Unwrap(pDecl));

  if(SUCCEEDED(hr))
  {
    SCOPED_LOCK(m_D3DLock);
    BindingTarget().SetDecl(pDecl, false);
    if(m_Recording)
      m_RecordingMask.decl = true;
  }

  if(SUCCEEDED(hr) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetVertexDeclaration, pDecl);
    MarkReferenced(pDecl, eFrameRef_Read);
  }

  RECORD_FAILED_CALL(hr, SetVertexDeclaration, pDecl);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl)
{
  if(ppDecl == NULL)
    return D3DERR_INVALIDCALL;

  SCOPED_LOCK(m_D3DLock);

  if(!m_Bindings.declFromFVF)
  {
    *ppDecl = AddRefBound(m_Bindings.decl);
    return S_OK;
  }

  // SetFVF made the runtime create a declaration internally, which we've never seen. Ask for it and
  // wrap it (once - the wrapper is reused while it's alive).
  IDirect3DVertexDeclaration9 *real = NULL;
  HRESULT hr = m_device->GetVertexDeclaration(&real);

  if(FAILED(hr) || real == NULL)
  {
    *ppDecl = NULL;
    return hr;
  }

  if(m_ResourceManager->HasWrapper(real))
  {
    WrappedD3D9Object *existing = GetWrappedObject(m_ResourceManager->GetWrapper(real));
    if(existing && existing->GetResourceType() == Resource_VertexDeclaration)
    {
      *ppDecl = (IDirect3DVertexDeclaration9 *)existing->GetWrappedUnknown();
      (*ppDecl)->AddRef();
      real->Release();
      return S_OK;
    }
  }

  // the new wrapper takes over the reference returned to us
  *ppDecl = new WrappedD3DVertexDeclaration9(real, this);
  return S_OK;
}

HRESULT __stdcall WrappedD3DDevice9::SetFVF(DWORD FVF)
{
  HRESULT hr = m_device->SetFVF(FVF);

  // setting an FVF replaces the vertex declaration with one owned by the runtime
  if(SUCCEEDED(hr))
  {
    SCOPED_LOCK(m_D3DLock);
    BindingTarget().SetDecl(NULL, true);
    if(m_Recording)
      m_RecordingMask.decl = true;
  }

  if(SUCCEEDED(hr) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetFVF, FVF);
  }

  RECORD_FAILED_CALL(hr, SetFVF, FVF);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::CreateVertexShader(CONST DWORD *pFunction,
                                                        IDirect3DVertexShader9 **ppShader)
{
  if(ppShader == NULL)
    return m_device->CreateVertexShader(pFunction, NULL);

  IDirect3DVertexShader9 *real = NULL;
  HRESULT hr = m_device->CreateVertexShader(pFunction, &real);
  WrapCreated<WrappedD3DVertexShader9>(hr, real, ppShader, this);
  if(SUCCEEDED(hr) && *ppShader)
    RecordCreation(
        GetWrappedObject(*ppShader), D3D9Chunk::CreateVertexShader,
        [&](WriteSerialiser &ser) { Serialise_CreateVertexShader(ser, pFunction, ppShader); });

  // only the name and result: the bytecode might not parse when creation failed
  if(FAILED(hr))
    RecordFailedCall(hr, ToStr(D3D9Chunk::CreateVertexShader));
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::SetVertexShader(IDirect3DVertexShader9 *pShader)
{
  HRESULT hr = m_device->SetVertexShader(Unwrap(pShader));

  if(SUCCEEDED(hr))
  {
    SCOPED_LOCK(m_D3DLock);
    BindingTarget().SetVertexShader(pShader);
    if(m_Recording)
      m_RecordingMask.vs = true;
  }

  if(SUCCEEDED(hr) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetVertexShader, pShader);
    MarkReferenced(pShader, eFrameRef_Read);
  }

  RECORD_FAILED_CALL(hr, SetVertexShader, pShader);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetVertexShader(IDirect3DVertexShader9 **ppShader)
{
  if(ppShader == NULL)
    return D3DERR_INVALIDCALL;

  SCOPED_LOCK(m_D3DLock);
  *ppShader = AddRefBound(m_Bindings.vs);
  return S_OK;
}

HRESULT __stdcall WrappedD3DDevice9::SetStreamSource(UINT StreamNumber,
                                                     IDirect3DVertexBuffer9 *pStreamData,
                                                     UINT OffsetInBytes, UINT Stride)
{
  HRESULT hr = m_device->SetStreamSource(StreamNumber, Unwrap(pStreamData), OffsetInBytes, Stride);

  if(SUCCEEDED(hr) && StreamNumber < D3D9_NUM_STREAMS)
  {
    SCOPED_LOCK(m_D3DLock);
    BindingTarget().SetStream(StreamNumber, pStreamData, OffsetInBytes, Stride);
    if(m_Recording)
      m_RecordingMask.streams |= (1U << StreamNumber);
  }

  if(SUCCEEDED(hr) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetStreamSource, StreamNumber, pStreamData, OffsetInBytes, Stride);
    MarkReferenced(pStreamData, eFrameRef_Read);
  }

  RECORD_FAILED_CALL(hr, SetStreamSource, StreamNumber, pStreamData, OffsetInBytes, Stride);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetStreamSource(UINT StreamNumber,
                                                     IDirect3DVertexBuffer9 **ppStreamData,
                                                     UINT *pOffsetInBytes, UINT *pStride)
{
  if(ppStreamData == NULL || pOffsetInBytes == NULL || pStride == NULL ||
     StreamNumber >= D3D9_NUM_STREAMS)
    return D3DERR_INVALIDCALL;

  SCOPED_LOCK(m_D3DLock);
  const D3D9StreamBinding &stream = m_Bindings.streams[StreamNumber];
  *ppStreamData = AddRefBound(stream.buffer);
  *pOffsetInBytes = stream.offset;
  *pStride = stream.stride;
  return S_OK;
}

HRESULT __stdcall WrappedD3DDevice9::SetIndices(IDirect3DIndexBuffer9 *pIndexData)
{
  HRESULT hr = m_device->SetIndices(Unwrap(pIndexData));

  if(SUCCEEDED(hr))
  {
    SCOPED_LOCK(m_D3DLock);
    BindingTarget().SetIndices(pIndexData);
    if(m_Recording)
      m_RecordingMask.indices = true;
  }

  if(SUCCEEDED(hr) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetIndices, pIndexData);
    MarkReferenced(pIndexData, eFrameRef_Read);
  }

  RECORD_FAILED_CALL(hr, SetIndices, pIndexData);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetIndices(IDirect3DIndexBuffer9 **ppIndexData)
{
  if(ppIndexData == NULL)
    return D3DERR_INVALIDCALL;

  SCOPED_LOCK(m_D3DLock);
  *ppIndexData = AddRefBound(m_Bindings.indices);
  return S_OK;
}

HRESULT __stdcall WrappedD3DDevice9::CreatePixelShader(CONST DWORD *pFunction,
                                                       IDirect3DPixelShader9 **ppShader)
{
  if(ppShader == NULL)
    return m_device->CreatePixelShader(pFunction, NULL);

  IDirect3DPixelShader9 *real = NULL;
  HRESULT hr = m_device->CreatePixelShader(pFunction, &real);
  WrapCreated<WrappedD3DPixelShader9>(hr, real, ppShader, this);
  if(SUCCEEDED(hr) && *ppShader)
    RecordCreation(
        GetWrappedObject(*ppShader), D3D9Chunk::CreatePixelShader,
        [&](WriteSerialiser &ser) { Serialise_CreatePixelShader(ser, pFunction, ppShader); });

  // only the name and result, as for vertex shaders
  if(FAILED(hr))
    RecordFailedCall(hr, ToStr(D3D9Chunk::CreatePixelShader));
  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::SetPixelShader(IDirect3DPixelShader9 *pShader)
{
  HRESULT hr = m_device->SetPixelShader(Unwrap(pShader));

  if(SUCCEEDED(hr))
  {
    SCOPED_LOCK(m_D3DLock);
    BindingTarget().SetPixelShader(pShader);
    if(m_Recording)
      m_RecordingMask.ps = true;
  }

  if(SUCCEEDED(hr) && ShouldRecordState())
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_STATE_CHUNK(SetPixelShader, pShader);
    MarkReferenced(pShader, eFrameRef_Read);
  }

  RECORD_FAILED_CALL(hr, SetPixelShader, pShader);

  return hr;
}

HRESULT __stdcall WrappedD3DDevice9::GetPixelShader(IDirect3DPixelShader9 **ppShader)
{
  if(ppShader == NULL)
    return D3DERR_INVALIDCALL;

  SCOPED_LOCK(m_D3DLock);
  *ppShader = AddRefBound(m_Bindings.ps);
  return S_OK;
}

HRESULT __stdcall WrappedD3DDevice9::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery)
{
  // a NULL output is how applications check whether a query type is supported
  if(ppQuery == NULL)
    return m_device->CreateQuery(Type, NULL);

  IDirect3DQuery9 *real = NULL;
  HRESULT hr = m_device->CreateQuery(Type, &real);
  WrapCreated<WrappedD3DQuery9>(hr, real, ppQuery, this);
  RecordCreated(hr, GetWrappedObject(*ppQuery), D3D9Chunk::CreateQuery,
                [&](WriteSerialiser &ser) { Serialise_CreateQuery(ser, Type, ppQuery); });
  return hr;
}

HRESULT WrappedD3DDevice9::ComposeRects(IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst,
                                        IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects,
                                        IDirect3DVertexBuffer9 *pDstRectDescs,
                                        D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset)
{
  RDCASSERT(m_deviceEx != NULL);
  PrepareForWrite(pDst);
  HRESULT hr = m_deviceEx->ComposeRects(Unwrap(pSrc), Unwrap(pDst), Unwrap(pSrcRectDescs), NumRects,
                                        Unwrap(pDstRectDescs), Operation, Xoffset, Yoffset);

  if(SUCCEEDED(hr) && IsActiveCapturing(m_State))
  {
    SCOPED_LOCK(m_D3DLock);
    RECORD_FRAME_CHUNK(ComposeRects, pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs, Operation,
                       Xoffset, Yoffset);
    MarkReferenced(pSrc, eFrameRef_Read);
    MarkReferenced(pDst, eFrameRef_PartialWrite);
    MarkReferenced(pSrcRectDescs, eFrameRef_Read);
    MarkReferenced(pDstRectDescs, eFrameRef_Read);
  }

  RECORD_FAILED_CALL(hr, ComposeRects, pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs,
                     Operation, Xoffset, Yoffset);

  return hr;
}

HRESULT WrappedD3DDevice9::CheckResourceResidency(IDirect3DResource9 **pResourceArray,
                                                  UINT32 NumResources)
{
  RDCASSERT(m_deviceEx != NULL);

  if(pResourceArray == NULL || NumResources == 0)
    return m_deviceEx->CheckResourceResidency(pResourceArray, NumResources);

  rdcarray<IDirect3DResource9 *> unwrapped;
  unwrapped.resize(NumResources);
  for(UINT32 i = 0; i < NumResources; i++)
    unwrapped[i] = Unwrap(pResourceArray[i]);

  return m_deviceEx->CheckResourceResidency(unwrapped.data(), NumResources);
}
