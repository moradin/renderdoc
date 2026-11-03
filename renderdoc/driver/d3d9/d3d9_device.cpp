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

namespace
{
// the devices capturing is possible on, for D3DPERF markers
Threading::CriticalSection &LiveDevicesLock()
{
  static Threading::CriticalSection lock;
  return lock;
}

rdcarray<WrappedD3DDevice9 *> &LiveDevices()
{
  static rdcarray<WrappedD3DDevice9 *> devices;
  return devices;
}
};

void D3D9_PERF_BeginEvent(DWORD col, const wchar_t *name)
{
  WrappedD3DDevice9::PerfMarker(D3D9Chunk::D3DPERF_BeginEvent, col, name);
}

void D3D9_PERF_EndEvent()
{
  WrappedD3DDevice9::PerfMarker(D3D9Chunk::D3DPERF_EndEvent, 0, NULL);
}

void D3D9_PERF_SetMarker(DWORD col, const wchar_t *name)
{
  WrappedD3DDevice9::PerfMarker(D3D9Chunk::D3DPERF_SetMarker, col, name);
}

void WrappedD3DDevice9::PerfMarker(D3D9Chunk chunk, DWORD col, const wchar_t *name)
{
  SCOPED_LOCK(LiveDevicesLock());

  for(WrappedD3DDevice9 *dev : LiveDevices())
  {
    if(!IsActiveCapturing(dev->m_State))
      continue;

    rdcstr Name = name ? StringFormat::Wide2UTF8(name) : rdcstr();

    SCOPED_LOCK(dev->m_D3DLock);
    WriteSerialiser &ser = dev->m_ScratchSerialiser;
    SCOPED_SERIALISE_CHUNK(chunk);
    if(chunk == D3D9Chunk::D3DPERF_BeginEvent)
      dev->Serialise_D3DPERF_BeginEvent(ser, col, Name);
    else if(chunk == D3D9Chunk::D3DPERF_EndEvent)
      dev->Serialise_D3DPERF_EndEvent(ser);
    else
      dev->Serialise_D3DPERF_SetMarker(ser, col, Name);
    dev->m_FrameRecord->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_D3DPERF_BeginEvent(SerialiserType &ser, D3DCOLOR col, rdcstr Name)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, col);
  SERIALISE_ELEMENT(Name).Important();

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_D3DPERF_EndEvent(SerialiserType &ser)
{
  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_D3DPERF_SetMarker(SerialiserType &ser, D3DCOLOR col, rdcstr Name)
{
  SERIALISE_ELEMENT_TYPED(uint32_t, col);
  SERIALISE_ELEMENT(Name).Important();

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

template bool WrappedD3DDevice9::Serialise_D3DPERF_BeginEvent(ReadSerialiser &ser, D3DCOLOR col,
                                                              rdcstr Name);
template bool WrappedD3DDevice9::Serialise_D3DPERF_BeginEvent(WriteSerialiser &ser, D3DCOLOR col,
                                                              rdcstr Name);
template bool WrappedD3DDevice9::Serialise_D3DPERF_EndEvent(ReadSerialiser &ser);
template bool WrappedD3DDevice9::Serialise_D3DPERF_EndEvent(WriteSerialiser &ser);
template bool WrappedD3DDevice9::Serialise_D3DPERF_SetMarker(ReadSerialiser &ser, D3DCOLOR col,
                                                             rdcstr Name);
template bool WrappedD3DDevice9::Serialise_D3DPERF_SetMarker(WriteSerialiser &ser, D3DCOLOR col,
                                                             rdcstr Name);

WrappedD3DDevice9::WrappedD3DDevice9(IDirect3DDevice9 *device, WrappedD3D9 *wrappedD3D, HWND wnd,
                                     const D3D9InitParams &params)
    : m_device(device),
      m_D3D(wrappedD3D),
      m_InitParams(params),
      m_ScratchSerialiser(new StreamWriter(1024), Ownership::Stream)
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

  InitCapture();

  if(!RenderDoc::Inst().IsReplayApp())
  {
    RenderDoc::Inst().AddDeviceFrameCapturer((IDirect3DDevice9 *)this, this);

    m_Wnd = wnd;

    if(wnd != NULL)
      RenderDoc::Inst().AddFrameCapturer(DeviceOwnedWindow((IDirect3DDevice9 *)this, wnd), this);

    SCOPED_LOCK(LiveDevicesLock());
    LiveDevices().push_back(this);
  }

  InitDeviceOwnedObjects();
}

WrappedD3DDevice9::WrappedD3DDevice9(const D3D9InitParams &params)
    : m_device(NULL),
      m_D3D(NULL),
      m_InitParams(params),
      m_ScratchSerialiser(new StreamWriter(1024), Ownership::Stream)
{
  m_State = CaptureState::LoadingReplaying;

  InitCapture();
}

void WrappedD3DDevice9::InitCapture()
{
  m_StructuredFile = m_StoredStructuredData = new SDFile;

  uint32_t flags = WriteSerialiser::ChunkDuration | WriteSerialiser::ChunkTimestamp |
                   WriteSerialiser::ChunkThreadID;

  if(RenderDoc::Inst().GetCaptureOptions().captureCallstacks)
    flags |= WriteSerialiser::ChunkCallstack;

  m_ScratchSerialiser.SetChunkMetadataRecording(flags);
  m_ScratchSerialiser.SetVersion(D3D9InitParams::CurrentVersion);

  m_ResourceManager = new D3D9ResourceManager(m_State, this);

  m_ScratchSerialiser.SetUserData(m_ResourceManager);

  m_DeviceResourceID = ResourceIDGen::GetNewUniqueID();

  if(IsCaptureMode(m_State))
  {
    m_FrameRecord = m_ResourceManager->AddResourceRecord(ResourceIDGen::GetNewUniqueID());
    m_FrameRecord->DataInSerialiser = false;
    m_FrameRecord->Length = 0;
    m_FrameRecord->InternalResource = true;
  }
}

WrappedD3DDevice9::~WrappedD3DDevice9()
{
  if(m_device && !RenderDoc::Inst().IsReplayApp())
  {
    RenderDoc::Inst().RemoveDeviceFrameCapturer((IDirect3DDevice9 *)this);

    if(m_Wnd != NULL)
      RenderDoc::Inst().RemoveFrameCapturer(DeviceOwnedWindow((IDirect3DDevice9 *)this, m_Wnd));

    SCOPED_LOCK(LiveDevicesLock());
    LiveDevices().removeOne(this);
  }

  // only uses real objects
  SAFE_DELETE(m_DebugManager);
  m_DepthReadback.Release();

  // Nothing the application can see is alive any more (every externally referenced object holds a
  // reference on the device), so dropping our internal references destroys every wrapper.
  SAFE_DELETE(m_Recording);
  FreeRecordingRecord();
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

  if(m_FrameRecord)
  {
    m_FrameRecord->DeleteChunks();
    m_FrameRecord->Delete(m_ResourceManager);
    m_FrameRecord = NULL;
  }

  FlushDeferredRecordReleases();

  if(m_ResourceManager)
    m_ResourceManager->Shutdown();
  SAFE_DELETE(m_ResourceManager);

  SAFE_DELETE(m_StoredStructuredData);

  if(m_device)
  {
    ULONG realRefs = m_device->Release();
    if(realRefs != 0)
      RDCWARN("Real D3D9 device still has %u references after the wrapper was destroyed", realRefs);
  }
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
    CreateOwnedRecord(wrapped);
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
  CreateOwnedRecord(wrapped);

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
  FreeRecordingRecord();

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

void WrappedD3DDevice9::RenderOverlay(IDirect3DSwapChain9 *realSwapChain, DeviceOwnedWindow devWnd)
{
  uint32_t overlay = RenderDoc::Inst().GetOverlayBits();

  if(devWnd.windowHandle == NULL || realSwapChain == NULL || m_DebugManager == NULL ||
     !(overlay & eRENDERDOC_Overlay_Enabled))
    return;

  IDirect3DSurface9 *backBuffer = NULL;
  if(FAILED(realSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
    return;

  // Everything here goes to the real device so our tracking of the application's state is
  // untouched. The debug manager saves and restores the pipeline state it changes; the targets,
  // viewport and scissor rect are saved here. Setting render target 0 resets both of those.
  IDirect3DSurface9 *prevRTs[4] = {};
  for(DWORD i = 0; i < ARRAY_COUNT(prevRTs); i++)
    m_device->GetRenderTarget(i, &prevRTs[i]);
  IDirect3DSurface9 *prevDepth = NULL;
  m_device->GetDepthStencilSurface(&prevDepth);
  D3DVIEWPORT9 prevViewport = {};
  m_device->GetViewport(&prevViewport);
  RECT prevScissor = {};
  m_device->GetScissorRect(&prevScissor);

  // the application shouldn't be inside a scene at Present, but if it is don't end its scene
  bool ownScene = SUCCEEDED(m_device->BeginScene());

  m_device->SetRenderTarget(0, backBuffer);
  for(DWORD i = 1; i < ARRAY_COUNT(prevRTs); i++)
    if(prevRTs[i])
      m_device->SetRenderTarget(i, NULL);
  // depth is disabled, but a depth buffer smaller than the back buffer makes drawing invalid
  m_device->SetDepthStencilSurface(NULL);

  D3DSURFACE_DESC bbDesc = {};
  backBuffer->GetDesc(&bbDesc);

  D3DVIEWPORT9 viewport = {0, 0, bbDesc.Width, bbDesc.Height, 0.f, 1.f};
  m_device->SetViewport(&viewport);

  m_DebugManager->SetOutputDimensions(bbDesc.Width, bbDesc.Height);
  m_DebugManager->SetOutputWindow((HWND)devWnd.windowHandle);

  rdcstr overlayText = RenderDoc::Inst().GetOverlayText(RDCDriver::D3D9, devWnd, m_FrameCounter, 0);

  overlayText += "D3D9 support is in development: captures can't be replayed yet\n";

  m_DebugManager->RenderText(0.0f, 0.0f, overlayText.c_str());

  if(ownScene)
    m_device->EndScene();

  // render target 0 first, the others were only unbound if they were set
  for(DWORD i = 0; i < ARRAY_COUNT(prevRTs); i++)
  {
    if(prevRTs[i])
      m_device->SetRenderTarget(i, prevRTs[i]);
    SAFE_RELEASE(prevRTs[i]);
  }
  m_device->SetDepthStencilSurface(prevDepth);
  SAFE_RELEASE(prevDepth);
  m_device->SetViewport(&prevViewport);
  m_device->SetScissorRect(&prevScissor);

  backBuffer->Release();
}

void WrappedD3DDevice9::HandlePresent(IDirect3DSwapChain9 *realSwapChain, HWND hDestWindowOverride)
{
  D3DPRESENT_PARAMETERS presentParams = {};
  if(realSwapChain)
    realSwapChain->GetPresentParameters(&presentParams);

  HWND wnd = presentParams.hDeviceWindow;
  if(hDestWindowOverride != NULL)
    wnd = hDestWindowOverride;

  DeviceOwnedWindow devWnd((IDirect3DDevice9 *)this, wnd);

  if(IsBackgroundCapturing(m_State))
    RenderDoc::Inst().Tick();

  // the first present becomes frame #1, this is at the end of the frame
  m_FrameCounter++;

  if(IsBackgroundCapturing(m_State))
    RenderOverlay(realSwapChain, devWnd);

  RenderDoc::Inst().AddActiveDriver(RDCDriver::D3D9, true);

  if(wnd == NULL || !RenderDoc::Inst().IsActiveWindow(devWnd))
    return;

  // end any capture that isn't application controlled. It fails if the device was lost during the
  // frame, try again on the next one.
  if(IsActiveCapturing(m_State) && !m_AppControlledCapture)
  {
    if(!RenderDoc::Inst().EndFrameCapture(devWnd))
      RenderDoc::Inst().TriggerCapture(1);
  }

  if(IsBackgroundCapturing(m_State) && RenderDoc::Inst().ShouldTriggerCapture(m_FrameCounter))
  {
    // wait until the device is usable again
    if(IsDeviceLost(wnd))
    {
      RenderDoc::Inst().TriggerCapture(1);
      return;
    }

    RenderDoc::Inst().StartFrameCapture(devWnd);

    if(IsActiveCapturing(m_State))
    {
      m_AppControlledCapture = false;
      m_CapturedFrames.back().frameNumber = m_FrameCounter;
    }
  }
}

void WrappedD3DDevice9::StartFrameCapture(DeviceOwnedWindow devWnd)
{
  SCOPED_LOCK(m_D3DLock);

  if(!IsBackgroundCapturing(m_State))
    return;

  RDCLOG("Starting capture");

  m_State = CaptureState::ActiveCapturing;

  m_AppControlledCapture = true;
  // the application's capture API can leave the device or window out
  m_CaptureWindow = devWnd;
  if(m_CaptureWindow.device == NULL)
    m_CaptureWindow.device = (IDirect3DDevice9 *)this;

  FrameDescription frame;
  // for app-controlled captures the frame number is meaningless, it's updated by HandlePresent
  // when the capture was triggered there.
  frame.frameNumber = ~0U;
  frame.captureTime = Timing::GetUnixTimestamp();
  m_CapturedFrames.push_back(frame);

  m_ResourceManager->ClearReferencedResources();

  m_FrameRecord->DeleteChunks();

  // device-owned objects are always included
  for(WrappedD3DSwapChain9 *swap : m_ImplicitSwapChains)
    MarkReferenced((IDirect3DSwapChain9 *)swap, eFrameRef_Read);
  MarkReferenced((IDirect3DSurface9 *)m_AutoDepthStencil, eFrameRef_Read);

  // as are the objects bound when the frame starts
  for(IDirect3DBaseTexture9 *tex : m_Bindings.textures)
    MarkReferenced(tex, eFrameRef_Read);
  for(const D3D9StreamBinding &s : m_Bindings.streams)
    MarkReferenced(s.buffer, eFrameRef_Read);
  MarkReferenced(m_Bindings.indices, eFrameRef_Read);
  MarkReferenced(m_Bindings.vs, eFrameRef_Read);
  MarkReferenced(m_Bindings.ps, eFrameRef_Read);
  MarkReferenced(m_Bindings.decl, eFrameRef_Read);
  MarkRenderTargetsReferenced();

  D3D9FetchDeviceState(m_device, m_InitParams.BehaviorFlags, m_LightIndices, m_PaletteNumbers,
                       m_FrameStartState);

  {
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(D3D9Chunk::BeginCaptureFrame);
    Serialise_BeginCaptureFrame(ser);
    m_FrameRecord->AddChunk(scope.Get());
  }

  m_ResourceManager->PrepareInitialContents();
}

bool WrappedD3DDevice9::EndFrameCapture(DeviceOwnedWindow devWnd)
{
  SCOPED_LOCK(m_D3DLock);

  if(!IsActiveCapturing(m_State))
    return true;

  if(IsDeviceLost((HWND)devWnd.windowHandle))
  {
    RDCWARN("Device was lost during the captured frame, discarding the capture");
    DiscardFrameCapture(devWnd);
    return false;
  }

  RDCLOG("Finished capture, Frame %u", m_CapturedFrames.back().frameNumber);

  // thumbnail from the swapchain presenting to the window, falling back to the first one
  IDirect3DSwapChain9 *thumbSwap = NULL;
  for(WrappedD3DSwapChain9 *swap : m_ImplicitSwapChains)
  {
    D3DPRESENT_PARAMETERS params = {};
    swap->GetReal()->GetPresentParameters(&params);
    if(thumbSwap == NULL || params.hDeviceWindow == (HWND)devWnd.windowHandle)
      thumbSwap = swap->GetReal();
  }

  RenderDoc::FramePixels fp;
  ThumbnailFromSwapChain(thumbSwap, fp);

  RDCFile *rdc =
      RenderDoc::Inst().CreateRDC(RDCDriver::D3D9, m_CapturedFrames.back().frameNumber, fp);

  StreamWriter *captureWriter = NULL;

  if(rdc)
  {
    SectionProperties props;

    // Compress with LZ4 so that it's fast
    props.flags = SectionFlags::LZ4Compressed;
    props.version = m_SectionVersion;
    props.type = SectionType::FrameCapture;

    captureWriter = rdc->WriteSection(props);
  }
  else
  {
    captureWriter = new StreamWriter(StreamWriter::InvalidStream);
  }

  uint64_t captureSectionSize = 0;

  {
    WriteSerialiser ser(captureWriter, Ownership::Stream);

    ser.SetChunkMetadataRecording(m_ScratchSerialiser.GetChunkMetadataRecording());

    ser.SetUserData(m_ResourceManager);

    {
      SCOPED_SERIALISE_CHUNK(SystemChunk::DriverInit, sizeof(D3D9InitParams) + 16);

      SERIALISE_ELEMENT(m_InitParams);
    }

    {
      SCOPED_SERIALISE_CHUNK(D3D9Chunk::DeviceInitialisation, 256);

      Serialise_DeviceInitialisation(ser);
    }

    m_ResourceManager->InsertReferencedChunks(ser);

    m_ResourceManager->InsertInitialContentsChunks(ser);

    m_ResourceManager->Serialise_InitialContentsNeeded(ser);

    {
      SCOPED_SERIALISE_CHUNK(SystemChunk::CaptureScope, 16);

      Serialise_CaptureScope(ser);
    }

    {
      std::map<int64_t, Chunk *> recordlist;
      m_FrameRecord->Insert(recordlist);

      float num = float(recordlist.size());
      float idx = 0.0f;

      for(auto it = recordlist.begin(); it != recordlist.end(); ++it)
      {
        RenderDoc::Inst().SetProgress(CaptureProgress::SerialiseFrameContents, idx / num);
        idx += 1.0f;
        it->second->Write(ser);
      }
    }

    captureSectionSize = captureWriter->GetOffset();
  }

  RDCLOG("Captured D3D9 frame with %f MB capture section",
         double(captureSectionSize) / (1024.0 * 1024.0));

  RenderDoc::Inst().FinishCaptureWriting(rdc, m_CapturedFrames.back().frameNumber);

  m_State = CaptureState::BackgroundCapturing;

  m_FrameRecord->DeleteChunks();

  m_ResourceManager->MarkUnwrittenResources();

  m_ResourceManager->ClearReferencedResources();

  m_ResourceManager->FreeInitialContents();

  FlushDeferredRecordReleases();

  return true;
}

bool WrappedD3DDevice9::DiscardFrameCapture(DeviceOwnedWindow devWnd)
{
  SCOPED_LOCK(m_D3DLock);

  if(!IsActiveCapturing(m_State))
    return true;

  RenderDoc::Inst().FinishCaptureWriting(NULL, m_CapturedFrames.back().frameNumber);

  m_CapturedFrames.pop_back();

  m_State = CaptureState::BackgroundCapturing;

  m_FrameRecord->DeleteChunks();

  m_ResourceManager->MarkUnwrittenResources();

  m_ResourceManager->ClearReferencedResources();

  m_ResourceManager->FreeInitialContents();

  FlushDeferredRecordReleases();

  return true;
}

bool WrappedD3DDevice9::IsDeviceLost(HWND wnd)
{
  if(m_deviceEx)
  {
    // D3D9Ex devices aren't lost the same way, but can hang or be removed
    HRESULT hr = m_deviceEx->CheckDeviceState(wnd);
    return hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED;
  }

  return m_device->TestCooperativeLevel() != D3D_OK;
}

bool WrappedD3DDevice9::DiscardCaptureForReset(bool &appControlled)
{
  if(!IsActiveCapturing(m_State))
    return false;

  appControlled = m_AppControlledCapture;

  RDCWARN("Device reset during a capture, discarding it and trying again after the reset");
  RenderDoc::Inst().DiscardFrameCapture(m_CaptureWindow);
  return true;
}

void WrappedD3DDevice9::AfterResetDiscard(bool appControlled, bool resetSucceeded)
{
  if(!appControlled)
  {
    // captures triggered at a Present start again at the next one
    RenderDoc::Inst().TriggerCapture(1);
    return;
  }

  // The application asked for this capture and will end it itself, so capture what's left of it:
  // everything after the reset.
  if(resetSucceeded)
    RenderDoc::Inst().StartFrameCapture(m_CaptureWindow);
  else
    RDCWARN("Reset failed, the application's capture can't be restarted");
}

void WrappedD3DDevice9::ThumbnailFromSwapChain(IDirect3DSwapChain9 *realSwapChain,
                                               RenderDoc::FramePixels &fp)
{
  if(realSwapChain == NULL)
    return;

  IDirect3DSurface9 *backBuffer = NULL;
  if(FAILED(realSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
    return;

  D3DSURFACE_DESC desc = {};
  backBuffer->GetDesc(&desc);

  // only the common 8-bit back buffer formats for now
  if(desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
  {
    RDCWARN("No thumbnail for back buffer format %s", ToStr(desc.Format).c_str());
    backBuffer->Release();
    return;
  }

  IDirect3DSurface9 *source = backBuffer;
  IDirect3DSurface9 *resolved = NULL;

  // GetRenderTargetData can't read multisampled surfaces, resolve first
  if(desc.MultiSampleType != D3DMULTISAMPLE_NONE)
  {
    HRESULT hr = m_device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                              D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, NULL);
    if(SUCCEEDED(hr) && resolved)
    {
      m_device->StretchRect(backBuffer, NULL, resolved, NULL, D3DTEXF_NONE);
      source = resolved;
    }
  }

  IDirect3DSurface9 *sysmem = NULL;
  HRESULT hr = m_device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                     D3DPOOL_SYSTEMMEM, &sysmem, NULL);

  if(SUCCEEDED(hr) && sysmem && SUCCEEDED(m_device->GetRenderTargetData(source, sysmem)))
  {
    D3DLOCKED_RECT locked = {};
    if(SUCCEEDED(sysmem->LockRect(&locked, NULL, D3DLOCK_READONLY)))
    {
      fp.len = (uint32_t)locked.Pitch * desc.Height;
      fp.data = new uint8_t[fp.len];
      memcpy(fp.data, locked.pBits, fp.len);

      sysmem->UnlockRect();

      fp.width = desc.Width;
      fp.height = desc.Height;
      fp.pitch = (uint32_t)locked.Pitch;
      fp.stride = 4;
      fp.bpc = 1;
      fp.bgra = true;
      fp.max_width = 2048;
      fp.pitch_requirement = 8;
    }
  }
  else
  {
    RDCWARN("Couldn't read back the back buffer for a thumbnail: %s", ToStr(hr).c_str());
  }

  SAFE_RELEASE(sysmem);
  SAFE_RELEASE(resolved);
  backBuffer->Release();
}

rdcarray<D3DPRESENT_PARAMETERS> WrappedD3DDevice9::GetImplicitPresentParams()
{
  rdcarray<D3DPRESENT_PARAMETERS> ret;
  for(WrappedD3DSwapChain9 *swap : m_ImplicitSwapChains)
  {
    D3DPRESENT_PARAMETERS params = {};
    swap->GetReal()->GetPresentParameters(&params);
    ret.push_back(params);
  }
  return ret;
}

rdcarray<ResourceId> WrappedD3DDevice9::GetImplicitSwapChainIDs()
{
  rdcarray<ResourceId> ret;
  for(WrappedD3DSwapChain9 *swap : m_ImplicitSwapChains)
    ret.push_back(swap->GetResourceID());
  return ret;
}

///////////////////////////////////////////////////////////////////////////////
// records and frame references

void WrappedD3DDevice9::CreateOwnedRecord(WrappedD3D9Object *obj)
{
  if(obj == NULL || !IsCaptureMode(m_State))
    return;

  D3D9ResourceRecord *record = m_ResourceManager->AddResourceRecord(obj->GetResourceID());
  record->DataInSerialiser = false;
  record->Length = 0;
  obj->SetResourceRecord(record);

  if(D3D9HasContents(obj))
    m_ResourceManager->MarkDirtyResource(obj->GetResourceID());
}

void WrappedD3DDevice9::ReleaseRecord(D3D9ResourceRecord *record)
{
  if(record == NULL)
    return;

  SCOPED_LOCK(m_D3DLock);

  // an object that dies mid-capture may still be needed by the frame
  if(IsActiveCapturing(m_State))
    m_DeferredRecordReleases.push_back(record);
  else
    record->Delete(m_ResourceManager);
}

void WrappedD3DDevice9::FlushDeferredRecordReleases()
{
  for(D3D9ResourceRecord *record : m_DeferredRecordReleases)
    record->Delete(m_ResourceManager);
  m_DeferredRecordReleases.clear();
}

void WrappedD3DDevice9::MarkReferenced(IUnknown *obj, FrameRefType refType)
{
  if(obj == NULL || !IsActiveCapturing(m_State))
    return;

  WrappedD3D9Object *wrapped = GetWrappedObject(obj);
  if(wrapped == NULL)
    return;

  m_ResourceManager->MarkResourceFrameReferenced(wrapped->GetResourceID(), refType);

  // texture levels have no contents of their own, their texture's contents are what's used
  WrappedD3D9Object *container = wrapped->GetContainer();
  if(container && container->GetResourceType() != Resource_SwapChain)
    m_ResourceManager->MarkResourceFrameReferenced(container->GetResourceID(), refType);
}

void WrappedD3DDevice9::MarkDrawReferenced()
{
  for(IDirect3DBaseTexture9 *tex : m_Bindings.textures)
    MarkReferenced(tex, eFrameRef_Read);
  for(const D3D9StreamBinding &s : m_Bindings.streams)
    MarkReferenced(s.buffer, eFrameRef_Read);
  MarkReferenced(m_Bindings.indices, eFrameRef_Read);
  MarkReferenced(m_Bindings.vs, eFrameRef_Read);
  MarkReferenced(m_Bindings.ps, eFrameRef_Read);
  MarkReferenced(m_Bindings.decl, eFrameRef_Read);
  MarkRenderTargetsReferenced();
}

void WrappedD3DDevice9::MarkRenderTargetsReferenced()
{
  for(IDirect3DSurface9 *rt : m_RenderTargets)
    MarkReferenced(rt, eFrameRef_PartialWrite);
  MarkReferenced(m_DepthStencil, eFrameRef_PartialWrite);
}

void WrappedD3DDevice9::RecordSurfaceLevel(WrappedD3DTexture9 *tex, UINT level,
                                           WrappedD3DSurface9 *surf)
{
  RecordCreation(
      surf, D3D9Chunk::Texture_GetSurfaceLevel,
      [&](WriteSerialiser &ser) { Serialise_Texture_GetSurfaceLevel(ser, tex, level, surf); }, tex);
}

void WrappedD3DDevice9::RecordCubeFace(WrappedD3DCubeTexture9 *cube, D3DCUBEMAP_FACES face,
                                       UINT level, WrappedD3DSurface9 *surf)
{
  RecordCreation(
      surf, D3D9Chunk::CubeTexture_GetCubeMapSurface,
      [&](WriteSerialiser &ser) {
        Serialise_CubeTexture_GetCubeMapSurface(ser, cube, face, level, surf);
      },
      cube);
}

void WrappedD3DDevice9::RecordVolumeLevel(WrappedD3DVolumeTexture9 *tex, UINT level,
                                          WrappedD3DVolume9 *vol)
{
  RecordCreation(
      vol, D3D9Chunk::VolumeTexture_GetVolumeLevel,
      [&](WriteSerialiser &ser) { Serialise_VolumeTexture_GetVolumeLevel(ser, tex, level, vol); },
      tex);
}

void WrappedD3DDevice9::RecordBackBuffer(WrappedD3DSwapChain9 *swap, UINT index,
                                         WrappedD3DSurface9 *surf)
{
  RecordCreation(
      surf, D3D9Chunk::SwapChain_GetBackBuffer,
      [&](WriteSerialiser &ser) { Serialise_SwapChain_GetBackBuffer(ser, swap, index, surf); }, swap);
}

///////////////////////////////////////////////////////////////////////////////
// calls on other objects that are recorded into the frame

#define RECORD_OBJECT_CALL(chunk, obj, refType, ...) \
  if(IsActiveCapturing(m_State))                     \
  {                                                  \
    SCOPED_LOCK(m_D3DLock);                          \
    USE_SCRATCH_SERIALISER();                        \
    SCOPED_SERIALISE_CHUNK(D3D9Chunk::chunk);        \
    CONCAT(Serialise_, chunk)(ser, __VA_ARGS__);     \
    m_FrameRecord->AddChunk(scope.Get());            \
    MarkReferenced(obj, refType);                    \
  }

void WrappedD3DDevice9::OnStateBlockCapture(IDirect3DStateBlock9 *sb)
{
  RECORD_OBJECT_CALL(StateBlock_Capture, sb, eFrameRef_PartialWrite, sb);
}

void WrappedD3DDevice9::OnStateBlockApply(IDirect3DStateBlock9 *sb)
{
  RECORD_OBJECT_CALL(StateBlock_Apply, sb, eFrameRef_Read, sb);
  if(IsActiveCapturing(m_State))
    MarkStateBlockReferenced((WrappedD3DStateBlock9 *)GetWrappedObject(sb));
}

void WrappedD3DDevice9::MarkStateBlockReferenced(WrappedD3DStateBlock9 *sb)
{
  if(sb == NULL)
    return;

  // the objects applying it binds
  const D3D9ObjectBindings &b = sb->GetBindings();
  const D3D9BindingMask &mask = sb->GetMask();
  for(uint32_t i = 0; i < D3D9_NUM_TEXTURE_SLOTS; i++)
    if(mask.textures & (1U << i))
      MarkReferenced(b.textures[i], eFrameRef_Read);
  for(uint32_t i = 0; i < D3D9_NUM_STREAMS; i++)
    if(mask.streams & (1U << i))
      MarkReferenced(b.streams[i].buffer, eFrameRef_Read);
  if(mask.indices)
    MarkReferenced(b.indices, eFrameRef_Read);
  if(mask.vs)
    MarkReferenced(b.vs, eFrameRef_Read);
  if(mask.ps)
    MarkReferenced(b.ps, eFrameRef_Read);
  if(mask.decl)
    MarkReferenced(b.decl, eFrameRef_Read);
}

void WrappedD3DDevice9::AddStateChunk(Chunk *chunk)
{
  const bool active = IsActiveCapturing(m_State);

  if(m_RecordingRecord)
    m_RecordingRecord->AddChunk(active ? chunk->Duplicate() : chunk);

  if(active)
    m_FrameRecord->AddChunk(chunk);
  else if(m_RecordingRecord == NULL)
    chunk->Delete();
}

void WrappedD3DDevice9::FreeRecordingRecord()
{
  if(m_RecordingRecord)
    m_RecordingRecord->Delete(m_ResourceManager);
  m_RecordingRecord = NULL;
}

void WrappedD3DDevice9::OnQueryIssue(IDirect3DQuery9 *query, DWORD flags)
{
  RECORD_OBJECT_CALL(Query_Issue, query, eFrameRef_Read, query, flags);
}

void WrappedD3DDevice9::OnGenerateMipSubLevels(IDirect3DBaseTexture9 *tex)
{
  RECORD_OBJECT_CALL(BaseTexture_GenerateMipSubLevels, tex, eFrameRef_PartialWrite, tex);
}

void WrappedD3DDevice9::OnSetAutoGenFilterType(IDirect3DBaseTexture9 *tex, D3DTEXTUREFILTERTYPE filter)
{
  RECORD_OBJECT_CALL(BaseTexture_SetAutoGenFilterType, tex, eFrameRef_Read, tex, filter);
}

void WrappedD3DDevice9::OnSetLOD(IDirect3DBaseTexture9 *tex, DWORD lod)
{
  RECORD_OBJECT_CALL(BaseTexture_SetLOD, tex, eFrameRef_Read, tex, lod);
}

void WrappedD3DDevice9::OnSwapChainPresent(IDirect3DSwapChain9 *swap, CONST RECT *pSourceRect,
                                           CONST RECT *pDestRect, HWND hDestWindowOverride,
                                           CONST RGNDATA *pDirtyRegion, DWORD dwFlags)
{
  RECORD_OBJECT_CALL(SwapChain_Present, swap, eFrameRef_Read, swap, pSourceRect, pDestRect,
                     hDestWindowOverride, pDirtyRegion, dwFlags);
}

#undef RECORD_OBJECT_CALL
