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

// Shader debugging (step 5). A vertex is debugged on the CPU from the same snapshot of the draw
// that VS Out computes its vertices from (InitPostVSBuffers, d3d9_postvs.cpp): the vertex's inputs
// are fetched as RunPostVS fetches them, and the shader steps through the same interpreter with
// the same constants and vertex textures, so the debugged outputs are VS Out's exactly. The trace
// itself is made by D3D9Emu::VSDebugger (d3d9_vs_debug.cpp).
//
// A pixel (step 5.7) is debugged as D3D11 debugs one: its inputs come from the GPU, and the shader
// runs on the CPU for the pixel's 2x2 quad (PSDebugger, d3d9_ps_debug.cpp). D3D9 has no UAVs, so
// the event is drawn again with generated pixel shaders (BuildInputReadback) that write the
// inputs, and their dsx and dsy, to A32B32G32R32F render targets of our own, four values a pass:
// - The draw is replayed up to the event without its draw first, so depth and stencil are what the
//   draw tests against, and every pass leaves them alone (ZWRITEENABLE FALSE, the stencil ops KEEP
//   and a write mask of 0), as the overlays do. The replay controller replays the event again
//   afterwards, and the device's state is put back before returning.
// - The scissor rect is the pixel alone, inside the draw's own. The GPU still runs the quad's
//   other pixels as helpers, so the derivatives are right. An occlusion query around the first
//   pass says whether the draw wrote the pixel at all.
// - The interpreter needs the inputs of the quad's other pixels too, for dsx, dsy and lookups.
//   ps_3_0's readback reads them in the pixel itself (picking them out with vPos). ps_2_x has no
//   vPos, so the passes are also drawn at each of the quad's other pixels (outside the draw's
//   scissor rect too, as helpers are), and QuadInputs takes a neighbour's values when its
//   derivatives are the pixel's bit for bit, which means the same primitive covered it.
// - Blending, alpha test, fog and sRGB writes are off; everything else (cull, fill and shade mode,
//   depth bias, clip planes, point sprites) is the draw's, since it decides which pixels are drawn
//   and what they get.
// - Each value is copied out of its target with StretchRect into a 1x1 target, then read back.
// Lookups and transcendentals (step 5.8) are made on the GPU too, while the replay is still at the
// event: the quad runs through the interpreter once there, with GPULookups behind a PSLookupCache,
// and the debugger then steps it again from the cache alone, since the UI asks for the steps later,
// wherever the replay is by then. A lookup draws the quad's four coordinates into a 2x2 target, one
// a pixel, with the shader's own instruction (texld, texldp or texldb) and the event's sampler, so
// the GPU works out the derivatives, level and anisotropy it did for the event: no filtering,
// comparison or border rule is emulated, and the results are the GPU's bit for bit.
// With overlapping triangles in one draw this debugs the last fragment that passes the depth and
// stencil test as they were before the draw, and with alpha test or texkill one that may have been
// rejected: 5.10 picks the fragment the GPU wrote. Multisampled targets aren't supported yet.
//
// Fixed function vertex and pixel processing aren't shaders. They say so in their reflection's
// debugStatus, which the UI shows, as do pixel shaders the interpreter can't debug yet.

#include "d3d9_device.h"
#include "d3d9_postvs.h"
#include "d3d9_ps_debug.h"
#include "d3d9_resources.h"
#include "d3d9_vs_debug.h"

// in d3d9_rendertexture.cpp
void D3D9SetDebugRenderState(IDirect3DDevice9 *dev);

