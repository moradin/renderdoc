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

#include "d3d9_contents.h"
#include "d3d9_device.h"
#include "d3d9_resources.h"

// See d3d9_contents.h for how contents are captured.

///////////////////////////////////////////////////////////////////////////////
// formats and layouts

D3D9FormatBlock D3D9GetFormatBlock(D3DFORMAT format)
{
  D3D9FormatBlock ret;

  // FOURCC formats aren't enumerators, so switch on the value
  switch((DWORD)format)
  {
    case D3DFMT_A8:
    case D3DFMT_P8:
    case D3DFMT_L8:
    case D3DFMT_A4L4:
    case D3DFMT_R3G3B2:
    case D3DFMT_S8_LOCKABLE:
    // buffers, whose regions are in bytes
    case D3DFMT_VERTEXDATA: ret.Bytes = 1; break;

    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8P8:
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_L16:
    case D3DFMT_R16F:
    case D3DFMT_CxV8U8:
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D15S1:
    case D3DFMT_INDEX16:
    case MAKEFOURCC('D', 'F', '1', '6'): ret.Bytes = 2; break;

    case D3DFMT_R8G8B8: ret.Bytes = 3; break;

    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_G16R16:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_X8L8V8U8:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
    case D3DFMT_A2W10V10U10:
    case D3DFMT_G16R16F:
    case D3DFMT_R32F:
    case D3DFMT_A2B10G10R10_XR_BIAS:
    case D3DFMT_D32:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D24FS8:
    case D3DFMT_D32_LOCKABLE:
    case D3DFMT_INDEX32:
    case MAKEFOURCC('I', 'N', 'T', 'Z'):
    case MAKEFOURCC('D', 'F', '2', '4'):
    case MAKEFOURCC('R', 'A', 'W', 'Z'): ret.Bytes = 4; break;

    case D3DFMT_A16B16G16R16:
    case D3DFMT_Q16W16V16U16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F: ret.Bytes = 8; break;

    case D3DFMT_A32B32G32R32F: ret.Bytes = 16; break;

    // two pixels share each 4-byte block
    case D3DFMT_UYVY:
    case D3DFMT_YUY2:
    case D3DFMT_R8G8_B8G8:
    case D3DFMT_G8R8_G8B8:
      ret.Width = 2;
      ret.Bytes = 4;
      break;

    case D3DFMT_DXT1:
    case MAKEFOURCC('A', 'T', 'I', '1'):
      ret.Width = ret.Height = 4;
      ret.Bytes = 8;
      break;

    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
    case MAKEFOURCC('A', 'T', 'I', '2'):
      ret.Width = ret.Height = 4;
      ret.Bytes = 16;
      break;

    default: break;
  }

  return ret;
}

namespace
{
uint32_t BlocksFor(uint32_t pixels, uint32_t blockSize)
{
  return (pixels + blockSize - 1) / blockSize;
}

bool IsLockableDepth(D3DFORMAT format)
{
  return format == D3DFMT_D16_LOCKABLE || format == D3DFMT_D32F_LOCKABLE ||
         format == D3DFMT_D32_LOCKABLE || format == D3DFMT_S8_LOCKABLE;
}

// D3DPOOL_DEFAULT textures that can't be locked or read back, so get a shadow
bool IsShadowed(D3DPOOL pool, DWORD usage)
{
  return pool == D3DPOOL_DEFAULT &&
         (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)) == 0;
}

void AddNote(D3D9InitialContentsData &contents, const rdcstr &note)
{
  if(!contents.Note.empty())
    contents.Note += "\n";
  contents.Note += note;
}

// the level-0 region of a dirty region, scaled to a level and aligned to the format's blocks
bool DirtyRegionAtLevel(const D3D9DirtyRegion &dirty, uint32_t level, D3DFORMAT format,
                        uint32_t width, uint32_t height, uint32_t depth, D3DBOX &box)
{
  if(!dirty.Dirty)
    return false;

  if(dirty.Whole)
  {
    box = {0, 0, width, height, 0, depth};
    return true;
  }

  const uint32_t scale = (1U << level) - 1;
  box.Left = dirty.Box.Left >> level;
  box.Top = dirty.Box.Top >> level;
  box.Front = dirty.Box.Front >> level;
  box.Right = RDCMIN((dirty.Box.Right + scale) >> level, width);
  box.Bottom = RDCMIN((dirty.Box.Bottom + scale) >> level, height);
  box.Back = RDCMIN((dirty.Box.Back + scale) >> level, depth);

  // locks of block-compressed formats must cover whole blocks
  D3D9FormatBlock block = D3D9GetFormatBlock(format);
  box.Left -= box.Left % block.Width;
  box.Top -= box.Top % block.Height;
  box.Right = RDCMIN(BlocksFor(box.Right, block.Width) * block.Width, width);
  box.Bottom = RDCMIN(BlocksFor(box.Bottom, block.Height) * block.Height, height);

  return box.Left < box.Right && box.Top < box.Bottom && box.Front < box.Back;
}

// writes a region of a level into its shadow, creating the shadow if needed. Returns false if the
// format's layout isn't known.
bool WriteShadow(bytebuf &shadow, D3DFORMAT format, uint32_t width, uint32_t height, uint32_t depth,
                 const D3DBOX &box, const void *src, uint32_t srcRowPitch, uint32_t srcSlicePitch)
{
  D3D9FormatBlock block = D3D9GetFormatBlock(format);
  if(block.Bytes == 0 || src == NULL)
    return false;

  const uint32_t rowPitch = BlocksFor(width, block.Width) * block.Bytes;
  const uint32_t rowCount = BlocksFor(height, block.Height);
  const size_t slicePitch = size_t(rowPitch) * rowCount;

  // contents of D3DPOOL_DEFAULT textures start out undefined, zero is as good as anything
  if(shadow.empty())
    shadow.resize(slicePitch * depth);

  const uint32_t x = box.Left / block.Width * block.Bytes;
  const uint32_t y = box.Top / block.Height;
  const uint32_t copyBytes =
      RDCMIN(BlocksFor(box.Right - box.Left, block.Width) * block.Bytes, rowPitch - x);
  const uint32_t rows = RDCMIN(BlocksFor(box.Bottom - box.Top, block.Height), rowCount - y);
  const uint32_t slices = RDCMIN(box.Back - box.Front, depth - box.Front);

  if(x >= rowPitch || y >= rowCount || box.Front >= depth)
    return true;

  const byte *srcBytes = (const byte *)src;
  for(uint32_t z = 0; z < slices; z++)
  {
    for(uint32_t r = 0; r < rows; r++)
    {
      byte *dst = shadow.data() + (box.Front + z) * slicePitch + size_t(y + r) * rowPitch + x;
      memcpy(dst, srcBytes + size_t(z) * srcSlicePitch + size_t(r) * srcRowPitch, copyBytes);
    }
  }

  return true;
}

// a whole level of a shadow as contents
void ShadowToContents(const bytebuf &shadow, D3DFORMAT format, uint32_t face, uint32_t level,
                      uint32_t width, uint32_t height, uint32_t depth,
                      D3D9InitialContentsData &contents)
{
  if(shadow.empty())
    return;

  D3D9FormatBlock block = D3D9GetFormatBlock(format);

  D3D9SubresourceData sub;
  sub.Face = face;
  sub.Level = level;
  sub.Width = width;
  sub.Height = height;
  sub.Depth = depth;
  sub.RowPitch = BlocksFor(width, block.Width) * block.Bytes;
  sub.RowCount = BlocksFor(height, block.Height);
  sub.Data = shadow;
  contents.Subresources.push_back(std::move(sub));
}

// the object holding obj's contents, and which of its subresources obj is. Texture levels belong
// to their texture, anything else to itself.
WrappedD3D9Object *ContentsOwner(WrappedD3D9Object *obj, UINT &face, UINT &level)
{
  WrappedD3D9Object *container = obj->GetContainer();
  if(container)
  {
    D3D9ResourceType type = container->GetResourceType();
    if(type == Resource_Texture || type == Resource_CubeTexture || type == Resource_VolumeTexture)
    {
      face = obj->GetChildFace();
      level = obj->GetChildLevel();
      return container;
    }
  }
  return obj;
}

};

