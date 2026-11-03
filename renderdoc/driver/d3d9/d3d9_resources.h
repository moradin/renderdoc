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

#include "d3d9_common.h"
#include "d3d9_contents.h"
#include "d3d9_manager.h"
#include "d3d9_state.h"

// Lifetime model
// --------------
// Every D3D9 object the application can see is a wrapper. The model follows how the D3D9 runtime
// behaves (and how Wine and DXVK implement it):
//
// * External references are the application's. When an object gains its first external reference
//   it takes a reference on the device, and drops it when the last one goes. So the device's
//   refcount includes every live object, as in the real runtime.
//
// * Internal references come from bindings on the device, state blocks, and objects the device
//   owns (implicit swapchains, the automatic depth-stencil surface). They keep the wrapper alive
//   but are invisible to the application.
//
// * A wrapper is deleted when both counts reach zero.
//
// * Standalone objects hold a reference on the real object only while they have external
//   references. Once the application lets go, whatever internally references the wrapper (a device
//   binding or a state block) also references the real object, so it stays alive. This matters for
//   IDirect3DDevice9::Reset, which fails while D3DPOOL_DEFAULT objects are still referenced: if we
//   held real references on objects the app had released, we'd make Reset fail.
//
// * Child objects (texture surface levels, cube faces, volume levels, swapchain back buffers) have
//   no refcount of their own. AddRef/Release forward to the container, as in the runtime. The
//   container owns the child wrappers and creates them on first use, so the same wrapper is
//   returned every time. Children never hold real references: the real container owns its real
//   children.

class WrappedD3D9Object
{
public:
  ResourceId GetResourceID() const { return m_ID; }
  D3D9ResourceRecord *GetResourceRecord() const { return m_pRecord; }
  void SetResourceRecord(D3D9ResourceRecord *record) { m_pRecord = record; }
  WrappedD3DDevice9 *GetWrappedDevice() const { return m_pDevice; }
  WrappedD3D9Object *GetContainer() const { return m_Container; }
  IUnknown *GetRealUnknown() const { return m_pRealUnknown; }

  virtual D3D9ResourceType GetResourceType() const = 0;
  // the wrapper's own COM interface
  virtual IUnknown *GetWrappedUnknown() = 0;

  ULONG ExtAddRef();
  ULONG ExtRelease();
  void IntAddRef();
  void IntRelease();

  // for children these are the child's own counts, not the container's
  int32_t GetExtRefCount() const { return m_ExtRef; }
  int32_t GetIntRefCount() const { return m_IntRef; }

  // contents tracking (see d3d9_contents.h), created on first use. The caller holds the device's
  // D3DLock.
  D3D9ObjectContents &GetContents();
  bool HasContentsTracking() const { return m_Contents != NULL; }

  // which level (and cube face) of the container a child is
  uint32_t GetChildFace() const { return m_ChildFace; }
  uint32_t GetChildLevel() const { return m_ChildLevel; }
  void SetChildSubresource(uint32_t face, uint32_t level)
  {
    m_ChildFace = face;
    m_ChildLevel = level;
  }

protected:
  enum class RealOwnership
  {
    // we hold a real reference whenever we have external references
    WhileExternal,
    // something else (the real device or real container) owns the real object
    None,
  };

  // standalone objects start with one external reference, owned objects and children with none.
  WrappedD3D9Object(IUnknown *real, WrappedD3DDevice9 *device, WrappedD3D9Object *container,
                    RealOwnership ownership);
  virtual ~WrappedD3D9Object();

  // register/unregister with the resource manager. Called by WrappedD3D9DeviceChild since it knows
  // the wrapped interface pointer.
  void RegisterWrapper(IUnknown *wrapped);
  void UnregisterWrapper(IUnknown *wrapped);

  // used when the real object behind a device-owned wrapper is replaced (e.g. after Reset)
  void ReplaceReal(IUnknown *wrapped, IUnknown *newReal);

  // destroy a child object owned by this container
  static void DestroyChild(WrappedD3D9Object *child) { delete child; }

  WrappedD3DDevice9 *m_pDevice;
  WrappedD3D9Object *m_Container;

private:
  WrappedD3D9Object(const WrappedD3D9Object &) = delete;
  WrappedD3D9Object &operator=(const WrappedD3D9Object &) = delete;

