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

#include "d3d9_resources.h"

// Implicit swapchains are created with the device and owned by it: the device holds an internal
// reference and the real swapchain belongs to the real device. Additional swapchains are ordinary
// standalone objects.
//
// Back buffers are children of their swapchain (refcounts forward to it). Reset can recreate the
// real back buffers, see OnDeviceReset.
class WrappedD3DSwapChain9 : public WrappedD3D9DeviceChild<IDirect3DSwapChain9Ex, IDirect3DSwapChain9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DSwapChain9);

  WrappedD3DSwapChain9(IDirect3DSwapChain9 *real, WrappedD3DDevice9 *device, bool implicit);
  ~WrappedD3DSwapChain9();

  D3D9ResourceType GetResourceType() const { return Resource_SwapChain; }
  const char *TypeName() const { return "IDirect3DSwapChain9"; }

  bool IsImplicit() const { return m_Implicit; }

  // returns the wrapper for a back buffer, creating it if needed, without adding a reference
  WrappedD3DSurface9 *GetBackBufferWrapper(UINT iBackBuffer);

  // called after a successful Reset/ResetEx on the device, with the real implicit swapchain
  void OnDeviceReset(IDirect3DSwapChain9 *newReal);

  //////////////////////////////
  // implement IUnknown

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject);

  //////////////////////////////
  // implement IDirect3DSwapChain9

  HRESULT STDMETHODCALLTYPE Present(CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                    HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion,
                                    DWORD dwFlags);
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9 *pDestSurface);
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                                          IDirect3DSurface9 **ppBackBuffer);
  HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *pRasterStatus)
  {
    return m_pReal->GetRasterStatus(pRasterStatus);
  }
  HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *pMode)
  {
    return m_pReal->GetDisplayMode(pMode);
  }
  HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters)
  {
    return m_pReal->GetPresentParameters(pPresentationParameters);
  }

  //////////////////////////////
  // implement IDirect3DSwapChain9Ex (only reachable if the real swapchain supports it)

  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount)
  {
    return m_pRealEx->GetLastPresentCount(pLastPresentCount);
  }
  HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics)
  {
    return m_pRealEx->GetPresentStats(pPresentationStatistics);
  }
  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation)
  {
    return m_pRealEx->GetDisplayModeEx(pMode, pRotation);
  }

private:
  void UpdateRealEx();

  // no reference held, valid as long as m_pReal is
  IDirect3DSwapChain9Ex *m_pRealEx = NULL;
  bool m_Implicit;

  rdcarray<WrappedD3DSurface9 *> m_BackBuffers;
  // back buffers the application still referenced when a Reset replaced them
  rdcarray<WrappedD3DSurface9 *> m_StaleBackBuffers;
};
