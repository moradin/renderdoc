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

#include "d3d9_swapchain.h"
#include "d3d9_device.h"

WrappedD3DSwapChain9::WrappedD3DSwapChain9(IDirect3DSwapChain9 *real, WrappedD3DDevice9 *device,
                                           bool implicit)
    : WrappedD3D9DeviceChild(real, device, NULL,
                             implicit ? RealOwnership::None : RealOwnership::WhileExternal),
      m_Implicit(implicit)
{
  UpdateRealEx();
}

WrappedD3DSwapChain9::~WrappedD3DSwapChain9()
{
  for(WrappedD3DSurface9 *s : m_BackBuffers)
    DestroyChild(s);
  for(WrappedD3DSurface9 *s : m_StaleBackBuffers)
    DestroyChild(s);
}

void WrappedD3DSwapChain9::UpdateRealEx()
{
  m_pRealEx = NULL;
  if(SUCCEEDED(m_pReal->QueryInterface(__uuidof(IDirect3DSwapChain9Ex), (void **)&m_pRealEx)) &&
     m_pRealEx)
  {
    // the Ex interface is the same object, we don't need a second reference
    m_pRealEx->Release();
  }
  else
  {
    m_pRealEx = NULL;
  }
}

HRESULT WrappedD3DSwapChain9::QueryInterface(REFIID riid, void **ppvObject)
{
  if(ppvObject == NULL)
    return E_POINTER;

  if(riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DSwapChain9) ||
     (riid == __uuidof(IDirect3DSwapChain9Ex) && m_pRealEx != NULL))
  {
    *ppvObject = (IDirect3DSwapChain9Ex *)this;
    AddRef();
    return S_OK;
  }

  if(riid != __uuidof(IDirect3DSwapChain9Ex))
    WarnUnknownGUID("IDirect3DSwapChain9", riid);

  *ppvObject = NULL;
  return E_NOINTERFACE;
}

WrappedD3DSurface9 *WrappedD3DSwapChain9::GetBackBufferWrapper(UINT iBackBuffer)
{
  SCOPED_LOCK(m_pDevice->D3DLock());

  IDirect3DSurface9 *real = NULL;
  HRESULT hr = m_pReal->GetBackBuffer(iBackBuffer, D3DBACKBUFFER_TYPE_MONO, &real);
  if(FAILED(hr) || real == NULL)
    return NULL;

  // the swapchain owns its back buffers
  real->Release();

  if(iBackBuffer >= m_BackBuffers.size())
    m_BackBuffers.resize(iBackBuffer + 1);

  WrappedD3DSurface9 *&wrapped = m_BackBuffers[iBackBuffer];

  if(wrapped && wrapped->GetReal() != real)
  {
    // shouldn't happen outside of Reset, which is handled in OnDeviceReset
    RDCWARN("Back buffer %u of swapchain changed unexpectedly", iBackBuffer);
    m_StaleBackBuffers.push_back(wrapped);
    wrapped = NULL;
  }

  if(wrapped == NULL)
    wrapped = new WrappedD3DSurface9(real, m_pDevice, this);

  return wrapped;
}

void WrappedD3DSwapChain9::OnDeviceReset(IDirect3DSwapChain9 *newReal)
{
  SCOPED_LOCK(m_pDevice->D3DLock());

  if(newReal && newReal != m_pReal)
  {
    ReplaceReal((IDirect3DSwapChain9Ex *)this, newReal);
    m_pReal = newReal;
    UpdateRealEx();
  }

  D3DPRESENT_PARAMETERS params = {};
  m_pReal->GetPresentParameters(&params);
  const UINT numBuffers = RDCMAX(params.BackBufferCount, 1U);

  for(UINT i = 0; i < m_BackBuffers.size(); i++)
  {
    WrappedD3DSurface9 *wrapped = m_BackBuffers[i];
    if(wrapped == NULL)
      continue;

    IDirect3DSurface9 *real = NULL;
    if(i < numBuffers && SUCCEEDED(m_pReal->GetBackBuffer(i, D3DBACKBUFFER_TYPE_MONO, &real)) && real)
      real->Release();

    // still the same surface, keep the wrapper
    if(real == wrapped->GetReal())
      continue;

    // the real back buffer was recreated. If nothing references the old wrapper we can free it,
    // otherwise keep it around (it's dead, but the application may still release it).
    if(wrapped->GetExtRefCount() == 0 && wrapped->GetIntRefCount() == 0)
    {
      DestroyChild(wrapped);
    }
    else
    {
      RDCWARN("Back buffer %u still referenced across Reset", i);
      m_StaleBackBuffers.push_back(wrapped);
    }

    m_BackBuffers[i] = NULL;
  }

  if(m_BackBuffers.size() > numBuffers)
    m_BackBuffers.resize(numBuffers);
}

HRESULT WrappedD3DSwapChain9::Present(CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                      HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion,
                                      DWORD dwFlags)
{
  m_pDevice->RenderOverlay(m_pReal, hDestWindowOverride);
  return m_pReal->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

HRESULT WrappedD3DSwapChain9::GetFrontBufferData(IDirect3DSurface9 *pDestSurface)
{
  return m_pReal->GetFrontBufferData(Unwrap(pDestSurface));
}

HRESULT WrappedD3DSwapChain9::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                                            IDirect3DSurface9 **ppBackBuffer)
{
  if(ppBackBuffer == NULL)
    return D3DERR_INVALIDCALL;

  *ppBackBuffer = NULL;

  // only mono back buffers exist in D3D9, let the runtime reject anything else
  if(Type != D3DBACKBUFFER_TYPE_MONO)
  {
    IDirect3DSurface9 *real = NULL;
    HRESULT hr = m_pReal->GetBackBuffer(iBackBuffer, Type, &real);
    SAFE_RELEASE(real);
    return FAILED(hr) ? hr : D3DERR_INVALIDCALL;
  }

  WrappedD3DSurface9 *wrapped = GetBackBufferWrapper(iBackBuffer);
  if(wrapped == NULL)
    return D3DERR_INVALIDCALL;

  wrapped->AddRef();
  *ppBackBuffer = wrapped;
  return S_OK;
}
