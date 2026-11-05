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

// every render state, except D3DRS_DEBUGMONITORTOKEN which only controls the debug runtime
static const D3DRENDERSTATETYPE RenderStates[] = {
    D3DRS_ZENABLE,
    D3DRS_FILLMODE,
    D3DRS_SHADEMODE,
    D3DRS_ZWRITEENABLE,
    D3DRS_ALPHATESTENABLE,
    D3DRS_LASTPIXEL,
    D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,
    D3DRS_CULLMODE,
    D3DRS_ZFUNC,
    D3DRS_ALPHAREF,
    D3DRS_ALPHAFUNC,
    D3DRS_DITHERENABLE,
    D3DRS_ALPHABLENDENABLE,
    D3DRS_FOGENABLE,
    D3DRS_SPECULARENABLE,
    D3DRS_FOGCOLOR,
    D3DRS_FOGTABLEMODE,
    D3DRS_FOGSTART,
    D3DRS_FOGEND,
    D3DRS_FOGDENSITY,
    D3DRS_RANGEFOGENABLE,
    D3DRS_STENCILENABLE,
    D3DRS_STENCILFAIL,
    D3DRS_STENCILZFAIL,
    D3DRS_STENCILPASS,
    D3DRS_STENCILFUNC,
    D3DRS_STENCILREF,
    D3DRS_STENCILMASK,
    D3DRS_STENCILWRITEMASK,
    D3DRS_TEXTUREFACTOR,
    D3DRS_WRAP0,
    D3DRS_WRAP1,
    D3DRS_WRAP2,
    D3DRS_WRAP3,
    D3DRS_WRAP4,
    D3DRS_WRAP5,
    D3DRS_WRAP6,
    D3DRS_WRAP7,
    D3DRS_CLIPPING,
    D3DRS_LIGHTING,
    D3DRS_AMBIENT,
    D3DRS_FOGVERTEXMODE,
    D3DRS_COLORVERTEX,
    D3DRS_LOCALVIEWER,
    D3DRS_NORMALIZENORMALS,
    D3DRS_DIFFUSEMATERIALSOURCE,
    D3DRS_SPECULARMATERIALSOURCE,
    D3DRS_AMBIENTMATERIALSOURCE,
    D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_VERTEXBLEND,
    D3DRS_CLIPPLANEENABLE,
    D3DRS_POINTSIZE,
    D3DRS_POINTSIZE_MIN,
    D3DRS_POINTSPRITEENABLE,
    D3DRS_POINTSCALEENABLE,
    D3DRS_POINTSCALE_A,
    D3DRS_POINTSCALE_B,
    D3DRS_POINTSCALE_C,
    D3DRS_MULTISAMPLEANTIALIAS,
    D3DRS_MULTISAMPLEMASK,
    D3DRS_PATCHEDGESTYLE,
    D3DRS_POINTSIZE_MAX,
    D3DRS_INDEXEDVERTEXBLENDENABLE,
    D3DRS_COLORWRITEENABLE,
    D3DRS_TWEENFACTOR,
    D3DRS_BLENDOP,
    D3DRS_POSITIONDEGREE,
    D3DRS_NORMALDEGREE,
    D3DRS_SCISSORTESTENABLE,
    D3DRS_SLOPESCALEDEPTHBIAS,
    D3DRS_ANTIALIASEDLINEENABLE,
    D3DRS_MINTESSELLATIONLEVEL,
    D3DRS_MAXTESSELLATIONLEVEL,
    D3DRS_ADAPTIVETESS_X,
    D3DRS_ADAPTIVETESS_Y,
    D3DRS_ADAPTIVETESS_Z,
    D3DRS_ADAPTIVETESS_W,
    D3DRS_ENABLEADAPTIVETESSELLATION,
    D3DRS_TWOSIDEDSTENCILMODE,
    D3DRS_CCW_STENCILFAIL,
    D3DRS_CCW_STENCILZFAIL,
    D3DRS_CCW_STENCILPASS,
    D3DRS_CCW_STENCILFUNC,
    D3DRS_COLORWRITEENABLE1,
    D3DRS_COLORWRITEENABLE2,
    D3DRS_COLORWRITEENABLE3,
    D3DRS_BLENDFACTOR,
    D3DRS_SRGBWRITEENABLE,
    D3DRS_DEPTHBIAS,
    D3DRS_WRAP8,
    D3DRS_WRAP9,
    D3DRS_WRAP10,
    D3DRS_WRAP11,
    D3DRS_WRAP12,
    D3DRS_WRAP13,
    D3DRS_WRAP14,
    D3DRS_WRAP15,
    D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_SRCBLENDALPHA,
    D3DRS_DESTBLENDALPHA,
    D3DRS_BLENDOPALPHA,
};

