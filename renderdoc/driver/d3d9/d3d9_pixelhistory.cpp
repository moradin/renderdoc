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
#include "d3d9_replay.h"
#include "d3d9_resources.h"

// Pixel history
// -------------
// The replay controller passes the events up to the current one that wrote the target (GetUsage):
// draws with it bound as a render target or depth stencil, Clear and ColorFill, the copies
// (StretchRect, UpdateSurface, UpdateTexture) and GenerateMipSubLevels. The frame is replayed up to
// each event in turn, the same partial replays the counters use (d3d9_counters.cpp), and an event
// that wrote the pixel gets a modification with the pixel before and after it. What the GPU does
// that this relies on was measured natively first (test_reports/d3d9_step6_1_native_probe.md).
//
// * Which draws touched the pixel: the draw is replayed inside an occlusion query, with the
//   constant colour shader of the vertex shader's model (as the overlays draw, d3d9_overlay.cpp)
//   and a scissor rect of the one pixel, and with culling, the depth and stencil tests, the alpha
//   test, user clip planes and colour writes off, as D3D11 does. So everything the draw rasterises
//   at the pixel counts, whether or not it then passed its tests. Depth clipping can't be turned
//   off, so a draw whose fragments at the pixel are all outside the depth range isn't listed.
// * Why a listed draw's fragments failed: the query is repeated with the pipeline's stages turned
//   back on in order (culling, user clip planes, the sample mask, the pixel shader with the alpha
//   test, the depth test, the stencil test), and the first that leaves nothing is the reason, as
//   D3D11's tests are. The scissor test is decided on the CPU. D3D9's user clip planes are
//   reported as viewClipped, since no other flag is closer.
// * Clears and copies are decided on the CPU: Clear writes inside the viewport, the scissor rect
//   when the scissor test is on, and its rects; ColorFill, StretchRect and UpdateSurface only in
//   their destination rectangle (D3D9WriteRegion, kept at load). UpdateTexture and
//   GenerateMipSubLevels are listed for any pixel of the subresources they write.
// * Values: a colour target's pixel is copied into a 1x1 render target of its format with
//   StretchRect, which resolves multisampled targets, and read back. A depth stencil's pixel is
//   read with the depth and stencil tests (ReadDepthStencilPixel, d3d9_depth.cpp). A colour
//   target's modifications have the bound depth stencil's values too, for draws.
// * As on D3D11, clears are modifications of their own, the copies and GenerateMipSubLevels are
//   direct writes, and a colour clear has no depth and a depth clear no colour.
//
// Each draw is one modification for now, failed only if all its fragments at the pixel did. The
// fragments within a draw, their shader outputs and primitive IDs are the next chunks of step 6
// (STATUS.md), so shaderOut is unavailable and the primitive unknown.

// d3d9_pixel.cpp and d3d9_overlay.cpp
bool D3D9DecodeTexel(D3DFORMAT format, const byte *p, FloatVector &out);
uint32_t D3D9BoundVertexShaderMajor(IDirect3DDevice9 *dev);

namespace
{
bool Contains(const RECT &r, uint32_t x, uint32_t y)
{
  return LONG(x) >= r.left && LONG(x) < r.right && LONG(y) >= r.top && LONG(y) < r.bottom;
}

// waits for a query's result. Returns false if it doesn't arrive, e.g. when the device is lost.
bool WaitForQuery(IDirect3DQuery9 *query, void *data, DWORD size)
{
  for(uint32_t i = 0; i < 1000000; i++)
  {
    HRESULT hr = query->GetData(data, size, D3DGETDATA_FLUSH);
    if(hr == S_OK)
      return true;
    if(hr != S_FALSE)
      return false;
    if(i > 1000)
      Threading::Sleep(0);
  }
  return false;
}

// Reads single pixels of render targets of one format, through a 1x1 render target and a 1x1
// system memory surface of that format.
struct PixelReader
{
  IDirect3DSurface9 *one = NULL;
  IDirect3DTexture9 *oneTex = NULL;
  IDirect3DSurface9 *sysmem = NULL;
  D3DFORMAT format = D3DFMT_UNKNOWN;
  rdcstr error;