namespace
{
ShaderDebugTrace *NoDebug(uint32_t eventId, const rdcstr &why)
{
  RDCWARN("Can't debug a vertex at event %u: %s", eventId, why.c_str());
  return new ShaderDebugTrace;
}

ShaderDebugTrace *NoPixelDebug(uint32_t eventId, uint32_t x, uint32_t y, const rdcstr &why)
{
  RDCWARN("Can't debug pixel (%u, %u) at event %u: %s", x, y, eventId, why.c_str());
  return new ShaderDebugTrace;
}

ResourceId WrappedIDOf(IUnknown *obj)
{
  WrappedD3D9Object *wrapped = obj ? GetWrappedObject(obj) : NULL;
  return wrapped ? wrapped->GetResourceID() : ResourceId();
}

D3D9Emu::VSConstants ToConstants(const D3D9ShaderConstants &c)
{
  D3D9Emu::VSConstants ret;
  ret.floats.resize(c.Float.size());
  for(size_t i = 0; i < c.Float.size(); i++)
    ret.floats[i] = D3D9Emu::Vec4(c.Float[i].x, c.Float[i].y, c.Float[i].z, c.Float[i].w);
  ret.ints.resize(c.Int.size());
  for(size_t i = 0; i < c.Int.size(); i++)
  {
    ret.ints[i].v[0] = c.Int[i].x;
    ret.ints[i].v[1] = c.Int[i].y;
    ret.ints[i].v[2] = c.Int[i].z;
    ret.ints[i].v[3] = c.Int[i].w;
  }
  ret.bools.resize(c.Bool.size());
  for(size_t i = 0; i < c.Bool.size(); i++)
    ret.bools[i] = c.Bool[i] ? 1 : 0;
  return ret;
}

// what a pixel debugger's interpreter needs, kept alive with it
struct PixelDebugData
{
  D3D9Emu::PSEmulator emu;
  D3D9Emu::VSConstants consts;
  // the quad's lookups and transcendentals, made on the GPU when the pixel was debugged
  D3D9Emu::PSLookupCache lookups;
};

// more than any shader should need: past it, lookups fail rather than draw on
const uint32_t MaxGPULookups = 1U << 16;

// D3D9Emu::PSLookupSource on the GPU (5.8). Each lookup is a draw of BuildSampleShader's shader to
// a 2x2 A32B32G32R32F target, its pixels the quad's, with the event's textures and sampler states;
// each transcendental a draw of BuildTranscendentalShader's. It's used while the replay is at the
// event, between SaveReplayDrawState and RestoreReplayDrawState, so it sets up what it draws with
// itself and leaves the samplers and textures alone.
class GPULookups : public D3D9Emu::PSLookupSource
{
public:
  GPULookups(IDirect3DDevice9 *dev, IDirect3DVertexShader9 *vs, IDirect3DVertexDeclaration9 *decl)
      : m_Dev(dev), m_VS(vs), m_Decl(decl)
  {
  }
  ~GPULookups()
  {
    SAFE_RELEASE(m_Target);
    SAFE_RELEASE(m_Read);
    for(auto &it : m_Shaders)
      SAFE_RELEASE(it.second);
  }

  uint32_t Draws() const { return m_Draws; }

  bool SampleQuad(const D3D9Emu::PSSample &sample, D3D9Emu::Vec4 results[D3D9Emu::QuadPixels],
                  rdcstr &error) override
  {
    D3D9BC::SamplerTextureType type = sample.type;
    if(type == D3D9BC::SamplerTextureType::Unknown)
      type = BoundType(sample.sampler);

    const uint32_t key = (uint32_t(sample.kind) << 16) | (uint32_t(type) << 8) | sample.sampler;
    IDirect3DPixelShader9 *ps = Shader(
        key, [&]() { return D3D9Emu::BuildSampleShader(sample.kind, sample.sampler, type); }, error);
    if(ps == NULL)
      return false;

    D3D9Emu::Vec4 consts[D3D9Emu::QuadPixels];
    const uint32_t count = D3D9Emu::SampleShaderConstants(sample, consts);
    return Draw(ps, consts, count, results, error);
  }

