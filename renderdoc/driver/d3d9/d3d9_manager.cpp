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

// Initial state handling is implemented with capture support (phase 2).

bool D3D9ResourceManager::Prepare_InitialState(IUnknown *res)
{
  return false;
}

uint64_t D3D9ResourceManager::GetSize_InitialState(ResourceId id, const D3D9InitialContents &data)
{
  return 128;
}

bool D3D9ResourceManager::Serialise_InitialState(WriteSerialiser &ser, ResourceId id,
                                                 D3D9ResourceRecord *record,
                                                 const D3D9InitialContents *data)
{
  return false;
}

void D3D9ResourceManager::Create_InitialState(ResourceId id, IUnknown *live, bool hasData)
{
}

void D3D9ResourceManager::Apply_InitialState(IUnknown *live, D3D9InitialContents &data)
{
}
