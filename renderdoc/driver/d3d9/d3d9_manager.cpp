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

#include "d3d9_manager.h"
#include "d3d9_device.h"
#include "d3d9_resources.h"

ResourceId D3D9ResourceManager::GetID(IUnknown *res)
{
  WrappedD3D9Object *obj = GetWrappedObject(res);
  return obj ? obj->GetResourceID() : ResourceId();
}

bool D3D9ResourceManager::ResourceTypeRelease(IUnknown *res)
{
  if(res)
    res->Release();

  return true;
}

// see d3d9_contents.h for how initial contents are read

bool D3D9ResourceManager::IsResourceTrackedForPersistency(IUnknown *const &res)
{
  // reading is postponed for every object with contents, which the frame then only reads if it
  // references them. Objects the application has released but that are still bound are the
  // exception: they can die mid-frame without passing through OnLastExternalRelease, so they're
  // read when the frame starts.
  WrappedD3D9Object *obj = GetWrappedObject(res);
  return obj && obj->GetExtRefCount() > 0;
}

bool D3D9ResourceManager::Prepare_InitialState(IUnknown *res)
{
  WrappedD3D9Object *obj = GetWrappedObject(res);
  if(obj == NULL)
    return false;

  D3D9InitialContentsData *data = new D3D9InitialContentsData;
  m_Device->ReadContents(obj, *data);

  SetInitialContents(obj->GetResourceID(), D3D9InitialContents(data));

  return true;
}

uint64_t D3D9ResourceManager::GetSize_InitialState(ResourceId id, const D3D9InitialContents &data)
{
  uint64_t ret = 128;

  if(data.data)
  {
    ret += data.data->Note.size();
    for(const D3D9SubresourceData &sub : data.data->Subresources)
      ret += 64 + sub.Data.size() + WriteSerialiser::GetChunkAlignment();
    // a generous estimate of a whole device state
    ret += data.data->StateBlock.size() * 256 * 1024;
    for(const D3D9DirtyRegion &dirty : data.data->Dirty)
      ret += 16 + dirty.Boxes.size() * sizeof(D3DBOX);
  }

  return ret;
}

template <typename SerialiserType>
bool D3D9ResourceManager::DoSerialise_InitialState(SerialiserType &ser, ResourceId id,
                                                   D3D9InitialContentsData &Contents)
{
  SERIALISE_ELEMENT(id).TypedAs("IDirect3DResource9 *"_lit);
  SERIALISE_ELEMENT(Contents);

  SERIALISE_CHECK_READ_ERRORS();

  // kept to be written back whenever the frame is replayed from the start
  if(ser.IsReading() && IsReplayMode(m_State) && HasResource(id))
  {
    if(!Contents.Note.empty())
      RDCLOG("Initial contents of %s: %s", ToStr(id).c_str(), Contents.Note.c_str());
    // read once, while the capture loads
    m_Device->CheckIgnoredLOD(id, Contents.LOD, 0);
    SetInitialContents(id, D3D9InitialContents(new D3D9InitialContentsData(std::move(Contents))));
  }

  return true;
}

bool D3D9ResourceManager::Serialise_InitialState(WriteSerialiser &ser, ResourceId id,
                                                 D3D9ResourceRecord *record,
                                                 const D3D9InitialContents *data)
{
  if(data && data->data)
    return DoSerialise_InitialState(ser, id, *data->data);

  // referenced but never read, which shouldn't happen
  D3D9InitialContentsData empty;
  empty.Note = "Contents weren't read";
  return DoSerialise_InitialState(ser, id, empty);
}

bool D3D9ResourceManager::ReadInitialState(ReadSerialiser &ser)
{
  D3D9InitialContentsData contents;
  return DoSerialise_InitialState(ser, ResourceId(), contents);
}

void D3D9ResourceManager::Create_InitialState(ResourceId id, IUnknown *live, bool hasData)
{
  // objects whose contents weren't stored keep whatever they have
}

void D3D9ResourceManager::Apply_InitialState(IUnknown *live, D3D9InitialContents &data)
{
  WrappedD3D9Object *obj = GetWrappedObject(live);
  if(obj == NULL || data.data == NULL)
    return;

  if(obj->GetResourceType() == Resource_StateBlock)
  {
    if(!data.data->StateBlock.empty())
      m_Device->ApplyStateBlockContents((WrappedD3DStateBlock9 *)obj, data.data->StateBlock[0]);
    return;
  }

  for(const D3D9SubresourceData &sub : data.data->Subresources)
    m_Device->WriteContents(obj, sub);

  // after the contents, whose writes made the texture dirty
  if(!data.data->Dirty.empty())
    m_Device->SetDirtyRegions(obj, data.data->Dirty);

  // undoes SetLOD calls in the frame too. Only D3DPOOL_MANAGED textures use it, so it does
  // nothing on a D3D9Ex replay device, which has none.
  if(IDirect3DBaseTexture9 *base = GetRealBaseTexture(obj))
    base->SetLOD(data.data->LOD);
}
