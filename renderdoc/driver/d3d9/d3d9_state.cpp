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

#include "d3d9_state.h"
#include "d3d9_resources.h"

void D3D9IntAddRef(IUnknown *wrapped)
{
  WrappedD3D9Object *obj = GetWrappedObject(wrapped);
  if(obj)
    obj->IntAddRef();
  else if(wrapped)
    RDCERR("Binding unwrapped D3D9 object %p", wrapped);
}

void D3D9IntRelease(IUnknown *wrapped)
{
  WrappedD3D9Object *obj = GetWrappedObject(wrapped);
  if(obj)
    obj->IntRelease();
}

D3D9BindingMask D3D9BindingMask::All()
{
  D3D9BindingMask ret;
  ret.textures = (1U << D3D9_NUM_TEXTURE_SLOTS) - 1;
  ret.streams = (1U << D3D9_NUM_STREAMS) - 1;
  ret.indices = ret.vs = ret.ps = ret.decl = true;
  return ret;
}

D3D9BindingMask D3D9BindingMask::ForStateBlockType(D3DSTATEBLOCKTYPE type)
{
  // Object bindings captured by each predefined state block type, matching the runtime as
  // implemented by Wine and DXVK: everything for D3DSBT_ALL, the vertex shader and declaration for
  // D3DSBT_VERTEXSTATE, and the pixel shader for D3DSBT_PIXELSTATE. Textures, streams and the index
  // buffer are only in D3DSBT_ALL.
  D3D9BindingMask ret;

  switch(type)
  {
    case D3DSBT_ALL: ret = All(); break;
    case D3DSBT_VERTEXSTATE:
      ret.vs = true;
      ret.decl = true;
      break;
    case D3DSBT_PIXELSTATE: ret.ps = true; break;
    default: break;
  }

  return ret;
}

void D3D9ObjectBindings::SetTexture(uint32_t slot, IDirect3DBaseTexture9 *tex)
{
  if(slot < D3D9_NUM_TEXTURE_SLOTS)
    D3D9SetBound(textures[slot], tex);
}

void D3D9ObjectBindings::SetStream(uint32_t stream, IDirect3DVertexBuffer9 *buffer, UINT offset,
                                   UINT stride)
{
  if(stream >= D3D9_NUM_STREAMS)
    return;

  D3D9SetBound(streams[stream].buffer, buffer);
  streams[stream].offset = offset;
  streams[stream].stride = stride;
}

void D3D9ObjectBindings::SetIndices(IDirect3DIndexBuffer9 *ib)
{
  D3D9SetBound(indices, ib);
}

void D3D9ObjectBindings::SetVertexShader(IDirect3DVertexShader9 *shader)
{
  D3D9SetBound(vs, shader);
}

void D3D9ObjectBindings::SetPixelShader(IDirect3DPixelShader9 *shader)
{
  D3D9SetBound(ps, shader);
}

void D3D9ObjectBindings::SetDecl(IDirect3DVertexDeclaration9 *d, bool fromFVF)
{
  D3D9SetBound(decl, fromFVF ? NULL : d);
  declFromFVF = fromFVF;
}

void D3D9ObjectBindings::CopyFrom(const D3D9ObjectBindings &src, const D3D9BindingMask &mask)
{
  if(&src == this)
    return;

  for(uint32_t i = 0; i < D3D9_NUM_TEXTURE_SLOTS; i++)
    if(mask.textures & (1U << i))
      SetTexture(i, src.textures[i]);

  for(uint32_t i = 0; i < D3D9_NUM_STREAMS; i++)
    if(mask.streams & (1U << i))
      SetStream(i, src.streams[i].buffer, src.streams[i].offset, src.streams[i].stride);

  if(mask.indices)
    SetIndices(src.indices);
  if(mask.vs)
    SetVertexShader(src.vs);
  if(mask.ps)
    SetPixelShader(src.ps);
  if(mask.decl)
    SetDecl(src.decl, src.declFromFVF);
}

void D3D9ObjectBindings::ReleaseAll()
{
  for(uint32_t i = 0; i < D3D9_NUM_TEXTURE_SLOTS; i++)
    SetTexture(i, NULL);
  for(uint32_t i = 0; i < D3D9_NUM_STREAMS; i++)
    SetStream(i, NULL, 0, 0);
  SetIndices(NULL);
  SetVertexShader(NULL);
  SetPixelShader(NULL);
  SetDecl(NULL, false);
}
