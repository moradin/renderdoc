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
#include "d3d9_debug.h"
#include "d3d9_resources.h"
#include "d3d9_swapchain.h"

WrappedD3DDevice9::WrappedD3DDevice9(IDirect3DDevice9 *device, WrappedD3D9 *wrappedD3D, HWND wnd)
    : m_device(device), m_D3D(wrappedD3D)
{
  m_D3D->AddRef();

  if(SUCCEEDED(m_device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void **)&m_deviceEx)) &&
     m_deviceEx)
    m_deviceEx->Release();
  else
    m_deviceEx = NULL;

  if(RenderDoc::Inst().IsReplayApp())
    m_State = CaptureState::LoadingReplaying;
  else
    m_State = CaptureState::BackgroundCapturing;

  m_ResourceManager = new D3D9ResourceManager(m_State, this);

  if(!RenderDoc::Inst().IsReplayApp())
  {
    RenderDoc::Inst().AddDeviceFrameCapturer((IDirect3DDevice9 *)this, this);

    m_Wnd = wnd;

    if(wnd != NULL)
      RenderDoc::Inst().AddFrameCapturer(DeviceOwnedWindow((IDirect3DDevice9 *)this, wnd), this);
  }

  InitDeviceOwnedObjects();
}

WrappedD3DDevice9::~WrappedD3DDevice9()
{
  if(!RenderDoc::Inst().IsReplayApp())
  {
    RenderDoc::Inst().RemoveDeviceFrameCapturer((IDirect3DDevice9 *)this);

    if(m_Wnd != NULL)
      RenderDoc::Inst().RemoveFrameCapturer(DeviceOwnedWindow((IDirect3DDevice9 *)this, m_Wnd));
  }

  // only uses real objects
  SAFE_DELETE(m_DebugManager);

  // Nothing the application can see is alive any more (every externally referenced object holds a
  // reference on the device), so dropping our internal references destroys every wrapper.
  SAFE_DELETE(m_Recording);
  m_Bindings.ReleaseAll();
  for(uint32_t i = 0; i < D3D9_NUM_RENDER_TARGETS; i++)
    D3D9SetBound(m_RenderTargets[i], (IDirect3DSurface9 *)NULL);
  D3D9SetBound(m_DepthStencil, (IDirect3DSurface9 *)NULL);

  if(m_AutoDepthStencil)
    m_AutoDepthStencil->IntRelease();
  m_AutoDepthStencil = NULL;

  for(WrappedD3DSwapChain9 *swap : m_ImplicitSwapChains)
    swap->IntRelease();
  m_ImplicitSwapChains.clear();

  if(m_ResourceManager)
    m_ResourceManager->Shutdown();
  SAFE_DELETE(m_ResourceManager);

  ULONG realRefs = m_device->Release();
  if(realRefs != 0)
    RDCWARN("Real D3D9 device still has %u references after the wrapper was destroyed", realRefs);
  m_device = NULL;
  m_deviceEx = NULL;

  SAFE_RELEASE(m_D3D);
}

void WrappedD3DDevice9::LazyInit()
{
  m_DebugManager = new D3D9DebugManager(this);
}

ULONG WrappedD3DDevice9::AddRef()
{
  return (ULONG)Atomic::Inc32(&m_RefCount);
}

ULONG WrappedD3DDevice9::Release()
{
  int32_t ref = Atomic::Dec32(&m_RefCount);
  if(ref == 0)
    delete this;
  return (ULONG)ref;
}

HRESULT WrappedD3DDevice9::QueryInterface(REFIID riid, void **ppvObject)
{
  // RenderDoc UUID {A7AA6116-9C8D-4BBA-9083-B4D816B71B78}
  static const GUID IRenderDoc_uuid = {
      0xa7aa6116, 0x9c8d, 0x4bba, {0x90, 0x83, 0xb4, 0xd8, 0x16, 0xb7, 0x1b, 0x78}};

  if(ppvObject == NULL)
    return E_POINTER;

  if(riid == IRenderDoc_uuid || riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9) ||
     (riid == __uuidof(IDirect3DDevice9Ex) && m_deviceEx != NULL))
  {
    AddRef();
    *ppvObject = (IDirect3DDevice9Ex *)this;
    return S_OK;
  }

  if(riid == __uuidof(IDirect3DDevice9Ex))
  {
    *ppvObject = NULL;
    return E_NOINTERFACE;
  }

  // Other interfaces (e.g. IDirect3DDevice9Video) are passed through. They'll operate on unwrapped
  // objects, which works for anything that doesn't hand objects back to us.
  WarnUnknownGUID("IDirect3DDevice9", riid);
  return m_device->QueryInterface(riid, ppvObject);
}