  bool Init(IDirect3DDevice9 *dev, D3DFORMAT fmt)
  {
    format = fmt;
    HRESULT hr = dev->CreateRenderTarget(1, 1, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &one, NULL);
    // some formats are only render targets as textures
    if(FAILED(hr))
    {
      hr = dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &oneTex, NULL);
      if(SUCCEEDED(hr))
        hr = oneTex->GetSurfaceLevel(0, &one);
    }
    if(SUCCEEDED(hr))
      hr = dev->CreateOffscreenPlainSurface(1, 1, fmt, D3DPOOL_SYSTEMMEM, &sysmem, NULL);
    if(FAILED(hr))
    {
      error = StringFormat::Fmt("%s pixels can't be read back: %s", ToStr(fmt).c_str(),
                                ToStr(hr).c_str());
      Release();
      return false;
    }
    return true;
  }

  bool Read(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, uint32_t x, uint32_t y, FloatVector &out)
  {
    if(!sysmem)
      return false;

    const RECT rect = {LONG(x), LONG(y), LONG(x + 1), LONG(y + 1)};
    HRESULT hr = dev->StretchRect(src, &rect, one, NULL, D3DTEXF_NONE);
    if(SUCCEEDED(hr))
      hr = dev->GetRenderTargetData(one, sysmem);
    D3DLOCKED_RECT locked = {};
    if(SUCCEEDED(hr))
      hr = sysmem->LockRect(&locked, NULL, D3DLOCK_READONLY);
    if(FAILED(hr))
    {
      error = StringFormat::Fmt("Couldn't read the pixel back: %s", ToStr(hr).c_str());
      return false;
    }

    const bool decoded = D3D9DecodeTexel(format, (const byte *)locked.pBits, out);
    sysmem->UnlockRect();
    if(!decoded)
      error = StringFormat::Fmt("%s pixels can't be decoded", ToStr(format).c_str());
    return decoded;
  }

  void Release()
  {
    SAFE_RELEASE(one);
    SAFE_RELEASE(oneTex);
    SAFE_RELEASE(sysmem);
  }
};

// the real surface of a texture's subresource, or of a surface, with a reference added
IDirect3DSurface9 *SubresourceSurface(D3D9ResourceManager *rm, ResourceId id, const Subresource &sub)
{
  WrappedD3D9Object *obj = rm->HasResource(id) ? GetWrappedObject(rm->GetResource(id)) : NULL;
  if(obj == NULL)
    return NULL;

  IDirect3DSurface9 *surf = NULL;
  switch(obj->GetResourceType())
  {
    case Resource_Texture:
      ((WrappedD3DTexture9 *)obj)->GetReal()->GetSurfaceLevel(sub.mip, &surf);
      break;
    case Resource_CubeTexture:
      ((WrappedD3DCubeTexture9 *)obj)
          ->GetReal()
          ->GetCubeMapSurface((D3DCUBEMAP_FACES)RDCMIN(sub.slice, 5U), sub.mip, &surf);
      break;
    case Resource_Surface:
      surf = ((WrappedD3DSurface9 *)obj)->GetReal();
      surf->AddRef();
      break;
    default: break;
  }
  return surf;
}
};