  bool Transcendental(D3D9BC::Opcode op, float a, float b, D3D9Emu::Vec4 &result, rdcstr &error) override
  {
    IDirect3DPixelShader9 *ps = Shader(
        0x1000000 | uint32_t(op), [&]() { return D3D9Emu::BuildTranscendentalShader(op); }, error);
    if(ps == NULL)
      return false;

    // the same operands in every pixel
    D3D9Emu::Vec4 consts[D3D9Emu::QuadPixels];
    for(D3D9Emu::Vec4 &c : consts)
      c = D3D9Emu::Vec4(a, b, 0.0f, 0.0f);
    D3D9Emu::Vec4 out[D3D9Emu::QuadPixels];
    if(!Draw(ps, consts, D3D9Emu::QuadPixels, out, error))
      return false;
    result = out[0];
    return true;
  }

private:
  // the type of the texture bound to sampler, for ps_1_x, which doesn't declare its samplers
  D3D9BC::SamplerTextureType BoundType(uint32_t sampler)
  {
    D3D9BC::SamplerTextureType ret = D3D9BC::SamplerTextureType::Tex2D;
    IDirect3DBaseTexture9 *tex = NULL;
    m_Dev->GetTexture(sampler, &tex);
    if(tex)
    {
      if(tex->GetType() == D3DRTYPE_CUBETEXTURE)
        ret = D3D9BC::SamplerTextureType::Cube;
      else if(tex->GetType() == D3DRTYPE_VOLUMETEXTURE)
        ret = D3D9BC::SamplerTextureType::Volume;
      tex->Release();
    }
    return ret;
  }

  template <typename Build>
  IDirect3DPixelShader9 *Shader(uint32_t key, Build build, rdcstr &error)
  {
    auto it = m_Shaders.find(key);
    if(it != m_Shaders.end())
      return it->second;

    const rdcarray<uint32_t> tokens = build();
    IDirect3DPixelShader9 *ps = NULL;
    HRESULT hr = m_Dev->CreatePixelShader((const DWORD *)tokens.data(), &ps);
    if(FAILED(hr))
    {
      error = "couldn't create the shader: " + ToStr(hr);
      return NULL;
    }
    m_Shaders[key] = ps;
    return ps;
  }

  bool Draw(IDirect3DPixelShader9 *ps, const D3D9Emu::Vec4 *consts, uint32_t count,
            D3D9Emu::Vec4 out[D3D9Emu::QuadPixels], rdcstr &error)
  {
    if(m_VS == NULL || m_Decl == NULL)
    {
      error = "the GPU has no shader model 3";
      return false;
    }
    if(m_Draws >= MaxGPULookups)
    {
      error = StringFormat::Fmt("more than %u", MaxGPULookups);
      return false;
    }

    if(m_Target == NULL)
    {
      HRESULT hr = m_Dev->CreateRenderTarget(2, 2, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0,
                                             FALSE, &m_Target, NULL);
      if(SUCCEEDED(hr))
        hr = m_Dev->CreateOffscreenPlainSurface(2, 2, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM,
                                                &m_Read, NULL);
      if(FAILED(hr))
      {
        error = "couldn't create a 2x2 A32B32G32R32F render target: " + ToStr(hr);
        return false;
      }
      D3D9SetDebugRenderState(m_Dev);
    }

    // a quad over the whole 2x2 target, through the texture viewer's vertex shader
    static const float quad[4][4] = {
        {0.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, -1.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, -1.0f, 0.0f, 1.0f},
    };
    const float vsConsts[8] = {-0.5f, 0.5f, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const D3DVIEWPORT9 viewport = {0, 0, 2, 2, 0.0f, 1.0f};

    const bool ownScene = SUCCEEDED(m_Dev->BeginScene());
    m_Dev->SetRenderTarget(0, m_Target);
    m_Dev->SetDepthStencilSurface(NULL);
    m_Dev->SetViewport(&viewport);
    m_Dev->SetVertexShader(m_VS);
    m_Dev->SetVertexShaderConstantF(0, vsConsts, 2);
    m_Dev->SetVertexDeclaration(m_Decl);
    m_Dev->SetPixelShader(ps);
    m_Dev->SetPixelShaderConstantF(0, consts[0].v, count);
    HRESULT hr = m_Dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
    if(ownScene)
      m_Dev->EndScene();
    m_Draws++;

    D3DLOCKED_RECT locked = {};
    if(SUCCEEDED(hr))
      hr = m_Dev->GetRenderTargetData(m_Target, m_Read);
    if(SUCCEEDED(hr))
      hr = m_Read->LockRect(&locked, NULL, D3DLOCK_READONLY);
    if(FAILED(hr))
    {
      error = "couldn't draw it or read it back: " + ToStr(hr);
      return false;
    }
    for(uint32_t i = 0; i < D3D9Emu::QuadPixels; i++)
    {
      const byte *row = (const byte *)locked.pBits + (i >> 1) * locked.Pitch;
      memcpy(out[i].v, row + (i & 1) * sizeof(out[i].v), sizeof(out[i].v));
    }
    m_Read->UnlockRect();
    return true;
  }

  IDirect3DDevice9 *m_Dev;
  IDirect3DVertexShader9 *m_VS;
  IDirect3DVertexDeclaration9 *m_Decl;
  IDirect3DSurface9 *m_Target = NULL;
  IDirect3DSurface9 *m_Read = NULL;
  std::map<uint32_t, IDirect3DPixelShader9 *> m_Shaders;
  uint32_t m_Draws = 0;
};

// the objects ReadPixelInputs makes, released however it returns
struct ReadbackObjects
{
  IDirect3DSurface9 *targets[4] = {};
  IDirect3DSurface9 *pick = NULL;
  IDirect3DSurface9 *pickRead = NULL;
  IDirect3DQuery9 *query = NULL;
  rdcarray<IDirect3DPixelShader9 *> shaders;

