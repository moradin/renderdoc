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

WrappedD3D9::WrappedD3D9(IDirect3D9 *direct3D9) : m_Direct3D(direct3D9)
{
  if(SUCCEEDED(m_Direct3D->QueryInterface(__uuidof(IDirect3D9Ex), (void **)&m_Direct3DEx)) &&
     m_Direct3DEx)
    m_Direct3DEx->Release();
  else
    m_Direct3DEx = NULL;
}

HRESULT __stdcall WrappedD3D9::QueryInterface(REFIID riid, void **ppvObj)
{
  if(ppvObj == NULL)
    return E_POINTER;

  if(riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9) ||
     (riid == __uuidof(IDirect3D9Ex) && m_Direct3DEx != NULL))
  {
    AddRef();
    *ppvObj = (IDirect3D9Ex *)this;
    return S_OK;
  }

  if(riid != __uuidof(IDirect3D9Ex))
    WarnUnknownGUID("IDirect3D9", riid);

  *ppvObj = NULL;
  return E_NOINTERFACE;
}

ULONG __stdcall WrappedD3D9::AddRef()
{
  return (ULONG)Atomic::Inc32(&m_RefCount);
}

ULONG __stdcall WrappedD3D9::Release()
{
  int32_t ref = Atomic::Dec32(&m_RefCount);
  if(ref == 0)
  {
    SAFE_RELEASE(m_Direct3D);
    delete this;
  }
  return (ULONG)ref;
}

HRESULT __stdcall WrappedD3D9::RegisterSoftwareDevice(void *pInitializeFunction)
{
  return m_Direct3D->RegisterSoftwareDevice(pInitializeFunction);
}

UINT __stdcall WrappedD3D9::GetAdapterCount()
{
  UINT count = m_Direct3D->GetAdapterCount();
  return count;
}

HRESULT __stdcall WrappedD3D9::GetAdapterIdentifier(UINT Adapter, DWORD Flags,
                                                    D3DADAPTER_IDENTIFIER9 *pIdentifier)
{
  return m_Direct3D->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
}

UINT __stdcall WrappedD3D9::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
{
  UINT count = m_Direct3D->GetAdapterModeCount(Adapter, Format);
  return count;
}

HRESULT __stdcall WrappedD3D9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode,
                                                D3DDISPLAYMODE *pMode)
{
  HRESULT res = m_Direct3D->EnumAdapterModes(Adapter, Format, Mode, pMode);
  return res;
}

HRESULT __stdcall WrappedD3D9::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode)
{
  return m_Direct3D->GetAdapterDisplayMode(Adapter, pMode);
}

HRESULT __stdcall WrappedD3D9::CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType,
                                               D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat,
                                               BOOL bWindowed)
{
  return m_Direct3D->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed);
}

HRESULT __stdcall WrappedD3D9::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType,
                                                 D3DFORMAT AdapterFormat, DWORD Usage,
                                                 D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
{
  return m_Direct3D->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
}

HRESULT __stdcall WrappedD3D9::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType,
                                                          D3DFORMAT SurfaceFormat, BOOL Windowed,
                                                          D3DMULTISAMPLE_TYPE MultiSampleType,
                                                          DWORD *pQualityLevels)
{
  return m_Direct3D->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed,
                                                MultiSampleType, pQualityLevels);
}

HRESULT __stdcall WrappedD3D9::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType,
                                                      D3DFORMAT AdapterFormat,
                                                      D3DFORMAT RenderTargetFormat,
                                                      D3DFORMAT DepthStencilFormat)
{
  return m_Direct3D->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat,
                                            DepthStencilFormat);
}

HRESULT __stdcall WrappedD3D9::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType,
                                                           D3DFORMAT SourceFormat,
                                                           D3DFORMAT TargetFormat)
{
  return m_Direct3D->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
}

HRESULT __stdcall WrappedD3D9::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps)
{
  return m_Direct3D->GetDeviceCaps(Adapter, DeviceType, pCaps);
}