static const D3DSAMPLERSTATETYPE SamplerStates[] = {
    D3DSAMP_ADDRESSU,    D3DSAMP_ADDRESSV,      D3DSAMP_ADDRESSW,    D3DSAMP_BORDERCOLOR,
    D3DSAMP_MAGFILTER,   D3DSAMP_MINFILTER,     D3DSAMP_MIPFILTER,   D3DSAMP_MIPMAPLODBIAS,
    D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY, D3DSAMP_SRGBTEXTURE, D3DSAMP_ELEMENTINDEX,
    D3DSAMP_DMAPOFFSET,
};

static const D3DTEXTURESTAGESTATETYPE TextureStageStates[] = {
    D3DTSS_COLOROP,        D3DTSS_COLORARG1,
    D3DTSS_COLORARG2,      D3DTSS_ALPHAOP,
    D3DTSS_ALPHAARG1,      D3DTSS_ALPHAARG2,
    D3DTSS_BUMPENVMAT00,   D3DTSS_BUMPENVMAT01,
    D3DTSS_BUMPENVMAT10,   D3DTSS_BUMPENVMAT11,
    D3DTSS_TEXCOORDINDEX,  D3DTSS_BUMPENVLSCALE,
    D3DTSS_BUMPENVLOFFSET, D3DTSS_TEXTURETRANSFORMFLAGS,
    D3DTSS_COLORARG0,      D3DTSS_ALPHAARG0,
    D3DTSS_RESULTARG,      D3DTSS_CONSTANT,
};

// texture stages, D3DDP_MAXTEXCOORD of them
static const DWORD NumTextureStages = 8;
static const DWORD NumWorldMatrices = 256;

static bool IsIdentity(const D3DMATRIX &m)
{
  for(int r = 0; r < 4; r++)
    for(int c = 0; c < 4; c++)
      if(m.m[r][c] != (r == c ? 1.0f : 0.0f))
        return false;
  return true;
}

// The number of shader constants depends on the shader model, the vertex processing mode and the
// driver, and a Get that asks for more than there are fails. Try each count in turn, largest first.
template <typename T, typename GetFunc>
static void FetchConstants(rdcarray<T> &out, std::initializer_list<UINT> counts, GetFunc get)
{
  for(UINT count : counts)
  {
    if(count == 0)
      continue;
    out.resize(count);
    if(SUCCEEDED(get(out.data(), count)))
      return;
  }
  out.clear();
}

