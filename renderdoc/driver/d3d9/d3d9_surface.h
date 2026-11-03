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

#pragma once
#include "d3d9_common.h"

class WrappedD3DSwapChain9;

class WrappedD3DSurface9 : IDirect3DSurface9
{
public:
  WrappedD3DSurface9(IDirect3DSurface9 *surface, WrappedD3DSwapChain9 *swapChain,
                     WrappedD3DDevice9 *device);
  WrappedD3DSurface9(IDirect3DSurface9 *surface, IDirect3DTexture9 *texture,
                     WrappedD3DDevice9 *device);

  ~WrappedD3DSurface9();

  /*** IUnknown methods ***/
  ULONG STDMETHODCALLTYPE AddRef() { return m_RefCounter.AddRef(); }
  ULONG STDMETHODCALLTYPE Release()
  {
    unsigned int ret = m_RefCounter.Release();
    // CheckForDeath();
    return ret;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject);

  /*** IDirect3DResource9 methods ***/
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice);
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void *pData, DWORD SizeOfData,
                                           DWORD Flags);
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData);
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid);
  DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew);
  DWORD STDMETHODCALLTYPE GetPriority();
  void STDMETHODCALLTYPE PreLoad();
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType();

  /*** IDirect3DSurface9 methods ***/
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer);
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc);
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags);
  HRESULT STDMETHODCALLTYPE UnlockRect();
  HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc);
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc);

private:
  IDirect3DSurface9 *m_Surface;

  IDirect3DTexture9 *m_ContainerTexture;

  WrappedD3DSwapChain9 *m_ContainerSwapChain;

  WrappedD3DDevice9 *m_Device;

  RefCounter9 m_RefCounter;
};