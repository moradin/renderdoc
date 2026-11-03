/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Baldur Karlsson
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

#include "d3d9_surface.h"
#include "d3d9_swapchain.h"

WrappedD3DSurface9::WrappedD3DSurface9(IDirect3DSurface9 *surface, WrappedD3DSwapChain9 *swapChain,
                                       WrappedD3DDevice9 *device)
    : m_Surface(surface),
      m_ContainerTexture(NULL),
      m_ContainerSwapChain(swapChain),
      m_Device(device),
      m_RefCounter(1)
{
  m_ContainerSwapChain->SoftRef();
}

WrappedD3DSurface9::WrappedD3DSurface9(IDirect3DSurface9 *surface, IDirect3DTexture9 *texture,
                                       WrappedD3DDevice9 *device)
    : m_Surface(surface),
      m_ContainerTexture(texture),
      m_ContainerSwapChain(NULL),
      m_Device(device),
      m_RefCounter(1)
{
}

WrappedD3DSurface9::~WrappedD3DSurface9()
{
  ULONG ref = m_Surface->Release();
  RDCASSERT(ref == 0);

  if(m_ContainerSwapChain)
  {
    m_ContainerSwapChain->SoftRelease();
  }

  // TODO fix this once the texture is wrapped
  // if (m_ContainerTexture)
  //{
  //	m_ContainerTexture->
  //}
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::QueryInterface(REFIID riid, void **ppvObject)
{
  return m_Surface->QueryInterface(riid, ppvObject);
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::GetDevice(IDirect3DDevice9 **ppDevice)
{
  IDirect3DDevice9 *device;
  HRESULT res = m_Surface->GetDevice(&device);
  if(res == S_OK)
  {
    // transfer the refcount to the wrapper
    m_Device->AddRef();
    device->Release();
    *ppDevice = m_Device;
  }
  return res;
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::SetPrivateData(REFGUID refguid, CONST void *pData,
                                                             DWORD SizeOfData, DWORD Flags)
{
  return m_Surface->SetPrivateData(refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::GetPrivateData(REFGUID refguid, void *pData,
                                                             DWORD *pSizeOfData)
{
  return m_Surface->GetPrivateData(refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::FreePrivateData(REFGUID refguid)
{
  return m_Surface->FreePrivateData(refguid);
}

DWORD STDMETHODCALLTYPE WrappedD3DSurface9::SetPriority(DWORD PriorityNew)
{
  return m_Surface->SetPriority(PriorityNew);
}

DWORD STDMETHODCALLTYPE WrappedD3DSurface9::GetPriority()
{
  return m_Surface->GetPriority();
}

void STDMETHODCALLTYPE WrappedD3DSurface9::PreLoad()
{
  m_Surface->PreLoad();
}

D3DRESOURCETYPE STDMETHODCALLTYPE WrappedD3DSurface9::GetType()
{
  return m_Surface->GetType();
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::GetContainer(REFIID riid, void **ppContainer)
{
  if(m_ContainerSwapChain != NULL)
  {
    return m_ContainerSwapChain->QueryInterface(riid, ppContainer);
  }
  else if(m_ContainerTexture != NULL)
  {
    return m_ContainerTexture->QueryInterface(riid, ppContainer);
  }
  else
  {
    return m_Device->QueryInterface(riid, ppContainer);
  }
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::GetDesc(D3DSURFACE_DESC *pDesc)
{
  return m_Surface->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::LockRect(D3DLOCKED_RECT *pLockedRect,
                                                       CONST RECT *pRect, DWORD Flags)
{
  return m_Surface->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::UnlockRect()
{
  return m_Surface->UnlockRect();
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::GetDC(HDC *phdc)
{
  return m_Surface->GetDC(phdc);
}

HRESULT STDMETHODCALLTYPE WrappedD3DSurface9::ReleaseDC(HDC hdc)
{
  return m_Surface->ReleaseDC(hdc);
}
