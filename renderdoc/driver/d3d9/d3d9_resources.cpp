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

#include "d3d9_resources.h"
#include "d3d9_device.h"
#include "d3d9_swapchain.h"

#undef D3D9_TYPE_MACRO
#define D3D9_TYPE_MACRO(a) WRAPPED_POOL_INST(a)

ALL_D3D9_POOLED_TYPES;

WrappedD3D9Object *GetWrappedObject(IUnknown *ptr)
{
  if(ptr == NULL)
    return NULL;

#undef D3D9_TYPE_MACRO
#define D3D9_TYPE_MACRO(a) \
  if(a::IsAlloc(ptr))      \
    return (WrappedD3D9Object *)(a *)(a::InnerType *)ptr;

  ALL_D3D9_POOLED_TYPES;

  return NULL;
}

HRESULT D3D9GetWrappedDevice(WrappedD3DDevice9 *device, IDirect3DDevice9 **ppDevice)
{
  if(ppDevice == NULL)
    return D3DERR_INVALIDCALL;

  device->AddRef();
  *ppDevice = device;
  return S_OK;
}

WrappedD3D9Object::WrappedD3D9Object(IUnknown *real, WrappedD3DDevice9 *device,
                                     WrappedD3D9Object *container, RealOwnership ownership)
    : m_pDevice(device),
      m_Container(container),
      m_pRealUnknown(real),
      m_Ownership(ownership),
      m_ID(ResourceIDGen::GetNewUniqueID()),
      m_ExtRef(container == NULL && ownership == RealOwnership::WhileExternal ? 1 : 0)
{
  // a new standalone object starts with the application's reference, which holds a reference on the
  // device. The real reference was returned by the creation call.
  if(m_ExtRef > 0)
    m_pDevice->AddRef();
}

WrappedD3D9Object::~WrappedD3D9Object()
{
  if(m_pRecord)
    m_pDevice->ReleaseRecord(m_pRecord);
  m_pRecord = NULL;
  SAFE_DELETE(m_Contents);
}

D3D9ObjectContents &WrappedD3D9Object::GetContents()
{
  if(m_Contents == NULL)
    m_Contents = new D3D9ObjectContents;
  return *m_Contents;
}

void D3D9RecordGenerateMipSubLevels(WrappedD3DDevice9 *device, IDirect3DBaseTexture9 *tex)
{
  device->OnGenerateMipSubLevels(tex);
}

void D3D9RecordSetAutoGenFilterType(WrappedD3DDevice9 *device, IDirect3DBaseTexture9 *tex,
                                    D3DTEXTUREFILTERTYPE filter)
{
  device->OnSetAutoGenFilterType(tex, filter);
}

void D3D9RecordSetLOD(WrappedD3DDevice9 *device, IDirect3DBaseTexture9 *tex, DWORD lod)
{
  device->OnSetLOD(tex, lod);
}

HRESULT WrappedD3DQuery9::Issue(DWORD dwIssueFlags)
{
  HRESULT hr = m_pReal->Issue(dwIssueFlags);
  if(SUCCEEDED(hr))
    m_pDevice->OnQueryIssue(this, dwIssueFlags);
  return hr;
}

ULONG WrappedD3D9Object::ExtAddRef()
{
  if(m_Container)
  {
    Atomic::Inc32(&m_ExtRef);
    return m_Container->ExtAddRef();
  }

  int32_t ref = Atomic::Inc32(&m_ExtRef);

  // first external reference: take back our real reference and our reference on the device. The
  // real object must still be alive since something internal references this wrapper.
  if(ref == 1)
  {
    if(m_Ownership == RealOwnership::WhileExternal)
      m_pRealUnknown->AddRef();
    m_pDevice->AddRef();
  }

  return (ULONG)ref;
}

ULONG WrappedD3D9Object::ExtRelease()
{
  if(m_Container)
  {
    Atomic::Dec32(&m_ExtRef);
    // may destroy the container, and us with it
    return m_Container->ExtRelease();
  }

  int32_t ref = Atomic::Dec32(&m_ExtRef);

  if(ref < 0)
  {
    RDCERR("%s over-released by the application", ToStr(GetResourceID()).c_str());
    Atomic::Inc32(&m_ExtRef);
    return 0;
  }

  if(ref == 0)
  {
    WrappedD3DDevice9 *dev = m_pDevice;

    // a capture in progress may still need the contents, read them while the real object is
    // certainly alive
    dev->OnLastExternalRelease(this);

    if(m_Ownership == RealOwnership::WhileExternal)
      m_pRealUnknown->Release();

    if(m_IntRef == 0)
      delete this;

    // release the device last, in case this was the last reference keeping it alive
    dev->Release();
  }

  return (ULONG)ref;
}