  ~ReadbackObjects()
  {
    for(IDirect3DSurface9 *&t : targets)
      SAFE_RELEASE(t);
    SAFE_RELEASE(pick);
    SAFE_RELEASE(pickRead);
    SAFE_RELEASE(query);
    for(IDirect3DPixelShader9 *&sh : shaders)
      SAFE_RELEASE(sh);
  }
};

// Draws the event again with the readback passes and reads what they wrote at (x, y), into
// results[pass][target], and for ps_2_x the same at the quad's other pixels (corners, see
// QuadInputs), each empty when the draw doesn't write it. The replay is at the event without its
// draw, and the caller puts the device's state back.
bool ReadPixelInputs(WrappedD3DDevice9 *device, uint32_t eventId, uint32_t x, uint32_t y,
                     const D3D9BC::Program &prog, const D3D9DeviceState &state,
                     rdcarray<D3D9Emu::ReadbackPass> &passes,
                     rdcarray<rdcarray<D3D9Emu::Vec4>> &results,
                     rdcarray<rdcarray<D3D9Emu::Vec4>> corners[3], rdcstr &error)
{
  IDirect3DDevice9 *dev = device->GetReal();

  D3DCAPS9 caps = {};
  dev->GetDeviceCaps(&caps);
  // ps_2_x's dsx and dsy, for the quad's other pixels. Without them those get the pixel's inputs.
  const bool gradients = (caps.PS20Caps.Caps & D3DPS20CAPS_GRADIENTINSTRUCTIONS) != 0;
  const uint32_t maxTargets = RDCCLAMP(uint32_t(caps.NumSimultaneousRTs), 1U, 4U);
  if(!gradients && prog.version.major < 3)
    RDCWARN(
        "The GPU has no ps_2_x gradient instructions, so the debugged pixel's neighbours get "
        "its own inputs and the derivatives are 0");

  if(!D3D9Emu::BuildInputReadback(prog, gradients, maxTargets, passes, error))
  {
    error = "its inputs can't be read back: " + error;
    return false;
  }

  D3DSURFACE_DESC rtDesc = {};
  {
    IDirect3DSurface9 *rt = NULL;
    if(FAILED(dev->GetRenderTarget(0, &rt)) || rt == NULL)
    {
      error = "the draw has no render target";
      return false;
    }
    rt->GetDesc(&rtDesc);
    rt->Release();
  }

  if(rtDesc.MultiSampleType != D3DMULTISAMPLE_NONE)
  {
    error = "multisampled render targets can't be debugged yet";
    return false;
  }
  if(x >= rtDesc.Width || y >= rtDesc.Height)
  {
    error = StringFormat::Fmt("it's outside the %ux%u render target", rtDesc.Width, rtDesc.Height);
    return false;
  }

  // the pixel has to be inside the draw's scissor rect. Its neighbours needn't be, as helpers.
  DWORD appScissor = FALSE;
  dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &appScissor);
  if(appScissor)
  {
    const RECT &sc = state.ScissorRect;
    if(LONG(x) < sc.left || LONG(x) >= sc.right || LONG(y) < sc.top || LONG(y) >= sc.bottom)
    {
      error = "the draw's scissor rect doesn't cover it";
      return false;
    }
  }