  IUnknown *m_pRealUnknown;
  RealOwnership m_Ownership;
  ResourceId m_ID;
  D3D9ResourceRecord *m_pRecord = NULL;
  int32_t m_ExtRef;
  int32_t m_IntRef = 0;
  D3D9ObjectContents *m_Contents = NULL;
  uint32_t m_ChildFace = 0;
  uint32_t m_ChildLevel = 0;
};

// look up the wrapper for a pointer the application gave us. Returns NULL for NULL or for pointers
// that aren't our wrappers.
WrappedD3D9Object *GetWrappedObject(IUnknown *ptr);

// shared implementation of GetDevice()
HRESULT D3D9GetWrappedDevice(WrappedD3DDevice9 *device, IDirect3DDevice9 **ppDevice);

// recording of calls on base textures, which are templated so can't see the device class
void D3D9RecordGenerateMipSubLevels(WrappedD3DDevice9 *device, IDirect3DBaseTexture9 *tex);
void D3D9RecordSetAutoGenFilterType(WrappedD3DDevice9 *device, IDirect3DBaseTexture9 *tex,
                                    D3DTEXTUREFILTERTYPE filter);
void D3D9RecordSetLOD(WrappedD3DDevice9 *device, IDirect3DBaseTexture9 *tex, DWORD lod);

// contents tracking, see d3d9_contents.cpp. face and level are the subresource of obj being locked
// (0 for buffers and surfaces), region is NULL for all of it.
bool D3D9IsActiveCapturing(WrappedD3DDevice9 *device);
// whether obj holds contents itself, rather than being part of an object that does
bool D3D9HasContents(WrappedD3D9Object *obj);
void D3D9PrepareForWrite(WrappedD3D9Object *obj);
void D3D9BeforeLock(WrappedD3D9Object *obj, UINT face, UINT level, const D3DBOX *region, DWORD flags);
void D3D9AfterLock(WrappedD3D9Object *obj, UINT face, UINT level, const D3DBOX *region, DWORD flags,
                   void *data, UINT rowPitch, UINT slicePitch);
void D3D9BeforeUnlock(WrappedD3D9Object *obj, UINT face, UINT level);
void D3D9AddDirtyRegion(WrappedD3D9Object *obj, UINT face, const D3DBOX *box);

// a RECT as a box one slice deep, NULL for NULL
inline const D3DBOX *D3D9RectToBox(const RECT *rect, D3DBOX &box)
{
  if(rect == NULL)
    return NULL;
  box.Left = (UINT)rect->left;
  box.Top = (UINT)rect->top;
  box.Right = (UINT)rect->right;
  box.Bottom = (UINT)rect->bottom;
  box.Front = 0;
  box.Back = 1;
  return &box;
}

template <typename Iface>
Iface *Unwrap(Iface *obj)
{
  if(obj == NULL)
    return NULL;

  WrappedD3D9Object *wrapped = GetWrappedObject(obj);
  if(wrapped == NULL)
  {
    RDCERR("Unwrapped D3D9 object %p passed to API - passing through", obj);
    return obj;
  }

  return (Iface *)wrapped->GetRealUnknown();
}

template <typename Iface>
ResourceId GetResID(Iface *obj)
{
  WrappedD3D9Object *wrapped = GetWrappedObject(obj);
  return wrapped ? wrapped->GetResourceID() : ResourceId();
}

// Base for all wrapped device children. NestedType is the interface we expose, RealType the real
// object we wrap (they only differ when the real object may not support the Ex interface).
//
// NestedType must stay the first base class: wrapped interface pointers and wrapper pointers are
// then the same address, which the pool lookups in GetWrappedObject rely on.
template <typename NestedType, typename RealType = NestedType>
class WrappedD3D9DeviceChild : public NestedType, public WrappedD3D9Object
{
protected:
  RealType *m_pReal;

  WrappedD3D9DeviceChild(RealType *real, WrappedD3DDevice9 *device, WrappedD3D9Object *container,
                         RealOwnership ownership)
      : WrappedD3D9Object(real, device, container, ownership), m_pReal(real)
  {
    RegisterWrapper((NestedType *)this);
  }

  virtual ~WrappedD3D9DeviceChild() { UnregisterWrapper((NestedType *)this); }

  // interfaces other than IUnknown and NestedType that this object can be queried as
  virtual bool IsParentInterface(REFIID riid) { return false; }

public:
  typedef NestedType InnerType;

  RealType *GetReal() const { return m_pReal; }
  IUnknown *GetWrappedUnknown() { return (NestedType *)this; }

