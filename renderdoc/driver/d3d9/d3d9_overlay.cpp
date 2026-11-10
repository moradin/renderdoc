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
#include "d3d9_vs_emulate.h"

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
// * The pass and fail overlays (backface cull, viewport and scissor, depth and stencil) keep the
//   application's pixel shader and alpha test, unlike D3D11's, so that pixels the shader discards
//   (texkill, clip) or the alpha test rejects show as not drawn, and the depth test uses the
//   shader's oDepth. The shader's colour can't be replaced, so the passes only clear channels of
//   a white target, with ZERO/ZERO blending and the colour write mask: alpha where the draw drew,
//   red and green where it passed. ResolveMaskPasses then turns that into red and green with
//   blending. Highlight drawcall and wireframe still draw the whole primitive in a constant
//   colour, as on the other APIs.
// * The clear before pass and clear before draw overlays clear the application's targets and
//   replay the draws, so they change the targets themselves (the overlay texture stays
//   transparent), as on the other APIs. The replay controller replays again when they're turned
//   off.
// * Nothing is drawn at an event between BeginStateBlock and EndStateBlock, as in the texture
//   viewer: our state calls would be recorded into the application's state block.
// * The triangle size overlays have no geometry shader to work out each triangle's area, as
//   D3D11's do, so they're drawn from the CPU post-transform vertices (d3d9_postvs.cpp), expanded
//   to a triangle list with each triangle's area in a vertex attribute. The pixel shader maps the
//   area through D3D11's colour ramp itself: D3D11 writes the area and the texture display maps it
//   (HEATMAP_TRISIZE), but D3D9's texture display has no heatmaps.
// * Quad overdraw needs UAVs, which D3D9 doesn't have. It returns a transparent overlay, so the
//   texture viewer doesn't keep showing the previous one.

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

// the triangle size overlay: the clip space position, and the area in pixels of its triangle
const char *TriangleSizeVS = R"(
struct v2f
{
  float4 pos : POSITION;
  float area : TEXCOORD0;
};

v2f main(float4 pos : POSITION, float area : TEXCOORD0)
{
  v2f OUT;
  OUT.pos = pos;
  OUT.area = area;
  return OUT;
}
)";

// RENDERDOC_TriangleSizePS (mesh.hlsl) and the texture display's HEATMAP_TRISIZE (texdisplay.hlsl)
// in one: the area through the colour ramp.
// c0-c21: the colour ramp (colorRamp in replay_driver.cpp)
const char *TriangleSizePS = R"(
float4 ramp[22] : register(c0);

float4 main(float area : TEXCOORD0) : COLOR0
{
  area = max(area, 0.001f);

  float bucket = 2.0f + floor(20.0f - 20.1f * (1.0f - exp(-0.4f * area)));

  // ps_3_0 can only index constants with the loop counter
  float4 col = ramp[0];
  [unroll] for(int i = 1; i < 22; i++)
    col = (bucket == (float)i) ? ramp[i] : col;
  return col;
}
)";

// a vertex of the triangle size overlay, TriangleSizeVS's input
struct TriSizeVertex
{
  Vec4f pos;
  float area;
};