bool D3D9HasContents(WrappedD3D9Object *obj)
{
  switch(obj->GetResourceType())
  {
    case Resource_VertexBuffer:
    case Resource_IndexBuffer:
    case Resource_Texture:
    case Resource_CubeTexture:
    case Resource_VolumeTexture:
    case Resource_StateBlock: return true;
    // surfaces that aren't texture levels: standalone, device-owned, or back buffers
    case Resource_Surface:
      return obj->GetContainer() == NULL ||
             obj->GetContainer()->GetResourceType() == Resource_SwapChain;
    default: return false;
  }
}

namespace
{

// the format and size of a subresource of obj, which isn't necessarily the object holding the
// contents (it can be a texture level)
bool GetSubresourceDesc(WrappedD3D9Object *obj, UINT face, UINT level, D3DFORMAT &format,
                        D3DBOX &whole)
{
  whole = {0, 0, 0, 1, 0, 1};

  switch(obj->GetResourceType())
  {
    case Resource_Surface:
    {
      D3DSURFACE_DESC desc = {};
      if(FAILED(((WrappedD3DSurface9 *)obj)->GetReal()->GetDesc(&desc)))
        return false;
      format = desc.Format;
      whole.Right = desc.Width;
      whole.Bottom = desc.Height;
      return true;
    }
    case Resource_Texture:
    case Resource_CubeTexture:
    {
      D3DSURFACE_DESC desc = {};
      HRESULT hr = obj->GetResourceType() == Resource_Texture
                       ? ((WrappedD3DTexture9 *)obj)->GetReal()->GetLevelDesc(level, &desc)
                       : ((WrappedD3DCubeTexture9 *)obj)->GetReal()->GetLevelDesc(level, &desc);
      if(FAILED(hr))
        return false;
      format = desc.Format;
      whole.Right = desc.Width;
      whole.Bottom = desc.Height;
      return true;
    }
    case Resource_Volume:
    case Resource_VolumeTexture:
    {
      D3DVOLUME_DESC desc = {};
      HRESULT hr = obj->GetResourceType() == Resource_Volume
                       ? ((WrappedD3DVolume9 *)obj)->GetReal()->GetDesc(&desc)
                       : ((WrappedD3DVolumeTexture9 *)obj)->GetReal()->GetLevelDesc(level, &desc);
      if(FAILED(hr))
        return false;
      format = desc.Format;
      whole.Right = desc.Width;
      whole.Bottom = desc.Height;
      whole.Back = desc.Depth;
      return true;
    }
    case Resource_VertexBuffer:
    {
      D3DVERTEXBUFFER_DESC desc = {};
      if(FAILED(((WrappedD3DVertexBuffer9 *)obj)->GetReal()->GetDesc(&desc)))
        return false;
      format = D3DFMT_VERTEXDATA;
      whole.Right = desc.Size;
      return true;
    }
    case Resource_IndexBuffer:
    {
      D3DINDEXBUFFER_DESC desc = {};
      if(FAILED(((WrappedD3DIndexBuffer9 *)obj)->GetReal()->GetDesc(&desc)))
        return false;
      // the region is in bytes, whatever the index size
      format = D3DFMT_VERTEXDATA;
      whole.Right = desc.Size;
      return true;
    }
    default: return false;
  }
}

///////////////////////////////////////////////////////////////////////////////
// reading contents back

template <typename Buffer, typename Desc>
void ReadBuffer(Buffer *buf, D3D9InitialContentsData &contents)
{
  Desc desc = {};
  buf->GetDesc(&desc);

  if(desc.Size == 0)
    return;

  void *data = NULL;
  HRESULT hr = buf->Lock(0, 0, &data, D3DLOCK_READONLY);
  if(FAILED(hr) || data == NULL)
  {
    AddNote(contents, StringFormat::Fmt("Couldn't lock the buffer: %s", ToStr(hr).c_str()));
    return;
  }

  D3D9SubresourceData sub;
  sub.Width = desc.Size;
  D3D9CopyFromLocked(sub, D3DFMT_VERTEXDATA, data, desc.Size, desc.Size);

  buf->Unlock();

  contents.Subresources.push_back(std::move(sub));
}

// reads a whole lockable surface
rdcstr LockSurface(IDirect3DSurface9 *surf, const D3DSURFACE_DESC &desc, D3D9SubresourceData &sub)
{
  D3DLOCKED_RECT locked = {};
  HRESULT hr = surf->LockRect(&locked, NULL, D3DLOCK_READONLY);
  if(FAILED(hr) || locked.pBits == NULL)
    return StringFormat::Fmt("Couldn't lock: %s", ToStr(hr).c_str());

  sub.Width = desc.Width;
  sub.Height = desc.Height;
  D3D9CopyFromLocked(sub, desc.Format, locked.pBits, (uint32_t)locked.Pitch, 0);

  surf->UnlockRect();
  return rdcstr();
}

// reads a render target through a system memory copy, resolving it first if it's multisampled
rdcstr ReadRenderTarget(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf, const D3DSURFACE_DESC &desc,
                        D3D9SubresourceData &sub)
{
  IDirect3DSurface9 *source = surf;
  IDirect3DSurface9 *resolved = NULL;
  IDirect3DSurface9 *sysmem = NULL;
  rdcstr err;

  HRESULT hr = S_OK;

  if(desc.MultiSampleType != D3DMULTISAMPLE_NONE)
  {
    hr = dev->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0,
                                 FALSE, &resolved, NULL);
    if(SUCCEEDED(hr))
      hr = dev->StretchRect(surf, NULL, resolved, NULL, D3DTEXF_NONE);
    if(FAILED(hr))
      err = StringFormat::Fmt("Couldn't resolve the multisampled render target: %s",
                              ToStr(hr).c_str());
    source = resolved;
  }

  if(err.empty())
  {
    hr = dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                          &sysmem, NULL);
    if(SUCCEEDED(hr))
      hr = dev->GetRenderTargetData(source, sysmem);
    if(FAILED(hr))
      err = StringFormat::Fmt("Couldn't copy the render target to system memory: %s",
                              ToStr(hr).c_str());
  }

  if(err.empty())
  {
    D3DSURFACE_DESC sysDesc = desc;
    sysDesc.Pool = D3DPOOL_SYSTEMMEM;
    sysDesc.MultiSampleType = D3DMULTISAMPLE_NONE;
    err = LockSurface(sysmem, sysDesc, sub);
  }

  SAFE_RELEASE(sysmem);
  SAFE_RELEASE(resolved);
  return err;
}

// reads a whole surface that isn't a texture level, or a level of a texture that isn't shadowed
rdcstr ReadSurface(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf, D3D9SubresourceData &sub)
{
  D3DSURFACE_DESC desc = {};
  surf->GetDesc(&desc);

  if(desc.Usage & D3DUSAGE_RENDERTARGET)
    return ReadRenderTarget(dev, surf, desc, sub);

  if((desc.Usage & D3DUSAGE_DEPTHSTENCIL) && !IsLockableDepth(desc.Format))
    return StringFormat::Fmt("Depth-stencil contents in %s can't be read back",
                             ToStr(desc.Format).c_str());

  return LockSurface(surf, desc, sub);
}

};