void WrappedD3D9Object::IntAddRef()
{
  Atomic::Inc32(&m_IntRef);
  if(m_Container)
    m_Container->IntAddRef();
}

void WrappedD3D9Object::IntRelease()
{
  int32_t ref = Atomic::Dec32(&m_IntRef);

  if(m_Container)
  {
    m_Container->IntRelease();
    return;
  }

  if(ref == 0 && m_ExtRef == 0)
    delete this;
}

void WrappedD3D9Object::RegisterWrapper(IUnknown *wrapped)
{
  D3D9ResourceManager *rm = m_pDevice->GetResourceManager();

  // a stale mapping can exist if a real object died while still referenced by one of our wrappers
  // (e.g. an application bug keeping a released object in a state block). The newest wrapper wins.
  if(rm->HasWrapper(m_pRealUnknown))
  {
    RDCWARN("Real D3D9 object %p already has a wrapper, replacing it", m_pRealUnknown);
    rm->OverrideWrapper(m_pRealUnknown);
  }

  rm->AddWrapper(wrapped, m_pRealUnknown);
  rm->AddResource(m_ID, wrapped);
}

void WrappedD3D9Object::UnregisterWrapper(IUnknown *wrapped)
{
  D3D9ResourceManager *rm = m_pDevice->GetResourceManager();
  rm->RemoveWrapper(wrapped, m_pRealUnknown);
  rm->ReleaseResource(m_ID);
}

void WrappedD3D9Object::ReplaceReal(IUnknown *wrapped, IUnknown *newReal)
{
  D3D9ResourceManager *rm = m_pDevice->GetResourceManager();
  rm->RemoveWrapper(wrapped, m_pRealUnknown);
  m_pRealUnknown = newReal;
  if(rm->HasWrapper(newReal))
    rm->OverrideWrapper(newReal);
  rm->AddWrapper(wrapped, newReal);
}

///////////////////////////////////////////////////////////////////////////////
// containers

HRESULT WrappedD3DSurface9::GetContainer(REFIID riid, void **ppContainer)
{
  if(ppContainer == NULL)
    return D3DERR_INVALIDCALL;

  // surfaces that aren't part of a texture or swapchain have the device as their container
  if(m_Container)
    return m_Container->GetWrappedUnknown()->QueryInterface(riid, ppContainer);

  return m_pDevice->QueryInterface(riid, ppContainer);
}

HRESULT WrappedD3DVolume9::GetContainer(REFIID riid, void **ppContainer)
{
  if(ppContainer == NULL)
    return D3DERR_INVALIDCALL;

  if(m_Container)
    return m_Container->GetWrappedUnknown()->QueryInterface(riid, ppContainer);

  return m_pDevice->QueryInterface(riid, ppContainer);
}

WrappedD3DTexture9::~WrappedD3DTexture9()
{
  for(WrappedD3DSurface9 *s : m_Levels)
    DestroyChild(s);
}

WrappedD3DSurface9 *WrappedD3DTexture9::GetLevel(UINT Level)
{
  SCOPED_LOCK(m_pDevice->D3DLock());

  if(Level >= m_Levels.size())
  {
    if(Level >= m_pReal->GetLevelCount())
      return NULL;
    m_Levels.resize(m_pReal->GetLevelCount());
  }

  if(m_Levels[Level] == NULL)
  {
    IDirect3DSurface9 *real = NULL;
    HRESULT hr = m_pReal->GetSurfaceLevel(Level, &real);
    if(FAILED(hr) || real == NULL)
      return NULL;

    // the real texture owns its levels, so we don't hold a reference
    real->Release();

    m_Levels[Level] = new WrappedD3DSurface9(real, m_pDevice, this);
    m_Levels[Level]->SetChildSubresource(0, Level);
    m_pDevice->RecordSurfaceLevel(this, Level, m_Levels[Level]);
  }

  return m_Levels[Level];
}