  ReadbackObjects objs;

  uint32_t numTargets = 0;
  for(const D3D9Emu::ReadbackPass &pass : passes)
    numTargets = RDCMAX(numTargets, (uint32_t)pass.items.size());
  // a shader without inputs still has a pass, writing 0 to oC0, for the occlusion query
  numTargets = RDCMAX(numTargets, 1U);

  for(uint32_t t = 0; t < numTargets; t++)
  {
    HRESULT hr = dev->CreateRenderTarget(rtDesc.Width, rtDesc.Height, D3DFMT_A32B32G32R32F,
                                         D3DMULTISAMPLE_NONE, 0, FALSE, &objs.targets[t], NULL);
    if(FAILED(hr))
    {
      error = StringFormat::Fmt("couldn't create a %ux%u A32B32G32R32F render target: %s",
                                rtDesc.Width, rtDesc.Height, ToStr(hr).c_str());
      return false;
    }
  }

  HRESULT hr = dev->CreateRenderTarget(1, 1, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE,
                                       &objs.pick, NULL);
  if(SUCCEEDED(hr))
    hr = dev->CreateOffscreenPlainSurface(1, 1, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM,
                                          &objs.pickRead, NULL);
  if(FAILED(hr))
  {
    error = "couldn't create the surfaces to read the pixel back: " + ToStr(hr);
    return false;
  }

  for(const D3D9Emu::ReadbackPass &pass : passes)
  {
    IDirect3DPixelShader9 *sh = NULL;
    hr = dev->CreatePixelShader((const DWORD *)pass.tokens.data(), &sh);
    if(FAILED(hr))
    {
      error = "couldn't create the shader that reads its inputs: " + ToStr(hr);
      return false;
    }
    objs.shaders.push_back(sh);
  }

  // which says whether the draw writes a pixel
  hr = dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &objs.query);
  if(FAILED(hr))
  {
    error = "the GPU has no occlusion queries, which say whether the draw writes it: " + ToStr(hr);
    return false;
  }

  // every pass leaves the depth stencil alone and writes its values as they are
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
  dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_CCW_STENCILPASS, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE2, 0xF);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE3, 0xF);

  // the passes drawn with the scissor rect on (px, py) alone, false if the draw doesn't write it
  auto drawAt = [&](uint32_t px, uint32_t py, rdcarray<rdcarray<D3D9Emu::Vec4>> &out) {
    const RECT rect = {LONG(px), LONG(py), LONG(px + 1), LONG(py + 1)};
    out.clear();
    out.resize(passes.size());

    for(size_t p = 0; p < passes.size(); p++)
    {
      const uint32_t count = (uint32_t)passes[p].items.size();

      // setting render target 0 resets the viewport and scissor rect
      for(uint32_t t = 0; t < D3D9_NUM_RENDER_TARGETS; t++)
        dev->SetRenderTarget(t, t == 0 || t < count ? objs.targets[t] : NULL);
      dev->SetViewport(&state.Viewport);
      dev->SetScissorRect(&rect);
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
      dev->SetPixelShader(objs.shaders[p]);

      const bool countSamples = p == 0;
      if(countSamples)
        objs.query->Issue(D3DISSUE_BEGIN);

      device->ReplayLog(0, eventId, eReplay_OnlyDraw);

      if(countSamples)
      {
        objs.query->Issue(D3DISSUE_END);
        DWORD samples = 0;
        while(objs.query->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH) == S_FALSE)
        {
        }
        if(samples == 0)
        {
          out.clear();
          return true;
        }
      }

      for(uint32_t t = 0; t < count; t++)
      {
        HRESULT readHR = dev->StretchRect(objs.targets[t], &rect, objs.pick, NULL, D3DTEXF_NONE);
        if(SUCCEEDED(readHR))
          readHR = dev->GetRenderTargetData(objs.pick, objs.pickRead);
        D3DLOCKED_RECT locked = {};
        if(SUCCEEDED(readHR))
          readHR = objs.pickRead->LockRect(&locked, NULL, D3DLOCK_READONLY);
        if(FAILED(readHR))
        {
          error = "couldn't read its inputs back: " + ToStr(readHR);
          return false;
        }
        D3D9Emu::Vec4 v;
        memcpy(v.v, locked.pBits, sizeof(v.v));
        objs.pickRead->UnlockRect();
        out[p].push_back(v);
      }
    }
    return true;
  };

  if(!drawAt(x, y, results))
    return false;
  if(results.empty())
  {
    error =
        "the draw doesn't write it: it's outside its primitives, or fails the depth or stencil "
        "test";
    return false;
  }

  // ps_2_x's neighbours, which ps_3_0 reads with vPos instead
  if(prog.version.major < 3 && gradients)
  {
    const uint32_t qx = x & ~1U, qy = y & ~1U;
    const uint32_t cornerPos[3][2] = {{qx, qy}, {qx + 1, qy}, {qx, qy + 1}};
    for(uint32_t c = 0; c < 3; c++)
    {
      corners[c].clear();
      const uint32_t cx = cornerPos[c][0], cy = cornerPos[c][1];
      if((cx == x && cy == y) || cx >= rtDesc.Width || cy >= rtDesc.Height)
        continue;
      if(!drawAt(cx, cy, corners[c]))
        return false;
    }
  }

  return true;
}
};    // namespace