// The triangles of a draw's post-transform vertices as a list, each vertex with the area of its
// triangle, as RENDERDOC_TriangleSizeGS (mesh.hlsl) works it out: after the perspective divide, in
// pixels of the viewport. Pretransformed positions are replaced by the clip space positions that
// the viewport maps to them. Points and lines have no triangles, as D3D11's geometry shader takes
// triangles.
void TriangleSizeVertices(const D3D9Emu::PostVSResult &res, uint32_t posComps, Topology topo,
                          const D3DVIEWPORT9 &vp, rdcarray<TriSizeVertex> &out)
{
  const uint32_t rows = res.numRows;
  uint32_t numTris = 0;
  switch(topo)
  {
    case Topology::TriangleList: numTris = rows / 3; break;
    case Topology::TriangleStrip:
    case Topology::TriangleFan: numTris = rows >= 3 ? rows - 2 : 0; break;
    default: return;
  }

  if(numTris == 0 || res.stride < sizeof(float))
    return;

  const uint32_t numVerts = uint32_t(res.vertexData.size() / res.stride);
  posComps = RDCMIN(posComps, RDCMIN(4U, res.stride / (uint32_t)sizeof(float)));

  const float vpW = float(vp.Width), vpH = float(vp.Height);
  const float depthRange = vp.MaxZ - vp.MinZ;

  auto position = [&](uint32_t row, Vec4f &pos) -> bool {
    uint32_t v = row;
    if(res.indexed)
      v = row < res.indices.size() ? res.indices[row] : ~0U;
    if(v >= numVerts)
      return false;

    float p[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    memcpy(p, res.vertexData.data() + size_t(v) * res.stride, posComps * sizeof(float));

    if(res.clipSpace)
    {
      pos = Vec4f(p[0], p[1], p[2], p[3]);
      return true;
    }

    // pretransformed: x and y in pixels (pixel centres at integers, as the viewport maps clip
    // space), z the depth and the reciprocal of w
    const float w = (p[3] != 0.0f && RDCISFINITE(p[3])) ? 1.0f / p[3] : 1.0f;
    const float x = vpW > 0.0f ? (p[0] - float(vp.X)) / vpW * 2.0f - 1.0f : 0.0f;
    const float y = vpH > 0.0f ? 1.0f - (p[1] - float(vp.Y)) / vpH * 2.0f : 0.0f;
    const float z = depthRange != 0.0f ? (p[2] - vp.MinZ) / depthRange : 0.0f;
    pos = Vec4f(x * w, y * w, z * w, w);
    return true;
  };

  out.reserve(out.size() + numTris * 3);

  for(uint32_t t = 0; t < numTris; t++)
  {
    uint32_t idx[3];
    if(topo == Topology::TriangleList)
    {
      idx[0] = t * 3;
      idx[1] = t * 3 + 1;
      idx[2] = t * 3 + 2;
    }
    else if(topo == Topology::TriangleStrip)
    {
      // odd triangles swap their first two vertices, which keeps the winding, as a geometry
      // shader sees them
      idx[0] = (t & 1) ? t + 1 : t;
      idx[1] = (t & 1) ? t : t + 1;
      idx[2] = t + 2;
    }
    else
    {
      idx[0] = 0;
      idx[1] = t + 1;
      idx[2] = t + 2;
    }

    TriSizeVertex tri[3];
    if(!position(idx[0], tri[0].pos) || !position(idx[1], tri[1].pos) ||
       !position(idx[2], tri[2].pos))
      continue;

    float sx[3], sy[3];
    for(int i = 0; i < 3; i++)
    {
      sx[i] = (tri[i].pos.x / tri[i].pos.w * 0.5f + 0.5f) * vpW;
      sy[i] = (tri[i].pos.y / tri[i].pos.w * 0.5f + 0.5f) * vpH;
    }

    auto length = [&](int a, int b) {
      return sqrtf((sx[a] - sx[b]) * (sx[a] - sx[b]) + (sy[a] - sy[b]) * (sy[a] - sy[b]));
    };

    const float ab = length(0, 1);
    const float bc = length(1, 2);
    const float ca = length(2, 0);
    const float s = (ab + bc + ca) / 2.0f;
    float area = sqrtf(s * (s - ab) * (s - bc) * (s - ca));

    // the pixel shader takes max(area, 0.001), which turns NaN into 0.001 on D3D11. An infinite
    // area becomes a large one, so it interpolates.
    if(!(area >= 0.001f))
      area = 0.001f;
    area = RDCMIN(area, 1.0e30f);

    for(int i = 0; i < 3; i++)
    {
      tri[i].area = area;
      out.push_back(tri[i]);
    }
  }
}

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
  SAFE_RELEASE(triSizeVS);
  SAFE_RELEASE(triSizePS);
  SAFE_RELEASE(triSizeDecl);
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
  SAFE_RELEASE(triSizeVB);
  triSizeVBSize = 0;
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

  rdcarray<DWORD> ps2, ps3, vs, border, triVS, triPS;
  rdcstr err = D3D9CompileDebugShader(OverlayColourPS, "ps_2_0", 0, ps2);
  if(err.empty())
    err = D3D9CompileDebugShader(OverlayColourPS, "ps_3_0", 0, ps3);
  if(err.empty())
    err = D3D9CompileDebugShader(OverlayQuadVS, "vs_3_0", 0, vs);
  if(err.empty())
    err = D3D9CompileDebugShader(OverlayBorderPS, "ps_3_0", 0, border);
  if(err.empty())
    err = D3D9CompileDebugShader(TriangleSizeVS, "vs_3_0", 0, triVS);
  if(err.empty())
    err = D3D9CompileDebugShader(TriangleSizePS, "ps_3_0", 0, triPS);

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
  if(SUCCEEDED(hr))
    hr = dev->CreateVertexShader(triVS.data(), &m_Overlay.triSizeVS);
  if(SUCCEEDED(hr))
    hr = dev->CreatePixelShader(triPS.data(), &m_Overlay.triSizePS);

  RDCCOMPILE_ASSERT(sizeof(TriSizeVertex) == 20, "TriSizeVertex doesn't match its declaration");
  const D3DVERTEXELEMENT9 triSizeElements[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  if(SUCCEEDED(hr))
    hr = dev->CreateVertexDeclaration(triSizeElements, &m_Overlay.triSizeDecl);

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
    // Unsupported, so the overlay is left transparent. With no overlay texture at all the texture
    // viewer would keep showing the previous overlay.
    case DebugOverlay::QuadOverdrawDraw:
    case DebugOverlay::QuadOverdrawPass:
    {
      static bool warned = false;
      WarnOnce(warned,
               "The quad overdraw overlays aren't supported for D3D9 captures: they need "
               "UAVs, which D3D9 doesn't have");
      break;
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

  if(overlay == DebugOverlay::QuadOverdrawDraw || overlay == DebugOverlay::QuadOverdrawPass)
  {
    // transparent (see above)
    ret = m_Overlay.id;
  }
  else if(overlay == DebugOverlay::ClearBeforeDraw || overlay == DebugOverlay::ClearBeforePass)
  {
    ret = RenderClearBeforeOverlay(texDesc, clearCol, overlay, eventId, passEvents);
  }
  else if(overlay == DebugOverlay::TriangleSizeDraw || overlay == DebugOverlay::TriangleSizePass)
  {
    ret = RenderTriangleSizeOverlay(target.surf, overlaySurf, overlay, eventId, passEvents);
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
  DWORD appCull = D3DCULL_CCW, appScissor = FALSE, appZEnable = D3DZB_TRUE, appStencil = FALSE,
        appAlphaTest = FALSE;
  dev->GetRenderState(D3DRS_CULLMODE, &appCull);
  dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &appScissor);
  dev->GetRenderState(D3DRS_ZENABLE, &appZEnable);
  dev->GetRenderState(D3DRS_STENCILENABLE, &appStencil);
  dev->GetRenderState(D3DRS_ALPHATESTENABLE, &appAlphaTest);

  // The pass and fail overlays draw with the application's pixel shader and alpha test (see
  // DrawMaskPass), the others with a constant colour.
  const bool passFail = overlay == DebugOverlay::BackfaceCull ||
                        overlay == DebugOverlay::ViewportScissor ||
                        overlay == DebugOverlay::Depth || overlay == DebugOverlay::Stencil;

  D3DSURFACE_DESC targetDesc = {};
  targetSurf->GetDesc(&targetDesc);

  IDirect3DSurface9 *ds = NULL;
  IDirect3DSurface9 *drawTarget = OverlayDrawTarget(targetSurf, overlaySurf, eventId, ds);

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
    else if(passFail)
      // the passes clear channels of it, and ResolveMaskPasses turns it into the colours
      background = 0xFFFFFFFF;

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

  if(passFail)
  {
    // the application's pixel shader stays bound, and its alpha test on
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, appAlphaTest);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  }
  else
  {
    dev->SetPixelShader(BoundVertexShaderMajor(dev) >= 3 ? m_Overlay.colourPS3 : m_Overlay.colourPS2);
  }

  auto drawPass = [&](float r, float g, float b, float a) {
    const float colour[4] = {r, g, b, a};
    dev->SetPixelShaderConstantF(0, colour, 1);
    m_pDevice->ReplayLog(0, eventId, eReplay_OnlyDraw);
  };

  // what the draw writes, in the pass and fail overlays: everything it draws clears alpha, and
  // what passes the test clears red and green too (see ResolveMaskPasses)
  auto drawAll = [&]() {
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA);
    m_pDevice->ReplayLog(0, eventId, eReplay_OnlyDraw);
  };
  auto drawPassing = [&]() {
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN);
    m_pDevice->ReplayLog(0, eventId, eReplay_OnlyDraw);
    ResolveMaskPasses(targetDesc.Width, targetDesc.Height);
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
      drawAll();
      dev->SetRenderState(D3DRS_CULLMODE, appCull);
      drawPassing();
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
      drawAll();
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, appScissor);
      drawPassing();

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
      drawAll();
      dev->SetRenderState(D3DRS_ZENABLE, ds ? appZEnable : noDepth);
      dev->SetRenderState(D3DRS_CULLMODE, appCull);
      drawPassing();
      break;
    }
    case DebugOverlay::Stencil:
    {
      dev->SetRenderState(D3DRS_ZENABLE, noDepth);

      // everything in red, then what passes the stencil test in green, with the application's
      // stencil functions, reference and read mask
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      drawAll();
      dev->SetRenderState(D3DRS_STENCILENABLE, ds ? appStencil : FALSE);
      dev->SetRenderState(D3DRS_CULLMODE, appCull);
      drawPassing();
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

void D3D9Replay::ResolveMaskPasses(UINT width, UINT height)
{
  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  // The bound target started out white (1,1,1,1). Everything the draw drew has alpha 0, and what
  // passed has red and green 0 too:
  //   not drawn (1,1,1,1) -> transparent (0,0,0,0)
  //   failed    (1,1,1,0) -> red         (1,0,0,1)
  //   passed    (0,0,1,0) -> green       (0,1,0,1)
  // Blending gives each channel from its own value and alpha, in three full screen quads:
  //   red = R * (1 - A), green and blue = (1 - G or B) * (1 - A), alpha = 1 - A
  // Alpha changes last, since the others read it. On a multisampled target this works on each
  // sample, and the edges come out partly covered when they're resolved.
  const bool ownScene = SUCCEEDED(dev->BeginScene());

  D3D9SetDebugDrawState(dev);
  dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
  dev->SetVertexShader(m_Overlay.quadVS);
  dev->SetPixelShader(m_Overlay.colourPS3);
  dev->SetVertexDeclaration(m_Debug.quadDecl);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);

  const D3DVIEWPORT9 full = {0, 0, width, height, 0.0f, 1.0f};
  dev->SetViewport(&full);

  struct Step
  {
    DWORD mask;
    float colour;
    D3DBLEND src, dst;
    D3DBLENDOP op;
  };
  const Step steps[] = {
      // R * (1 - A)
      {D3DCOLORWRITEENABLE_RED, 0.0f, D3DBLEND_ZERO, D3DBLEND_INVDESTALPHA, D3DBLENDOP_ADD},
      // 1 * (1 - A) - G * (1 - A), and the same for blue
      {D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE, 1.0f, D3DBLEND_INVDESTALPHA,
       D3DBLEND_INVDESTALPHA, D3DBLENDOP_SUBTRACT},
      // 1 - A
      {D3DCOLORWRITEENABLE_ALPHA, 1.0f, D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_SUBTRACT},
  };

  for(const Step &step : steps)
  {
    const float colour[4] = {step.colour, step.colour, step.colour, step.colour};
    dev->SetPixelShaderConstantF(0, colour, 1);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, step.mask);
    dev->SetRenderState(D3DRS_SRCBLEND, step.src);
    dev->SetRenderState(D3DRS_DESTBLEND, step.dst);
    dev->SetRenderState(D3DRS_BLENDOP, step.op);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, ViewportQuad, sizeof(Vec4f));
  }

  if(ownScene)
    dev->EndScene();
}

