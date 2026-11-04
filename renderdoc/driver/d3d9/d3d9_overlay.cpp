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

#include "maths/vec.h"
#include "d3d9_device.h"
#include "d3d9_replay.h"
#include "d3d9_resources.h"

// The texture viewer's overlays
// -----------------------------
// The overlay is drawn into a texture of our own (m_Overlay.tex), the size of the target being
// viewed, which the texture viewer then displays blended over the target. Each overlay draws the
// event again with a constant colour pixel shader and some state changed, as D3D11 does
// (d3d11_overlay.cpp has what each overlay means and its colours).
//
// * The replay controller calls RenderOverlay with the frame replayed up to the event without its
//   draw, and replays the draw (eReplay_OnlyDraw) after it, as on D3D11. So the depth stencil isn't
//   copied: it holds what the draw tests against, and every pass draws with depth and stencil
//   writes off (ZWRITEENABLE FALSE, the stencil ops KEEP and a write mask of 0), which leaves it
//   as it was. The device's state is put back before returning.
// * D3D9 needs the depth stencil to be at least as big as the render target and to have the same
//   multisample type, so the passes draw into a surface the size of the viewed target. A
//   multisampled one is drawn into a multisampled render target (m_Overlay.msTarget) and resolved
//   into the overlay texture with StretchRect.
// * D3D9 only links a ps_3_0 pixel shader with a vs_3_0 vertex shader, and ps_3_0 can't be used
//   with fixed function vertex processing or vs_1_1-vs_2_x, so the constant colour shader is
//   ps_3_0 for vs_3_0 draws and ps_2_0 for the others.
// * The clear before pass and clear before draw overlays clear the application's targets and
//   replay the draws, so they change the targets themselves (the overlay texture stays
//   transparent), as on the other APIs. The replay controller replays again when they're turned
//   off.
// * Nothing is drawn at an event between BeginStateBlock and EndStateBlock, as in the texture
//   viewer: our state calls would be recorded into the application's state block.

// d3d9_rendertexture.cpp
rdcstr D3D9CompileDebugShader(const char *source, const char *profile, DWORD flags,
                              rdcarray<DWORD> &bytecode);
void D3D9SetDebugDrawState(IDirect3DDevice9 *dev);

namespace
{
// c0: the colour
const char *OverlayColourPS = R"(
float4 colour : register(c0);

float4 main() : COLOR0
{
  return colour;
}
)";

// draws a quad given in clip space, for the viewport and scissor rectangles
const char *OverlayQuadVS = R"(
float4 main(float4 pos : POSITION) : POSITION
{
  return pos;
}
)";

// D3D11's checkerboard with a border, for the viewport and scissor rectangles.
// c0: xy = the rectangle's top left corner in pixels, zw = its size
// c1: x = the border's width (negative for none), y = the size of a checker square
// c2, c3: the checker colours of the border, c4: the colour inside it
const char *OverlayBorderPS = R"(
float4 rect : register(c0);
float4 params : register(c1);
float4 primary : register(c2);
float4 secondary : register(c3);
float4 inner : register(c4);

float4 main(float2 vpos : VPOS) : COLOR0
{
  // VPOS is the pixel's integer coordinates, D3D11's SV_Position its centre
  float2 rel = vpos + 0.5f - rect.xy;

  if(params.x >= 0.0f && rel.x >= params.x && rel.x <= rect.z - params.x && rel.y >= params.x &&
     rel.y <= rect.w - params.x)
    return inner;

  float2 ab = fmod(rel, (params.y * 2.0f).xx);

  bool first = ((ab.x < params.y && ab.y < params.y) || (ab.x > params.y && ab.y > params.y));

  return first ? primary : secondary;
}
)";

// a quad covering the viewport, as a triangle strip
const Vec4f ViewportQuad[4] = {
    Vec4f(-1.0f, 1.0f, 0.0f, 1.0f),
    Vec4f(1.0f, 1.0f, 0.0f, 1.0f),
    Vec4f(-1.0f, -1.0f, 0.0f, 1.0f),
    Vec4f(1.0f, -1.0f, 0.0f, 1.0f),
};