void WrappedD3DDevice9::InitDeviceOwnedObjects()
{
  const UINT numSwaps = m_device->GetNumberOfSwapChains();

  for(UINT i = 0; i < numSwaps; i++)
  {
    IDirect3DSwapChain9 *real = NULL;
    if(FAILED(m_device->GetSwapChain(i, &real)) || real == NULL)
    {
      RDCERR("Couldn't get implicit swapchain %u", i);
      continue;
    }

    // the device owns its implicit swapchains
    real->Release();

    WrappedD3DSwapChain9 *wrapped = new WrappedD3DSwapChain9(real, this, true);
    wrapped->IntAddRef();
    m_ImplicitSwapChains.push_back(wrapped);
  }

  WrapAutoDepthStencil();

  // a new (or reset) device renders to the first back buffer of the first swapchain, with the
  // automatic depth stencil surface if there is one
  if(!m_ImplicitSwapChains.empty())
  {
    WrappedD3DSurface9 *bb = m_ImplicitSwapChains[0]->GetBackBufferWrapper(0);
    D3D9SetBound(m_RenderTargets[0], (IDirect3DSurface9 *)bb);

    IDirect3DSurface9 *realRT = NULL;
    if(SUCCEEDED(m_device->GetRenderTarget(0, &realRT)) && realRT)
    {
      if(bb == NULL || realRT != bb->GetReal())
        RDCWARN("Initial render target isn't the first back buffer");
      realRT->Release();
    }
  }

  D3D9SetBound(m_DepthStencil, (IDirect3DSurface9 *)m_AutoDepthStencil);
}

void WrappedD3DDevice9::WrapAutoDepthStencil()
{
  // only called at creation and after Reset, when the bound depth stencil is the automatic one
  IDirect3DSurface9 *real = NULL;
  if(FAILED(m_device->GetDepthStencilSurface(&real)) || real == NULL)
  {
    // no automatic depth stencil (any more). The old one stays alive if the application still
    // references it.
    if(m_AutoDepthStencil)
      m_AutoDepthStencil->IntRelease();
    m_AutoDepthStencil = NULL;
    return;
  }

  // the device owns the automatic depth stencil surface
  real->Release();

  if(m_AutoDepthStencil && m_AutoDepthStencil->GetReal() == real)
    return;

  // standalone surface, but with no external references and owned by the device
  WrappedD3DSurface9 *wrapped = new WrappedD3DSurface9(real, this, (WrappedD3D9Object *)NULL);
  wrapped->IntAddRef();

  // the old one stays alive if the application still references it
  if(m_AutoDepthStencil)
    m_AutoDepthStencil->IntRelease();

  m_AutoDepthStencil = wrapped;
}

void WrappedD3DDevice9::OnReset()
{
  SCOPED_LOCK(m_D3DLock);

  if(m_Recording)
  {
    RDCWARN("Device reset while recording a state block");
    SAFE_DELETE(m_Recording);
  }

  // all state goes back to defaults
  m_Bindings.ReleaseAll();
  for(uint32_t i = 0; i < D3D9_NUM_RENDER_TARGETS; i++)
    D3D9SetBound(m_RenderTargets[i], (IDirect3DSurface9 *)NULL);
  D3D9SetBound(m_DepthStencil, (IDirect3DSurface9 *)NULL);

  // implicit swapchains, their back buffers and the automatic depth stencil may all be new
  for(UINT i = 0; i < m_ImplicitSwapChains.size(); i++)
  {
    IDirect3DSwapChain9 *real = NULL;
    if(SUCCEEDED(m_device->GetSwapChain(i, &real)) && real)
    {
      real->Release();
      m_ImplicitSwapChains[i]->OnDeviceReset(real);
    }
  }

  WrapAutoDepthStencil();

  if(!m_ImplicitSwapChains.empty())
    D3D9SetBound(m_RenderTargets[0],
                 (IDirect3DSurface9 *)m_ImplicitSwapChains[0]->GetBackBufferWrapper(0));

  D3D9SetBound(m_DepthStencil, (IDirect3DSurface9 *)m_AutoDepthStencil);
}