HRESULT WrappedD3DTexture9::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel)
{
  if(ppSurfaceLevel == NULL)
    return D3DERR_INVALIDCALL;

  WrappedD3DSurface9 *surf = GetLevel(Level);
  if(surf == NULL)
  {
    *ppSurfaceLevel = NULL;
    return D3DERR_INVALIDCALL;
  }

  surf->AddRef();
  *ppSurfaceLevel = surf;
  return S_OK;
}

WrappedD3DCubeTexture9::~WrappedD3DCubeTexture9()
{
  for(WrappedD3DSurface9 *s : m_Faces)
    DestroyChild(s);
}

WrappedD3DSurface9 *WrappedD3DCubeTexture9::GetFace(D3DCUBEMAP_FACES FaceType, UINT Level)
{
  SCOPED_LOCK(m_pDevice->D3DLock());

  if((UINT)FaceType >= 6)
    return NULL;

  const size_t idx = Level * 6 + (UINT)FaceType;

  if(idx >= m_Faces.size())
  {
    if(Level >= m_pReal->GetLevelCount())
      return NULL;
    m_Faces.resize(m_pReal->GetLevelCount() * 6);
  }

  if(m_Faces[idx] == NULL)
  {
    IDirect3DSurface9 *real = NULL;
    HRESULT hr = m_pReal->GetCubeMapSurface(FaceType, Level, &real);
    if(FAILED(hr) || real == NULL)
      return NULL;

    real->Release();

    m_Faces[idx] = new WrappedD3DSurface9(real, m_pDevice, this);
    m_Faces[idx]->SetChildSubresource((uint32_t)FaceType, Level);
    m_pDevice->RecordCubeFace(this, FaceType, Level, m_Faces[idx]);
  }

  return m_Faces[idx];
}

HRESULT WrappedD3DCubeTexture9::GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level,
                                                  IDirect3DSurface9 **ppCubeMapSurface)
{
  if(ppCubeMapSurface == NULL)
    return D3DERR_INVALIDCALL;

  WrappedD3DSurface9 *surf = GetFace(FaceType, Level);
  if(surf == NULL)
  {
    *ppCubeMapSurface = NULL;
    return D3DERR_INVALIDCALL;
  }

  surf->AddRef();
  *ppCubeMapSurface = surf;
  return S_OK;
}

WrappedD3DVolumeTexture9::~WrappedD3DVolumeTexture9()
{
  for(WrappedD3DVolume9 *v : m_Levels)
    DestroyChild(v);
}

WrappedD3DVolume9 *WrappedD3DVolumeTexture9::GetLevel(UINT Level)
{
  SCOPED_LOCK(m_pDevice->D3DLock());

  if(Level >= m_Levels.size())
  {
    if(Level >= m_pReal->GetLevelCount())
      return NULL;
    m_Levels.resize(m_pReal->GetLevelCount());
  }

  if(m_Levels[Level] == NULL)
  {
    IDirect3DVolume9 *real = NULL;
    HRESULT hr = m_pReal->GetVolumeLevel(Level, &real);
    if(FAILED(hr) || real == NULL)
      return NULL;

    real->Release();

    m_Levels[Level] = new WrappedD3DVolume9(real, m_pDevice, this);
    m_Levels[Level]->SetChildSubresource(0, Level);
    m_pDevice->RecordVolumeLevel(this, Level, m_Levels[Level]);
  }

  return m_Levels[Level];
}

HRESULT WrappedD3DVolumeTexture9::GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel)
{
  if(ppVolumeLevel == NULL)
    return D3DERR_INVALIDCALL;

  WrappedD3DVolume9 *vol = GetLevel(Level);
  if(vol == NULL)
  {
    *ppVolumeLevel = NULL;
    return D3DERR_INVALIDCALL;
  }

  vol->AddRef();
  *ppVolumeLevel = vol;
  return S_OK;
}

///////////////////////////////////////////////////////////////////////////////
// state blocks

HRESULT WrappedD3DStateBlock9::Capture()
{
  HRESULT hr = m_pReal->Capture();
  if(SUCCEEDED(hr))
  {
    m_pDevice->CaptureBindings(m_Bindings, m_Mask);
    m_pDevice->OnStateBlockCapture(this);
  }
  return hr;
}

HRESULT WrappedD3DStateBlock9::Apply()
{
  HRESULT hr = m_pReal->Apply();
  if(SUCCEEDED(hr))
  {
    m_pDevice->ApplyBindings(m_Bindings, m_Mask);
    m_pDevice->OnStateBlockApply(this);
  }
  return hr;
}