void D3D9CopyFromLocked(D3D9SubresourceData &dst, D3DFORMAT format, const void *src,
                        uint32_t srcRowPitch, uint32_t srcSlicePitch)
{
  D3D9FormatBlock block = D3D9GetFormatBlock(format);

  if(block.Bytes != 0)
  {
    dst.RowPitch = BlocksFor(dst.Width, block.Width) * block.Bytes;
    dst.RowCount = BlocksFor(dst.Height, block.Height);
  }
  else if(dst.X != 0)
  {
    // without the layout we can't tell where the region's rows end, and copying whole rows from
    // the start of the region could read past the end of the locked memory
    RDCWARN("Not recording a region of a %s subresource, its layout isn't known",
            ToStr(format).c_str());
    dst.RowPitch = dst.RowCount = 0;
    return;
  }
  else
  {
    // unknown layout, keep the driver's rows
    dst.RowPitch = srcRowPitch;
    dst.RowCount = dst.Height;
  }

  const size_t slicePitch = size_t(dst.RowPitch) * dst.RowCount;
  dst.Data.resize(slicePitch * dst.Depth);

  if(src == NULL)
    return;

  const uint32_t copyBytes = RDCMIN(dst.RowPitch, srcRowPitch);
  const byte *srcBytes = (const byte *)src;

  for(uint32_t z = 0; z < dst.Depth; z++)
    for(uint32_t r = 0; r < dst.RowCount; r++)
      memcpy(dst.Data.data() + z * slicePitch + size_t(r) * dst.RowPitch,
             srcBytes + size_t(z) * srcSlicePitch + size_t(r) * srcRowPitch, copyBytes);
}

void D3D9DirtyRegion::Add(const D3DBOX *box)
{
  if(box == NULL)
  {
    Dirty = Whole = true;
    return;
  }

  if(Dirty && Whole)
    return;

  if(!Dirty)
  {
    Box = *box;
    Dirty = true;
    Whole = false;
    return;
  }

  Box.Left = RDCMIN(Box.Left, box->Left);
  Box.Top = RDCMIN(Box.Top, box->Top);
  Box.Front = RDCMIN(Box.Front, box->Front);
  Box.Right = RDCMAX(Box.Right, box->Right);
  Box.Bottom = RDCMAX(Box.Bottom, box->Bottom);
  Box.Back = RDCMAX(Box.Back, box->Back);
}

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3D9SubresourceData &el)
{
  SERIALISE_MEMBER(Face);
  SERIALISE_MEMBER(Level);
  SERIALISE_MEMBER(X);
  SERIALISE_MEMBER(Y);
  SERIALISE_MEMBER(Z);
  SERIALISE_MEMBER(Width);
  SERIALISE_MEMBER(Height);
  SERIALISE_MEMBER(Depth);
  SERIALISE_MEMBER(RowPitch);
  SERIALISE_MEMBER(RowCount);
  SERIALISE_MEMBER(Data);
}

INSTANTIATE_SERIALISE_TYPE(D3D9SubresourceData);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3D9StateBlockContents &el)
{
  SERIALISE_MEMBER(Textures);
  SERIALISE_MEMBER(StreamSources);
  SERIALISE_MEMBER(StreamOffsets);
  SERIALISE_MEMBER(StreamStrides);
  SERIALISE_MEMBER(Indices);
  SERIALISE_MEMBER(VertexShader);
  SERIALISE_MEMBER(PixelShader);
  SERIALISE_MEMBER(VertexDeclaration);
  SERIALISE_MEMBER(DeclarationFromFVF);
  SERIALISE_MEMBER(State);
}

INSTANTIATE_SERIALISE_TYPE(D3D9StateBlockContents);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, D3D9InitialContentsData &el)
{
  SERIALISE_MEMBER(Subresources);
  SERIALISE_MEMBER(StateBlock);
  SERIALISE_MEMBER(Note);
}

INSTANTIATE_SERIALISE_TYPE(D3D9InitialContentsData);

///////////////////////////////////////////////////////////////////////////////
// initial contents

void WrappedD3DDevice9::ReadContents(WrappedD3D9Object *obj, D3D9InitialContentsData &contents)
{
  SCOPED_LOCK(m_D3DLock);

  switch(obj->GetResourceType())
  {
    case Resource_VertexBuffer:
      ReadBuffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(
          ((WrappedD3DVertexBuffer9 *)obj)->GetReal(), contents);
      break;
    case Resource_IndexBuffer:
      ReadBuffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(
          ((WrappedD3DIndexBuffer9 *)obj)->GetReal(), contents);
      break;
    case Resource_Surface:
    {
      D3D9SubresourceData sub;
      rdcstr err = ReadSurface(m_device, ((WrappedD3DSurface9 *)obj)->GetReal(), sub);
      if(err.empty())
        contents.Subresources.push_back(std::move(sub));
      else
        AddNote(contents, err);
      break;
    }
    case Resource_Texture:
    case Resource_CubeTexture:
    {
      const bool cube = obj->GetResourceType() == Resource_CubeTexture;
      IDirect3DTexture9 *tex = cube ? NULL : ((WrappedD3DTexture9 *)obj)->GetReal();
      IDirect3DCubeTexture9 *cubeTex = cube ? ((WrappedD3DCubeTexture9 *)obj)->GetReal() : NULL;

      const UINT levels = cube ? cubeTex->GetLevelCount() : tex->GetLevelCount();
      const UINT faces = cube ? 6 : 1;

      D3DSURFACE_DESC desc = {};
      if(cube)
        cubeTex->GetLevelDesc(0, &desc);
      else
        tex->GetLevelDesc(0, &desc);

      if(IsShadowed(desc.Pool, desc.Usage))
      {
        if(D3D9GetFormatBlock(desc.Format).Bytes == 0)
        {
          AddNote(contents, StringFormat::Fmt("The layout of %s isn't known, so this "
                                              "D3DPOOL_DEFAULT texture has no shadow copy",
                                              ToStr(desc.Format).c_str()));
          break;
        }

        const rdcarray<bytebuf> *shadow =
            obj->HasContentsTracking() ? &obj->GetContents().Shadow : NULL;

        for(UINT face = 0; shadow && face < faces; face++)
        {
          for(UINT level = 0; level < levels && face * levels + level < shadow->size(); level++)
          {
            D3DSURFACE_DESC levelDesc = {};
            if(cube)
              cubeTex->GetLevelDesc(level, &levelDesc);
            else
              tex->GetLevelDesc(level, &levelDesc);

            ShadowToContents(shadow->at(face * levels + level), desc.Format, face, level,
                             levelDesc.Width, levelDesc.Height, 1, contents);
          }
        }

        if(contents.Subresources.empty())
          AddNote(contents,
                  "Nothing has been uploaded to this D3DPOOL_DEFAULT texture with "
                  "UpdateTexture or UpdateSurface, so its contents are undefined");
        break;
      }

      if((desc.Usage & D3DUSAGE_DEPTHSTENCIL) && !IsLockableDepth(desc.Format))
      {
        AddNote(contents, StringFormat::Fmt("Depth-stencil contents in %s can't be read back",
                                            ToStr(desc.Format).c_str()));
        break;
      }

      for(UINT face = 0; face < faces; face++)
      {
        for(UINT level = 0; level < levels; level++)
        {
          D3D9SubresourceData sub;
          sub.Face = face;
          sub.Level = level;

          rdcstr err;

          if(desc.Usage & D3DUSAGE_RENDERTARGET)
          {
            IDirect3DSurface9 *surf = NULL;
            HRESULT hr = cube ? cubeTex->GetCubeMapSurface((D3DCUBEMAP_FACES)face, level, &surf)
                              : tex->GetSurfaceLevel(level, &surf);
            if(SUCCEEDED(hr) && surf)
            {
              err = ReadSurface(m_device, surf, sub);
              surf->Release();
            }
            else
            {
              err = StringFormat::Fmt("Couldn't get the surface: %s", ToStr(hr).c_str());
            }
          }
          else
          {
            D3DSURFACE_DESC levelDesc = {};
            D3DLOCKED_RECT locked = {};
            HRESULT hr;
            if(cube)
            {
              cubeTex->GetLevelDesc(level, &levelDesc);
              hr = cubeTex->LockRect((D3DCUBEMAP_FACES)face, level, &locked, NULL, D3DLOCK_READONLY);
            }
            else
            {
              tex->GetLevelDesc(level, &levelDesc);
              hr = tex->LockRect(level, &locked, NULL, D3DLOCK_READONLY);
            }

            if(SUCCEEDED(hr) && locked.pBits)
            {
              sub.Width = levelDesc.Width;
              sub.Height = levelDesc.Height;
              D3D9CopyFromLocked(sub, levelDesc.Format, locked.pBits, (uint32_t)locked.Pitch, 0);
              if(cube)
                cubeTex->UnlockRect((D3DCUBEMAP_FACES)face, level);
              else
                tex->UnlockRect(level);
            }
            else
            {
              err = StringFormat::Fmt("Couldn't lock: %s", ToStr(hr).c_str());
            }
          }

          if(err.empty())
            contents.Subresources.push_back(std::move(sub));
          else
            AddNote(contents, StringFormat::Fmt("Face %u level %u: %s", face, level, err.c_str()));
        }
      }
      break;
    }
    case Resource_VolumeTexture:
    {
      IDirect3DVolumeTexture9 *tex = ((WrappedD3DVolumeTexture9 *)obj)->GetReal();
      const UINT levels = tex->GetLevelCount();

      D3DVOLUME_DESC desc = {};
      tex->GetLevelDesc(0, &desc);

      const bool shadowed = IsShadowed(desc.Pool, desc.Usage);

      if(shadowed && D3D9GetFormatBlock(desc.Format).Bytes == 0)
      {
        AddNote(contents, StringFormat::Fmt("The layout of %s isn't known, so this "
                                            "D3DPOOL_DEFAULT texture has no shadow copy",
                                            ToStr(desc.Format).c_str()));
        break;
      }

      for(UINT level = 0; level < levels; level++)
      {
        D3DVOLUME_DESC levelDesc = {};
        tex->GetLevelDesc(level, &levelDesc);

        if(shadowed)
        {
          if(obj->HasContentsTracking() && level < obj->GetContents().Shadow.size())
            ShadowToContents(obj->GetContents().Shadow[level], desc.Format, 0, level,
                             levelDesc.Width, levelDesc.Height, levelDesc.Depth, contents);
          continue;
        }

        D3DLOCKED_BOX locked = {};
        HRESULT hr = tex->LockBox(level, &locked, NULL, D3DLOCK_READONLY);
        if(FAILED(hr) || locked.pBits == NULL)
        {
          AddNote(contents,
                  StringFormat::Fmt("Level %u: couldn't lock: %s", level, ToStr(hr).c_str()));
          continue;
        }

        D3D9SubresourceData sub;
        sub.Level = level;
        sub.Width = levelDesc.Width;
        sub.Height = levelDesc.Height;
        sub.Depth = levelDesc.Depth;
        D3D9CopyFromLocked(sub, levelDesc.Format, locked.pBits, (uint32_t)locked.RowPitch,
                           (uint32_t)locked.SlicePitch);
        tex->UnlockBox(level);

        contents.Subresources.push_back(std::move(sub));
      }

      if(shadowed && contents.Subresources.empty())
        AddNote(contents,
                "Nothing has been uploaded to this D3DPOOL_DEFAULT texture with UpdateTexture, "
                "so its contents are undefined");
      break;
    }
    case Resource_StateBlock:
      ReadStateBlock((WrappedD3DStateBlock9 *)obj, contents);
      break;
    default: AddNote(contents, "This kind of object has no contents"); break;
  }
}

