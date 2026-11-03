#include "d3d9_swapchain.h"

#include "../dxgi/dxgi_common.h"
#include "serialise/serialiser.h"

WrappedD3DSwapChain9::WrappedD3DSwapChain9(IDirect3DSwapChain9 *swapChain,
                                           WrappedD3DDevice9 *device, UINT index, bool implicit)
    : m_RefCounter(1),
      m_SoftRefCounter(0),
      m_Implicit(implicit),
      m_SwapChain(swapChain),
      m_Device(device),
      m_Index(index)
{
  m_SwapChain->QueryInterface(__uuidof(IDirect3DSwapChain9Ex), (void **)&m_SwapChainEx);
  m_Device->SoftRef();
  m_Device->InternalRef();    // swapchains are never set as resources
}

HRESULT __stdcall WrappedD3DSwapChain9::QueryInterface(REFIID riid, void **ppvObj)
{
  if(riid == __uuidof(IDirect3DSwapChain9))
  {
    HRESULT hr = m_SwapChain->QueryInterface(riid, ppvObj);

    if(SUCCEEDED(hr))
    {
      AddRef();
      m_SwapChain->Release();
      *ppvObj = this;
      return S_OK;
    }
    else
    {
      *ppvObj = NULL;
      return hr;
    }
  }
  else if(riid == __uuidof(IDirect3DSwapChain9Ex))
  {
    HRESULT hr = m_SwapChain->QueryInterface(riid, ppvObj);

    if(SUCCEEDED(hr))
    {
      AddRef();
      m_SwapChain->Release();
      *ppvObj = this;
      return S_OK;
    }
    else
    {
      *ppvObj = NULL;
      return hr;
    }
  }
  else
  {
    WarnUnknownGUID("IDirect3DSwapChain", riid);
  }

  return m_SwapChain->QueryInterface(riid, ppvObj);
}

ULONG __stdcall WrappedD3DSwapChain9::AddRef()
{
  unsigned int ref = m_RefCounter.SoftRef(m_Device);
  return m_Implicit ? ref - 1 : ref;
}

ULONG __stdcall WrappedD3DSwapChain9::Release()
{
  ULONG ref = m_RefCounter.SoftRelease(m_Device);
  if(ref == 0)
  {
    m_Device->RemoveSwapchain(m_Index);
    m_Device->SoftRelease();

    m_SwapChainEx->Release();

    UINT refCount = m_SwapChain->Release();
    RDCASSERT(refCount == 0);
    delete this;
  }
  return m_Implicit ? ref - 1 : ref;
}

HRESULT __stdcall WrappedD3DSwapChain9::Present(CONST RECT *pSourceRect, CONST RECT *pDestRect,
                                                HWND hDestWindowOverride,
                                                CONST RGNDATA *pDirtyRegion, DWORD dwFlags)
{
  m_Device->RenderOverlay(hDestWindowOverride);
  return m_SwapChain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

HRESULT __stdcall WrappedD3DSwapChain9::GetFrontBufferData(IDirect3DSurface9 *pDestSurface)
{
  return m_SwapChain->GetFrontBufferData(pDestSurface);
}

HRESULT __stdcall WrappedD3DSwapChain9::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                                                      IDirect3DSurface9 **ppBackBuffer)
{
  // IDirect3DSurface9* surface;
  HRESULT res = m_SwapChain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
  if(res == S_OK)
  {
  }

  return res;
}

HRESULT __stdcall WrappedD3DSwapChain9::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus)
{
  return m_SwapChain->GetRasterStatus(pRasterStatus);
}

HRESULT __stdcall WrappedD3DSwapChain9::GetDisplayMode(D3DDISPLAYMODE *pMode)
{
  return m_SwapChain->GetDisplayMode(pMode);
}

HRESULT __stdcall WrappedD3DSwapChain9::GetDevice(IDirect3DDevice9 **ppDevice)
{
  IDirect3DDevice9 *device;
  HRESULT res = m_SwapChain->GetDevice(&device);
  if(res == S_OK)
  {
    // transfer the refcount to the wrapper
    m_Device->AddRef();
    device->Release();
    *ppDevice = m_Device;
  }
  return res;
}

HRESULT __stdcall WrappedD3DSwapChain9::GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters)
{
  return m_SwapChain->GetPresentParameters(pPresentationParameters);
}

HRESULT __stdcall WrappedD3DSwapChain9::GetLastPresentCount(UINT *pLastPresentCount)
{
  RDCASSERT(m_SwapChainEx != NULL);
  return m_SwapChainEx->GetLastPresentCount(pLastPresentCount);
}

HRESULT __stdcall WrappedD3DSwapChain9::GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics)
{
  RDCASSERT(m_SwapChainEx != NULL);
  return m_SwapChainEx->GetPresentStats(pPresentationStatistics);
}

HRESULT __stdcall WrappedD3DSwapChain9::GetDisplayModeEx(D3DDISPLAYMODEEX *pMode,
                                                         D3DDISPLAYROTATION *pRotation)
{
  RDCASSERT(m_SwapChainEx != NULL);
  return m_SwapChainEx->GetDisplayModeEx(pMode, pRotation);
}