IDirect3DSurface9 *D3D9Replay::OverlayDrawTarget(IDirect3DSurface9 *targetSurf,
                                                 IDirect3DSurface9 *overlaySurf, uint32_t eventId,
                                                 IDirect3DSurface9 *&ds)
{
  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  D3DSURFACE_DESC targetDesc = {};
  targetSurf->GetDesc(&targetDesc);

  // the application's depth stencil stays bound. D3D9 needs it to be at least as big as the
  // target and to have the same multisample type, so we draw at the target's size and with the
  // depth stencil's multisample type.
  ds = NULL;
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

  if(msType == D3DMULTISAMPLE_NONE)
    return overlaySurf;

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

  return m_Overlay.msTarget;
}

ResourceId D3D9Replay::RenderTriangleSizeOverlay(IDirect3DSurface9 *targetSurf,
                                                 IDirect3DSurface9 *overlaySurf,
                                                 DebugOverlay overlay, uint32_t eventId,
                                                 const rdcarray<uint32_t> &passEvents)
{
  rdcarray<uint32_t> events;
  if(overlay == DebugOverlay::TriangleSizePass)
    events = passEvents;
  events.push_back(eventId);

  // the replay is at the event without its draw. For the pass, go back to its first draw.
  if(events.size() > 1)
  {
    m_pDevice->ReplayLog(0, events[0], eReplay_WithoutDraw);
    ClearReplayCaches();
  }

  // the multisampled target drawn into, once it's been cleared, to resolve into the overlay
  IDirect3DSurface9 *msDrawn = NULL;

  for(size_t i = 0; i < events.size(); i++)
  {
    const ActionDescription *action = m_pDevice->GetAction(events[i]);
    if(action && (action->flags & ActionFlags::Drawcall))
    {
      // the draw's inputs are read from the device while it's at the event (unless they're
      // cached), and its vertices computed from them
      InitPostVSBuffers(events[i]);

      // as in RenderDrawOverlay, the state is put back before the draw is replayed. Nothing is
      // drawn while a state block is recorded (SaveReplayDrawState warned).
      D3D9DeviceState state;
      if(m_pDevice->SaveReplayDrawState(state))
      {
        IDirect3DSurface9 *ds = NULL;
        IDirect3DSurface9 *drawTarget = OverlayDrawTarget(targetSurf, overlaySurf, events[i], ds);
        if(drawTarget)
        {
          // a multisampled target starts out transparent, as the overlay texture does
          const bool clear = drawTarget != overlaySurf && drawTarget != msDrawn;
          if(drawTarget != overlaySurf)
            msDrawn = drawTarget;
          DrawTriangleSizes(events[i], state, drawTarget, ds, clear);
        }
        SAFE_RELEASE(ds);
        m_pDevice->RestoreReplayDrawState(state);
      }
    }

    // the pass's draws and what's between them, up to the event without its draw, which the
    // replay controller replays after the overlay
    if(i + 1 < events.size())
    {
      m_pDevice->ReplayLog(0, events[i], eReplay_OnlyDraw);
      m_pDevice->ReplayLog(events[i] + 1, events[i + 1], eReplay_WithoutDraw);
      // the post-transform data decodes vertex textures from what they hold at the event
      ClearReplayCaches();
    }
  }

  // multisampled targets are resolved into the overlay texture
  if(msDrawn && msDrawn == m_Overlay.msTarget)
  {
    HRESULT hr = m_pDevice->GetReal()->StretchRect(msDrawn, NULL, overlaySurf, NULL, D3DTEXF_NONE);
    if(FAILED(hr))
      RDCWARN("Couldn't resolve the multisampled overlay: %s", ToStr(hr).c_str());
  }

  return m_Overlay.id;
}