void WrappedD3DDevice9::ReadStateBlock(WrappedD3DStateBlock9 *sb, D3D9InitialContentsData &contents)
{
  // the state calls would go into the state block being recorded
  if(m_Recording)
  {
    AddNote(contents, "Couldn't read the state block, another one was being recorded");
    return;
  }

  D3D9DeviceState before;
  D3D9FetchDeviceState(m_device, m_InitParams.BehaviorFlags, m_LightIndices, m_PaletteNumbers,
                       before);

  HRESULT hr = sb->GetReal()->Apply();
  if(FAILED(hr))
  {
    AddNote(contents, StringFormat::Fmt("Couldn't apply the state block: %s", ToStr(hr).c_str()));
    return;
  }

  D3D9StateBlockContents sbContents;
  D3D9FetchDeviceState(m_device, m_InitParams.BehaviorFlags, m_LightIndices, m_PaletteNumbers,
                       sbContents.State);

  // put the device back. Applying a state block can change any state, including (for D3DSBT_ALL)
  // copying vertex shader constants c0-c127 over c128-c255, so compare everything.
  D3D9RestoreDeviceState(m_device, before, sbContents.State);
  RestoreRealBindings(before.FVF);

  // the bindings after applying it, as we track them
  {
    D3D9ObjectBindings applied;
    applied.CopyFrom(m_Bindings, D3D9BindingMask::All());
    applied.CopyFrom(sb->GetBindings(), sb->GetMask());

    for(IDirect3DBaseTexture9 *tex : applied.textures)
      sbContents.Textures.push_back(GetResID(tex));
    for(const D3D9StreamBinding &s : applied.streams)
    {
      sbContents.StreamSources.push_back(GetResID(s.buffer));
      sbContents.StreamOffsets.push_back(s.offset);
      sbContents.StreamStrides.push_back(s.stride);
    }
    sbContents.Indices = GetResID(applied.indices);
    sbContents.VertexShader = GetResID(applied.vs);
    sbContents.PixelShader = GetResID(applied.ps);
    sbContents.VertexDeclaration = GetResID(applied.decl);
    sbContents.DeclarationFromFVF = applied.declFromFVF;
  }

  contents.StateBlock.push_back(std::move(sbContents));
}

void WrappedD3DDevice9::RestoreRealBindings(DWORD fvf)
{
  for(uint32_t slot = 0; slot < D3D9_NUM_TEXTURE_SLOTS; slot++)
    m_device->SetTexture(D3D9SlotToSampler(slot), Unwrap(m_Bindings.textures[slot]));
  for(UINT i = 0; i < D3D9_NUM_STREAMS; i++)
    m_device->SetStreamSource(i, Unwrap(m_Bindings.streams[i].buffer), m_Bindings.streams[i].offset,
                              m_Bindings.streams[i].stride);
  m_device->SetIndices(Unwrap(m_Bindings.indices));
  m_device->SetVertexShader(Unwrap(m_Bindings.vs));
  m_device->SetPixelShader(Unwrap(m_Bindings.ps));
  // SetFVF also sets the runtime's declaration for it
  if(m_Bindings.declFromFVF)
    m_device->SetFVF(fvf);
  else
    m_device->SetVertexDeclaration(Unwrap(m_Bindings.decl));
}

void WrappedD3DDevice9::PrepareForWrite(WrappedD3D9Object *obj)
{
  if(obj == NULL || !IsActiveCapturing(m_State))
    return;

  UINT face = 0, level = 0;
  WrappedD3D9Object *owner = ContentsOwner(obj, face, level);
  if(!D3D9HasContents(owner))
    return;

  // the resource manager's lock is always taken after ours
  SCOPED_LOCK(m_D3DLock);
  m_ResourceManager->PrepareIfPostponed(owner->GetResourceID());
}

void WrappedD3DDevice9::PrepareForWrite(IUnknown *obj)
{
  if(obj && IsActiveCapturing(m_State))
    PrepareForWrite(GetWrappedObject(obj));
}

void WrappedD3DDevice9::PrepareRenderTargetsForWrite()
{
  if(!IsActiveCapturing(m_State))
    return;

  SCOPED_LOCK(m_D3DLock);
  for(IDirect3DSurface9 *rt : m_RenderTargets)
    PrepareForWrite(rt);
  PrepareForWrite(m_DepthStencil);
}

void WrappedD3DDevice9::OnLastExternalRelease(WrappedD3D9Object *obj)
{
  if(!IsActiveCapturing(m_State) || !D3D9HasContents(obj))
    return;

  // whether or not anything still binds it, this is the last time it's certainly alive
  SCOPED_LOCK(m_D3DLock);
  m_ResourceManager->PrepareIfPostponed(obj->GetResourceID());
}

///////////////////////////////////////////////////////////////////////////////
// shadows of D3DPOOL_DEFAULT textures