rdcarray<PixelModification> D3D9Replay::PixelHistory(rdcarray<EventUsage> events, ResourceId target,
                                                     uint32_t x, uint32_t y, const Subresource &sub,
                                                     CompType typeCast)
{
  rdcarray<PixelModification> history;

  if(events.empty())
    return history;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  IDirect3DSurface9 *targetSurf = SubresourceSurface(m_pDevice->GetResourceManager(), target, sub);
  if(targetSurf == NULL)
  {
    RDCERR("Pixel history: %s mip %u slice %u isn't a surface that can be drawn to",
           ToStr(target).c_str(), sub.mip, sub.slice);
    return history;
  }

  D3DSURFACE_DESC targetDesc = {};
  targetSurf->GetDesc(&targetDesc);
  const bool targetIsDepth = (targetDesc.Usage & D3DUSAGE_DEPTHSTENCIL) != 0;

  if(x >= targetDesc.Width || y >= targetDesc.Height)
  {
    SAFE_RELEASE(targetSurf);
    return history;
  }

  PixelReader reader;
  if(!targetIsDepth && !reader.Init(dev, targetDesc.Format))
    RDCWARN("Pixel history: %s", reader.error.c_str());

  // the queries that find which draws touched the pixel. Without them every draw is listed.
  IDirect3DQuery9 *occlusion = NULL;
  dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occlusion);
  const bool canQuery = occlusion != NULL && InitOverlayRender();
  if(!canQuery)
    RDCWARN("Pixel history: occlusion queries aren't available, so every draw is listed");

  // whether the surface is bound where the event writes it
  auto boundAsTarget = [&]() {
    if(targetIsDepth)
      return Unwrap(m_pDevice->GetBoundDepthStencil()) == targetSurf;
    for(uint32_t i = 0; i < D3D9_NUM_RENDER_TARGETS; i++)
      if(Unwrap(m_pDevice->GetBoundRenderTarget(i)) == targetSurf)
        return true;
    return false;
  };

  // Whether the draw at eventId rasterises anything at the pixel, and if so, why its fragments
  // there all failed, in fails. Each stage of the pipeline is turned back on in order, and the
  // first after which nothing is left is the reason: culling, user clip planes, the scissor test,
  // the sample mask, the pixel shader (texkill) with the alpha test and alpha to coverage, the
  // depth test, then the stencil test. Nothing is written by these draws.
  auto testDraw = [&](uint32_t eventId, PixelModification &fails) {
    D3D9DeviceState state;
    // a state block is being recorded: our state calls would go into it, so it can't be tested
    if(!canQuery || !m_pDevice->SaveReplayDrawState(state))
      return true;

    DWORD cull = D3DCULL_NONE, clipPlanes = 0, scissorTest = FALSE, sampleMask = ~0U,
          alphaTest = FALSE, atoc = D3DFMT_UNKNOWN, zenable = D3DZB_FALSE, stencil = FALSE,
          twoSided = FALSE;
    dev->GetRenderState(D3DRS_CULLMODE, &cull);
    dev->GetRenderState(D3DRS_CLIPPLANEENABLE, &clipPlanes);
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);
    dev->GetRenderState(D3DRS_MULTISAMPLEMASK, &sampleMask);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    dev->GetRenderState(D3DRS_ADAPTIVETESS_Y, &atoc);
    dev->GetRenderState(D3DRS_ZENABLE, &zenable);
    dev->GetRenderState(D3DRS_STENCILENABLE, &stencil);
    dev->GetRenderState(D3DRS_TWOSIDEDSTENCILMODE, &twoSided);
    RECT scissor = {};
    dev->GetScissorRect(&scissor);
    IDirect3DPixelShader9 *ps = NULL;
    dev->GetPixelShader(&ps);
    float psConst0[4] = {};
    dev->GetPixelShaderConstantF(0, psConst0, 1);
    const bool hasDepthStencil = m_pDevice->GetBoundDepthStencil() != NULL;

    IDirect3DPixelShader9 *colourPS =
        D3D9BoundVertexShaderMajor(dev) >= 3 ? m_Overlay.colourPS3 : m_Overlay.colourPS2;
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const RECT pixel = {LONG(x), LONG(y), LONG(x + 1), LONG(y + 1)};

    enum Stage
    {
      Stage_Rasterise,
      Stage_Cull,
      Stage_ClipPlanes,
      Stage_SampleMask,
      Stage_Shader,
      Stage_Depth,
      Stage_Stencil,
    };

    bool queryFailed = false;

    // the samples the draw leaves at the pixel with the stages up to s on
    auto samplesWith = [&](Stage s) -> DWORD {
      const bool shader = s >= Stage_Shader;
      dev->SetPixelShader(shader ? ps : colourPS);
      dev->SetPixelShaderConstantF(0, shader ? psConst0 : white, 1);
      dev->SetRenderState(D3DRS_ALPHATESTENABLE, shader ? alphaTest : FALSE);
      // NVIDIA's alpha to coverage switch
      dev->SetRenderState(D3DRS_ADAPTIVETESS_Y, shader ? atoc : D3DFMT_UNKNOWN);

      dev->SetRenderState(D3DRS_CULLMODE, s >= Stage_Cull ? cull : D3DCULL_NONE);
      dev->SetRenderState(D3DRS_CLIPPLANEENABLE, s >= Stage_ClipPlanes ? clipPlanes : 0);
      dev->SetRenderState(D3DRS_MULTISAMPLEMASK, s >= Stage_SampleMask ? sampleMask : ~0U);
      dev->SetRenderState(D3DRS_ZENABLE, s >= Stage_Depth ? zenable : D3DZB_FALSE);
      dev->SetRenderState(D3DRS_STENCILENABLE, s >= Stage_Stencil ? stencil : FALSE);
      dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, s >= Stage_Stencil ? twoSided : FALSE);

      // never written
      dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
      dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
      dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
      dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0);
      dev->SetRenderState(D3DRS_COLORWRITEENABLE2, 0);
      dev->SetRenderState(D3DRS_COLORWRITEENABLE3, 0);

      // the scissor test is decided on the CPU, below
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
      dev->SetScissorRect(&pixel);

      occlusion->Issue(D3DISSUE_BEGIN);
      m_pDevice->ReplayLog(0, eventId, eReplay_OnlyDraw);
      occlusion->Issue(D3DISSUE_END);

      DWORD samples = 0;
      if(!WaitForQuery(occlusion, &samples, sizeof(samples)))
      {
        RDCWARN("Pixel history: no occlusion result for event %u", eventId);
        queryFailed = true;
        // listed, and not failed
        return 1;
      }
      return samples;
    };

    const bool touches = samplesWith(Stage_Rasterise) > 0;

    if(touches && !queryFailed)
    {
      // only the first test that removes everything is the reason
      if(cull != D3DCULL_NONE && samplesWith(Stage_Cull) == 0)
        fails.backfaceCulled = true;
      // no flag is closer for D3D9's user clip planes, which clip as the view volume does
      else if(clipPlanes != 0 && samplesWith(Stage_ClipPlanes) == 0)
        fails.viewClipped = true;
      else if(scissorTest && !Contains(scissor, x, y))
        fails.scissorClipped = true;
      else if(sampleMask != ~0U && samplesWith(Stage_SampleMask) == 0)
        fails.sampleMasked = true;
      // fixed function pixel processing has no texkill
      else if((ps || alphaTest || atoc != D3DFMT_UNKNOWN) && samplesWith(Stage_Shader) == 0)
        fails.shaderDiscarded = true;
      else if(zenable != D3DZB_FALSE && hasDepthStencil && samplesWith(Stage_Depth) == 0)
        fails.depthTestFailed = true;
      else if(stencil && hasDepthStencil && samplesWith(Stage_Stencil) == 0)
        fails.stencilTestFailed = true;

      // a test whose query didn't come back isn't reported
      if(queryFailed)
        fails = PixelModification();
    }

    m_pDevice->RestoreReplayDrawState(state);
    SAFE_RELEASE(ps);

    return touches;
  };

  // whether a Clear at the current event covers the pixel
  auto clearCovers = [&](const D3D9WriteRegion &region) {
    D3DVIEWPORT9 vp = {};
    dev->GetViewport(&vp);
    if(x < vp.X || x >= vp.X + vp.Width || y < vp.Y || y >= vp.Y + vp.Height)
      return false;

    DWORD scissor = FALSE;
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
    RECT rect = {};
    if(scissor && SUCCEEDED(dev->GetScissorRect(&rect)) && !Contains(rect, x, y))
      return false;

    if(region.rects.empty())
      return true;
    for(const RECT &r : region.rects)
      if(Contains(r, x, y))
        return true;
    return false;
  };

  bool warnedColour = false, warnedDepth = false;

  // the pixel's colour (for colour targets) and depth and stencil (from ds, if there is one)
  auto readValue = [&](ModificationValue &val, IDirect3DSurface9 *ds) {
    RDCEraseEl(val.col);
    val.depth = -1.0f;
    val.stencil = -1;

    if(!targetIsDepth)
    {
      FloatVector v;
      if(reader.Read(dev, targetSurf, x, y, v))
      {
        val.col.floatValue[0] = v.x;
        val.col.floatValue[1] = v.y;
        val.col.floatValue[2] = v.z;
        val.col.floatValue[3] = v.w;
      }
      else if(!warnedColour)
      {
        RDCWARN("Pixel history: %s", reader.error.c_str());
        warnedColour = true;
      }
    }

    if(ds)
    {
      rdcstr err = m_pDevice->ReadDepthStencilPixel(ds, x, y, sub.sample, val.depth, val.stencil);
      if(!err.empty())
      {
        // in use, but unknown
        val.depth = -2.0f;
        val.stencil = -2;
        if(!warnedDepth)
        {
          RDCWARN("Pixel history: %s", err.c_str());
          warnedDepth = true;
        }
      }
    }
  };

  // events before this have been replayed
  uint32_t replayedTo = 0;

  for(const EventUsage &use : events)
  {
    const uint32_t eventId = use.eventId;
    const ActionDescription *action = m_pDevice->GetAction(eventId);
    if(action == NULL)
      continue;

    m_pDevice->ReplayLog(replayedTo, eventId, eReplay_WithoutDraw);
    // the event itself is replayed below if it wrote the pixel, otherwise with the next range
    replayedTo = eventId;

    const D3D9WriteRegion *region = m_pDevice->GetWriteRegion(eventId);

    bool written = false, clear = false, direct = false, draw = false;
    // why a draw's fragments at the pixel failed, if they all did
    PixelModification fails = {};

    if(region && region->clear)
    {
      clear = true;
      const DWORD flags = targetIsDepth ? (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL) : D3DCLEAR_TARGET;
      written = (region->clearFlags & flags) && boundAsTarget() && clearCovers(*region);
    }
    else if(action->flags & ActionFlags::Drawcall)
    {
      draw = true;
      written = boundAsTarget() && testDraw(eventId, fails);
    }
    else if(action->flags &
            (ActionFlags::Clear | ActionFlags::Copy | ActionFlags::Resolve | ActionFlags::GenMips))
    {
      // ColorFill, the copies and GenerateMipSubLevels, into their destination
      clear = bool(action->flags & ActionFlags::Clear);
      direct = !clear;
      if(action->copyDestination == target)
      {
        if(action->flags & ActionFlags::GenMips)
        {
          // every level but the top one
          written = sub.mip > 0;
        }
        else if(region)
        {
          // ColorFill, StretchRect, UpdateSurface: one subresource, a rectangle of it
          const Subresource &dst = action->copyDestinationSubresource;
          if(dst.mip == sub.mip && dst.slice == sub.slice)
            for(const RECT &r : region->rects)
              written |= Contains(r, x, y);
        }
        else
        {
          // UpdateTexture copies every level, GetRenderTargetData a whole surface
          written = true;
        }
      }
    }

    if(!written)
      continue;

    PixelModification mod = fails;
    mod.eventId = eventId;
    mod.directShaderWrite = direct;
    mod.unboundPS = false;
    mod.fragIndex = 0;
    mod.primitiveID = ~0U;

    // a depth target's own values, or a colour target's with the bound depth stencil's for draws.
    // A colour clear has no depth, as on D3D11.
    IDirect3DSurface9 *ds = NULL;
    if(targetIsDepth)
      ds = targetSurf;
    else if(draw)
      ds = Unwrap(m_pDevice->GetBoundDepthStencil());

    readValue(mod.preMod, ds);

    m_pDevice->ReplayLog(0, eventId, eReplay_OnlyDraw);
    replayedTo = eventId + 1;

    readValue(mod.postMod, ds);

    // what a draw's shader wrote is found per fragment, which isn't done yet. What a clear or
    // copy writes is its result.
    if(draw)
      mod.shaderOut.SetInvalid();
    else
      mod.shaderOut = mod.postMod;

    history.push_back(mod);
  }

  SAFE_RELEASE(occlusion);
  reader.Release();
  SAFE_RELEASE(targetSurf);

  // the replay controller replays to its event afterwards
  return history;
}
