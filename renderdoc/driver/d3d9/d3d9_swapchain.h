/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of  software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and  permission notice shall be included in
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
#include "d3d9_device.h"
#include "d3d9_surface.h"

class WrappedD3DSwapChain9 : public IDirect3DSwapChain9Ex
{
public:
  WrappedD3DSwapChain9(IDirect3DSwapChain9 *swapChain, WrappedD3DDevice9 *device, UINT index,
                       bool implicit);

  ULONG SoftRef() { return m_SoftRefCounter.AddRef(); }
  ULONG SoftRelease() { return m_SoftRefCounter.Release(); }
  /*** IUnknown methods ***/
  virtual HRESULT __stdcall QueryInterface(REFIID riid, void **ppvObj);
  virtual ULONG __stdcall AddRef();
  virtual ULONG __stdcall Release();

  /*** IDirect3DSwapChain9 methods ***/
  virtual HRESULT __stdcall Present(CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                    HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion,
                                    DWORD dwFlags);
  virtual HRESULT __stdcall GetFrontBufferData(IDirect3DSurface9 *pDestSurface);
  virtual HRESULT __stdcall GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                                          IDirect3DSurface9 **ppBackBuffer);
  virtual HRESULT __stdcall GetRasterStatus(D3DRASTER_STATUS *pRasterStatus);
  virtual HRESULT __stdcall GetDisplayMode(D3DDISPLAYMODE *pMode);
  virtual HRESULT __stdcall GetDevice(IDirect3DDevice9 **ppDevice);
  virtual HRESULT __stdcall GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters);

  /*** IDirect3DSwapChain9Ex methods ***/
  virtual HRESULT __stdcall GetLastPresentCount(UINT *pLastPresentCount);
  virtual HRESULT __stdcall GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics);
  virtual HRESULT __stdcall GetDisplayModeEx(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation);

private:
  RefCounter9 m_RefCounter;
  RefCounter9 m_SoftRefCounter;

  bool m_Implicit;

  IDirect3DSwapChain9 *m_SwapChain;
  IDirect3DSwapChain9Ex *m_SwapChainEx;
  WrappedD3DDevice9 *m_Device;
  UINT m_Index;

  rdcarray<WrappedD3DSurface9 *> m_BackBuffers;
};