void WrappedD3DDevice9::UpdateShadowFromTexture(IDirect3DBaseTexture9 *pSourceTexture,
                                                IDirect3DBaseTexture9 *pDestinationTexture)
{
  WrappedD3D9Object *src = GetWrappedObject(pSourceTexture);
  WrappedD3D9Object *dst = GetWrappedObject(pDestinationTexture);
  if(src == NULL || dst == NULL || src->GetResourceType() != dst->GetResourceType())
    return;

  SCOPED_LOCK(m_D3DLock);

  D3D9ObjectContents &srcContents = src->GetContents();

  switch(dst->GetResourceType())
  {
    case Resource_Texture:
    case Resource_CubeTexture:
    {
      const bool cube = dst->GetResourceType() == Resource_CubeTexture;

      IDirect3DTexture9 *srcTex = cube ? NULL : ((WrappedD3DTexture9 *)src)->GetReal();
      IDirect3DTexture9 *dstTex = cube ? NULL : ((WrappedD3DTexture9 *)dst)->GetReal();
      IDirect3DCubeTexture9 *srcCube = cube ? ((WrappedD3DCubeTexture9 *)src)->GetReal() : NULL;
      IDirect3DCubeTexture9 *dstCube = cube ? ((WrappedD3DCubeTexture9 *)dst)->GetReal() : NULL;

      D3DSURFACE_DESC dstDesc = {};
      if(cube)
        dstCube->GetLevelDesc(0, &dstDesc);
      else
        dstTex->GetLevelDesc(0, &dstDesc);

      if(!IsShadowed(dstDesc.Pool, dstDesc.Usage))
        break;

      const UINT srcLevels = cube ? srcCube->GetLevelCount() : srcTex->GetLevelCount();
      const UINT dstLevels = cube ? dstCube->GetLevelCount() : dstTex->GetLevelCount();
      // extra source levels are the largest ones, and are skipped
      const UINT skip = srcLevels > dstLevels ? srcLevels - dstLevels : 0;
      const UINT faces = cube ? 6 : 1;

      rdcarray<bytebuf> &shadow = dst->GetContents().Shadow;
      shadow.resize(faces * dstLevels);

      for(UINT face = 0; face < faces; face++)
      {
        for(UINT level = 0; level < dstLevels; level++)
        {
          const UINT srcLevel = level + skip;

          D3DSURFACE_DESC levelDesc = {};
          if(cube)
            dstCube->GetLevelDesc(level, &levelDesc);
          else
            dstTex->GetLevelDesc(level, &levelDesc);

          D3DBOX box;
          if(!DirtyRegionAtLevel(srcContents.Dirty[face], srcLevel, levelDesc.Format,
                                 levelDesc.Width, levelDesc.Height, 1, box))
            continue;

          RECT rect = {(LONG)box.Left, (LONG)box.Top, (LONG)box.Right, (LONG)box.Bottom};
          D3DLOCKED_RECT locked = {};
          HRESULT hr = cube ? srcCube->LockRect((D3DCUBEMAP_FACES)face, srcLevel, &locked, &rect,
                                                D3DLOCK_READONLY)
                            : srcTex->LockRect(srcLevel, &locked, &rect, D3DLOCK_READONLY);
          if(FAILED(hr))
          {
            RDCWARN("Couldn't read UpdateTexture source for the shadow of %s: %s",
                    ToStr(dst->GetResourceID()).c_str(), ToStr(hr).c_str());
            continue;
          }

          WriteShadow(shadow[face * dstLevels + level], levelDesc.Format, levelDesc.Width,
                      levelDesc.Height, 1, box, locked.pBits, (uint32_t)locked.Pitch, 0);

          if(cube)
            srcCube->UnlockRect((D3DCUBEMAP_FACES)face, srcLevel);
          else
            srcTex->UnlockRect(srcLevel);
        }
      }
      break;
    }
    case Resource_VolumeTexture:
    {
      IDirect3DVolumeTexture9 *srcTex = ((WrappedD3DVolumeTexture9 *)src)->GetReal();
      IDirect3DVolumeTexture9 *dstTex = ((WrappedD3DVolumeTexture9 *)dst)->GetReal();

      D3DVOLUME_DESC dstDesc = {};
      dstTex->GetLevelDesc(0, &dstDesc);
      if(!IsShadowed(dstDesc.Pool, dstDesc.Usage))
        break;

      const UINT srcLevels = srcTex->GetLevelCount();
      const UINT dstLevels = dstTex->GetLevelCount();
      const UINT skip = srcLevels > dstLevels ? srcLevels - dstLevels : 0;

      rdcarray<bytebuf> &shadow = dst->GetContents().Shadow;
      shadow.resize(dstLevels);

      for(UINT level = 0; level < dstLevels; level++)
      {
        const UINT srcLevel = level + skip;

        D3DVOLUME_DESC levelDesc = {};
        dstTex->GetLevelDesc(level, &levelDesc);

        D3DBOX box;
        if(!DirtyRegionAtLevel(srcContents.Dirty[0], srcLevel, levelDesc.Format, levelDesc.Width,
                               levelDesc.Height, levelDesc.Depth, box))
          continue;

        D3DLOCKED_BOX locked = {};
        HRESULT hr = srcTex->LockBox(srcLevel, &locked, &box, D3DLOCK_READONLY);
        if(FAILED(hr))
        {
          RDCWARN("Couldn't read UpdateTexture source for the shadow of %s: %s",
                  ToStr(dst->GetResourceID()).c_str(), ToStr(hr).c_str());
          continue;
        }

        WriteShadow(shadow[level], levelDesc.Format, levelDesc.Width, levelDesc.Height,
                    levelDesc.Depth, box, locked.pBits, (uint32_t)locked.RowPitch,
                    (uint32_t)locked.SlicePitch);

        srcTex->UnlockBox(srcLevel);
      }
      break;
    }
    default: break;
  }

  // UpdateTexture copies the source's dirty region and then clears it
  for(D3D9DirtyRegion &dirty : srcContents.Dirty)
    dirty.Clear();
}

void WrappedD3DDevice9::UpdateShadowFromSurface(IDirect3DSurface9 *pSourceSurface,
                                                CONST RECT *pSourceRect,
                                                IDirect3DSurface9 *pDestinationSurface,
                                                CONST POINT *pDestPoint)
{
  WrappedD3D9Object *dstSurf = GetWrappedObject(pDestinationSurface);
  WrappedD3D9Object *srcSurf = GetWrappedObject(pSourceSurface);
  if(dstSurf == NULL || srcSurf == NULL)
    return;

  UINT face = 0, level = 0;
  WrappedD3D9Object *owner = ContentsOwner(dstSurf, face, level);
  if(owner == dstSurf)
    return;

  SCOPED_LOCK(m_D3DLock);

  D3DSURFACE_DESC levelDesc = {};
  UINT levels = 0;
  if(owner->GetResourceType() == Resource_Texture)
  {
    IDirect3DTexture9 *tex = ((WrappedD3DTexture9 *)owner)->GetReal();
    tex->GetLevelDesc(level, &levelDesc);
    levels = tex->GetLevelCount();
  }
  else if(owner->GetResourceType() == Resource_CubeTexture)
  {
    IDirect3DCubeTexture9 *tex = ((WrappedD3DCubeTexture9 *)owner)->GetReal();
    tex->GetLevelDesc(level, &levelDesc);
    levels = tex->GetLevelCount();
  }
  else
  {
    return;
  }

  if(!IsShadowed(levelDesc.Pool, levelDesc.Usage))
    return;

  IDirect3DSurface9 *src = ((WrappedD3DSurface9 *)srcSurf)->GetReal();
  D3DSURFACE_DESC srcDesc = {};
  src->GetDesc(&srcDesc);

  RECT srcRect = {0, 0, (LONG)srcDesc.Width, (LONG)srcDesc.Height};
  if(pSourceRect)
    srcRect = *pSourceRect;
  POINT dstPoint = {0, 0};
  if(pDestPoint)
    dstPoint = *pDestPoint;

  D3DLOCKED_RECT locked = {};
  HRESULT hr = src->LockRect(&locked, &srcRect, D3DLOCK_READONLY);
  if(FAILED(hr))
  {
    RDCWARN("Couldn't read UpdateSurface source for the shadow of %s: %s",
            ToStr(owner->GetResourceID()).c_str(), ToStr(hr).c_str());
    return;
  }

  D3DBOX box = {(UINT)dstPoint.x,
                (UINT)dstPoint.y,
                (UINT)(dstPoint.x + srcRect.right - srcRect.left),
                (UINT)(dstPoint.y + srcRect.bottom - srcRect.top),
                0,
                1};

  rdcarray<bytebuf> &shadow = owner->GetContents().Shadow;
  const UINT faces = owner->GetResourceType() == Resource_CubeTexture ? 6 : 1;
  shadow.resize(faces * levels);

  WriteShadow(shadow[face * levels + level], levelDesc.Format, levelDesc.Width, levelDesc.Height, 1,
              box, locked.pBits, (uint32_t)locked.Pitch, 0);

  src->UnlockRect();
}