  //////////////////////////////
  // implement IUnknown

  ULONG STDMETHODCALLTYPE AddRef() { return ExtAddRef(); }
  ULONG STDMETHODCALLTYPE Release() { return ExtRelease(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject)
  {
    if(ppvObject == NULL)
      return E_POINTER;

    if(riid == __uuidof(IUnknown) || riid == __uuidof(NestedType) || IsParentInterface(riid))
    {
      *ppvObject = (NestedType *)this;
      AddRef();
      return S_OK;
    }

    WarnUnknownGUID(TypeName(), riid);
    *ppvObject = NULL;
    return E_NOINTERFACE;
  }

  virtual const char *TypeName() const = 0;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice)
  {
    return D3D9GetWrappedDevice(m_pDevice, ppDevice);
  }
};

// IDirect3DResource9 methods
template <typename NestedType>
class WrappedD3D9Resource : public WrappedD3D9DeviceChild<NestedType>
{
protected:
  typedef WrappedD3D9DeviceChild<NestedType> Base;
  typedef typename WrappedD3D9Object::RealOwnership RealOwnership;

  WrappedD3D9Resource(NestedType *real, WrappedD3DDevice9 *device, WrappedD3D9Object *container,
                      RealOwnership ownership)
      : Base(real, device, container, ownership)
  {
  }

  bool IsParentInterface(REFIID riid) { return riid == __uuidof(IDirect3DResource9); }

public:
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void *pData, DWORD SizeOfData,
                                           DWORD Flags)
  {
    return this->m_pReal->SetPrivateData(refguid, pData, SizeOfData, Flags);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData)
  {
    return this->m_pReal->GetPrivateData(refguid, pData, pSizeOfData);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid)
  {
    return this->m_pReal->FreePrivateData(refguid);
  }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew)
  {
    return this->m_pReal->SetPriority(PriorityNew);
  }
  DWORD STDMETHODCALLTYPE GetPriority() { return this->m_pReal->GetPriority(); }
  void STDMETHODCALLTYPE PreLoad() { this->m_pReal->PreLoad(); }
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() { return this->m_pReal->GetType(); }
};

// IDirect3DBaseTexture9 methods
template <typename NestedType>
class WrappedD3D9BaseTexture : public WrappedD3D9Resource<NestedType>
{
protected:
  typedef WrappedD3D9Resource<NestedType> Base;

  WrappedD3D9BaseTexture(NestedType *real, WrappedD3DDevice9 *device)
      : Base(real, device, NULL, WrappedD3D9Object::RealOwnership::WhileExternal)
  {
  }

  bool IsParentInterface(REFIID riid)
  {
    return riid == __uuidof(IDirect3DBaseTexture9) || Base::IsParentInterface(riid);
  }

public:
  DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew)
  {
    DWORD ret = this->m_pReal->SetLOD(LODNew);
    D3D9RecordSetLOD(this->m_pDevice, (NestedType *)this, LODNew);
    return ret;
  }
  DWORD STDMETHODCALLTYPE GetLOD() { return this->m_pReal->GetLOD(); }
  DWORD STDMETHODCALLTYPE GetLevelCount() { return this->m_pReal->GetLevelCount(); }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType)
  {
    HRESULT ret = this->m_pReal->SetAutoGenFilterType(FilterType);
    if(SUCCEEDED(ret))
      D3D9RecordSetAutoGenFilterType(this->m_pDevice, (NestedType *)this, FilterType);
    return ret;
  }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType()
  {
    return this->m_pReal->GetAutoGenFilterType();
  }
  void STDMETHODCALLTYPE GenerateMipSubLevels()
  {
    D3D9PrepareForWrite(this);
    this->m_pReal->GenerateMipSubLevels();
    D3D9RecordGenerateMipSubLevels(this->m_pDevice, (NestedType *)this);
  }
};