D3DCOLOR ToD3DColor(float r, float g, float b, float a)
{
  auto toByte = [](float f) -> DWORD {
    if(!(f > 0.0f))
      return 0;
    if(f >= 1.0f)
      return 255;
    return DWORD(f * 255.0f + 0.5f);
  };
  return D3DCOLOR_ARGB(toByte(a), toByte(r), toByte(g), toByte(b));
}

// the major version of the bound vertex shader, 0 for fixed function
uint32_t BoundVertexShaderMajor(IDirect3DDevice9 *dev)
{
  IDirect3DVertexShader9 *vs = NULL;
  if(FAILED(dev->GetVertexShader(&vs)) || vs == NULL)
    return 0;

  uint32_t ret = 0;
  UINT size = 0;
  if(SUCCEEDED(vs->GetFunction(NULL, &size)) && size >= sizeof(DWORD))
  {
    bytebuf data;
    data.resize(size);
    if(SUCCEEDED(vs->GetFunction(data.data(), &size)))
    {
      DWORD version;
      memcpy(&version, data.data(), sizeof(version));
      ret = (version >> 8) & 0xff;
    }
  }

  vs->Release();
  return ret;
}

bool HasStencil(D3DFORMAT format)
{
  switch((DWORD)format)
  {
    case D3DFMT_D15S1:
    case D3DFMT_D24S8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case MAKEFOURCC('I', 'N', 'T', 'Z'): return true;
    default: return false;
  }
}

// a viewport for a rectangle, clamped to a width x height target. False if nothing's left of it.
bool ClampedViewport(LONG left, LONG top, LONG right, LONG bottom, UINT width, UINT height,
                     D3DVIEWPORT9 &view)
{
  left = RDCCLAMP(left, 0L, (LONG)width);
  right = RDCCLAMP(right, 0L, (LONG)width);
  top = RDCCLAMP(top, 0L, (LONG)height);
  bottom = RDCCLAMP(bottom, 0L, (LONG)height);
  if(right <= left || bottom <= top)
    return false;

  view.X = (DWORD)left;
  view.Y = (DWORD)top;
  view.Width = DWORD(right - left);
  view.Height = DWORD(bottom - top);
  view.MinZ = 0.0f;
  view.MaxZ = 1.0f;
  return true;
}

// a render target or the depth stencil the viewed texture is bound as
struct BoundTarget
{
  // on the real device, not owned
  IDirect3DSurface9 *surf = NULL;
  uint32_t mip = 0;
  uint32_t face = 0;
};

bool FindBoundTarget(WrappedD3DDevice9 *device, ResourceId id, BoundTarget &target)
{
  if(id == ResourceId())
    return false;

  IDirect3DSurface9 *bound[D3D9_NUM_RENDER_TARGETS + 1] = {};
  for(uint32_t i = 0; i < D3D9_NUM_RENDER_TARGETS; i++)
    bound[i] = device->GetBoundRenderTarget(i);
  bound[D3D9_NUM_RENDER_TARGETS] = device->GetBoundDepthStencil();

  for(IDirect3DSurface9 *surf : bound)
  {
    if(surf == NULL || device->SurfaceTextureID(surf) != id)
      continue;

    WrappedD3D9Object *wrapped = GetWrappedObject(surf);
    target.surf = Unwrap(surf);
    // swapchain back buffers and standalone surfaces are level 0
    if(wrapped && wrapped->GetContainer() &&
       wrapped->GetContainer()->GetResourceType() != Resource_SwapChain)
    {
      target.mip = wrapped->GetChildLevel();
      target.face = wrapped->GetChildFace();
    }
    return true;
  }

  return false;
}

void WarnOnce(bool &warned, const char *msg)
{
  if(!warned)
    RDCWARN("%s", msg);
  warned = true;
}
};

///////////////////////////////////////////////////////////////////////////////
// setup