///////////////////////////////////////////////////////////////////////////////
// locks

void WrappedD3DDevice9::OnLock(WrappedD3D9Object *obj, UINT face, UINT level, const D3DBOX *region,
                               DWORD flags)
{
  if(flags & D3DLOCK_READONLY)
    return;

  UINT ownerFace = face, ownerLevel = level;
  WrappedD3D9Object *owner = ContentsOwner(obj, ownerFace, ownerLevel);
  const D3D9ResourceType type = owner->GetResourceType();

  // the runtime only records dirty regions for locks on level 0
  if((type == Resource_Texture || type == Resource_CubeTexture || type == Resource_VolumeTexture) &&
     ownerLevel == 0 && ownerFace < 6 && (flags & D3DLOCK_NO_DIRTY_UPDATE) == 0)
  {
    SCOPED_LOCK(m_D3DLock);
    owner->GetContents().Dirty[ownerFace].Add(region);
  }

  PrepareForWrite(owner);
}

void WrappedD3DDevice9::OnLocked(WrappedD3D9Object *obj, UINT face, UINT level, const D3DBOX *region,
                                 DWORD flags, void *data, UINT rowPitch, UINT slicePitch)
{
  // Only locks that write are recorded. They're tracked whether or not a capture is running, so
  // that a lock taken before the capture starts is recorded when it ends during the capture.
  if(flags & D3DLOCK_READONLY)
    return;

  D3D9LockInfo info;
  info.Face = face;
  info.Level = level;
  info.Flags = flags;
  info.Data = (byte *)data;
  info.RowPitch = rowPitch;
  info.SlicePitch = slicePitch;

  if(region)
  {
    info.Region = *region;
  }
  else
  {
    D3DFORMAT format;
    if(!GetSubresourceDesc(obj, face, level, format, info.Region))
      return;
  }

  SCOPED_LOCK(m_D3DLock);
  obj->GetContents().Locks.push_back(info);
}

void WrappedD3DDevice9::OnUnlock(WrappedD3D9Object *obj, UINT face, UINT level)
{
  SCOPED_LOCK(m_D3DLock);

  if(!obj->HasContentsTracking())
    return;

  rdcarray<D3D9LockInfo> &locks = obj->GetContents().Locks;

  int32_t idx = -1;
  for(int32_t i = locks.count() - 1; i >= 0; i--)
  {
    if(locks[i].Face == face && locks[i].Level == level)
    {
      idx = i;
      break;
    }
  }

  if(idx < 0)
    return;

  D3D9LockInfo info = locks[idx];
  locks.erase(idx);

  if(!IsActiveCapturing(m_State))
    return;

  D3DFORMAT format;
  D3DBOX whole;
  if(!GetSubresourceDesc(obj, face, level, format, whole))
    return;

  D3D9SubresourceData data;
  data.Face = face;
  data.Level = level;
  data.X = info.Region.Left;
  data.Y = info.Region.Top;
  data.Z = info.Region.Front;
  data.Width = info.Region.Right - info.Region.Left;
  data.Height = info.Region.Bottom - info.Region.Top;
  data.Depth = info.Region.Back - info.Region.Front;
  D3D9CopyFromLocked(data, format, info.Data, info.RowPitch, info.SlicePitch);

  {
    USE_SCRATCH_SERIALISER();

    switch(obj->GetResourceType())
    {
#define RECORD_UNLOCK(type, chunk, iface)                                       \
  case type:                                                                    \
  {                                                                             \
    SCOPED_SERIALISE_CHUNK(D3D9Chunk::chunk, data.Data.size() + 256);           \
    Serialise_Unlock(ser, (iface *)obj->GetWrappedUnknown(), info.Flags, data); \
    m_FrameRecord->AddChunk(scope.Get());                                       \
    break;                                                                      \
  }
      RECORD_UNLOCK(Resource_VertexBuffer, VertexBuffer_Unlock, IDirect3DVertexBuffer9);
      RECORD_UNLOCK(Resource_IndexBuffer, IndexBuffer_Unlock, IDirect3DIndexBuffer9);
      RECORD_UNLOCK(Resource_Texture, Texture_UnlockRect, IDirect3DTexture9);
      RECORD_UNLOCK(Resource_CubeTexture, CubeTexture_UnlockRect, IDirect3DCubeTexture9);
      RECORD_UNLOCK(Resource_VolumeTexture, VolumeTexture_UnlockBox, IDirect3DVolumeTexture9);
      RECORD_UNLOCK(Resource_Surface, Surface_UnlockRect, IDirect3DSurface9);
      RECORD_UNLOCK(Resource_Volume, Volume_UnlockBox, IDirect3DVolume9);
#undef RECORD_UNLOCK
      default: return;
    }
  }

  MarkReferenced(obj->GetWrappedUnknown(), eFrameRef_PartialWrite);
}

void WrappedD3DDevice9::OnAddDirtyRegion(WrappedD3D9Object *obj, UINT face, const D3DBOX *box)
{
  SCOPED_LOCK(m_D3DLock);

  if(face < 6)
    obj->GetContents().Dirty[face].Add(box);

  if(!IsActiveCapturing(m_State))
    return;

  USE_SCRATCH_SERIALISER();

  switch(obj->GetResourceType())
  {
    case Resource_Texture:
    {
      RECT rect;
      const RECT *pDirtyRect = box ? &rect : NULL;
      if(box)
        rect = {(LONG)box->Left, (LONG)box->Top, (LONG)box->Right, (LONG)box->Bottom};
      SCOPED_SERIALISE_CHUNK(D3D9Chunk::Texture_AddDirtyRect);
      Serialise_Texture_AddDirtyRect(ser, (IDirect3DTexture9 *)obj->GetWrappedUnknown(), pDirtyRect);
      m_FrameRecord->AddChunk(scope.Get());
      break;
    }
    case Resource_CubeTexture:
    {
      RECT rect;
      const RECT *pDirtyRect = box ? &rect : NULL;
      if(box)
        rect = {(LONG)box->Left, (LONG)box->Top, (LONG)box->Right, (LONG)box->Bottom};
      SCOPED_SERIALISE_CHUNK(D3D9Chunk::CubeTexture_AddDirtyRect);
      Serialise_CubeTexture_AddDirtyRect(ser, (IDirect3DCubeTexture9 *)obj->GetWrappedUnknown(),
                                         (D3DCUBEMAP_FACES)face, pDirtyRect);
      m_FrameRecord->AddChunk(scope.Get());
      break;
    }
    case Resource_VolumeTexture:
    {
      SCOPED_SERIALISE_CHUNK(D3D9Chunk::VolumeTexture_AddDirtyBox);
      Serialise_VolumeTexture_AddDirtyBox(ser, (IDirect3DVolumeTexture9 *)obj->GetWrappedUnknown(),
                                          box);
      m_FrameRecord->AddChunk(scope.Get());
      break;
    }
    default: return;
  }

  MarkReferenced(obj->GetWrappedUnknown(), eFrameRef_Read);
}