ShaderDebugTrace *D3D9Replay::DebugVertex(uint32_t eventId, uint32_t vertid, uint32_t instid,
                                          uint32_t idx, uint32_t view)
{
  // the vertex is found from idx, as D3D11 does. There's no multiview.
  (void)vertid;
  (void)view;

  // the replay is at the event, as for GetPostVSData
  InitPostVSBuffers(eventId);

  auto it = m_PostVS.find(eventId);
  if(it == m_PostVS.end())
    return NoDebug(eventId, "it isn't a draw");

  std::shared_ptr<PostVSEvent> ev = it->second;
  if(!ev->status.empty())
    return NoDebug(eventId, ev->status);
  if(!ev->hasVS)
    return NoDebug(eventId, "fixed function vertex processing isn't a shader");

  auto sh = m_Shaders.find(ev->vsId);
  if(sh == m_Shaders.end())
    return NoDebug(eventId, "the vertex shader's bytecode wasn't kept");

  const D3D9Emu::DrawInputs &draw = ev->draw;

  // idx is the mesh viewer's IDX column: an indexed draw's index with the base vertex added, or a
  // non-indexed draw's vertex counted from its first
  const uint32_t vertex = draw.indexed ? idx : draw.firstVertex + idx;
  const uint32_t instance = RDCMIN(instid, ev->numInstances - 1);

  D3D9Emu::Vec4 inputs[D3D9Emu::MaxVSInputs];
  D3D9Emu::FetchVSInputs(draw, D3D9Emu::BindVSInputs(ev->vs.GetProgram(), draw.elements), vertex,
                         instance, inputs);

  rdcarray<uint32_t> lines;
  sh->second.GetDisassembly(&lines);

  D3D9Emu::VSDebugger *debugger = new D3D9Emu::VSDebugger;
  // the emulator, constants and textures are the event's
  debugger->owner = ev;
  return debugger->Begin(ev->vs, sh->second, lines, inputs, ev->consts, ev->textures);
}

