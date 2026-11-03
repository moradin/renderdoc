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

#include "common/wrapped_pool.h"
#include "core/core.h"
#include "core/resource_manager.h"
#include "serialise/serialiser.h"
#include "d3d9_common.h"

enum D3D9ResourceType
{
  Resource_Unknown = 0,
  Resource_SwapChain,
  Resource_Surface,
  Resource_Volume,
  Resource_Texture,
  Resource_CubeTexture,
  Resource_VolumeTexture,
  Resource_VertexBuffer,
  Resource_IndexBuffer,
  Resource_VertexDeclaration,
  Resource_VertexShader,
  Resource_PixelShader,
  Resource_StateBlock,
  Resource_Query,
};

struct D3D9ResourceRecord : public ResourceRecord
{
  enum
  {
    NullResource = NULL
  };

  D3D9ResourceRecord(ResourceId id) : ResourceRecord(id, true) {}
};

// initial contents are captured in phase 2, for now this is empty
struct D3D9InitialContents
{
  template <typename Configuration>
  void Free(ResourceManager<Configuration> *rm)
  {
  }
};

struct D3D9ResourceManagerConfiguration
{
  typedef IUnknown *WrappedResourceType;
  typedef IUnknown *RealResourceType;
  typedef D3D9ResourceRecord RecordType;
  typedef D3D9InitialContents InitialContentData;
};

class D3D9ResourceManager : public ResourceManager<D3D9ResourceManagerConfiguration>
{
public:
  D3D9ResourceManager(CaptureState &state, WrappedD3DDevice9 *dev)
      : ResourceManager(state), m_Device(dev)
  {
  }

private:
  ResourceId GetID(IUnknown *res);

  bool ResourceTypeRelease(IUnknown *res);

  bool Prepare_InitialState(IUnknown *res);
  uint64_t GetSize_InitialState(ResourceId id, const D3D9InitialContents &data);
  bool Serialise_InitialState(WriteSerialiser &ser, ResourceId id, D3D9ResourceRecord *record,
                              const D3D9InitialContents *data);
  void Create_InitialState(ResourceId id, IUnknown *live, bool hasData);
  void Apply_InitialState(IUnknown *live, D3D9InitialContents &data);

  WrappedD3DDevice9 *m_Device;
};