namespace
{
// the whole of a surface as it is now, with the face and level of the texture it belongs to
rdcstr ReadWholeSurface(IDirect3DDevice9 *dev, WrappedD3D9Object *surf, D3D9SubresourceData &data)
{
  UINT face = 0, level = 0;
  ContentsOwner(surf, face, level);
  data.Face = face;
  data.Level = level;
  return ReadSurface(dev, ((WrappedD3DSurface9 *)surf)->GetReal(), data);
}
};

void WrappedD3DDevice9::OnReleaseDC(WrappedD3D9Object *surf)
{
  if(!IsActiveCapturing(m_State))
    return;

  SCOPED_LOCK(m_D3DLock);

  D3D9SubresourceData data;
  rdcstr err = ReadWholeSurface(m_device, surf, data);
  if(!err.empty())
  {
    RDCWARN("Couldn't read %s after GDI drawing: %s", ToStr(surf->GetResourceID()).c_str(),
            err.c_str());
    return;
  }

  {
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(D3D9Chunk::Surface_ReleaseDC, data.Data.size() + 256);
    Serialise_Surface_ReleaseDC(ser, (IDirect3DSurface9 *)surf->GetWrappedUnknown(), data);
    m_FrameRecord->AddChunk(scope.Get());
  }

  MarkReferenced(surf->GetWrappedUnknown(), eFrameRef_PartialWrite);
}

void WrappedD3DDevice9::OnGetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface)
{
  WrappedD3D9Object *dst = GetWrappedObject(pDestSurface);
  if(!IsActiveCapturing(m_State) || dst == NULL)
    return;

  SCOPED_LOCK(m_D3DLock);

  // the front buffer is what's on screen, which replay can't reproduce, so keep what was copied
  D3D9SubresourceData data;
  rdcstr err = ReadWholeSurface(m_device, dst, data);
  if(!err.empty())
  {
    RDCWARN("Couldn't read GetFrontBufferData's destination: %s", err.c_str());
    return;
  }

  {
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(D3D9Chunk::GetFrontBufferData, data.Data.size() + 256);
    Serialise_GetFrontBufferData(ser, iSwapChain, pDestSurface, data);
    m_FrameRecord->AddChunk(scope.Get());
  }

  MarkReferenced(pDestSurface, eFrameRef_PartialWrite);
}

void WrappedD3DDevice9::OnSwapChainGetFrontBufferData(IDirect3DSwapChain9 *swap,
                                                      IDirect3DSurface9 *pDestSurface)
{
  WrappedD3D9Object *dst = GetWrappedObject(pDestSurface);
  if(!IsActiveCapturing(m_State) || dst == NULL)
    return;

  SCOPED_LOCK(m_D3DLock);

  D3D9SubresourceData data;
  rdcstr err = ReadWholeSurface(m_device, dst, data);
  if(!err.empty())
  {
    RDCWARN("Couldn't read GetFrontBufferData's destination: %s", err.c_str());
    return;
  }

  {
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(D3D9Chunk::SwapChain_GetFrontBufferData, data.Data.size() + 256);
    Serialise_SwapChain_GetFrontBufferData(ser, swap, pDestSurface, data);
    m_FrameRecord->AddChunk(scope.Get());
  }

  MarkReferenced(swap, eFrameRef_Read);
  MarkReferenced(pDestSurface, eFrameRef_PartialWrite);
}

///////////////////////////////////////////////////////////////////////////////
// serialisation of the chunks above

template <typename SerialiserType, typename Iface>
bool WrappedD3DDevice9::Serialise_Unlock(SerialiserType &ser, Iface *pResource, DWORD Flags,
                                         D3D9SubresourceData &Contents)
{
  SERIALISE_ELEMENT(pResource).Important();
  SERIALISE_ELEMENT_TYPED(uint32_t, Flags);
  SERIALISE_ELEMENT(Contents);

  SERIALISE_CHECK_READ_ERRORS();

  // replay is implemented in phase 3

  return true;
}

#define INSTANTIATE_UNLOCK(iface)                                                                \
  template bool WrappedD3DDevice9::Serialise_Unlock(ReadSerialiser &ser, iface *pResource,       \
                                                    DWORD Flags, D3D9SubresourceData &Contents); \
  template bool WrappedD3DDevice9::Serialise_Unlock(WriteSerialiser &ser, iface *pResource,      \
                                                    DWORD Flags, D3D9SubresourceData &Contents);