ShaderDebugTrace *D3D9Replay::DebugPixel(uint32_t eventId, uint32_t x, uint32_t y,
                                         const DebugPixelInputs &inputs)
{
  // the sample and primitive aren't chosen: see the top of the file
  (void)inputs;

  const ActionDescription *action = m_pDevice->GetAction(eventId);
  if(action == NULL || !(action->flags & ActionFlags::Drawcall))
    return NoPixelDebug(eventId, x, y, "it isn't a draw");

  // the replay is at the event with its draw. Before it, depth and stencil are what it tests
  // against.
  m_pDevice->ReplayLog(0, eventId, eReplay_WithoutDraw);

  const ResourceId psId = WrappedIDOf(m_pDevice->GetBindings().ps);
  if(psId == ResourceId())
    return NoPixelDebug(eventId, x, y, "fixed function pixel processing isn't a shader");

  auto sh = m_Shaders.find(psId);
  if(sh == m_Shaders.end())
    return NoPixelDebug(eventId, x, y, "the pixel shader's bytecode wasn't kept");

  D3D9BC::ShaderInfo &info = sh->second;
  const ShaderDebugInfo &debugInfo = info.GetReflection().debugInfo;
  if(!debugInfo.debuggable)
    return NoPixelDebug(eventId, x, y, debugInfo.debugStatus);

  std::shared_ptr<PixelDebugData> data(new PixelDebugData);
  if(!data->emu.Init(info.GetProgram()))
    return NoPixelDebug(eventId, x, y, "the pixel shader can't be emulated: " + data->emu.GetError());

  D3D9DeviceState state;
  if(!m_pDevice->SaveReplayDrawState(state))
    return NoPixelDebug(eventId, x, y, "a state block is being recorded");

  data->consts = data->emu.ApplyLiterals(ToConstants(state.PixelShaderConstants));

  rdcarray<D3D9Emu::ReadbackPass> passes;
  rdcarray<rdcarray<D3D9Emu::Vec4>> results, corners[3];
  rdcstr error;
  const bool read = ReadPixelInputs(m_pDevice, eventId, x, y, info.GetProgram(), state, passes,
                                    results, corners, error);

  D3D9Emu::PSInputs quad[D3D9Emu::QuadPixels];
  uint32_t gpuDraws = 0;
  if(read)
  {
    D3D9Emu::QuadInputs(passes, results, corners, x, y, quad);

    // the quad runs once now, with its lookups and transcendentals drawn on the GPU while the
    // replay is at the event, and the debugger steps it again from what they returned
    const bool sm3 = InitDebugRender();
    GPULookups gpu(m_pDevice->GetReal(), sm3 ? m_Debug.texDisplayVS : NULL,
                   sm3 ? m_Debug.quadDecl : NULL);
    data->lookups.SetSource(&gpu);
    D3D9Emu::PSQuad run;
    run.Start(data->emu, quad, data->consts, &data->lookups, NULL);
    run.Run();
    data->lookups.SetSource(NULL);
    gpuDraws = gpu.Draws();
  }

  m_pDevice->RestoreReplayDrawState(state);

  if(!read)
    return NoPixelDebug(eventId, x, y, error);
  if(data->lookups.Failures() > 0)
    return NoPixelDebug(eventId, x, y,
                        "its lookups couldn't be made on the GPU: " + data->lookups.GetError());

  RDCLOG("Debugging pixel (%u, %u) at event %u: %zu lookups and %zu transcendentals, %u GPU draws",
         x, y, eventId, data->lookups.NumSamples(), data->lookups.NumTranscendentals(), gpuDraws);

  rdcarray<uint32_t> lines;
  info.GetDisassembly(&lines);

  D3D9Emu::PSDebugger *debugger = new D3D9Emu::PSDebugger;
  // the emulator, constants and lookups are the debugger's
  debugger->owner = data;
  return debugger->Begin(data->emu, info, lines, quad, D3D9Emu::QuadPixelOf(x, y), data->consts,
                         &data->lookups, NULL);
}

rdcarray<ShaderDebugState> D3D9Replay::ContinueDebug(ShaderDebugger *debugger)
{
  if(debugger == NULL)
    return {};

  // vertex and pixel debuggers both step through StepDebugger
  D3D9Emu::StepDebugger *step = (D3D9Emu::StepDebugger *)debugger;
  rdcarray<ShaderDebugState> ret = step->Continue();
  if(ret.empty() && !step->GetError().empty())
    RDCWARN("The debugged shader stopped early: %s", step->GetError().c_str());
  return ret;
}

void D3D9Replay::FreeDebugger(ShaderDebugger *debugger)
{
  delete debugger;
}
