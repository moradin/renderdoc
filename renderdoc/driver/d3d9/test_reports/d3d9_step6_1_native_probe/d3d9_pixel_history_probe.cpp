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

// A temporary probe for pixel history (step 6.1): what the GPU does natively with the things pixel
// history will rely on. Built into demos_x64.exe for the run and taken out again. Writes probe.txt
// to PROBE_OUT. Sections:
//   A  occlusion queries scissored to one pixel
//   B  1x1 StretchRect copies of each render target format, and of multisampled targets
//   C  drawing part of a draw's primitives: the same pixels and values as the whole draw?
//   D  stencil INCR_SAT counting a pixel's fragments, and EQUAL picking fragment N
//   E  Clear, ColorFill and StretchRect against the scissor, viewport and write mask
//   F  depth clipping, D3DRS_CLIPPING and user clip planes
//   G  sRGB writes and blending
//   H  how long a query and a pixel readback take

#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <algorithm>
#include <functional>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "d3d9_test.h"

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

RD_TEST(D3D9_Pixel_History_Probe, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Probe of what pixel history relies on";

  // pretransformed, with a colour (flat shading picks a primitive's) and four texcoords
  struct Vert
  {
    float x, y, z, rhw;
    DWORD c;
    float t[4];
  };
  static const DWORD VertFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0);

  // clip space, for the vertex shader draws
  struct ClipVert
  {
    float p[4];
    float t[4];
  };

  FILE *out = NULL;
  pD3DAssemble assemble = NULL;
  uint32_t rng = 12345;

  IDirect3DSurface9Ptr backBuffer, backDepth;
  IDirect3DQuery9Ptr query;
  IDirect3DPixelShader9Ptr psT0, psV0, psC0, psKill;
  IDirect3DVertexShader9Ptr vsClip;
  IDirect3DVertexDeclaration9Ptr clipDecl;

  void P(const char *fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fflush(out);
  }

  uint32_t Rand()
  {
    rng = rng * 1664525u + 1013904223u;
    return rng;
  }
  float RandF(float lo, float hi) { return lo + (hi - lo) * float(Rand() >> 8) / float(1 << 24); }

  static DWORD Bits(float f)
  {
    DWORD b;
    memcpy(&b, &f, sizeof(b));
    return b;
  }

  IDirect3DPixelShader9Ptr PS(const char *src)
  {
    ID3DBlobPtr code, errors;
    IDirect3DPixelShader9Ptr ps;
    if(FAILED(assemble(src, strlen(src), NULL, NULL, NULL, 0, &code, &errors)) || !code)
    {
      P("# assemble failed: %s\n", errors ? (const char *)errors->GetBufferPointer() : "?");
      return ps;
    }
    if(FAILED(dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &ps)))
      P("# CreatePixelShader failed for %s\n", src);
    return ps;
  }

  IDirect3DVertexShader9Ptr VS(const char *src)
  {
    ID3DBlobPtr code, errors;
    IDirect3DVertexShader9Ptr vs;
    if(FAILED(assemble(src, strlen(src), NULL, NULL, NULL, 0, &code, &errors)) || !code)
    {
      P("# assemble failed: %s\n", errors ? (const char *)errors->GetBufferPointer() : "?");
      return vs;
    }
    if(FAILED(dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &vs)))
      P("# CreateVertexShader failed for %s\n", src);
    return vs;
  }

  // state every section starts from: no depth, stencil, blending, culling, alpha test, scissor
  void BaseState()
  {
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
    dev->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_POINTSIZE, Bits(1.0f));
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetVertexShader(NULL);
    dev->SetFVF(VertFVF);
    dev->SetPixelShader(psT0);
  }

  void SetViewport(IDirect3DSurface9 * rt)
  {
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
  }

  void Scissor(LONG x, LONG y, LONG w = 1, LONG h = 1)
  {
    RECT r = {x, y, x + w, y + h};
    dev->SetScissorRect(&r);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
  }

  DWORD Query(const std::function<void()> &draw)
  {
    query->Issue(D3DISSUE_BEGIN);
    draw();
    query->Issue(D3DISSUE_END);
    DWORD n = 0;
    while(query->GetData(&n, sizeof(n), D3DGETDATA_FLUSH) == S_FALSE)
    {
    }
    return n;
  }

  // a whole A32B32G32R32F target read back
  std::vector<float> ReadF(IDirect3DSurface9 * rt)
  {
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    std::vector<float> ret(d.Width * d.Height * 4, -12345.0f);
    IDirect3DSurface9Ptr src = rt, resolve, sys;
    dev->EndScene();
    if(d.MultiSampleType != D3DMULTISAMPLE_NONE)
    {
      dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolve,
                              NULL);
      dev->StretchRect(rt, NULL, resolve, NULL, D3DTEXF_NONE);
      src = resolve;
    }
    dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    D3DLOCKED_RECT lr = {};
    if(SUCCEEDED(dev->GetRenderTargetData(src, sys)) &&
       SUCCEEDED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
      for(UINT y = 0; y < d.Height; y++)
        memcpy(&ret[y * d.Width * 4], (const byte *)lr.pBits + y * lr.Pitch, d.Width * 16);
      sys->UnlockRect();
    }
    dev->BeginScene();
    return ret;
  }

  // the bytes of a whole surface of any format read back (non-multisampled)
  std::vector<byte> ReadBytes(IDirect3DSurface9 * rt, UINT bpp)
  {
    D3DSURFACE_DESC d;
    rt->GetDesc(&d);
    std::vector<byte> ret(d.Width * d.Height * bpp, 0xCD);
    IDirect3DSurface9Ptr sys;
    dev->EndScene();
    dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    D3DLOCKED_RECT lr = {};
    if(sys && SUCCEEDED(dev->GetRenderTargetData(rt, sys)) &&
       SUCCEEDED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
      for(UINT y = 0; y < d.Height; y++)
        memcpy(&ret[y * d.Width * bpp], (const byte *)lr.pBits + y * lr.Pitch, d.Width * bpp);
      sys->UnlockRect();
    }
    dev->BeginScene();
    return ret;
  }

  static int CountDiff(const std::vector<float> &a, const std::vector<float> &b)
  {
    int n = 0;
    for(size_t i = 0; i < a.size(); i += 4)
      if(memcmp(&a[i], &b[i], 16) != 0)
        n++;
    return n;
  }

  // pixels whose value isn't the clear colour (1, 0, 1, 1)
  static int Covered(const std::vector<float> &a)
  {
    int n = 0;
    for(size_t i = 0; i < a.size(); i += 4)
      if(!(a[i] == 1.0f && a[i + 1] == 0.0f && a[i + 2] == 1.0f && a[i + 3] == 1.0f))
        n++;
    return n;
  }

  static const DWORD ClearCol = D3DCOLOR_ARGB(255, 255, 0, 255);

  // a triangle covering the whole of any target here
  static void BigTri(Vert v[3], float t0, float z = 0.5f)
  {
    const float p[3][2] = {{-100.0f, -100.0f}, {400.0f, -100.0f}, {-100.0f, 400.0f}};
    for(int i = 0; i < 3; i++)
      v[i] = {p[i][0], p[i][1], z, 1.0f, D3DCOLOR_ARGB(255, 0, 0, 0), {t0, 0.0f, 0.0f, 1.0f}};
  }

  /////////////////////////////////////////////////////////////////////////////////////////////
  void SectionA()
  {
    P("\n== A: occlusion queries scissored to one pixel ==\n");
    IDirect3DSurface9Ptr rt;
    dev->CreateRenderTarget(16, 16, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(NULL);
    SetViewport(rt);
    BaseState();

    std::vector<Vert> tris(3 * 8);
    for(int k = 0; k < 8; k++)
      BigTri(&tris[3 * k], float(k));
    for(int k = 1; k <= 5; k++)
    {
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
      const DWORD all =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, k, tris.data(), sizeof(Vert)); });
      Scissor(5, 7);
      const DWORD one =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, k, tris.data(), sizeof(Vert)); });
      dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
      const DWORD noWrite =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, k, tris.data(), sizeof(Vert)); });
      dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
      P("  %d covering triangles: %u unscissored (16x16), %u scissored to (5, 7), %u with "
        "COLORWRITEENABLE 0\n",
        k, all, one, noWrite);
    }

    // a triangle covering half of pixel (5, 7)'s neighbourhood: the pixel's centre exactly on an
    // edge (top-left rule), and just either side of it
    for(float e : {6.99f, 7.0f, 7.01f})
    {
      Vert v[3];
      BigTri(v, 1.0f);
      v[0].y = v[1].y = e;
      Scissor(5, 7);
      const DWORD n = Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      P("  a triangle whose top edge is at y %.2f, pixel (5, 7) centre at 7.0: %u\n", e, n);
    }

    // alpha test and texkill
    Scissor(5, 7);
    {
      Vert v[3];
      BigTri(v, 1.0f);
      for(Vert &vt : v)
        vt.t[3] = 0.25f;
      dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
      dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
      dev->SetRenderState(D3DRS_ALPHAREF, 0x80);
      const DWORD fail =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      for(Vert &vt : v)
        vt.t[3] = 0.75f;
      const DWORD pass =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      for(Vert &vt : v)
        vt.t[0] = -1.0f;
      dev->SetPixelShader(psKill);
      const DWORD kill =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      for(Vert &vt : v)
        vt.t[0] = 1.0f;
      const DWORD keep =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      dev->SetPixelShader(psT0);
      P("  alpha test failing %u, passing %u; texkill killing %u, keeping %u\n", fail, pass, kill,
        keep);
    }

    // culling, and a fixed function draw (no pixel shader)
    {
      Vert v[3];
      BigTri(v, 1.0f);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
      const DWORD cw =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
      const DWORD ccw =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetPixelShader(NULL);
      const DWORD ffp =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      dev->SetPixelShader(psT0);
      P("  the triangle (clockwise on screen) with CULL_CW %u, CULL_CCW %u; fixed function %u\n",
        cw, ccw, ffp);
    }

    // multisampled, fully covered and edge pixels
    IDirect3DSurface9Ptr ms;
    dev->CreateRenderTarget(16, 16, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &ms,
                            NULL);
    if(ms)
    {
      dev->SetRenderTarget(0, ms);
      Vert v[3];
      BigTri(v, 1.0f);
      Scissor(5, 7);
      const DWORD full =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 0x5);
      const DWORD masked =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
      dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
      const DWORD noAA =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
      v[0].y = v[1].y = 7.0f;
      const DWORD edge =
          Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
      P("  4x multisampled: fully covered %u, MULTISAMPLEMASK 0x5 %u, MULTISAMPLEANTIALIAS FALSE "
        "%u, top edge on the centre %u\n",
        full, masked, noAA, edge);
    }
    dev->SetRenderTarget(0, backBuffer);
  }

  /////////////////////////////////////////////////////////////////////////////////////////////
  struct Fmt
  {
    D3DFORMAT f;
    const char *name;
    UINT bpp;
  };

  void SectionB()
  {
    P("\n== B: 1x1 StretchRect of each render target format ==\n");
    const Fmt fmts[] = {
        {D3DFMT_A8R8G8B8, "A8R8G8B8", 4},
        {D3DFMT_X8R8G8B8, "X8R8G8B8", 4},
        {D3DFMT_A8B8G8R8, "A8B8G8R8", 4},
        {D3DFMT_X8B8G8R8, "X8B8G8R8", 4},
        {D3DFMT_R5G6B5, "R5G6B5", 2},
        {D3DFMT_X1R5G5B5, "X1R5G5B5", 2},
        {D3DFMT_A1R5G5B5, "A1R5G5B5", 2},
        {D3DFMT_A4R4G4B4, "A4R4G4B4", 2},
        {D3DFMT_X4R4G4B4, "X4R4G4B4", 2},
        {D3DFMT_R3G3B2, "R3G3B2", 1},
        {D3DFMT_A8R3G3B2, "A8R3G3B2", 2},
        {D3DFMT_A2R10G10B10, "A2R10G10B10", 4},
        {D3DFMT_A2B10G10R10, "A2B10G10R10", 4},
        {D3DFMT_A16B16G16R16, "A16B16G16R16", 8},
        {D3DFMT_G16R16, "G16R16", 4},
        {D3DFMT_A8, "A8", 1},
        {D3DFMT_L8, "L8", 1},
        {D3DFMT_A8L8, "A8L8", 2},
        {D3DFMT_L16, "L16", 2},
        {D3DFMT_R16F, "R16F", 2},
        {D3DFMT_G16R16F, "G16R16F", 4},
        {D3DFMT_A16B16G16R16F, "A16B16G16R16F", 8},
        {D3DFMT_R32F, "R32F", 4},
        {D3DFMT_G32R32F, "G32R32F", 8},
        {D3DFMT_A32B32G32R32F, "A32B32G32R32F", 16},
    };
    const UINT W = 16, H = 16;
    for(const Fmt &f : fmts)
    {
      const HRESULT surf = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                  D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
                                                  D3DRTYPE_SURFACE, f.f);
      const HRESULT tex = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                 D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
                                                 D3DRTYPE_TEXTURE, f.f);
      if(FAILED(surf) && FAILED(tex))
      {
        P("  %-14s not a render target here\n", f.name);
        continue;
      }
      IDirect3DSurface9Ptr rt, one, sys, sysOne;
      IDirect3DTexture9Ptr rtTex;
      HRESULT hr = dev->CreateRenderTarget(W, H, f.f, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
      const char *kind = "surface";
      if(FAILED(hr))
      {
        hr = dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, f.f, D3DPOOL_DEFAULT, &rtTex, NULL);
        if(SUCCEEDED(hr))
          rtTex->GetSurfaceLevel(0, &rt);
        kind = "texture";
      }
      IDirect3DTexture9Ptr oneTex;
      const char *step = "the render target";
      if(SUCCEEDED(hr))
      {
        step = "the 1x1 target";
        hr = dev->CreateRenderTarget(1, 1, f.f, D3DMULTISAMPLE_NONE, 0, FALSE, &one, NULL);
        if(FAILED(hr))
        {
          hr = dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, f.f, D3DPOOL_DEFAULT, &oneTex,
                                  NULL);
          if(SUCCEEDED(hr))
            oneTex->GetSurfaceLevel(0, &one);
        }
      }
      if(SUCCEEDED(hr))
      {
        step = "a system memory surface";
        hr = dev->CreateOffscreenPlainSurface(W, H, f.f, D3DPOOL_SYSTEMMEM, &sys, NULL);
      }
      if(SUCCEEDED(hr))
        hr = dev->CreateOffscreenPlainSurface(1, 1, f.f, D3DPOOL_SYSTEMMEM, &sysOne, NULL);
      if(FAILED(hr))
      {
        P("  %-14s couldn't create %s: %08x\n", f.name, step, (UINT)hr);
        continue;
      }
      // random bits, then a few float specials in the float formats
      std::vector<byte> src(W * H * f.bpp);
      for(byte &b : src)
        b = byte(Rand() >> 24);
      if(f.f == D3DFMT_A32B32G32R32F || f.f == D3DFMT_R32F || f.f == D3DFMT_G32R32F)
      {
        const DWORD specials[] = {0x7FC00000, 0xFFC00001, 0x7F800000, 0xFF800000,
                                  0x00000001, 0x807FFFFF, 0x80000000, 0x00000000};
        memcpy(src.data(), specials, sizeof(specials));
      }
      if(f.f == D3DFMT_A16B16G16R16F || f.f == D3DFMT_R16F || f.f == D3DFMT_G16R16F)
      {
        const WORD specials[] = {0x7E00, 0xFE01, 0x7C00, 0xFC00, 0x0001, 0x83FF, 0x8000, 0x0000};
        memcpy(src.data(), specials, sizeof(specials));
      }
      D3DLOCKED_RECT lr = {};
      sys->LockRect(&lr, NULL, 0);
      for(UINT y = 0; y < H; y++)
        memcpy((byte *)lr.pBits + y * lr.Pitch, &src[y * W * f.bpp], W * f.bpp);
      sys->UnlockRect();
      dev->EndScene();
      const HRESULT up = dev->UpdateSurface(sys, NULL, rt, NULL);
      dev->BeginScene();
      const std::vector<byte> whole = ReadBytes(rt, f.bpp);
      int wholeDiff = 0;
      for(UINT i = 0; i < W * H; i++)
        wholeDiff += memcmp(&whole[i * f.bpp], &src[i * f.bpp], f.bpp) != 0;

      int copyDiff = 0, failed = 0;
      std::string examples;
      for(int filter = 0; filter < 2; filter++)
      {
        int diff = 0;
        for(UINT y = 0; y < H; y++)
        {
          for(UINT x = 0; x < W; x++)
          {
            RECT r = {LONG(x), LONG(y), LONG(x + 1), LONG(y + 1)};
            dev->EndScene();
            HRESULT h = dev->StretchRect(rt, &r, one, NULL, filter ? D3DTEXF_POINT : D3DTEXF_NONE);
            if(SUCCEEDED(h))
              h = dev->GetRenderTargetData(one, sysOne);
            byte v[16] = {};
            if(SUCCEEDED(h) && SUCCEEDED(sysOne->LockRect(&lr, NULL, D3DLOCK_READONLY)))
            {
              memcpy(v, lr.pBits, f.bpp);
              sysOne->UnlockRect();
            }
            dev->BeginScene();
            if(FAILED(h))
            {
              failed++;
              continue;
            }
            const byte *want = &whole[(y * W + x) * f.bpp];
            if(memcmp(v, want, f.bpp) != 0)
            {
              diff++;
              if(examples.size() < 300)
              {
                char buf[128];
                std::string a, b;
                for(UINT i = 0; i < f.bpp; i++)
                {
                  sprintf(buf, "%02x", want[f.bpp - 1 - i]);
                  a += buf;
                  sprintf(buf, "%02x", v[f.bpp - 1 - i]);
                  b += buf;
                }
                sprintf(buf, " (%u,%u) %s->%s", x, y, a.c_str(), b.c_str());
                examples += buf;
              }
            }
          }
        }
        if(filter == 0)
          copyDiff = diff;
        else
          P("  %-14s %s: UpdateSurface %08x, read back whole %d of %u differ from what was "
            "uploaded; 1x1 copies (NONE) %d differ, (POINT) %d differ, %d failed%s%s\n",
            f.name, kind, (UINT)up, wholeDiff, W * H, copyDiff, diff, failed,
            examples.empty() ? "" : "; e.g.", examples.c_str());
      }
    }

    // a mip level and a cube face of render target textures, A8R8G8B8
    {
      IDirect3DTexture9Ptr tex;
      IDirect3DCubeTexture9Ptr cube;
      dev->CreateTexture(32, 32, 2, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex,
                         NULL);
      dev->CreateCubeTexture(16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube,
                             NULL);
      IDirect3DSurface9Ptr mip1, face, one, sysOne, sys;
      if(tex)
        tex->GetSurfaceLevel(1, &mip1);
      if(cube)
        cube->GetCubeMapSurface(D3DCUBEMAP_FACE_NEGATIVE_Y, 0, &face);
      dev->CreateRenderTarget(1, 1, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &one, NULL);
      dev->CreateOffscreenPlainSurface(1, 1, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysOne, NULL);
      dev->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
      for(IDirect3DSurface9 *s : {mip1.GetInterfacePtr(), face.GetInterfacePtr()})
      {
        if(!s)
          continue;
        D3DLOCKED_RECT lr = {};
        sys->LockRect(&lr, NULL, 0);
        for(UINT y = 0; y < 16; y++)
          for(UINT x = 0; x < 16; x++)
            ((DWORD *)((byte *)lr.pBits + y * lr.Pitch))[x] = Rand();
        sys->UnlockRect();
        dev->EndScene();
        dev->UpdateSurface(sys, NULL, s, NULL);
        dev->BeginScene();
        const std::vector<byte> whole = ReadBytes(s, 4);
        int diff = 0;
        for(UINT y = 0; y < 16; y++)
          for(UINT x = 0; x < 16; x++)
          {
            RECT r = {LONG(x), LONG(y), LONG(x + 1), LONG(y + 1)};
            dev->EndScene();
            dev->StretchRect(s, &r, one, NULL, D3DTEXF_NONE);
            dev->GetRenderTargetData(one, sysOne);
            sysOne->LockRect(&lr, NULL, D3DLOCK_READONLY);
            diff += memcmp(lr.pBits, &whole[(y * 16 + x) * 4], 4) != 0;
            sysOne->UnlockRect();
            dev->BeginScene();
          }
        P("  A8R8G8B8 %s: 1x1 copies %d of 256 differ\n",
          s == mip1.GetInterfacePtr() ? "texture mip 1" : "cube face -Y", diff);
      }
    }

    // multisampled: each pixel's 1x1 resolve against the whole target's resolve
    for(D3DFORMAT f : {D3DFMT_A8R8G8B8, D3DFMT_A32B32G32R32F})
    {
      IDirect3DSurface9Ptr ms, resolve, one, sysOne;
      const UINT bpp = f == D3DFMT_A8R8G8B8 ? 4 : 16;
      dev->CreateRenderTarget(16, 16, f, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &ms, NULL);
      dev->CreateRenderTarget(16, 16, f, D3DMULTISAMPLE_NONE, 0, FALSE, &resolve, NULL);
      dev->CreateRenderTarget(1, 1, f, D3DMULTISAMPLE_NONE, 0, FALSE, &one, NULL);
      dev->CreateOffscreenPlainSurface(1, 1, f, D3DPOOL_SYSTEMMEM, &sysOne, NULL);
      if(!ms || !resolve || !one || !sysOne)
      {
        P("  4x %s: couldn't create\n", f == D3DFMT_A8R8G8B8 ? "A8R8G8B8" : "A32B32G32R32F");
        continue;
      }
      // triangles with edges through pixels, so pixels are partly covered
      dev->SetRenderTarget(0, ms);
      dev->SetDepthStencilSurface(NULL);
      SetViewport(ms);
      BaseState();
      dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 20, 40, 60), 1.0f, 0);
      std::vector<Vert> v(3 * 6);
      for(Vert &vt : v)
        vt = {RandF(-4.0f, 20.0f), RandF(-4.0f, 20.0f), 0.5f, 1.0f, 0,
              {RandF(0, 1), RandF(0, 1), RandF(0, 1), RandF(0, 1)}};
      dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 6, v.data(), sizeof(Vert));
      dev->EndScene();
      HRESULT whole = dev->StretchRect(ms, NULL, resolve, NULL, D3DTEXF_NONE);
      dev->BeginScene();
      const std::vector<byte> ref = ReadBytes(resolve, bpp);
      int diff = 0, failed = 0;
      for(UINT y = 0; y < 16; y++)
        for(UINT x = 0; x < 16; x++)
        {
          RECT r = {LONG(x), LONG(y), LONG(x + 1), LONG(y + 1)};
          dev->EndScene();
          HRESULT h = dev->StretchRect(ms, &r, one, NULL, D3DTEXF_NONE);
          if(SUCCEEDED(h))
            h = dev->GetRenderTargetData(one, sysOne);
          D3DLOCKED_RECT lr = {};
          if(SUCCEEDED(h) && SUCCEEDED(sysOne->LockRect(&lr, NULL, D3DLOCK_READONLY)))
          {
            diff += memcmp(lr.pBits, &ref[(y * 16 + x) * bpp], bpp) != 0;
            sysOne->UnlockRect();
          }
          else
            failed++;
          dev->BeginScene();
        }
      P("  4x %s: whole resolve %08x; 1x1 resolves %d of 256 differ from it, %d failed\n",
        f == D3DFMT_A8R8G8B8 ? "A8R8G8B8" : "A32B32G32R32F", (UINT)whole, diff, failed);
      dev->SetRenderTarget(0, backBuffer);
    }

    // inside a scene
    {
      IDirect3DSurface9Ptr rt, one;
      dev->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
      dev->CreateRenderTarget(1, 1, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &one, NULL);
      RECT r = {3, 3, 4, 4};
      const HRESULT h = dev->StretchRect(rt, &r, one, NULL, D3DTEXF_NONE);
      P("  StretchRect inside BeginScene/EndScene: %08x\n", (UINT)h);
    }
  }

  /////////////////////////////////////////////////////////////////////////////////////////////
  // C: a draw against each of its primitives drawn alone, in order
  IDirect3DSurface9Ptr rtC;

  void ClearC() { dev->Clear(0, NULL, D3DCLEAR_TARGET, ClearCol, 1.0f, 0); }

  void Compare(const char *what, const std::function<void()> &whole,
               const std::function<void(int)> &one, int count, const char *how)
  {
    ClearC();
    whole();
    const std::vector<float> a = ReadF(rtC);
    ClearC();
    for(int p = 0; p < count; p++)
      one(p);
    const std::vector<float> b = ReadF(rtC);
    P("  %-44s %3d primitives cover %4d pixels; each alone (%s): %d pixels differ\n", what, count,
      Covered(a), how, CountDiff(a, b));
  }

  std::vector<Vert> RandVerts(int n, float size)
  {
    std::vector<Vert> v(n);
    float cx = 0, cy = 0;
    for(int i = 0; i < n; i++)
    {
      if(i % 3 == 0)
      {
        cx = RandF(0.0f, 32.0f);
        cy = RandF(0.0f, 32.0f);
      }
      v[i] = {cx + RandF(-size, size),
              cy + RandF(-size, size),
              0.5f,
              1.0f,
              D3DCOLOR_ARGB(255, i & 0xFF, (i >> 8) & 0xFF, 0),
              {RandF(0, 1), RandF(0, 1), RandF(0, 1), RandF(0, 1)}};
    }
    return v;
  }

  void SectionC()
  {
    P("\n== C: a draw's primitives drawn alone, in order, against the whole draw ==\n");
    dev->CreateRenderTarget(32, 32, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rtC,
                            NULL);
    dev->SetRenderTarget(0, rtC);
    dev->SetDepthStencilSurface(NULL);
    SetViewport(rtC);
    BaseState();
    const int N = 40;
    const UINT S = sizeof(Vert);

    for(int cull = 0; cull < 2; cull++)
    {
      dev->SetRenderState(D3DRS_CULLMODE, cull ? D3DCULL_CCW : D3DCULL_NONE);
      P(" cull %s:\n", cull ? "CCW" : "none");

      // lists, strips and fans from a vertex buffer
      std::vector<Vert> list = RandVerts(3 * N, 10.0f);
      std::vector<Vert> strip = RandVerts(N + 2, 16.0f);
      std::vector<Vert> fan = RandVerts(N + 2, 16.0f);
      IDirect3DVertexBuffer9Ptr vbList = MakeVB(list.data(), UINT(list.size() * S));
      IDirect3DVertexBuffer9Ptr vbStrip = MakeVB(strip.data(), UINT(strip.size() * S));
      IDirect3DVertexBuffer9Ptr vbFan = MakeVB(fan.data(), UINT(fan.size() * S));

      dev->SetStreamSource(0, vbList, 0, S);
      Compare("DrawPrimitive TRIANGLELIST", [&]() { dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, N); },
              [&](int p) { dev->DrawPrimitive(D3DPT_TRIANGLELIST, 3 * p, 1); }, N, "start 3p");
      // prefixes: the first m primitives alone, against those of the whole draw drawn alone
      {
        int worst = 0;
        for(int m = 1; m <= N; m += 7)
        {
          ClearC();
          dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, m);
          const std::vector<float> a = ReadF(rtC);
          ClearC();
          for(int p = 0; p < m; p++)
            dev->DrawPrimitive(D3DPT_TRIANGLELIST, 3 * p, 1);
          worst = std::max(worst, CountDiff(a, ReadF(rtC)));
        }
        P("  %-44s prefixes of 1, 8, 15... primitives: at most %d pixels differ\n",
          "DrawPrimitive TRIANGLELIST", worst);
      }

      dev->SetStreamSource(0, vbStrip, 0, S);
      Compare("DrawPrimitive TRIANGLESTRIP", [&]() { dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, N); },
              [&](int p) { dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, p, 1); }, N, "start p");
      Compare("DrawPrimitive TRIANGLESTRIP, odd ones reversed",
              [&]() { dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, N); },
              [&](int p) {
                if(p % 2 == 0)
                {
                  dev->SetStreamSource(0, vbStrip, 0, S);
                  dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, p, 1);
                }
                else
                {
                  // the strip's odd triangle is (p+1, p, p+2) in D3D9's winding order
                  Vert v[3] = {strip[p + 1], strip[p], strip[p + 2]};
                  dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, S);
                }
              },
              N, "odd as UP lists p+1, p, p+2");
      dev->SetStreamSource(0, vbStrip, 0, S);
      {
        int worst = 0;
        for(int m = 1; m <= N; m += 7)
        {
          ClearC();
          dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, m);
          const std::vector<float> a = ReadF(rtC);
          ClearC();
          for(int p = 0; p < m; p++)
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, p, 1);
          worst = std::max(worst, CountDiff(a, ReadF(rtC)));
        }
        P("  %-44s prefixes of 1, 8, 15... primitives: at most %d pixels differ\n",
          "DrawPrimitive TRIANGLESTRIP (each alone from p)", worst);
      }

      dev->SetStreamSource(0, vbFan, 0, S);
      Compare("DrawPrimitive TRIANGLEFAN", [&]() { dev->DrawPrimitive(D3DPT_TRIANGLEFAN, 0, N); },
              [&](int p) {
                Vert v[3] = {fan[0], fan[p + 1], fan[p + 2]};
                dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, S);
              },
              N, "UP lists 0, p+1, p+2");

      // indexed, with a base vertex and indices in a random order
      std::vector<WORD> idx(3 * N);
      for(int i = 0; i < 3 * N; i++)
        idx[i] = WORD(Rand() % (3 * N));
      IDirect3DIndexBuffer9Ptr ib;
      dev->CreateIndexBuffer(UINT(idx.size() * 2), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL);
      void *ptr = NULL;
      ib->Lock(0, 0, &ptr, 0);
      memcpy(ptr, idx.data(), idx.size() * 2);
      ib->Unlock();
      std::vector<Vert> listPad(5);
      listPad.insert(listPad.end(), list.begin(), list.end());
      IDirect3DVertexBuffer9Ptr vbPad = MakeVB(listPad.data(), UINT(listPad.size() * S));
      dev->SetStreamSource(0, vbPad, 0, S);
      dev->SetIndices(ib);
      Compare("DrawIndexedPrimitive TRIANGLELIST, base 5",
              [&]() { dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 5, 0, 3 * N, 0, N); },
              [&](int p) { dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 5, 0, 3 * N, 3 * p, 1); },
              N, "start index 3p");
      Compare("DrawIndexedPrimitive TRIANGLESTRIP, base 5",
              [&]() { dev->DrawIndexedPrimitive(D3DPT_TRIANGLESTRIP, 5, 0, 3 * N, 0, N); },
              [&](int p) { dev->DrawIndexedPrimitive(D3DPT_TRIANGLESTRIP, 5, 0, 3 * N, p, 1); }, N,
              "start index p");

      // the UP draws
      Compare("DrawPrimitiveUP TRIANGLELIST",
              [&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, N, list.data(), S); },
              [&](int p) { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, &list[3 * p], S); }, N,
              "pointer + 3p");
      Compare("DrawIndexedPrimitiveUP TRIANGLELIST",
              [&]() {
                dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3 * N, N, idx.data(),
                                            D3DFMT_INDEX16, list.data(), S);
              },
              [&](int p) {
                dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3 * N, 1, &idx[3 * p],
                                            D3DFMT_INDEX16, list.data(), S);
              },
              N, "indices + 3p");

      if(cull == 0)
      {
        // lines and points
        dev->SetStreamSource(0, vbList, 0, S);
        Compare("DrawPrimitive LINELIST", [&]() { dev->DrawPrimitive(D3DPT_LINELIST, 0, N); },
                [&](int p) { dev->DrawPrimitive(D3DPT_LINELIST, 2 * p, 1); }, N, "start 2p");
        Compare("DrawPrimitive LINESTRIP", [&]() { dev->DrawPrimitive(D3DPT_LINESTRIP, 0, N); },
                [&](int p) { dev->DrawPrimitive(D3DPT_LINESTRIP, p, 1); }, N, "start p");
        Compare("DrawPrimitive POINTLIST", [&]() { dev->DrawPrimitive(D3DPT_POINTLIST, 0, N); },
                [&](int p) { dev->DrawPrimitive(D3DPT_POINTLIST, p, 1); }, N, "start p");
        dev->SetRenderState(D3DRS_POINTSPRITEENABLE, TRUE);
        dev->SetRenderState(D3DRS_POINTSIZE, Bits(5.0f));
        Compare("DrawPrimitive POINTLIST, sprites of 5", [&]() { dev->DrawPrimitive(D3DPT_POINTLIST, 0, N); },
                [&](int p) { dev->DrawPrimitive(D3DPT_POINTLIST, p, 1); }, N, "start p");
        dev->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_POINTSIZE, Bits(1.0f));

        // which vertex flat shading takes: v0 is the colour, whose red is the vertex's index. A
        // 2-triangle strip and fan over the target; a pixel inside each triangle.
        dev->SetPixelShader(psV0);
        dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);
        {
          auto vtx = [](float x, float y, int i) {
            Vert v = {x, y, 0.5f, 1.0f, D3DCOLOR_ARGB(255, i, 0, 0), {0, 0, 0, 1}};
            return v;
          };
          const Vert fl[3] = {vtx(-0.5f, -0.5f, 0), vtx(31.5f, -0.5f, 1), vtx(-0.5f, 31.5f, 2)};
          const Vert fs[4] = {vtx(-0.5f, -0.5f, 0), vtx(31.5f, -0.5f, 1), vtx(-0.5f, 31.5f, 2),
                                 vtx(31.5f, 31.5f, 3)};
          const Vert ff[4] = {vtx(-0.5f, -0.5f, 0), vtx(31.5f, -0.5f, 1), vtx(31.5f, 31.5f, 2),
                               vtx(-0.5f, 31.5f, 3)};
          auto at = [&](int x, int y) {
            const std::vector<float> img = ReadF(rtC);
            return int(img[(y * 32 + x) * 4] * 255.0f + 0.5f);
          };
          ClearC();
          dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, fl, S);
          const int l0 = at(4, 4);
          ClearC();
          dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, fs, S);
          const int s0 = at(4, 4), s1 = at(28, 28);
          ClearC();
          dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, ff, S);
          const int f0 = at(28, 4), f1 = at(4, 28);
          P("  flat shading takes vertex: list triangle 0 -> %d; strip triangles 0, 1 -> %d, %d; "
            "fan triangles 0, 1 -> %d, %d\n",
            l0, s0, s1, f0, f1);
        }
        dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        dev->SetPixelShader(psT0);
      }
      dev->SetIndices(NULL);
    }
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    // instancing (needs vs_3_0 and indexed draws): 4 instances of 8 triangles
    {
      IDirect3DVertexShader9Ptr vs = VS(
          "vs_3_0\ndcl_position v0\ndcl_texcoord v1\ndcl_texcoord1 v2\n"
          "dcl_position o0\ndcl_texcoord o1\nmov r0, v2\nadd o0, v0, r0\nmov o1, v1\n");
      IDirect3DPixelShader9Ptr ps = PS("ps_3_0\ndcl_texcoord v0\nmov oC0, v0\n");
      D3DVERTEXELEMENT9 els[] = {
          {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
          {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
          D3DDECL_END(),
      };
      IDirect3DVertexDeclaration9Ptr decl;
      dev->CreateVertexDeclaration(els, &decl);
      const int T = 8, I = 4;
      std::vector<ClipVert> verts(3 * T);
      for(ClipVert &v : verts)
        v = {{RandF(-1, 1), RandF(-1, 1), 0.5f, 1.0f}, {RandF(0, 1), RandF(0, 1), RandF(0, 1), 1.0f}};
      float inst[I][4];
      for(int k = 0; k < I; k++)
      {
        inst[k][0] = RandF(-0.3f, 0.3f);
        inst[k][1] = RandF(-0.3f, 0.3f);
        inst[k][2] = inst[k][3] = 0.0f;
      }
      std::vector<WORD> idx(3 * T);
      for(int i = 0; i < 3 * T; i++)
        idx[i] = WORD(i);
      IDirect3DVertexBuffer9Ptr vb = MakeVB(verts.data(), UINT(verts.size() * sizeof(ClipVert)));
      IDirect3DVertexBuffer9Ptr ivb = MakeVB(inst, sizeof(inst));
      IDirect3DIndexBuffer9Ptr ib;
      dev->CreateIndexBuffer(UINT(idx.size() * 2), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL);
      void *ptr = NULL;
      ib->Lock(0, 0, &ptr, 0);
      memcpy(ptr, idx.data(), idx.size() * 2);
      ib->Unlock();
      if(vs && ps && decl)
      {
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->SetVertexDeclaration(decl);
        dev->SetIndices(ib);
        dev->SetStreamSource(0, vb, 0, sizeof(ClipVert));
        auto instances = [&](int first, int count) {
          dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | UINT(count));
          dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
          dev->SetStreamSource(1, ivb, UINT(first * 16), 16);
        };
        auto whole = [&]() {
          instances(0, I);
          dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3 * T, 0, T);
        };
        Compare("instanced, each instance's each primitive", whole,
                [&](int q) {
                  instances(q / T, 1);
                  dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3 * T, 3 * (q % T), 1);
                },
                T * I, "instance-major");
        Compare("instanced, each instance's each primitive", whole,
                [&](int q) {
                  instances(q % I, 1);
                  dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3 * T, 3 * (q / I), 1);
                },
                T * I, "primitive-major");
        Compare("instanced, each primitive for every instance", whole,
                [&](int p) {
                  instances(0, I);
                  dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3 * T, 3 * p, 1);
                },
                T, "4 instances each");
        Compare("instanced, each instance whole", whole,
                [&](int k) {
                  instances(k, 1);
                  dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3 * T, 0, T);
                },
                I, "1 instance each");
        dev->SetStreamSourceFreq(0, 1);
        dev->SetStreamSourceFreq(1, 1);
        dev->SetStreamSource(1, NULL, 0, 0);
        dev->SetIndices(NULL);
      }
      BaseState();
    }
    dev->SetRenderTarget(0, backBuffer);
  }

  /////////////////////////////////////////////////////////////////////////////////////////////
  void SectionD()
  {
    P("\n== D: stencil INCR_SAT counting fragments, EQUAL picking fragment N ==\n");
    for(int msaa = 0; msaa < 2; msaa++)
    {
      const D3DMULTISAMPLE_TYPE type = msaa ? D3DMULTISAMPLE_4_SAMPLES : D3DMULTISAMPLE_NONE;
      IDirect3DSurface9Ptr rt, ds;
      dev->CreateRenderTarget(16, 16, D3DFMT_A32B32G32R32F, type, 0, FALSE, &rt, NULL);
      dev->CreateDepthStencilSurface(16, 16, D3DFMT_D24S8, type, 0, FALSE, &ds, NULL);
      if(!rt || !ds)
      {
        P("  couldn't create the %s targets\n", msaa ? "4x" : "plain");
        continue;
      }
      dev->SetRenderTarget(0, rt);
      dev->SetDepthStencilSurface(ds);
      SetViewport(rt);
      BaseState();
      if(msaa)
        dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);

      auto pick = [&](DWORD ref) {
        dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        dev->SetRenderState(D3DRS_STENCILREF, ref);
        dev->SetRenderState(D3DRS_STENCILMASK, 0xFF);
        dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
        dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_INCRSAT);
        dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_INCRSAT);
        dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_INCRSAT);
        dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, TRUE);
        dev->SetRenderState(D3DRS_CCW_STENCILFUNC, D3DCMP_EQUAL);
        dev->SetRenderState(D3DRS_CCW_STENCILPASS, D3DSTENCILOP_INCRSAT);
        dev->SetRenderState(D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_INCRSAT);
        dev->SetRenderState(D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_INCRSAT);
        dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_STENCIL, ClearCol, 1.0f, 0);
      };

      // 6 triangles over the whole target, alternating winding, t0.x = the primitive's index
      std::vector<Vert> v(3 * 6);
      for(int k = 0; k < 6; k++)
      {
        BigTri(&v[3 * k], float(10 + k));
        if(k % 2)
          std::swap(v[3 * k + 1], v[3 * k + 2]);
      }
      std::string got;
      int wrongPixels = 0;
      for(DWORD n = 0; n <= 6; n++)
      {
        pick(n);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 6, v.data(), sizeof(Vert));
        const std::vector<float> img = ReadF(rt);
        for(size_t i = 0; i < img.size(); i += 4)
          if(img[i] != (n < 6 ? float(10 + n) : 1.0f))
            wrongPixels++;
        char buf[32];
        sprintf(buf, " %g", img[(7 * 16 + 5) * 4]);
        got += buf;
      }
      P("  %s, 6 triangles (both windings, red 10 + their index), ref 0-6 write:%s; %d pixels "
        "not as expected (10 + n, then the clear colour's 1)\n",
        msaa ? "4x, MULTISAMPLEANTIALIAS FALSE" : "plain", got.c_str(), wrongPixels);

      if(!msaa)
      {
        // saturation at 255: 300 triangles
        std::vector<Vert> many(3 * 300);
        for(int k = 0; k < 300; k++)
          BigTri(&many[3 * k], float(10 + k));
        std::string sat;
        for(DWORD n : {0u, 200u, 254u, 255u})
        {
          pick(n);
          dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 300, many.data(), sizeof(Vert));
          char buf[32];
          sprintf(buf, " ref %u -> %g", n, ReadF(rt)[0]);
          sat += buf;
        }
        P("  300 triangles (red 10 + their index):%s\n", sat.c_str());

        // with texkill and the alpha test: do killed fragments count? primitive 1 is killed by
        // texkill (t0.y < 0), primitive 2 by the alpha test (t0.w 0.25 against > 0.5)
        std::vector<Vert> k4(3 * 4);
        for(int k = 0; k < 4; k++)
        {
          BigTri(&k4[3 * k], float(10 + k));
          for(int i = 0; i < 3; i++)
          {
            k4[3 * k + i].t[1] = k == 1 ? -1.0f : 0.0f;
            k4[3 * k + i].t[3] = k == 2 ? 0.25f : 1.0f;
          }
        }
        dev->SetPixelShader(psKill);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        dev->SetRenderState(D3DRS_ALPHAREF, 0x80);
        std::string kill;
        for(DWORD n = 0; n <= 4; n++)
        {
          pick(n);
          dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 4, k4.data(), sizeof(Vert));
          char buf[32];
          sprintf(buf, " %g", ReadF(rt)[0]);
          kill += buf;
        }
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetPixelShader(psT0);
        P("  4 triangles (red 10 + index), 1 killed by texkill and 2 by the alpha test: ref 0-4 "
          "write:%s (1 is the clear colour)\n",
          kill.c_str());

        // with the depth test failing for primitive 1: does ZFAIL INCRSAT count it?
        std::vector<Vert> d3(3 * 3);
        for(int k = 0; k < 3; k++)
          BigTri(&d3[3 * k], float(10 + k), k == 1 ? 0.9f : 0.1f);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        std::string zf;
        for(DWORD n = 0; n <= 3; n++)
        {
          pick(n);
          dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 0.5f, 0);
          dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 3, d3.data(), sizeof(Vert));
          char buf[32];
          sprintf(buf, " %g", ReadF(rt)[0]);
          zf += buf;
        }
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        P("  3 triangles (red 10 + index), 1 failing the depth test (STENCILZFAIL INCRSAT too): "
          "ref 0-3 write:%s (1 is the clear colour)\n",
          zf.c_str());
      }
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
      dev->SetRenderTarget(0, backBuffer);
      dev->SetDepthStencilSurface(backDepth);
    }
  }

  /////////////////////////////////////////////////////////////////////////////////////////////
  // the bounding box of the pixels equal to (r, g, b) / 255, and their count
  std::string Where(const std::vector<float> &img, UINT w, float r, float g, float b)
  {
    int n = 0, x0 = 99, y0 = 99, x1 = -1, y1 = -1;
    for(size_t i = 0; i < img.size(); i += 4)
      if(img[i] == r / 255.0f && img[i + 1] == g / 255.0f && img[i + 2] == b / 255.0f)
      {
        const int x = int((i / 4) % w), y = int((i / 4) / w);
        n++;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    char buf[96];
    if(n == 0)
      sprintf(buf, "none");
    else
      sprintf(buf, "%d pixels in (%d,%d)-(%d,%d)", n, x0, y0, x1, y1);
    return buf;
  }

  void SectionE()
  {
    P("\n== E: Clear, ColorFill and StretchRect against the scissor, viewport and write mask ==\n");
    IDirect3DSurface9Ptr rt, ds, src;
    dev->CreateRenderTarget(16, 16, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    dev->CreateRenderTarget(16, 16, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &src, NULL);
    dev->CreateDepthStencilSurface(16, 16, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL);
    dev->SetRenderTarget(0, src);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 255), 1.0f, 0);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(ds);
    BaseState();
    const DWORD green = D3DCOLOR_ARGB(255, 0, 255, 0);
    D3DVIEWPORT9 full = {0, 0, 16, 16, 0.0f, 1.0f}, vpSmall = {4, 4, 8, 8, 0.0f, 1.0f};
    RECT sc = {2, 2, 7, 7};
    auto reset = [&]() {
      dev->SetViewport(&full);
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
      dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
      dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 255, 0, 0), 1.0f, 0);
    };
    auto report = [&](const char *what) {
      const std::vector<float> img = ReadF(rt);
      P("  %-58s green: %s; blue: %s\n", what, Where(img, 16, 0, 255, 0).c_str(),
        Where(img, 16, 0, 0, 255).c_str());
    };

    reset();
    dev->SetViewport(&vpSmall);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, green, 1.0f, 0);
    report("Clear, viewport (4,4) 8x8");
    reset();
    dev->SetScissorRect(&sc);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, green, 1.0f, 0);
    report("Clear, scissor (2,2)-(7,7)");
    reset();
    dev->SetViewport(&vpSmall);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, green, 1.0f, 0);
    report("Clear, viewport (4,4) 8x8 and scissor (2,2)-(7,7)");
    reset();
    {
      D3DRECT r = {0, 0, 4, 4};
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
      dev->Clear(1, &r, D3DCLEAR_TARGET, green, 1.0f, 0);
      report("Clear of rect (0,0)-(4,4), scissor (2,2)-(7,7)");
    }
    reset();
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_GREEN);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, green, 1.0f, 0);
    report("Clear, COLORWRITEENABLE green only (red stays = yellow)");
    {
      const std::vector<float> img = ReadF(rt);
      P("    pixel (0,0) is (%g, %g, %g, %g)\n", img[0], img[1], img[2], img[3]);
    }
    reset();
    dev->SetViewport(&vpSmall);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    {
      RECT r = {0, 0, 4, 4};
      dev->EndScene();
      const HRESULT h = dev->ColorFill(rt, &r, green);
      dev->BeginScene();
      P("  ColorFill %08x\n", (UINT)h);
      report("ColorFill of (0,0)-(4,4), viewport and scissor as above");
    }
    reset();
    dev->SetViewport(&vpSmall);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    {
      RECT r = {0, 0, 16, 16};
      dev->EndScene();
      dev->StretchRect(src, &r, rt, &r, D3DTEXF_NONE);
      dev->BeginScene();
      report("StretchRect of all of a blue target, viewport and scissor as above");
    }

    // depth clears: does the scissor limit them? Cleared to 1, then 0 with the scissor, then a
    // draw at 0.5 with LESS turns green where the depth is still 1
    reset();
    dev->Clear(0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    dev->SetScissorRect(&sc);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 0.0f, 0);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    dev->SetPixelShader(psC0);
    const float g[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    dev->SetPixelShaderConstantF(0, g, 1);
    {
      Vert v[3];
      BigTri(v, 0.0f, 0.5f);
      dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert));
    }
    report("depth Clear to 0 with scissor (2,2)-(7,7); draw at 0.5 LESS");
    BaseState();
    dev->SetRenderTarget(0, backBuffer);
    dev->SetDepthStencilSurface(backDepth);
    dev->SetViewport(&full);
  }

  /////////////////////////////////////////////////////////////////////////////////////////////
  void SectionF()
  {
    P("\n== F: depth clipping, D3DRS_CLIPPING and user clip planes ==\n");
    const D3DFORMAT INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
    IDirect3DTexture9Ptr depthTex;
    IDirect3DSurface9Ptr depth, rt, readRT;
    dev->CreateTexture(16, 16, 1, D3DUSAGE_DEPTHSTENCIL, INTZ, D3DPOOL_DEFAULT, &depthTex, NULL);
    if(depthTex)
      depthTex->GetSurfaceLevel(0, &depth);
    dev->CreateRenderTarget(16, 16, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    dev->CreateRenderTarget(16, 16, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &readRT,
                            NULL);
    IDirect3DPixelShader9Ptr psDepth =
        PS("ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0\n");
    if(!depth || !psDepth)
    {
      P("  no INTZ depth texture\n");
      return;
    }
    // the INTZ depth at each pixel of row 7, by sampling it
    auto readDepth = [&]() {
      dev->SetRenderTarget(0, readRT);
      dev->SetDepthStencilSurface(NULL);
      BaseState();
      dev->SetPixelShader(psDepth);
      dev->SetTexture(0, depthTex);
      dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      Vert v[4];
      const float c[4][2] = {{-0.5f, -0.5f}, {15.5f, -0.5f}, {-0.5f, 15.5f}, {15.5f, 15.5f}};
      for(int i = 0; i < 4; i++)
        v[i] = {c[i][0], c[i][1], 0.5f, 1.0f, 0, {(c[i][0] + 0.5f) / 16.0f, (c[i][1] + 0.5f) / 16.0f, 0, 1}};
      dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vert));
      dev->SetTexture(0, NULL);
      std::vector<float> img = ReadF(readRT);
      dev->SetRenderTarget(0, rt);
      dev->SetDepthStencilSurface(depth);
      return img;
    };
    auto prepare = [&]() {
      dev->SetRenderTarget(0, rt);
      dev->SetDepthStencilSurface(depth);
      D3DVIEWPORT9 vp = {0, 0, 16, 16, 0.0f, 1.0f};
      dev->SetViewport(&vp);
      BaseState();
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, ClearCol, 0.25f, 0);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
      dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    };
    auto clipDraw = [&](float zl, float zr) {
      // the whole target in clip space, z from zl at the left edge to zr at the right
      ClipVert v[4] = {
          {{-1, 1, zl, 1}, {0, 0, 0, 1}},
          {{1, 1, zr, 1}, {0, 0, 0, 1}},
          {{-1, -1, zl, 1}, {0, 0, 0, 1}},
          {{1, -1, zr, 1}, {0, 0, 0, 1}},
      };
      dev->SetVertexShader(vsClip);
      dev->SetVertexDeclaration(clipDecl);
      dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(ClipVert));
    };
    auto rhwDraw = [&](float z) {
      Vert v[4];
      const float c[4][2] = {{-0.5f, -0.5f}, {15.5f, -0.5f}, {-0.5f, 15.5f}, {15.5f, 15.5f}};
      for(int i = 0; i < 4; i++)
        v[i] = {c[i][0], c[i][1], z, 1.0f, 0, {0, 0, 0, 1}};
      dev->SetVertexShader(NULL);
      dev->SetFVF(VertFVF);
      dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vert));
    };
    auto row = [&](const std::vector<float> &d) {
      std::string s;
      for(int x = 0; x < 16; x += 3)
      {
        char buf[32];
        sprintf(buf, " %.4f", d[(7 * 16 + x) * 4]);
        s += buf;
      }
      return s;
    };
    for(int clipping = 1; clipping >= 0; clipping--)
    {
      for(int c = 0; c < 4; c++)
      {
        const float zl[] = {0.5f, 1.5f, -0.5f, -0.5f}, zr[] = {0.5f, 1.5f, -0.5f, 1.5f};
        prepare();
        dev->SetRenderState(D3DRS_CLIPPING, clipping ? TRUE : FALSE);
        const DWORD n = Query([&]() { clipDraw(zl[c], zr[c]); });
        P("  vs_2_0, CLIPPING %s, z %4.1f to %4.1f: %3u pixels, depths in row 7 (x 0, 3, 6...):%s\n",
          clipping ? "TRUE " : "FALSE", zl[c], zr[c], n, row(readDepth()).c_str());
      }
      for(float z : {1.5f, -0.5f})
      {
        prepare();
        dev->SetRenderState(D3DRS_CLIPPING, clipping ? TRUE : FALSE);
        const DWORD n = Query([&]() { rhwDraw(z); });
        P("  XYZRHW, CLIPPING %s, z %4.1f: %3u pixels, depths:%s\n", clipping ? "TRUE " : "FALSE",
          z, n, row(readDepth()).c_str());
      }
      // a user clip plane keeping x >= 0 in clip space (the right half)
      {
        prepare();
        dev->SetRenderState(D3DRS_CLIPPING, clipping ? TRUE : FALSE);
        const float plane[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        dev->SetClipPlane(0, plane);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, D3DCLIPPLANE0);
        const DWORD n = Query([&]() { clipDraw(0.5f, 0.5f); });
        const DWORD nr = Query([&]() { rhwDraw(0.5f); });
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        P("  clip plane x >= 0, CLIPPING %s: vs_2_0 %u pixels, XYZRHW %u pixels\n",
          clipping ? "TRUE " : "FALSE", n, nr);
      }
    }
    BaseState();
    dev->SetRenderTarget(0, backBuffer);
    dev->SetDepthStencilSurface(backDepth);
  }

  /////////////////////////////////////////////////////////////////////////////////////////////
  void SectionG()
  {
    P("\n== G: sRGB writes and blending ==\n");
    if(FAILED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                     D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBWRITE,
                                     D3DRTYPE_SURFACE, D3DFMT_A8R8G8B8)))
    {
      P("  A8R8G8B8 has no sRGB writes\n");
      return;
    }
    IDirect3DSurface9Ptr rt;
    dev->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(NULL);
    SetViewport(rt);
    BaseState();
    dev->SetPixelShader(psC0);
    Vert v[3];
    BigTri(v, 0.0f);
    auto pixel = [&]() {
      const std::vector<byte> b = ReadBytes(rt, 4);
      char buf[64];
      sprintf(buf, "%02x %02x %02x %02x", b[2], b[1], b[0], b[3]);
      return std::string(buf);
    };
    for(int srgb = 0; srgb < 2; srgb++)
    {
      std::string s;
      for(float f : {0.0f, 0.002f, 0.0031308f, 0.04045f, 0.2f, 0.5f, 0.735f, 1.0f})
      {
        const float c[4] = {f, f, f, f};
        dev->SetPixelShaderConstantF(0, c, 1);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, srgb ? TRUE : FALSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert));
        char buf[64];
        sprintf(buf, " %g->%s", f, pixel().substr(0, 2).c_str());
        s += buf;
      }
      P("  SRGBWRITEENABLE %s, red stored:%s\n", srgb ? "TRUE " : "FALSE", s.c_str());
    }
    // blending with sRGB writes: the target holds 0x80, the shader 0.1, ONE + ONE; and 0.0 with
    // alpha 0.5, SRCALPHA + INVSRCALPHA
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    for(int mode = 0; mode < 2; mode++)
    {
      dev->SetRenderState(D3DRS_SRCBLEND, mode ? D3DBLEND_SRCALPHA : D3DBLEND_ONE);
      dev->SetRenderState(D3DRS_DESTBLEND, mode ? D3DBLEND_INVSRCALPHA : D3DBLEND_ONE);
      dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0x80, 0x80, 0x80, 0x80), 1.0f, 0);
      const float c[4] = {mode ? 0.0f : 0.1f, 0.0f, 0.0f, mode ? 0.5f : 0.1f};
      dev->SetPixelShaderConstantF(0, c, 1);
      dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert));
      P("  sRGB writes, target 80 80 80 80, %s: stored (r g b a) %s\n",
        mode ? "0.0 alpha 0.5 SRCALPHA/INVSRCALPHA" : "0.1 ONE/ONE", pixel().c_str());
    }
    BaseState();
    dev->SetRenderTarget(0, backBuffer);
  }

  /////////////////////////////////////////////////////////////////////////////////////////////
  void SectionH()
  {
    P("\n== H: how long a query and a pixel readback take ==\n");
    IDirect3DSurface9Ptr rt, one, sysOne;
    dev->CreateRenderTarget(256, 256, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    dev->CreateRenderTarget(1, 1, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &one, NULL);
    dev->CreateOffscreenPlainSurface(1, 1, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysOne, NULL);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(NULL);
    SetViewport(rt);
    BaseState();
    Scissor(100, 100);
    Vert v[3];
    BigTri(v, 0.0f);
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    const int R = 200;
    QueryPerformanceCounter(&t0);
    for(int i = 0; i < R; i++)
      Query([&]() { dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert)); });
    QueryPerformanceCounter(&t1);
    const double q = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart) / R;
    QueryPerformanceCounter(&t0);
    for(int i = 0; i < R; i++)
    {
      RECT r = {100, 100, 101, 101};
      dev->EndScene();
      dev->StretchRect(rt, &r, one, NULL, D3DTEXF_NONE);
      dev->GetRenderTargetData(one, sysOne);
      D3DLOCKED_RECT lr;
      sysOne->LockRect(&lr, NULL, D3DLOCK_READONLY);
      sysOne->UnlockRect();
      dev->BeginScene();
    }
    QueryPerformanceCounter(&t1);
    const double rb = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart) / R;
    P("  an occlusion query around a one-triangle draw, waited for: %.3f ms; a 1x1 StretchRect, "
      "GetRenderTargetData and lock: %.3f ms (each the mean of %d)\n",
      q, rb, R);
    BaseState();
    dev->SetRenderTarget(0, backBuffer);
  }

  int main()
  {
    if(!Init())
      return 3;
    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    assemble = d3dcompiler ? (pD3DAssemble)GetProcAddress(d3dcompiler, "D3DAssemble") : NULL;
    if(!assemble)
      return 4;

    const char *dir = getenv("PROBE_OUT");
    out = fopen((std::string(dir ? dir : ".") + "/probe.txt").c_str(), "w");
    if(!out)
      return 5;

    D3DADAPTER_IDENTIFIER9 ident = {};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident);
    P("adapter %s, driver %u.%u.%u.%u\n", ident.Description, HIWORD(ident.DriverVersion.HighPart),
      LOWORD(ident.DriverVersion.HighPart), HIWORD(ident.DriverVersion.LowPart),
      LOWORD(ident.DriverVersion.LowPart));

    psT0 = PS("ps_2_0\ndcl t0\nmov oC0, t0\n");
    psV0 = PS("ps_2_0\ndcl v0\nmov oC0, v0\n");
    psC0 = PS("ps_2_0\nmov oC0, c0\n");
    psKill = PS("ps_2_0\ndcl t0\ntexkill t0\nmov oC0, t0\n");
    vsClip = VS("vs_2_0\ndcl_position v0\ndcl_texcoord v1\nmov oPos, v0\nmov oT0, v1\n");
    D3DVERTEXELEMENT9 els[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    dev->CreateVertexDeclaration(els, &clipDecl);
    dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query);
    if(!psT0 || !psV0 || !psC0 || !psKill || !vsClip || !clipDecl || !query)
      return 6;

    dev->GetRenderTarget(0, &backBuffer);
    dev->GetDepthStencilSurface(&backDepth);

    dev->BeginScene();
    SectionA();
    SectionB();
    SectionC();
    SectionD();
    SectionE();
    SectionF();
    SectionG();
    SectionH();
    dev->EndScene();

    fclose(out);
    out = NULL;
    TEST_LOG("done");
    return 0;
  }
};

REGISTER_TEST();