void D3D9Replay::OverlayRender::ReleaseShaders()
{
  SAFE_RELEASE(colourPS2);
  SAFE_RELEASE(colourPS3);
  SAFE_RELEASE(quadVS);
  SAFE_RELEASE(borderPS);
  tried = ok = false;
}

void D3D9Replay::OverlayRender::ReleaseTextures()
{
  // the overlay texture is a wrapper with one external reference, ours
  SAFE_RELEASE(tex);
  id = ResourceId();
  cube = false;
  width = height = mips = 0;
  SAFE_RELEASE(msTarget);
}

void D3D9Replay::ReleaseOverlay()
{
  m_Overlay.ReleaseTextures();
  m_Overlay.ReleaseShaders();
}

bool D3D9Replay::InitOverlayRender()
{
  if(m_Overlay.tried)
    return m_Overlay.ok;

  m_Overlay.tried = true;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  rdcarray<DWORD> ps2, ps3, vs, border;
  rdcstr err = D3D9CompileDebugShader(OverlayColourPS, "ps_2_0", 0, ps2);
  if(err.empty())
    err = D3D9CompileDebugShader(OverlayColourPS, "ps_3_0", 0, ps3);
  if(err.empty())
    err = D3D9CompileDebugShader(OverlayQuadVS, "vs_3_0", 0, vs);
  if(err.empty())
    err = D3D9CompileDebugShader(OverlayBorderPS, "ps_3_0", 0, border);

  if(!err.empty())
  {
    RDCERR("The overlays can't draw: %s", err.c_str());
    return false;
  }

  HRESULT hr = dev->CreatePixelShader(ps2.data(), &m_Overlay.colourPS2);
  if(SUCCEEDED(hr))
    hr = dev->CreatePixelShader(ps3.data(), &m_Overlay.colourPS3);
  if(SUCCEEDED(hr))
    hr = dev->CreateVertexShader(vs.data(), &m_Overlay.quadVS);
  if(SUCCEEDED(hr))
    hr = dev->CreatePixelShader(border.data(), &m_Overlay.borderPS);

  if(FAILED(hr))
  {
    RDCERR("Couldn't create the overlay shaders: %s", ToStr(hr).c_str());
    m_Overlay.ReleaseShaders();
    // don't try again every repaint
    m_Overlay.tried = true;
    return false;
  }

  m_Overlay.ok = true;
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// which events and targets

bool D3D9Replay::IsRenderOutput(ResourceId id)
{
  BoundTarget target;
  return FindBoundTarget(m_pDevice, id, target);
}

// the draws before eventId in its pass: back to the last clear, or to where the targets changed.
// The same as D3D11's.
rdcarray<uint32_t> D3D9Replay::GetPassEvents(uint32_t eventId)
{
  rdcarray<uint32_t> passEvents;

  const ActionDescription *action = m_pDevice->GetAction(eventId);

  const ActionDescription *start = action;
  while(start && start->previousAction && !(start->previousAction->flags & ActionFlags::Clear))
  {
    const ActionDescription *prev = start->previousAction;

    if(start->outputs != prev->outputs || start->depthOut != prev->depthOut)
      break;

    start = prev;
  }

  while(start)
  {
    if(start == action)
      break;

    if(start->flags & ActionFlags::Drawcall)
      passEvents.push_back(start->eventId);

    start = start->nextAction;
  }

  return passEvents;
}

///////////////////////////////////////////////////////////////////////////////
// drawing

ResourceId D3D9Replay::RenderOverlay(ResourceId texid, FloatVector clearCol, DebugOverlay overlay,
                                     uint32_t eventId, const rdcarray<uint32_t> &passEvents)
{
  switch(overlay)
  {
    // display modes of RenderTexture, not overlays
    case DebugOverlay::NoOverlay:
    case DebugOverlay::NaN:
    case DebugOverlay::Clipping: return ResourceId();
    // With no overlay texture the texture viewer shows the target as it is, and picks and saves
    // the target instead of the overlay.
    case DebugOverlay::QuadOverdrawDraw:
    case DebugOverlay::QuadOverdrawPass:
    {
      static bool warned = false;
      WarnOnce(warned,
               "The quad overdraw overlays aren't supported for D3D9 captures: they need "
               "UAVs, which D3D9 doesn't have");
      return ResourceId();
    }
    case DebugOverlay::TriangleSizeDraw:
    case DebugOverlay::TriangleSizePass:
    {
      // TODO: the triangle size overlays need the post-transform positions, which come with
      // GetPostVSBuffers (phase 3e mesh output). Draw them like D3D11 does once that's in.
      static bool warned = false;
      WarnOnce(warned, "The triangle size overlays aren't supported for D3D9 captures yet");
      return ResourceId();
    }
    default: break;
  }

  if(m_pDevice->FatalErrorCheck() != ResultCode::Succeeded)
  {
    m_Overlay.ReleaseTextures();
    return ResourceId();
  }

  if(!InitDebugRender() || !InitOverlayRender())
    return ResourceId();

  BoundTarget target;
  if(!FindBoundTarget(m_pDevice, texid, target))
  {
    RDCWARN("%s isn't bound as a target at event %u, there's no overlay for it",
            ToStr(texid).c_str(), eventId);
    return ResourceId();
  }

  const TextureDescription texDesc = GetTexture(texid);
  if(texDesc.resourceId == ResourceId())
    return ResourceId();

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  // the overlay texture has the target's size and mips, and is a cube if the target is one, so it
  // lines up with whichever subresource the texture viewer shows
  {
    const bool cube = texDesc.cubemap;
    const uint32_t width = RDCMAX(1U, texDesc.width);
    const uint32_t height = RDCMAX(1U, texDesc.height);
    const uint32_t mips = texDesc.type == TextureType::Texture2DMS ? 1U : RDCMAX(1U, texDesc.mips);

    if(m_Overlay.tex && (m_Overlay.cube != cube || m_Overlay.width != width ||
                         m_Overlay.height != height || m_Overlay.mips != mips))
    {
      SAFE_RELEASE(m_Overlay.tex);
      m_Overlay.id = ResourceId();
    }

    if(m_Overlay.tex == NULL)
    {
      // created through the wrapper so that it has a ResourceId and the texture viewer can
      // display, pick and save it like the application's textures
      HRESULT hr = S_OK;
      if(cube)
      {
        IDirect3DCubeTexture9 *tex = NULL;
        hr = m_pDevice->CreateCubeTexture(width, mips, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                          D3DPOOL_DEFAULT, &tex, NULL);
        m_Overlay.tex = tex;
      }
      else
      {
        IDirect3DTexture9 *tex = NULL;
        hr = m_pDevice->CreateTexture(width, height, mips, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                      D3DPOOL_DEFAULT, &tex, NULL);
        m_Overlay.tex = tex;
      }

      if(FAILED(hr) || m_Overlay.tex == NULL)
      {
        RDCERR("Couldn't create the %ux%u overlay texture: %s", width, height, ToStr(hr).c_str());
        m_Overlay.tex = NULL;
        return ResourceId();
      }

      m_Overlay.id = GetResID(m_Overlay.tex);
      m_Overlay.cube = cube;
      m_Overlay.width = width;
      m_Overlay.height = height;
      m_Overlay.mips = mips;
    }
  }

  // pick pixel's decoded copies of the previous overlay are out of date now
  for(auto it = m_Decoded.begin(); it != m_Decoded.end();)
  {
    if(it->first.id == m_Overlay.id)
    {
      SAFE_RELEASE(it->second.upload);
      it = m_Decoded.erase(it);
    }
    else
    {
      ++it;
    }
  }

  // every subresource starts out transparent. ColorFill doesn't touch the device's state.
  IDirect3DSurface9 *overlaySurf = NULL;
  {
    WrappedD3D9Object *wrapped = GetWrappedObject(m_Overlay.tex);
    const uint32_t faces = m_Overlay.cube ? 6 : 1;
    for(uint32_t face = 0; face < faces; face++)
    {
      for(uint32_t mip = 0; mip < m_Overlay.mips; mip++)
      {
        IDirect3DSurface9 *surf = NULL;
        if(m_Overlay.cube)
          ((WrappedD3DCubeTexture9 *)wrapped)
              ->GetReal()
              ->GetCubeMapSurface((D3DCUBEMAP_FACES)face, mip, &surf);
        else
          ((WrappedD3DTexture9 *)wrapped)->GetReal()->GetSurfaceLevel(mip, &surf);

        if(surf == NULL)
          continue;

        dev->ColorFill(surf, NULL, 0);

        if(face == RDCMIN(target.face, faces - 1) && mip == RDCMIN(target.mip, m_Overlay.mips - 1))
          overlaySurf = surf;
        else
          surf->Release();
      }
    }
  }

  if(overlaySurf == NULL)
  {
    RDCERR("Couldn't get the overlay texture's surface");
    return ResourceId();
  }

  ResourceId ret;

  if(overlay == DebugOverlay::ClearBeforeDraw || overlay == DebugOverlay::ClearBeforePass)
  {
    ret = RenderClearBeforeOverlay(texDesc, clearCol, overlay, eventId, passEvents);
  }
  else
  {
    const ActionDescription *action = m_pDevice->GetAction(eventId);
    if(action && (action->flags & ActionFlags::Drawcall))
    {
      ret = RenderDrawOverlay(target.surf, overlaySurf, overlay, eventId);
    }
    else
    {
      // nothing to draw again, only the background
      D3DCOLOR background = 0;
      if(overlay == DebugOverlay::Drawcall)
        background = ToD3DColor(0.0f, 0.0f, 0.0f, 0.5f);
      else if(overlay == DebugOverlay::Wireframe)
        background = ToD3DColor(200.0f / 255.0f, 1.0f, 0.0f, 0.0f);
      dev->ColorFill(overlaySurf, NULL, background);
      ret = m_Overlay.id;
    }
  }

  SAFE_RELEASE(overlaySurf);

  return ret;
}

ResourceId D3D9Replay::RenderClearBeforeOverlay(const TextureDescription &texDesc,
                                                FloatVector clearCol, DebugOverlay overlay,
                                                uint32_t eventId,
                                                const rdcarray<uint32_t> &passEvents)
{
  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  rdcarray<uint32_t> events;
  if(overlay == DebugOverlay::ClearBeforePass)
    events = passEvents;
  events.push_back(eventId);

  // the replay is at the event without its draw. For the pass, go back to its first draw.
  if(events.size() > 1)
    m_pDevice->ReplayLog(0, events[0], eReplay_WithoutDraw);

  // SaveReplayDrawState refuses while a state block is recorded, and our state calls would be
  // recorded. The pass is still replayed, to get back to the event.
  bool cleared = false;
  D3D9DeviceState state;
  if(m_pDevice->SaveReplayDrawState(state))
  {
    // Clear covers the viewport, and the scissor rect when it's enabled: clear all of the targets
    IDirect3DSurface9 *rt = NULL;
    D3DSURFACE_DESC rtDesc = {};
    if(SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt)
      rt->GetDesc(&rtDesc);
    SAFE_RELEASE(rt);

    IDirect3DSurface9 *ds = NULL;
    D3DSURFACE_DESC dsDesc = {};
    if(SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds)
      ds->GetDesc(&dsDesc);

    D3DVIEWPORT9 full = {0, 0, RDCMAX(rtDesc.Width, 1U), RDCMAX(rtDesc.Height, 1U), 0.0f, 1.0f};
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetViewport(&full);

    DWORD flags = D3DCLEAR_TARGET;
    float depth = 1.0f;

    // the depth too when it's the depth stencil being viewed, to help with shadow maps, as D3D11
    // does
    if(ds && (texDesc.creationFlags & TextureCategory::DepthTarget))
    {
      DWORD func = D3DCMP_LESSEQUAL;
      dev->GetRenderState(D3DRS_ZFUNC, &func);

      // EQUAL and NOTEQUAL would change what the draws do, and ALWAYS has no right answer
      if(func != D3DCMP_EQUAL && func != D3DCMP_NOTEQUAL && func != D3DCMP_ALWAYS)
      {
        flags |= D3DCLEAR_ZBUFFER;
        if(HasStencil(dsDesc.Format))
          flags |= D3DCLEAR_STENCIL;
        depth = (func == D3DCMP_LESS || func == D3DCMP_LESSEQUAL) ? 1.0f : 0.0f;
      }
    }
    SAFE_RELEASE(ds);

    HRESULT hr = dev->Clear(0, NULL, flags,
                            ToD3DColor(clearCol.x, clearCol.y, clearCol.z, clearCol.w), depth, 0);
    if(FAILED(hr))
      RDCWARN("Couldn't clear the targets for the clear before overlay: %s", ToStr(hr).c_str());

    m_pDevice->RestoreReplayDrawState(state);
    cleared = true;
  }

  // the pass's draws and what's between them, up to the event without its draw, which the replay
  // controller replays after the overlay
  for(size_t i = 0; i + 1 < events.size(); i++)
  {
    m_pDevice->ReplayLog(0, events[i], eReplay_OnlyDraw);
    m_pDevice->ReplayLog(events[i] + 1, events[i + 1], eReplay_WithoutDraw);
  }

  // the targets' contents changed
  ClearReplayCaches();

  // the overlay texture stays transparent
  return cleared ? m_Overlay.id : ResourceId();
}

ResourceId D3D9Replay::RenderDrawOverlay(IDirect3DSurface9 *targetSurf,
                                         IDirect3DSurface9 *overlaySurf, DebugOverlay overlay,
                                         uint32_t eventId)
{
  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  // The replay controller has replayed up to the event without its draw, so the targets are what
  // the draw tests against, and it replays the draw after the overlay. The state has to be the
  // same when we return.
  D3D9DeviceState state;
  if(!m_pDevice->SaveReplayDrawState(state))
  {
    // a state block is being recorded, so nothing can be drawn (SaveReplayDrawState warned)
    return ResourceId();
  }

  // the application's states the overlays use
  DWORD appCull = D3DCULL_CCW, appScissor = FALSE, appZEnable = D3DZB_TRUE, appStencil = FALSE;
  dev->GetRenderState(D3DRS_CULLMODE, &appCull);
  dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &appScissor);
  dev->GetRenderState(D3DRS_ZENABLE, &appZEnable);
  dev->GetRenderState(D3DRS_STENCILENABLE, &appStencil);

  D3DSURFACE_DESC targetDesc = {};
  targetSurf->GetDesc(&targetDesc);

  // the application's depth stencil stays bound. D3D9 needs it to be at least as big as the
  // target and to have the same multisample type, so we draw at the target's size and with the
  // depth stencil's multisample type.
  IDirect3DSurface9 *ds = NULL;
  dev->GetDepthStencilSurface(&ds);

  D3DMULTISAMPLE_TYPE msType = targetDesc.MultiSampleType;
  DWORD msQuality = targetDesc.MultiSampleQuality;
  if(ds)
  {
    D3DSURFACE_DESC dsDesc = {};
    ds->GetDesc(&dsDesc);
    if(dsDesc.Width < targetDesc.Width || dsDesc.Height < targetDesc.Height)
    {
      RDCWARN(
          "The depth stencil at event %u is smaller than the target, it's left out of the "
          "overlay",
          eventId);
      SAFE_RELEASE(ds);
    }
    else
    {
      msType = dsDesc.MultiSampleType;
      msQuality = dsDesc.MultiSampleQuality;
    }
  }

  IDirect3DSurface9 *drawTarget = overlaySurf;
  if(msType != D3DMULTISAMPLE_NONE)
  {
    if(m_Overlay.msTarget)
    {
      D3DSURFACE_DESC msDesc = {};
      m_Overlay.msTarget->GetDesc(&msDesc);
      if(msDesc.Width != targetDesc.Width || msDesc.Height != targetDesc.Height ||
         msDesc.MultiSampleType != msType || msDesc.MultiSampleQuality != msQuality)
        SAFE_RELEASE(m_Overlay.msTarget);
    }

    if(m_Overlay.msTarget == NULL)
    {
      HRESULT hr = dev->CreateRenderTarget(targetDesc.Width, targetDesc.Height, D3DFMT_A8R8G8B8,
                                           msType, msQuality, FALSE, &m_Overlay.msTarget, NULL);
      if(FAILED(hr))
      {
        RDCERR("Couldn't create a multisampled render target for the overlay: %s", ToStr(hr).c_str());
        m_Overlay.msTarget = NULL;
      }
    }

    drawTarget = m_Overlay.msTarget;
  }

  if(drawTarget == NULL)
  {
    SAFE_RELEASE(ds);
    m_pDevice->RestoreReplayDrawState(state);
    return ResourceId();
  }

  dev->SetRenderTarget(0, drawTarget);
  for(DWORD i = 1; i < D3D9_NUM_RENDER_TARGETS; i++)
    dev->SetRenderTarget(i, NULL);
  dev->SetDepthStencilSurface(ds);

  // every pass leaves the depth stencil alone, and writes the constant colour as it is
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

  // the background, over the whole target. SetRenderTarget set the viewport to all of it.
  {
    D3DCOLOR background = 0;
    if(overlay == DebugOverlay::Drawcall)
      background = ToD3DColor(0.0f, 0.0f, 0.0f, 0.5f);
    else if(overlay == DebugOverlay::Wireframe)
      background = ToD3DColor(200.0f / 255.0f, 1.0f, 0.0f, 0.0f);

    const D3DVIEWPORT9 full = {0, 0, targetDesc.Width, targetDesc.Height, 0.0f, 1.0f};
    dev->SetViewport(&full);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, background, 1.0f, 0);
  }

  // SetRenderTarget reset the viewport and scissor rect
  if(FAILED(dev->SetViewport(&state.Viewport)))
    RDCWARN("Couldn't set the viewport at event %u for its overlay", eventId);
  dev->SetScissorRect(&state.ScissorRect);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, appScissor);

  dev->SetPixelShader(BoundVertexShaderMajor(dev) >= 3 ? m_Overlay.colourPS3 : m_Overlay.colourPS2);

  auto drawPass = [&](float r, float g, float b, float a) {
    const float colour[4] = {r, g, b, a};
    dev->SetPixelShaderConstantF(0, colour, 1);
    m_pDevice->ReplayLog(0, eventId, eReplay_OnlyDraw);
  };

  const DWORD noDepth = D3DZB_FALSE;

  switch(overlay)
  {
    case DebugOverlay::Drawcall:
    {
      dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_ZENABLE, noDepth);
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
      drawPass(0.8f, 0.1f, 0.8f, 1.0f);
      break;
    }
    case DebugOverlay::Wireframe:
    {
      dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_ZENABLE, noDepth);
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      drawPass(200.0f / 255.0f, 1.0f, 0.0f, 1.0f);
      break;
    }
    case DebugOverlay::BackfaceCull:
    {
      dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
      dev->SetRenderState(D3DRS_ZENABLE, noDepth);
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);

      // everything in red, then what isn't culled in green
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      drawPass(1.0f, 0.0f, 0.0f, 1.0f);
      dev->SetRenderState(D3DRS_CULLMODE, appCull);
      drawPass(0.0f, 1.0f, 0.0f, 1.0f);
      break;
    }
    case DebugOverlay::ViewportScissor:
    {
      dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_ZENABLE, noDepth);
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);

      // the draw without the scissor test in red, then with it in green
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
      drawPass(1.0f, 0.0f, 0.0f, 1.0f);
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, appScissor);
      drawPass(0.0f, 1.0f, 0.0f, 1.0f);

      // then the viewport, and the scissor rect, as D3D11 shows them
      const bool ownScene = SUCCEEDED(dev->BeginScene());

      D3D9SetDebugDrawState(dev);
      dev->SetVertexShader(m_Overlay.quadVS);
      dev->SetPixelShader(m_Overlay.borderPS);
      dev->SetVertexDeclaration(m_Debug.quadDecl);
      dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

      auto drawRect = [&](const D3DVIEWPORT9 &view, const Vec4f &primary, const Vec4f &secondary,
                          const Vec4f &inner) {
        const float consts[5][4] = {
            {float(view.X), float(view.Y), float(view.Width), float(view.Height)},
            {3.0f, 16.0f, 0.0f, 0.0f},
            {primary.x, primary.y, primary.z, primary.w},
            {secondary.x, secondary.y, secondary.z, secondary.w},
            {inner.x, inner.y, inner.z, inner.w},
        };
        dev->SetPixelShaderConstantF(0, &consts[0][0], 5);
        dev->SetViewport(&view);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, ViewportQuad, sizeof(Vec4f));
      };

      D3DVIEWPORT9 view;
      const D3DVIEWPORT9 &vp = state.Viewport;
      // the same colour twice, for no checkerboard
      if(ClampedViewport((LONG)vp.X, (LONG)vp.Y, LONG(vp.X + vp.Width), LONG(vp.Y + vp.Height),
                         targetDesc.Width, targetDesc.Height, view))
        drawRect(view, Vec4f(0.1f, 0.1f, 0.1f, 1.0f), Vec4f(0.1f, 0.1f, 0.1f, 1.0f),
                 Vec4f(0.2f, 0.2f, 0.9f, 0.4f));

      // a black and white checkered border, nothing inside
      const RECT &sc = state.ScissorRect;
      if(appScissor && ClampedViewport(sc.left, sc.top, sc.right, sc.bottom, targetDesc.Width,
                                       targetDesc.Height, view))
        drawRect(view, Vec4f(1.0f, 1.0f, 1.0f, 1.0f), Vec4f(0.0f, 0.0f, 0.0f, 1.0f),
                 Vec4f(0.0f, 0.0f, 0.0f, 0.0f));

      if(ownScene)
        dev->EndScene();
      break;
    }
    case DebugOverlay::Depth:
    {
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);

      // everything in red, then what passes the depth test in green. Without a depth stencil (or
      // with the test off) everything passes. The depth bias states are the application's.
      dev->SetRenderState(D3DRS_ZENABLE, noDepth);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      drawPass(1.0f, 0.0f, 0.0f, 1.0f);
      dev->SetRenderState(D3DRS_ZENABLE, ds ? appZEnable : noDepth);
      dev->SetRenderState(D3DRS_CULLMODE, appCull);
      drawPass(0.0f, 1.0f, 0.0f, 1.0f);
      break;
    }
    case DebugOverlay::Stencil:
    {
      dev->SetRenderState(D3DRS_ZENABLE, noDepth);

      // everything in red, then what passes the stencil test in green, with the application's
      // stencil functions, reference and read mask
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      drawPass(1.0f, 0.0f, 0.0f, 1.0f);
      dev->SetRenderState(D3DRS_STENCILENABLE, ds ? appStencil : FALSE);
      dev->SetRenderState(D3DRS_CULLMODE, appCull);
      drawPass(0.0f, 1.0f, 0.0f, 1.0f);
      break;
    }
    default: RDCERR("Unexpected overlay %s", ToStr(overlay).c_str()); break;
  }

  // multisampled targets are resolved into the overlay texture
  if(drawTarget != overlaySurf)
  {
    HRESULT hr = dev->StretchRect(drawTarget, NULL, overlaySurf, NULL, D3DTEXF_NONE);
    if(FAILED(hr))
      RDCWARN("Couldn't resolve the multisampled overlay: %s", ToStr(hr).c_str());
  }

  SAFE_RELEASE(ds);

  // back to the state before the draw. The UP draws unbind stream 0 (as they do natively), which
  // the tracked bindings follow, but the draw doesn't use it.
  m_pDevice->RestoreReplayDrawState(state);

  return m_Overlay.id;
}