void D3D9Replay::DrawTriangleSizes(uint32_t eventId, const D3D9DeviceState &state,
                                   IDirect3DSurface9 *drawTarget, IDirect3DSurface9 *ds, bool clear)
{
  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  const ActionDescription *action = m_pDevice->GetAction(eventId);
  if(action == NULL)
    return;

  // every instance's triangles, drawn in order. Hardware instancing only applies to indexed
  // draws, as in the post-transform data. Instances without data are left out.
  rdcarray<TriSizeVertex> verts;
  const uint32_t numInstances =
      (action->flags & ActionFlags::Indexed) ? RDCMAX(1U, action->numInstances) : 1U;
  for(uint32_t inst = 0; inst < numInstances; inst++)
  {
    MeshFormat fmt = GetPostVSBuffers(eventId, inst, 0, MeshDataStage::VSOut);
    if(fmt.vertexResourceId == ResourceId())
      continue;

    const D3D9Emu::PostVSResult *res = GetPostVSResult(eventId, inst);
    if(res == NULL || !res->hasPosition)
      continue;

    TriangleSizeVertices(*res, fmt.format.compCount, fmt.topology, state.Viewport, verts);
  }

  if(verts.empty() && !clear)
    return;

  // the application's rasterizer states and depth and stencil tests, which D3D11 keeps too, with
  // the writes off
  const D3DRENDERSTATETYPE kept[] = {
      D3DRS_FILLMODE,          D3DRS_CULLMODE,
      D3DRS_ZENABLE,           D3DRS_ZFUNC,
      D3DRS_STENCILENABLE,     D3DRS_TWOSIDEDSTENCILMODE,
      D3DRS_DEPTHBIAS,         D3DRS_SLOPESCALEDEPTHBIAS,
      D3DRS_SCISSORTESTENABLE, D3DRS_MULTISAMPLEANTIALIAS,
  };
  DWORD keptValues[ARRAY_COUNT(kept)] = {};
  for(size_t i = 0; i < ARRAY_COUNT(kept); i++)
    dev->GetRenderState(kept[i], &keptValues[i]);

  dev->SetRenderTarget(0, drawTarget);
  dev->SetDepthStencilSurface(ds);

  D3D9SetDebugDrawState(dev);

  // SetRenderTarget set the viewport to all of the target, and the scissor test is off
  if(clear)
    dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);

  if(verts.empty())
    return;

  for(size_t i = 0; i < ARRAY_COUNT(kept); i++)
    dev->SetRenderState(kept[i], keptValues[i]);

  if(ds == NULL)
  {
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  }

  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
  dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
  dev->SetRenderState(D3DRS_CCW_STENCILPASS, D3DSTENCILOP_KEEP);

  if(FAILED(dev->SetViewport(&state.Viewport)))
    RDCWARN("Couldn't set the viewport at event %u for its overlay", eventId);
  dev->SetScissorRect(&state.ScissorRect);

  D3DCAPS9 caps = {};
  dev->GetDeviceCaps(&caps);
  // whole triangles, as many as one draw can have
  const uint32_t maxTris =
      RDCMAX(1U, RDCMIN((uint32_t)caps.MaxPrimitiveCount, (uint32_t)caps.MaxVertexIndex / 3U));
  const uint32_t numTris = uint32_t(verts.size() / 3);
  const uint32_t vbSize = RDCMIN(numTris, maxTris) * 3 * (uint32_t)sizeof(TriSizeVertex);

  if(m_Overlay.triSizeVB == NULL || m_Overlay.triSizeVBSize < vbSize)
  {
    SAFE_RELEASE(m_Overlay.triSizeVB);
    m_Overlay.triSizeVBSize = 0;
    HRESULT hr = dev->CreateVertexBuffer(vbSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                         D3DPOOL_DEFAULT, &m_Overlay.triSizeVB, NULL);
    if(FAILED(hr))
    {
      RDCERR("Couldn't create the triangle size overlay's vertex buffer: %s", ToStr(hr).c_str());
      m_Overlay.triSizeVB = NULL;
      return;
    }
    m_Overlay.triSizeVBSize = vbSize;
  }

  const bool ownScene = SUCCEEDED(dev->BeginScene());

  dev->SetVertexDeclaration(m_Overlay.triSizeDecl);
  dev->SetVertexShader(m_Overlay.triSizeVS);
  dev->SetPixelShader(m_Overlay.triSizePS);
  dev->SetPixelShaderConstantF(0, &colorRamp[0].x, ARRAY_COUNT(colorRamp));
  dev->SetStreamSource(0, m_Overlay.triSizeVB, 0, sizeof(TriSizeVertex));

  for(uint32_t first = 0; first < numTris; first += maxTris)
  {
    const uint32_t count = RDCMIN(maxTris, numTris - first);
    const UINT bytes = count * 3 * (UINT)sizeof(TriSizeVertex);

    void *dst = NULL;
    HRESULT hr = m_Overlay.triSizeVB->Lock(0, bytes, &dst, D3DLOCK_DISCARD);
    if(FAILED(hr) || dst == NULL)
    {
      RDCERR("Couldn't write the triangle size overlay's vertices: %s", ToStr(hr).c_str());
      break;
    }
    memcpy(dst, verts.data() + size_t(first) * 3, bytes);
    m_Overlay.triSizeVB->Unlock();

    dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, count);
  }

  if(ownScene)
    dev->EndScene();
}