class WrappedD3DSurface9 : public WrappedD3D9Resource<IDirect3DSurface9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DSurface9);

  enum
  {
    TypeEnum = Resource_Surface,
  };

  // standalone surface, created by the application
  WrappedD3DSurface9(IDirect3DSurface9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9Resource(real, device, NULL, RealOwnership::WhileExternal)
  {
  }

  // a surface owned by a container (texture or swapchain), or by the device itself (the
  // automatic depth-stencil surface) when container is NULL
  WrappedD3DSurface9(IDirect3DSurface9 *real, WrappedD3DDevice9 *device, WrappedD3D9Object *container)
      : WrappedD3D9Resource(real, device, container, RealOwnership::None)
  {
  }

  D3D9ResourceType GetResourceType() const { return Resource_Surface; }
  const char *TypeName() const { return "IDirect3DSurface9"; }

  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer);
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) { return m_pReal->GetDesc(pDesc); }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags);
  HRESULT STDMETHODCALLTYPE UnlockRect();
  // GDI drawing isn't recorded, but the initial contents are read before it happens
  HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc)
  {
    D3D9PrepareForWrite(this);
    return m_pReal->GetDC(phdc);
  }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) { return m_pReal->ReleaseDC(hdc); }
};

class WrappedD3DVolume9 : public WrappedD3D9DeviceChild<IDirect3DVolume9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DVolume9);

  enum
  {
    TypeEnum = Resource_Volume,
  };

  // volumes only exist as levels of volume textures
  WrappedD3DVolume9(IDirect3DVolume9 *real, WrappedD3DDevice9 *device, WrappedD3D9Object *container)
      : WrappedD3D9DeviceChild(real, device, container, RealOwnership::None)
  {
  }

  D3D9ResourceType GetResourceType() const { return Resource_Volume; }
  const char *TypeName() const { return "IDirect3DVolume9"; }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void *pData, DWORD SizeOfData,
                                           DWORD Flags)
  {
    return m_pReal->SetPrivateData(refguid, pData, SizeOfData, Flags);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData)
  {
    return m_pReal->GetPrivateData(refguid, pData, pSizeOfData);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid)
  {
    return m_pReal->FreePrivateData(refguid);
  }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer);
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC *pDesc) { return m_pReal->GetDesc(pDesc); }
  HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX *pLockedVolume, CONST D3DBOX *pBox, DWORD Flags);
  HRESULT STDMETHODCALLTYPE UnlockBox();
};

class WrappedD3DTexture9 : public WrappedD3D9BaseTexture<IDirect3DTexture9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DTexture9);

  enum
  {
    TypeEnum = Resource_Texture,
  };

  WrappedD3DTexture9(IDirect3DTexture9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9BaseTexture(real, device)
  {
  }
  ~WrappedD3DTexture9();

  D3D9ResourceType GetResourceType() const { return Resource_Texture; }
  const char *TypeName() const { return "IDirect3DTexture9"; }

  // returns the wrapper for a level, creating it if needed, without adding a reference
  WrappedD3DSurface9 *GetLevel(UINT Level);

  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc)
  {
    return m_pReal->GetLevelDesc(Level, pDesc);
  }
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel);
  HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect,
                                     DWORD Flags);
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level);
  HRESULT STDMETHODCALLTYPE AddDirtyRect(CONST RECT *pDirtyRect);

private:
  rdcarray<WrappedD3DSurface9 *> m_Levels;
};

class WrappedD3DCubeTexture9 : public WrappedD3D9BaseTexture<IDirect3DCubeTexture9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DCubeTexture9);

  enum
  {
    TypeEnum = Resource_CubeTexture,
  };

  WrappedD3DCubeTexture9(IDirect3DCubeTexture9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9BaseTexture(real, device)
  {
  }
  ~WrappedD3DCubeTexture9();

  D3D9ResourceType GetResourceType() const { return Resource_CubeTexture; }
  const char *TypeName() const { return "IDirect3DCubeTexture9"; }

  WrappedD3DSurface9 *GetFace(D3DCUBEMAP_FACES FaceType, UINT Level);

  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc)
  {
    return m_pReal->GetLevelDesc(Level, pDesc);
  }
  HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level,
                                              IDirect3DSurface9 **ppCubeMapSurface);
  HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES FaceType, UINT Level,
                                     D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags);
  HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level);
  HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES FaceType, CONST RECT *pDirtyRect);

private:
  // indexed by level * 6 + face
  rdcarray<WrappedD3DSurface9 *> m_Faces;
};