HMONITOR __stdcall WrappedD3D9::GetAdapterMonitor(UINT Adapter)
{
  return m_Direct3D->GetAdapterMonitor(Adapter);
}

HRESULT __stdcall WrappedD3D9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                            DWORD BehaviorFlags,
                                            D3DPRESENT_PARAMETERS *pPresentationParameters,
                                            IDirect3DDevice9 **ppReturnedDeviceInterface)
{
  if(ppReturnedDeviceInterface == NULL)
    return D3DERR_INVALIDCALL;

  IDirect3DDevice9 *device = NULL;
  HRESULT res = m_Direct3D->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                         pPresentationParameters, &device);
  if(SUCCEEDED(res) && device != NULL)
  {
    RDCLOG("App creating d3d9 device");

    HWND wnd = pPresentationParameters->hDeviceWindow;
    if(wnd == NULL)
      wnd = hFocusWindow;

    if(!wnd)
      RDCWARN("Couldn't find valid non-NULL window at CreateDevice time");

    WrappedD3DDevice9 *wrappedDevice = new WrappedD3DDevice9(device, this, wnd);
    wrappedDevice->LazyInit();    // TODO this can be moved later probably
    *ppReturnedDeviceInterface = wrappedDevice;
  }
  else if(ppReturnedDeviceInterface)
  {
    *ppReturnedDeviceInterface = NULL;
  }

  return res;
}

UINT __stdcall WrappedD3D9::GetAdapterModeCountEx(UINT Adapter, CONST D3DDISPLAYMODEFILTER *pFilter)
{
  RDCASSERT(m_Direct3DEx != NULL);
  return m_Direct3DEx->GetAdapterModeCountEx(Adapter, pFilter);
}

HRESULT __stdcall WrappedD3D9::EnumAdapterModesEx(UINT Adapter, CONST D3DDISPLAYMODEFILTER *pFilter,
                                                  UINT Mode, D3DDISPLAYMODEEX *pMode)
{
  RDCASSERT(m_Direct3DEx != NULL);
  return m_Direct3DEx->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode);
}

HRESULT __stdcall WrappedD3D9::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode,
                                                       D3DDISPLAYROTATION *pRotation)
{
  RDCASSERT(m_Direct3DEx != NULL);
  return m_Direct3DEx->GetAdapterDisplayModeEx(Adapter, pMode, pRotation);
}

HRESULT __stdcall WrappedD3D9::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType,
                                              HWND hFocusWindow, DWORD BehaviorFlags,
                                              D3DPRESENT_PARAMETERS *pPresentationParameters,
                                              D3DDISPLAYMODEEX *pFullscreenDisplayMode,
                                              IDirect3DDevice9Ex **ppReturnedDeviceInterface)
{
  RDCASSERT(m_Direct3DEx != NULL);

  if(ppReturnedDeviceInterface == NULL)
    return D3DERR_INVALIDCALL;

  IDirect3DDevice9Ex *device = NULL;

  HRESULT res =
      m_Direct3DEx->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                   pPresentationParameters, pFullscreenDisplayMode, &device);
  if(SUCCEEDED(res) && device != NULL)
  {
    RDCLOG("App creating D3D9 DeviceEx");

    HWND wnd = pPresentationParameters->hDeviceWindow;
    if(wnd == NULL)
      wnd = hFocusWindow;

    if(!wnd)
      RDCWARN("Couldn't find valid non-NULL window at CreateDevice time");

    WrappedD3DDevice9 *wrappedDevice = new WrappedD3DDevice9(device, this, wnd);
    wrappedDevice->LazyInit();    // TODO this can be moved later probably
    *ppReturnedDeviceInterface = wrappedDevice;
  }
  else if(ppReturnedDeviceInterface)
  {
    *ppReturnedDeviceInterface = NULL;
  }

  return res;
}

HRESULT __stdcall WrappedD3D9::GetAdapterLUID(UINT Adapter, LUID *pLUID)
{
  RDCASSERT(m_Direct3DEx != NULL);
  return m_Direct3DEx->GetAdapterLUID(Adapter, pLUID);
}