void WrappedD3DDevice9::CaptureBindings(D3D9ObjectBindings &dst, const D3D9BindingMask &mask)
{
  SCOPED_LOCK(m_D3DLock);
  dst.CopyFrom(m_Bindings, mask);
}

void WrappedD3DDevice9::ApplyBindings(const D3D9ObjectBindings &src, const D3D9BindingMask &mask)
{
  SCOPED_LOCK(m_D3DLock);

  if(m_Recording)
  {
    m_Recording->CopyFrom(src, mask);
    m_RecordingMask.textures |= mask.textures;
    m_RecordingMask.streams |= mask.streams;
    m_RecordingMask.indices |= mask.indices;
    m_RecordingMask.vs |= mask.vs;
    m_RecordingMask.ps |= mask.ps;
    m_RecordingMask.decl |= mask.decl;
  }
  else
  {
    m_Bindings.CopyFrom(src, mask);
  }
}

void WrappedD3DDevice9::RenderOverlay(IDirect3DSwapChain9 *realSwapChain, HWND hDestWindowOverride)
{
  RenderDoc::Inst().Tick();

  D3DPRESENT_PARAMETERS presentParams = {};
  if(realSwapChain)
    realSwapChain->GetPresentParameters(&presentParams);

  HWND wnd = presentParams.hDeviceWindow;
  if(hDestWindowOverride != NULL)
    wnd = hDestWindowOverride;

  DeviceOwnedWindow devWnd((IDirect3DDevice9 *)this, wnd);

  m_FrameCounter++;

  uint32_t overlay = RenderDoc::Inst().GetOverlayBits();

  if(wnd != NULL && realSwapChain != NULL && m_DebugManager && (overlay & eRENDERDOC_Overlay_Enabled))
  {
    IDirect3DSurface9 *backBuffer = NULL;
    if(SUCCEEDED(realSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer)
    {
      // Everything here goes to the real device so the application's state, and our tracking of
      // it, is untouched. A D3DSBT_ALL state block restores everything except render targets,
      // which we save separately.
      IDirect3DSurface9 *prevRT = NULL;
      m_device->GetRenderTarget(0, &prevRT);

      IDirect3DStateBlock9 *stateBlock = NULL;
      m_device->CreateStateBlock(D3DSBT_ALL, &stateBlock);

      m_device->BeginScene();
      m_device->SetRenderTarget(0, backBuffer);

      D3DSURFACE_DESC bbDesc = {};
      backBuffer->GetDesc(&bbDesc);

      D3DVIEWPORT9 viewport = {0, 0, bbDesc.Width, bbDesc.Height, 0.f, 1.f};
      m_device->SetViewport(&viewport);

      m_DebugManager->SetOutputDimensions(bbDesc.Width, bbDesc.Height);
      m_DebugManager->SetOutputWindow(presentParams.hDeviceWindow);

      rdcstr overlayText = RenderDoc::Inst().GetOverlayText(RDCDriver::D3D9, devWnd, m_FrameCounter,
                                                            RenderDoc::eOverlay_CaptureDisabled);

      overlayText += "Captures not supported with D3D9\n";

      if(!overlayText.empty())
        m_DebugManager->RenderText(0.0f, 0.0f, overlayText.c_str());

      m_device->EndScene();

      // restore the render target first: setting it resets the viewport, which the state block
      // then restores
      if(prevRT)
      {
        m_device->SetRenderTarget(0, prevRT);
        prevRT->Release();
      }

      if(stateBlock)
      {
        stateBlock->Apply();
        stateBlock->Release();
      }

      backBuffer->Release();
    }
  }

  RenderDoc::Inst().AddActiveDriver(RDCDriver::D3D9, true);
  RenderDoc::Inst().SetDriverUnsupportedMessage(RDCDriver::D3D9,
                                                "D3D9 capture is not yet supported");
}

void WrappedD3DDevice9::StartFrameCapture(DeviceOwnedWindow devWnd)
{
  RDCERR("Capture not supported on D3D9");
}

bool WrappedD3DDevice9::EndFrameCapture(DeviceOwnedWindow devWnd)
{
  RDCERR("Capture not supported on D3D9");
  return false;
}

bool WrappedD3DDevice9::DiscardFrameCapture(DeviceOwnedWindow devWnd)
{
  RDCERR("Capture not supported on D3D9");
  return false;
}