class WrappedD3DVolumeTexture9 : public WrappedD3D9BaseTexture<IDirect3DVolumeTexture9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DVolumeTexture9);

  enum
  {
    TypeEnum = Resource_VolumeTexture,
  };

  WrappedD3DVolumeTexture9(IDirect3DVolumeTexture9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9BaseTexture(real, device)
  {
  }
  ~WrappedD3DVolumeTexture9();

  D3D9ResourceType GetResourceType() const { return Resource_VolumeTexture; }
  const char *TypeName() const { return "IDirect3DVolumeTexture9"; }

  WrappedD3DVolume9 *GetLevel(UINT Level);

  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc)
  {
    return m_pReal->GetLevelDesc(Level, pDesc);
  }
  HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel);
  HRESULT STDMETHODCALLTYPE LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, CONST D3DBOX *pBox,
                                    DWORD Flags);
  HRESULT STDMETHODCALLTYPE UnlockBox(UINT Level);
  HRESULT STDMETHODCALLTYPE AddDirtyBox(CONST D3DBOX *pDirtyBox);

private:
  rdcarray<WrappedD3DVolume9 *> m_Levels;
};

template <typename NestedType, typename DescType, D3D9ResourceType ResType>
class WrappedD3D9Buffer : public WrappedD3D9Resource<NestedType>
{
protected:
  typedef WrappedD3D9Resource<NestedType> Base;

  WrappedD3D9Buffer(NestedType *real, WrappedD3DDevice9 *device)
      : Base(real, device, NULL, WrappedD3D9Object::RealOwnership::WhileExternal)
  {
  }

public:
  D3D9ResourceType GetResourceType() const { return ResType; }

  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags)
  {
    // locks are only tracked while capturing, so that they can be recorded. Buffers have no dirty
    // regions to keep up to date.
    if(!D3D9IsActiveCapturing(this->m_pDevice))
      return this->m_pReal->Lock(OffsetToLock, SizeToLock, ppbData, Flags);

    // a size of 0 locks to the end of the buffer
    DescType desc = {};
    this->m_pReal->GetDesc(&desc);
    if(SizeToLock == 0)
      SizeToLock = desc.Size > OffsetToLock ? desc.Size - OffsetToLock : 0;

    D3DBOX region = {OffsetToLock, 0, OffsetToLock + SizeToLock, 1, 0, 1};

    D3D9BeforeLock(this, 0, 0, &region, Flags);
    HRESULT hr = this->m_pReal->Lock(OffsetToLock, SizeToLock, ppbData, Flags);
    if(SUCCEEDED(hr) && ppbData)
      D3D9AfterLock(this, 0, 0, &region, Flags, *ppbData, SizeToLock, SizeToLock);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE Unlock()
  {
    D3D9BeforeUnlock(this, 0, 0);
    return this->m_pReal->Unlock();
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DescType *pDesc) { return this->m_pReal->GetDesc(pDesc); }
};

class WrappedD3DVertexBuffer9
    : public WrappedD3D9Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC, Resource_VertexBuffer>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DVertexBuffer9);

  enum
  {
    TypeEnum = Resource_VertexBuffer,
  };

  WrappedD3DVertexBuffer9(IDirect3DVertexBuffer9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9Buffer(real, device)
  {
  }

  const char *TypeName() const { return "IDirect3DVertexBuffer9"; }
};

class WrappedD3DIndexBuffer9
    : public WrappedD3D9Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC, Resource_IndexBuffer>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DIndexBuffer9);

  enum
  {
    TypeEnum = Resource_IndexBuffer,
  };

  WrappedD3DIndexBuffer9(IDirect3DIndexBuffer9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9Buffer(real, device)
  {
  }

  const char *TypeName() const { return "IDirect3DIndexBuffer9"; }
};

class WrappedD3DVertexDeclaration9 : public WrappedD3D9DeviceChild<IDirect3DVertexDeclaration9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DVertexDeclaration9);

  enum
  {
    TypeEnum = Resource_VertexDeclaration,
  };

  WrappedD3DVertexDeclaration9(IDirect3DVertexDeclaration9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9DeviceChild(real, device, NULL, RealOwnership::WhileExternal)
  {
  }

  D3D9ResourceType GetResourceType() const { return Resource_VertexDeclaration; }
  const char *TypeName() const { return "IDirect3DVertexDeclaration9"; }

  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements)
  {
    return m_pReal->GetDeclaration(pElement, pNumElements);
  }
};

class WrappedD3DVertexShader9 : public WrappedD3D9DeviceChild<IDirect3DVertexShader9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DVertexShader9);

  enum
  {
    TypeEnum = Resource_VertexShader,
  };

  WrappedD3DVertexShader9(IDirect3DVertexShader9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9DeviceChild(real, device, NULL, RealOwnership::WhileExternal)
  {
  }

  D3D9ResourceType GetResourceType() const { return Resource_VertexShader; }
  const char *TypeName() const { return "IDirect3DVertexShader9"; }

  HRESULT STDMETHODCALLTYPE GetFunction(void *pData, UINT *pSizeOfData)
  {
    return m_pReal->GetFunction(pData, pSizeOfData);
  }
};