void D3D9FetchDeviceState(IDirect3DDevice9 *dev, DWORD behaviorFlags, const D3D9IndexSet &lights,
                          const D3D9IndexSet &palettes, D3D9DeviceState &state)
{
  state = D3D9DeviceState();

  D3DCAPS9 caps = {};
  dev->GetDeviceCaps(&caps);

  for(D3DRENDERSTATETYPE rs : RenderStates)
  {
    D3D9RenderStateValue v;
    v.State = rs;
    if(SUCCEEDED(dev->GetRenderState(rs, &v.Value)))
      state.RenderStates.push_back(v);
  }

  for(uint32_t slot = 0; slot < D3D9_NUM_TEXTURE_SLOTS; slot++)
  {
    D3D9SamplerStates s;
    s.Sampler = D3D9SlotToSampler(slot);
    for(D3DSAMPLERSTATETYPE ss : SamplerStates)
    {
      D3D9SamplerStateValue v;
      v.State = ss;
      if(SUCCEEDED(dev->GetSamplerState(s.Sampler, ss, &v.Value)))
        s.States.push_back(v);
    }
    state.SamplerStates.push_back(s);
  }

  for(DWORD stage = 0; stage < NumTextureStages; stage++)
  {
    D3D9TextureStageStates s;
    s.Stage = stage;
    for(D3DTEXTURESTAGESTATETYPE tss : TextureStageStates)
    {
      D3D9TextureStageStateValue v;
      v.State = tss;
      if(SUCCEEDED(dev->GetTextureStageState(stage, tss, &v.Value)))
        s.States.push_back(v);
    }
    state.TextureStageStates.push_back(s);
  }

  {
    rdcarray<D3DTRANSFORMSTATETYPE> transforms = {D3DTS_VIEW, D3DTS_PROJECTION};
    for(DWORD i = 0; i < NumTextureStages; i++)
      transforms.push_back(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + i));
    for(DWORD i = 0; i < NumWorldMatrices; i++)
      transforms.push_back(D3DTRANSFORMSTATETYPE(D3DTS_WORLDMATRIX(i)));

    for(D3DTRANSFORMSTATETYPE ts : transforms)
    {
      D3D9TransformValue v;
      v.State = ts;
      if(FAILED(dev->GetTransform(ts, &v.Matrix)))
        continue;
      if(ts > D3DTS_WORLD && IsIdentity(v.Matrix))
        continue;
      state.Transforms.push_back(v);
    }
  }

  dev->GetViewport(&state.Viewport);
  dev->GetScissorRect(&state.ScissorRect);
  dev->GetMaterial(&state.Material);

  for(uint32_t idx : lights.Indices())
  {
    D3D9LightState l;
    l.Index = idx;
    // lights that were never set fail, and Reset clears them all
    if(SUCCEEDED(dev->GetLight(idx, &l.Light)) && SUCCEEDED(dev->GetLightEnable(idx, &l.Enabled)))
      state.Lights.push_back(l);
  }

  for(DWORD i = 0; i < caps.MaxUserClipPlanes; i++)
  {
    D3D9Vec4f p;
    if(FAILED(dev->GetClipPlane(i, &p.x)))
      break;
    state.ClipPlanes.push_back(p);
  }

  // Software vertex processing has 8192 float and 2048 int and bool vertex shader constants. Mixed
  // devices have those only while in software mode, so try the hardware count after.
  const bool softwareVP =
      (behaviorFlags & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)) != 0;
  const UINT swFloat = softwareVP ? 8192 : 0, swIntBool = softwareVP ? 2048 : 0;

  D3D9ShaderConstants &vs = state.VertexShaderConstants;
  FetchConstants(vs.Float, {swFloat, (UINT)caps.MaxVertexShaderConst, 256U},
                 [dev](D3D9Vec4f *data, UINT count) {
                   return dev->GetVertexShaderConstantF(0, &data->x, count);
                 });
  FetchConstants(vs.Int, {swIntBool, 16U}, [dev](D3D9Vec4i *data, UINT count) {
    return dev->GetVertexShaderConstantI(0, (int *)&data->x, count);
  });
  FetchConstants(vs.Bool, {swIntBool, 16U}, [dev](BOOL *data, UINT count) {
    return dev->GetVertexShaderConstantB(0, data, count);
  });

  // ps_3_0 has 224 float constants, ps_2_0 32 and ps_1_x 8
  D3D9ShaderConstants &ps = state.PixelShaderConstants;
  FetchConstants(ps.Float, {224U, 32U, 8U}, [dev](D3D9Vec4f *data, UINT count) {
    return dev->GetPixelShaderConstantF(0, &data->x, count);
  });
  FetchConstants(ps.Int, {16U}, [dev](D3D9Vec4i *data, UINT count) {
    return dev->GetPixelShaderConstantI(0, (int *)&data->x, count);
  });
  FetchConstants(ps.Bool, {16U}, [dev](BOOL *data, UINT count) {
    return dev->GetPixelShaderConstantB(0, data, count);
  });

  dev->GetFVF(&state.FVF);

  for(UINT i = 0; i < D3D9_NUM_STREAMS; i++)
  {
    UINT freq = 1;
    dev->GetStreamSourceFreq(i, &freq);
    state.StreamFrequencies.push_back(freq);
  }

  state.NPatchMode = dev->GetNPatchMode();
  state.SoftwareVertexProcessing = dev->GetSoftwareVertexProcessing();
  dev->GetCurrentTexturePalette(&state.CurrentTexturePalette);

  for(uint32_t idx : palettes.Indices())
  {
    D3D9Palette p;
    p.Number = idx;
    if(SUCCEEDED(dev->GetPaletteEntries(idx, p.Entries.data())))
      state.Palettes.push_back(p);
  }

  dev->GetClipStatus(&state.ClipStatus);
}