INSTANTIATE_UNLOCK(IDirect3DVertexBuffer9);
INSTANTIATE_UNLOCK(IDirect3DIndexBuffer9);
INSTANTIATE_UNLOCK(IDirect3DTexture9);
INSTANTIATE_UNLOCK(IDirect3DCubeTexture9);
INSTANTIATE_UNLOCK(IDirect3DVolumeTexture9);
INSTANTIATE_UNLOCK(IDirect3DSurface9);
INSTANTIATE_UNLOCK(IDirect3DVolume9);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_Surface_ReleaseDC(SerialiserType &ser, IDirect3DSurface9 *pSurface,
                                                    D3D9SubresourceData &Contents)
{
  SERIALISE_ELEMENT(pSurface).Important();
  // the whole surface after GDI drew to it
  SERIALISE_ELEMENT(Contents);

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_GetFrontBufferData(SerialiserType &ser, UINT iSwapChain,
                                                     IDirect3DSurface9 *pDestSurface,
                                                     D3D9SubresourceData &Contents)
{
  SERIALISE_ELEMENT(iSwapChain);
  SERIALISE_ELEMENT(pDestSurface).Important();
  // what was copied, since replay can't reproduce what was on screen
  SERIALISE_ELEMENT(Contents);

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_SwapChain_GetFrontBufferData(SerialiserType &ser,
                                                               IDirect3DSwapChain9 *pSwapChain,
                                                               IDirect3DSurface9 *pDestSurface,
                                                               D3D9SubresourceData &Contents)
{
  SERIALISE_ELEMENT(pSwapChain);
  SERIALISE_ELEMENT(pDestSurface).Important();
  SERIALISE_ELEMENT(Contents);

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

template bool WrappedD3DDevice9::Serialise_Surface_ReleaseDC(ReadSerialiser &ser,
                                                             IDirect3DSurface9 *pSurface,
                                                             D3D9SubresourceData &Contents);
template bool WrappedD3DDevice9::Serialise_Surface_ReleaseDC(WriteSerialiser &ser,
                                                             IDirect3DSurface9 *pSurface,
                                                             D3D9SubresourceData &Contents);
template bool WrappedD3DDevice9::Serialise_GetFrontBufferData(ReadSerialiser &ser, UINT iSwapChain,
                                                              IDirect3DSurface9 *pDestSurface,
                                                              D3D9SubresourceData &Contents);
template bool WrappedD3DDevice9::Serialise_GetFrontBufferData(WriteSerialiser &ser, UINT iSwapChain,
                                                              IDirect3DSurface9 *pDestSurface,
                                                              D3D9SubresourceData &Contents);
template bool WrappedD3DDevice9::Serialise_SwapChain_GetFrontBufferData(
    ReadSerialiser &ser, IDirect3DSwapChain9 *pSwapChain, IDirect3DSurface9 *pDestSurface,
    D3D9SubresourceData &Contents);
template bool WrappedD3DDevice9::Serialise_SwapChain_GetFrontBufferData(
    WriteSerialiser &ser, IDirect3DSwapChain9 *pSwapChain, IDirect3DSurface9 *pDestSurface,
    D3D9SubresourceData &Contents);

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_Texture_AddDirtyRect(SerialiserType &ser,
                                                       IDirect3DTexture9 *pTexture,
                                                       CONST RECT *pDirtyRect)
{
  SERIALISE_ELEMENT(pTexture);
  SERIALISE_ELEMENT_OPT(pDirtyRect).TypedAs("RECT"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_CubeTexture_AddDirtyRect(SerialiserType &ser,
                                                           IDirect3DCubeTexture9 *pCubeTexture,
                                                           D3DCUBEMAP_FACES FaceType,
                                                           CONST RECT *pDirtyRect)
{
  SERIALISE_ELEMENT(pCubeTexture);
  SERIALISE_ELEMENT(FaceType);
  SERIALISE_ELEMENT_OPT(pDirtyRect).TypedAs("RECT"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

template <typename SerialiserType>
bool WrappedD3DDevice9::Serialise_VolumeTexture_AddDirtyBox(SerialiserType &ser,
                                                            IDirect3DVolumeTexture9 *pVolumeTexture,
                                                            CONST D3DBOX *pDirtyBox)
{
  SERIALISE_ELEMENT(pVolumeTexture);
  SERIALISE_ELEMENT_OPT(pDirtyBox);

  SERIALISE_CHECK_READ_ERRORS();

  return true;
}

INSTANTIATE_FUNCTION_SERIALISED(void, WrappedD3DDevice9, Texture_AddDirtyRect,
                                IDirect3DTexture9 *pTexture, CONST RECT *pDirtyRect);
INSTANTIATE_FUNCTION_SERIALISED(void, WrappedD3DDevice9, CubeTexture_AddDirtyRect,
                                IDirect3DCubeTexture9 *pCubeTexture, D3DCUBEMAP_FACES FaceType,
                                CONST RECT *pDirtyRect);
INSTANTIATE_FUNCTION_SERIALISED(void, WrappedD3DDevice9, VolumeTexture_AddDirtyBox,
                                IDirect3DVolumeTexture9 *pVolumeTexture, CONST D3DBOX *pDirtyBox);

///////////////////////////////////////////////////////////////////////////////
// the wrappers' side

bool D3D9IsActiveCapturing(WrappedD3DDevice9 *device)
{
  return IsActiveCapturing(device->GetState());
}

void D3D9PrepareForWrite(WrappedD3D9Object *obj)
{
  obj->GetWrappedDevice()->PrepareForWrite(obj);
}

void D3D9BeforeLock(WrappedD3D9Object *obj, UINT face, UINT level, const D3DBOX *region, DWORD flags)
{
  obj->GetWrappedDevice()->OnLock(obj, face, level, region, flags);
}

void D3D9AfterLock(WrappedD3D9Object *obj, UINT face, UINT level, const D3DBOX *region, DWORD flags,
                   void *data, UINT rowPitch, UINT slicePitch)
{
  obj->GetWrappedDevice()->OnLocked(obj, face, level, region, flags, data, rowPitch, slicePitch);
}

void D3D9BeforeUnlock(WrappedD3D9Object *obj, UINT face, UINT level)
{
  obj->GetWrappedDevice()->OnUnlock(obj, face, level);
}

void D3D9AddDirtyRegion(WrappedD3D9Object *obj, UINT face, const D3DBOX *box)
{
  obj->GetWrappedDevice()->OnAddDirtyRegion(obj, face, box);
}

HRESULT WrappedD3DSurface9::LockRect(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags)
{
  D3DBOX box;
  const D3DBOX *region = D3D9RectToBox(pRect, box);
  D3D9BeforeLock(this, 0, 0, region, Flags);
  HRESULT hr = m_pReal->LockRect(pLockedRect, pRect, Flags);
  if(SUCCEEDED(hr) && pLockedRect)
    D3D9AfterLock(this, 0, 0, region, Flags, pLockedRect->pBits, (UINT)pLockedRect->Pitch, 0);
  return hr;
}

HRESULT WrappedD3DSurface9::UnlockRect()
{
  D3D9BeforeUnlock(this, 0, 0);
  return m_pReal->UnlockRect();
}

HRESULT WrappedD3DSurface9::ReleaseDC(HDC hdc)
{
  HRESULT hr = m_pReal->ReleaseDC(hdc);
  if(SUCCEEDED(hr))
    m_pDevice->OnReleaseDC(this);
  return hr;
}

HRESULT WrappedD3DVolume9::LockBox(D3DLOCKED_BOX *pLockedVolume, CONST D3DBOX *pBox, DWORD Flags)
{
  D3D9BeforeLock(this, 0, 0, pBox, Flags);
  HRESULT hr = m_pReal->LockBox(pLockedVolume, pBox, Flags);
  if(SUCCEEDED(hr) && pLockedVolume)
    D3D9AfterLock(this, 0, 0, pBox, Flags, pLockedVolume->pBits, (UINT)pLockedVolume->RowPitch,
                  (UINT)pLockedVolume->SlicePitch);
  return hr;
}

HRESULT WrappedD3DVolume9::UnlockBox()
{
  D3D9BeforeUnlock(this, 0, 0);
  return m_pReal->UnlockBox();
}

HRESULT WrappedD3DTexture9::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect,
                                     DWORD Flags)
{
  D3DBOX box;
  const D3DBOX *region = D3D9RectToBox(pRect, box);
  D3D9BeforeLock(this, 0, Level, region, Flags);
  HRESULT hr = m_pReal->LockRect(Level, pLockedRect, pRect, Flags);
  if(SUCCEEDED(hr) && pLockedRect)
    D3D9AfterLock(this, 0, Level, region, Flags, pLockedRect->pBits, (UINT)pLockedRect->Pitch, 0);
  return hr;
}

HRESULT WrappedD3DTexture9::UnlockRect(UINT Level)
{
  D3D9BeforeUnlock(this, 0, Level);
  return m_pReal->UnlockRect(Level);
}

HRESULT WrappedD3DTexture9::AddDirtyRect(CONST RECT *pDirtyRect)
{
  HRESULT hr = m_pReal->AddDirtyRect(pDirtyRect);
  if(SUCCEEDED(hr))
  {
    D3DBOX box;
    D3D9AddDirtyRegion(this, 0, D3D9RectToBox(pDirtyRect, box));
  }
  return hr;
}

HRESULT WrappedD3DCubeTexture9::LockRect(D3DCUBEMAP_FACES FaceType, UINT Level,
                                         D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags)
{
  D3DBOX box;
  const D3DBOX *region = D3D9RectToBox(pRect, box);
  D3D9BeforeLock(this, (UINT)FaceType, Level, region, Flags);
  HRESULT hr = m_pReal->LockRect(FaceType, Level, pLockedRect, pRect, Flags);
  if(SUCCEEDED(hr) && pLockedRect)
    D3D9AfterLock(this, (UINT)FaceType, Level, region, Flags, pLockedRect->pBits,
                  (UINT)pLockedRect->Pitch, 0);
  return hr;
}

HRESULT WrappedD3DCubeTexture9::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level)
{
  D3D9BeforeUnlock(this, (UINT)FaceType, Level);
  return m_pReal->UnlockRect(FaceType, Level);
}

HRESULT WrappedD3DCubeTexture9::AddDirtyRect(D3DCUBEMAP_FACES FaceType, CONST RECT *pDirtyRect)
{
  HRESULT hr = m_pReal->AddDirtyRect(FaceType, pDirtyRect);
  if(SUCCEEDED(hr))
  {
    D3DBOX box;
    D3D9AddDirtyRegion(this, (UINT)FaceType, D3D9RectToBox(pDirtyRect, box));
  }
  return hr;
}

HRESULT WrappedD3DVolumeTexture9::LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume,
                                          CONST D3DBOX *pBox, DWORD Flags)
{
  D3D9BeforeLock(this, 0, Level, pBox, Flags);
  HRESULT hr = m_pReal->LockBox(Level, pLockedVolume, pBox, Flags);
  if(SUCCEEDED(hr) && pLockedVolume)
    D3D9AfterLock(this, 0, Level, pBox, Flags, pLockedVolume->pBits, (UINT)pLockedVolume->RowPitch,
                  (UINT)pLockedVolume->SlicePitch);
  return hr;
}

HRESULT WrappedD3DVolumeTexture9::UnlockBox(UINT Level)
{
  D3D9BeforeUnlock(this, 0, Level);
  return m_pReal->UnlockBox(Level);
}

HRESULT WrappedD3DVolumeTexture9::AddDirtyBox(CONST D3DBOX *pDirtyBox)
{
  HRESULT hr = m_pReal->AddDirtyBox(pDirtyBox);
  if(SUCCEEDED(hr))
    D3D9AddDirtyRegion(this, 0, pDirtyBox);
  return hr;
}