class WrappedD3DPixelShader9 : public WrappedD3D9DeviceChild<IDirect3DPixelShader9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DPixelShader9);

  enum
  {
    TypeEnum = Resource_PixelShader,
  };

  WrappedD3DPixelShader9(IDirect3DPixelShader9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9DeviceChild(real, device, NULL, RealOwnership::WhileExternal)
  {
  }

  D3D9ResourceType GetResourceType() const { return Resource_PixelShader; }
  const char *TypeName() const { return "IDirect3DPixelShader9"; }

  HRESULT STDMETHODCALLTYPE GetFunction(void *pData, UINT *pSizeOfData)
  {
    return m_pReal->GetFunction(pData, pSizeOfData);
  }
};

class WrappedD3DQuery9 : public WrappedD3D9DeviceChild<IDirect3DQuery9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DQuery9);

  enum
  {
    TypeEnum = Resource_Query,
  };

  WrappedD3DQuery9(IDirect3DQuery9 *real, WrappedD3DDevice9 *device)
      : WrappedD3D9DeviceChild(real, device, NULL, RealOwnership::WhileExternal)
  {
  }

  D3D9ResourceType GetResourceType() const { return Resource_Query; }
  const char *TypeName() const { return "IDirect3DQuery9"; }

  D3DQUERYTYPE STDMETHODCALLTYPE GetType() { return m_pReal->GetType(); }
  DWORD STDMETHODCALLTYPE GetDataSize() { return m_pReal->GetDataSize(); }
  HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags);
  HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags)
  {
    return m_pReal->GetData(pData, dwSize, dwGetDataFlags);
  }
};

class WrappedD3DStateBlock9 : public WrappedD3D9DeviceChild<IDirect3DStateBlock9>
{
public:
  ALLOCATE_WITH_WRAPPED_POOL(WrappedD3DStateBlock9);

  enum
  {
    TypeEnum = Resource_StateBlock,
  };

  // the bindings and mask are taken from the device: either a snapshot of the current state
  // (CreateStateBlock) or what was recorded (BeginStateBlock/EndStateBlock)
  WrappedD3DStateBlock9(IDirect3DStateBlock9 *real, WrappedD3DDevice9 *device,
                        const D3D9BindingMask &mask, const D3D9ObjectBindings &bindings)
      : WrappedD3D9DeviceChild(real, device, NULL, RealOwnership::WhileExternal), m_Mask(mask)
  {
    m_Bindings.CopyFrom(bindings, mask);
  }

  D3D9ResourceType GetResourceType() const { return Resource_StateBlock; }
  const char *TypeName() const { return "IDirect3DStateBlock9"; }

  HRESULT STDMETHODCALLTYPE Capture();
  HRESULT STDMETHODCALLTYPE Apply();

private:
  D3D9BindingMask m_Mask;
  D3D9ObjectBindings m_Bindings;
};

// WrappedD3DSwapChain9 is declared in d3d9_swapchain.h, include it to use this list
#define ALL_D3D9_POOLED_TYPES                    \
  D3D9_TYPE_MACRO(WrappedD3DSurface9);           \
  D3D9_TYPE_MACRO(WrappedD3DVolume9);            \
  D3D9_TYPE_MACRO(WrappedD3DTexture9);           \
  D3D9_TYPE_MACRO(WrappedD3DCubeTexture9);       \
  D3D9_TYPE_MACRO(WrappedD3DVolumeTexture9);     \
  D3D9_TYPE_MACRO(WrappedD3DVertexBuffer9);      \
  D3D9_TYPE_MACRO(WrappedD3DIndexBuffer9);       \
  D3D9_TYPE_MACRO(WrappedD3DVertexDeclaration9); \
  D3D9_TYPE_MACRO(WrappedD3DVertexShader9);      \
  D3D9_TYPE_MACRO(WrappedD3DPixelShader9);       \
  D3D9_TYPE_MACRO(WrappedD3DQuery9);             \
  D3D9_TYPE_MACRO(WrappedD3DStateBlock9);        \
  D3D9_TYPE_MACRO(WrappedD3DSwapChain9);