namespace
{
template <typename T>
bool SameBytes(const T &a, const T &b)
{
  return memcmp(&a, &b, sizeof(T)) == 0;
}

template <typename T>
bool SameArray(const rdcarray<T> &a, const rdcarray<T> &b)
{
  return a.size() == b.size() && (a.empty() || memcmp(a.data(), b.data(), a.byteSize()) == 0);
}

// states lists are keyed: find the value for a key in the other list
template <typename T, typename Key>
const T *FindState(const rdcarray<T> &list, Key key)
{
  for(const T &v : list)
    if(v.State == key)
      return &v;
  return NULL;
}
};

void D3D9RestoreDeviceState(IDirect3DDevice9 *dev, const D3D9DeviceState &target,
                            const D3D9DeviceState &current)
{
  for(const D3D9RenderStateValue &v : target.RenderStates)
  {
    const D3D9RenderStateValue *cur = FindState(current.RenderStates, v.State);
    if(cur == NULL || cur->Value != v.Value)
      dev->SetRenderState(v.State, v.Value);
  }

  for(const D3D9SamplerStates &s : target.SamplerStates)
  {
    const D3D9SamplerStates *curSampler = NULL;
    for(const D3D9SamplerStates &c : current.SamplerStates)
      if(c.Sampler == s.Sampler)
        curSampler = &c;

    for(const D3D9SamplerStateValue &v : s.States)
    {
      const D3D9SamplerStateValue *cur = curSampler ? FindState(curSampler->States, v.State) : NULL;
      if(cur == NULL || cur->Value != v.Value)
        dev->SetSamplerState(s.Sampler, v.State, v.Value);
    }
  }

  for(const D3D9TextureStageStates &s : target.TextureStageStates)
  {
    const D3D9TextureStageStates *curStage = NULL;
    for(const D3D9TextureStageStates &c : current.TextureStageStates)
      if(c.Stage == s.Stage)
        curStage = &c;

    for(const D3D9TextureStageStateValue &v : s.States)
    {
      const D3D9TextureStageStateValue *cur = curStage ? FindState(curStage->States, v.State) : NULL;
      if(cur == NULL || cur->Value != v.Value)
        dev->SetTextureStageState(s.Stage, v.State, v.Value);
    }
  }

  // world matrices that are the identity are left out of both lists
  for(const D3D9TransformValue &v : target.Transforms)
  {
    const D3D9TransformValue *cur = FindState(current.Transforms, v.State);
    if(cur == NULL || !SameBytes(cur->Matrix, v.Matrix))
      dev->SetTransform(v.State, &v.Matrix);
  }
  for(const D3D9TransformValue &v : current.Transforms)
  {
    if(FindState(target.Transforms, v.State) == NULL)
    {
      D3DMATRIX identity = {};
      identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
      dev->SetTransform(v.State, &identity);
    }
  }

  if(!SameBytes(target.Viewport, current.Viewport))
    dev->SetViewport(&target.Viewport);
  if(!SameBytes(target.ScissorRect, current.ScissorRect))
    dev->SetScissorRect(&target.ScissorRect);
  if(!SameBytes(target.Material, current.Material))
    dev->SetMaterial(&target.Material);

  for(const D3D9LightState &l : target.Lights)
  {
    const D3D9LightState *cur = NULL;
    for(const D3D9LightState &c : current.Lights)
      if(c.Index == l.Index)
        cur = &c;

    if(cur == NULL || !SameBytes(cur->Light, l.Light))
      dev->SetLight(l.Index, &l.Light);
    if(cur == NULL || (cur->Enabled != 0) != (l.Enabled != 0))
      dev->LightEnable(l.Index, l.Enabled);
  }
  // a light that didn't exist in target can't be removed again, only disabled
  for(const D3D9LightState &c : current.Lights)
  {
    bool found = false;
    for(const D3D9LightState &l : target.Lights)
      found |= (l.Index == c.Index);
    if(!found && c.Enabled)
      dev->LightEnable(c.Index, FALSE);
  }

  for(size_t i = 0; i < target.ClipPlanes.size(); i++)
    if(i >= current.ClipPlanes.size() || !SameBytes(target.ClipPlanes[i], current.ClipPlanes[i]))
      dev->SetClipPlane((DWORD)i, &target.ClipPlanes[i].x);

  const D3D9ShaderConstants &tvs = target.VertexShaderConstants, &cvs = current.VertexShaderConstants;
  if(!SameArray(tvs.Float, cvs.Float) && !tvs.Float.empty())
    dev->SetVertexShaderConstantF(0, &tvs.Float[0].x, (UINT)tvs.Float.size());
  if(!SameArray(tvs.Int, cvs.Int) && !tvs.Int.empty())
    dev->SetVertexShaderConstantI(0, (const int *)&tvs.Int[0].x, (UINT)tvs.Int.size());
  if(!SameArray(tvs.Bool, cvs.Bool) && !tvs.Bool.empty())
    dev->SetVertexShaderConstantB(0, tvs.Bool.data(), (UINT)tvs.Bool.size());

  const D3D9ShaderConstants &tps = target.PixelShaderConstants, &cps = current.PixelShaderConstants;
  if(!SameArray(tps.Float, cps.Float) && !tps.Float.empty())
    dev->SetPixelShaderConstantF(0, &tps.Float[0].x, (UINT)tps.Float.size());
  if(!SameArray(tps.Int, cps.Int) && !tps.Int.empty())
    dev->SetPixelShaderConstantI(0, (const int *)&tps.Int[0].x, (UINT)tps.Int.size());
  if(!SameArray(tps.Bool, cps.Bool) && !tps.Bool.empty())
    dev->SetPixelShaderConstantB(0, tps.Bool.data(), (UINT)tps.Bool.size());

  for(size_t i = 0; i < target.StreamFrequencies.size(); i++)
    if(i >= current.StreamFrequencies.size() ||
       target.StreamFrequencies[i] != current.StreamFrequencies[i])
      dev->SetStreamSourceFreq((UINT)i, target.StreamFrequencies[i]);

  if(target.NPatchMode != current.NPatchMode)
    dev->SetNPatchMode(target.NPatchMode);
  if((target.SoftwareVertexProcessing != 0) != (current.SoftwareVertexProcessing != 0))
    dev->SetSoftwareVertexProcessing(target.SoftwareVertexProcessing);
  if(target.CurrentTexturePalette != current.CurrentTexturePalette)
    dev->SetCurrentTexturePalette(target.CurrentTexturePalette);

  for(const D3D9Palette &p : target.Palettes)
  {
    const D3D9Palette *cur = NULL;
    for(const D3D9Palette &c : current.Palettes)
      if(c.Number == p.Number)
        cur = &c;
    if(cur == NULL || memcmp(cur->Entries.data(), p.Entries.data(), sizeof(PALETTEENTRY) * 256) != 0)
      dev->SetPaletteEntries(p.Number, p.Entries.data());
  }

  if(!SameBytes(target.ClipStatus, current.ClipStatus))
    dev->SetClipStatus(&target.ClipStatus);
}
