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

// a temporary probe for pixel shader debugging (step 5.4): what the GPU does natively with the
// pixel centre and vPos, vFace, ps_1_x ranges, colour inputs, derivatives, texldd, the
// transcendentals, and which fragment a replacement pixel shader reads. Writes one file per
// section to PROBE_OUT.

#define _CRT_SECURE_NO_WARNINGS
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "d3d9_test.h"

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

RD_TEST(D3D9_PS_Probe, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Probe of pixel shader behaviour";

  struct V4
  {
    float x, y, z, w;
  };

  struct Vert
  {
    V4 pos;
    V4 t[4];
    V4 c[2];
  };

  struct Targets
  {
    UINT w = 0, h = 0;
    IDirect3DSurface9Ptr rt[4];
    IDirect3DSurface9Ptr ds;
  };

  pD3DAssemble assemble = NULL;
  std::string outDir;
  FILE *out = NULL;
  IDirect3DVertexDeclaration9Ptr decl;
  IDirect3DVertexShader9Ptr vs2, vs3;
  std::map<std::string, IDirect3DPixelShader9Ptr> psCache;
  D3DCAPS9 caps = {};

  void Open(const char *name)
  {
    if(out)
      fclose(out);
    out = fopen((outDir + "/" + name + ".txt").c_str(), "w");
    TEST_LOG("section %s", name);
  }

  void P(const char *fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
  }

  std::vector<DWORD> Assemble(const std::string &src, std::string *err = NULL)
  {
    ID3DBlobPtr code, errors;
    if(FAILED(assemble(src.c_str(), src.size(), NULL, NULL, NULL, 0, &code, &errors)) || !code)
    {
      if(err)
        *err = errors ? (const char *)errors->GetBufferPointer() : "?";
      return {};
    }
    const DWORD *p = (const DWORD *)code->GetBufferPointer();
    return std::vector<DWORD>(p, p + code->GetBufferSize() / 4);
  }

  // NULL if it doesn't assemble or the runtime rejects it, with the reason in the section's file
  IDirect3DPixelShader9 *PS(const std::string &src)
  {
    auto it = psCache.find(src);
    if(it != psCache.end())
      return it->second;
    std::string err;
    std::vector<DWORD> code = Assemble(src, &err);
    IDirect3DPixelShader9Ptr ps;
    if(code.empty())
    {
      std::string first = src.substr(0, src.find('\n'));
      P("# assemble failed (%s ...): %s\n", first.c_str(), err.c_str());
      for(char &c : err)
        if(c == '\n')
          c = ' ';
      TEST_LOG("assemble failed: %s", err.c_str());
    }
    else
    {
      HRESULT hr = dev->CreatePixelShader(code.data(), &ps);
      if(FAILED(hr))
      {
        P("# CreatePixelShader failed %08x for:\n%s\n", (UINT)hr, src.c_str());
        TEST_LOG("CreatePixelShader failed %08x", (UINT)hr);
      }
    }
    psCache[src] = ps;
    return ps;
  }

  Targets MakeTargets(UINT w, UINT h, int n, D3DFORMAT f0 = D3DFMT_A32B32G32R32F,
                      bool depth = false, D3DFORMAT fn = D3DFMT_A32B32G32R32F)
  {
    Targets t;
    t.w = w;
    t.h = h;
    for(int i = 0; i < n; i++)
    {
      HRESULT hr = dev->CreateRenderTarget(w, h, i == 0 ? f0 : fn, D3DMULTISAMPLE_NONE, 0, FALSE,
                                           &t.rt[i], NULL);
      if(FAILED(hr))
        TEST_LOG("CreateRenderTarget %ux%u failed %08x", w, h, (UINT)hr);
    }
    if(depth)
      dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &t.ds, NULL);
    return t;
  }

  HRESULT Bind(const Targets &t, bool clear = true, float z = 1.0f)
  {
    HRESULT ret = S_OK;
    for(DWORD i = 0; i < 4; i++)
    {
      HRESULT hr = dev->SetRenderTarget(i, t.rt[i]);
      if(FAILED(hr) && (i == 0 || t.rt[i]))
        ret = hr;
    }
    dev->SetDepthStencilSurface(t.ds);
    D3DVIEWPORT9 vp = {0, 0, t.w, t.h, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    if(clear)
      dev->Clear(0, NULL, D3DCLEAR_TARGET | (t.ds ? D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL : 0),
                 D3DCOLOR_ARGB(7, 7, 7, 7), z, 0);
    return ret;
  }

  std::vector<V4> Read(IDirect3DSurface9 * rt)
  {
    std::vector<V4> ret;
    D3DSURFACE_DESC desc = {};
    rt->GetDesc(&desc);
    IDirect3DSurface9Ptr read;
    dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &read,
                                     NULL);
    D3DLOCKED_RECT lr = {};
    if(FAILED(dev->GetRenderTargetData(rt, read)) ||
       FAILED(read->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
      TEST_LOG("readback failed");
      ret.resize(desc.Width * desc.Height);
      return ret;
    }
    for(UINT y = 0; y < desc.Height; y++)
    {
      const byte *row = (const byte *)lr.pBits + y * lr.Pitch;
      for(UINT x = 0; x < desc.Width; x++)
      {
        V4 v = {};
        if(desc.Format == D3DFMT_A32B32G32R32F)
        {
          memcpy(&v, row + x * 16, 16);
        }
        else if(desc.Format == D3DFMT_A16B16G16R16F)
        {
          const uint16_t *h = (const uint16_t *)(row + x * 8);
          v = {FromHalf(h[0]), FromHalf(h[1]), FromHalf(h[2]), FromHalf(h[3])};
        }
        else if(desc.Format == D3DFMT_A8R8G8B8)
        {
          const byte *b = row + x * 4;
          v = {b[2] / 255.0f, b[1] / 255.0f, b[0] / 255.0f, b[3] / 255.0f};
        }
        ret.push_back(v);
      }
    }
    read->UnlockRect();
    return ret;
  }

  static float FromHalf(uint16_t h)
  {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t e = (h >> 10) & 0x1f;
    uint32_t m = h & 0x3ff;
    float f;
    if(e == 0)
    {
      f = ldexpf(float(m), -24);
      return sign ? -f : f;
    }
    uint32_t u =
        e == 31 ? (sign | 0x7f800000 | (m << 13)) : (sign | ((e - 15 + 127) << 23) | (m << 13));
    memcpy(&f, &u, 4);
    return f;
  }

  static uint16_t ToHalf(float f)
  {
    // round to nearest even, for comparing _pp results
    uint32_t u;
    memcpy(&u, &f, 4);
    uint32_t sign = (u >> 16) & 0x8000;
    int32_t e = int32_t((u >> 23) & 0xff) - 127 + 15;
    uint32_t m = u & 0x7fffff;
    if(((u >> 23) & 0xff) == 0xff)
      return uint16_t(sign | 0x7c00 | (m ? 0x200 : 0));
    if(e >= 31)
      return uint16_t(sign | 0x7c00);
    if(e <= 0)
    {
      if(e < -10)
        return uint16_t(sign);
      m |= 0x800000;
      uint32_t shift = uint32_t(14 - e);
      uint32_t r = m >> shift;
      uint32_t rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
      if(rem > half || (rem == half && (r & 1)))
        r++;
      return uint16_t(sign | r);
    }
    uint32_t r = (uint32_t(e) << 10) | (m >> 13);
    uint32_t rem = m & 0x1fff;
    if(rem > 0x1000 || (rem == 0x1000 && (r & 1)))
      r++;
    return uint16_t(sign | r);
  }

  void ResetState()
  {
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
    dev->SetRenderState(D3DRS_ALPHAREF, 0);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xf);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE2, 0xf);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE3, 0xf);
    float one = 1.0f;
    dev->SetRenderState(D3DRS_POINTSIZE, *(DWORD *)&one);
    for(DWORD s = 0; s < 4; s++)
    {
      dev->SetTexture(s, NULL);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
      dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
      dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
      dev->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, 1);
    }
  }

  // a vertex at a screen position, with clip w (for perspective)
  static Vert At(float sx, float sy, float W, float H, float z = 0.5f, float w = 1.0f)
  {
    Vert v = {};
    float cx = sx * 2.0f / W - 1.0f, cy = 1.0f - sy * 2.0f / H;
    v.pos = {cx * w, cy * w, z * w, w};
    return v;
  }

  // a = top left, b = top right, c = bottom left, d = bottom right
  static void PushQuad(std::vector<Vert> & v, const Vert &a, const Vert &b, const Vert &c,
                       const Vert &d)
  {
    v.push_back(a);
    v.push_back(b);
    v.push_back(c);
    v.push_back(c);
    v.push_back(b);
    v.push_back(d);
  }

  HRESULT Draw(IDirect3DPixelShader9 * ps, const std::vector<Vert> &v,
               D3DPRIMITIVETYPE prim = D3DPT_TRIANGLELIST, bool sm3 = false)
  {
    if(!ps || v.empty())
      return E_FAIL;
    UINT count = prim == D3DPT_TRIANGLELIST
                     ? UINT(v.size() / 3)
                     : (prim == D3DPT_LINELIST ? UINT(v.size() / 2) : UINT(v.size()));
    dev->SetVertexDeclaration(decl);
    dev->SetVertexShader(sm3 ? vs3 : vs2);
    dev->SetPixelShader(ps);
    dev->BeginScene();
    HRESULT hr = dev->DrawPrimitiveUP(prim, count, v.data(), sizeof(Vert));
    dev->EndScene();
    return hr;
  }

  static bool IsSM3(const std::string &src)
  {
    return src.compare(0, 6, "ps_3_0") == 0;
  }

  HRESULT DrawSrc(const std::string &src, const std::vector<Vert> &v,
                  D3DPRIMITIVETYPE prim = D3DPT_TRIANGLELIST)
  {
    return Draw(PS(src), v, prim, IsSM3(src));
  }

  // what a float target cleared to D3DCOLOR_ARGB(7, 7, 7, 7) holds, measured
  float sentinel = 0.0f;
  bool Unwritten(const V4 &v)
  {
    return v.x == sentinel && v.y == sentinel && v.z == sentinel && v.w == sentinel;
  }

  static int64_t Ord(float f)
  {
    int32_t i;
    memcpy(&i, &f, 4);
    return i >= 0 ? int64_t(i) : -int64_t(i & 0x7fffffff);
  }

  static int64_t Ulps(float a, float b)
  {
    if(isnan(a) && isnan(b))
      return 0;
    if(isnan(a) || isnan(b))
      return INT64_MAX;
    if(isinf(a) || isinf(b))
      return a == b ? 0 : INT64_MAX;
    int64_t d = Ord(a) - Ord(b);
    return d < 0 ? -d : d;
  }

  static uint32_t Bits(float f)
  {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
  }

  static float FromBits(uint32_t u)
  {
    float f;
    memcpy(&f, &u, 4);
    return f;
  }

  //////////////////////////////////////////////////////////////////////////////////////////////
  // A: where pixels are sampled, vPos

  void Centre()
  {
    Open("a_centre");
    const UINT W = 16, H = 8;
    Targets t = MakeTargets(W, H, 2);
    auto c = [&](float sx, float sy) {
      Vert r = At(sx, sy, float(W), float(H));
      r.t[0] = {sx, sy, 0.0f, 1.0f};
      r.c[0] = {sx / W, sy / H, 0.0f, 1.0f};
      return r;
    };
    std::vector<Vert> full;
    PushQuad(full, c(0, 0), c(float(W), 0), c(0, float(H)), c(float(W), float(H)));

    const char *ps3 =
        "ps_3_0\ndef c0, 0, 0, 0, 0\ndcl_texcoord0 v0\ndcl vPos.xy\nmov oC0, v0\nmov r0, c0\n"
        "mov r0.xy, vPos\nmov oC1, r0\n";
    const char *ps2 = "ps_2_0\ndcl t0\nmov oC0, t0\n";

    ResetState();
    Bind(t);
    HRESULT hr = DrawSrc(ps3, full);
    std::vector<V4> a = Read(t.rt[0]), b = Read(t.rt[1]);
    P("full target quad, screen 0..%u x 0..%u, texcoord0 = the vertex's screen position\n", W, H);
    P("ps_3_0 draw %08x: pixel x y = texcoord0.xy interpolated | vPos.xy\n", (UINT)hr);
    for(UINT y = 0; y < H; y++)
      for(UINT x = 0; x < W; x++)
        P("%u %u = %.9g %.9g | %.9g %.9g\n", x, y, a[y * W + x].x, a[y * W + x].y, b[y * W + x].x,
          b[y * W + x].y);

    ResetState();
    Bind(t);
    hr = DrawSrc(ps2, full);
    a = Read(t.rt[0]);
    P("ps_2_0 draw %08x: pixel x y = t0.xy interpolated\n", (UINT)hr);
    for(UINT y = 0; y < H; y++)
      for(UINT x = 0; x < W; x++)
        P("%u %u = %.9g %.9g\n", x, y, a[y * W + x].x, a[y * W + x].y);

    // a viewport that doesn't start at 0: is vPos relative to the target or the viewport?
    ResetState();
    Bind(t);
    D3DVIEWPORT9 vp = {4, 2, 8, 4, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    {
      auto cv = [&](float sx, float sy) {
        Vert r = At(sx, sy, 8.0f, 4.0f);
        r.t[0] = {sx, sy, 0.0f, 1.0f};
        return r;
      };
      std::vector<Vert> v;
      PushQuad(v, cv(0, 0), cv(8, 0), cv(0, 4), cv(8, 4));
      hr = DrawSrc(ps3, v);
    }
    a = Read(t.rt[0]);
    b = Read(t.rt[1]);
    P("viewport X 4 Y 2 W 8 H 4, ps_3_0 draw %08x: pixel x y (of the target) = texcoord0.xy "
      "(viewport relative screen position) | vPos.xy\n",
      (UINT)hr);
    for(UINT y = 0; y < H; y++)
      for(UINT x = 0; x < W; x++)
        if(!Unwritten(a[y * W + x]))
          P("%u %u = %.9g %.9g | %.9g %.9g\n", x, y, a[y * W + x].x, a[y * W + x].y, b[y * W + x].x,
            b[y * W + x].y);

    // which pixels a rectangle with fractional edges covers
    const float rects[][4] = {
        {2.0f, 1.0f, 5.0f, 4.0f},     {2.25f, 1.25f, 5.25f, 4.25f}, {2.5f, 1.5f, 5.5f, 4.5f},
        {2.75f, 1.75f, 5.75f, 4.75f}, {2.49f, 1.49f, 5.49f, 4.49f}, {2.51f, 1.51f, 5.51f, 4.51f},
    };
    for(const auto &r : rects)
    {
      ResetState();
      Bind(t);
      std::vector<Vert> v;
      PushQuad(v, c(r[0], r[1]), c(r[2], r[1]), c(r[0], r[3]), c(r[2], r[3]));
      hr = DrawSrc("ps_2_0\ndef c0, 1, 1, 1, 1\nmov oC0, c0\n", v);
      a = Read(t.rt[0]);
      P("coverage of x %g..%g y %g..%g (draw %08x), # = written:\n", r[0], r[2], r[1], r[3],
        (UINT)hr);
      for(UINT y = 0; y < H; y++)
      {
        std::string row;
        for(UINT x = 0; x < W; x++)
          row += a[y * W + x].x == 1.0f ? '#' : '.';
        P("  %s\n", row.c_str());
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////////////////////////
  // B: vFace

  void Face()
  {
    Open("b_vface");
    const UINT W = 32, H = 16;
    Targets t = MakeTargets(W, H, 1);
    auto c = [&](float sx, float sy) { return At(sx, sy, float(W), float(H)); };

    struct Region
    {
      const char *name;
      D3DPRIMITIVETYPE prim;
      std::vector<Vert> v;
      UINT x0, y0, x1, y1;
    };
    // screen space has y down: "clockwise" is as it looks on screen
    std::vector<Region> regions = {
        {"big clockwise triangle", D3DPT_TRIANGLELIST, {c(1, 1), c(9, 1), c(1, 9)}, 1, 1, 9, 9},
        {"big counter-clockwise triangle", D3DPT_TRIANGLELIST, {c(11, 1), c(11, 9), c(19, 1)}, 11, 1, 19, 9},
        {"small clockwise triangle", D3DPT_TRIANGLELIST, {c(21, 1), c(24, 1), c(21, 4)}, 21, 1, 24, 4},
        {"small counter-clockwise triangle",
         D3DPT_TRIANGLELIST,
         {c(26, 1), c(26, 4), c(29, 1)},
         26,
         1,
         29,
         4},
        {"line", D3DPT_LINELIST, {c(1.5f, 12.5f), c(15.5f, 12.5f)}, 0, 11, 16, 14},
        {"point", D3DPT_POINTLIST, {c(20.25f, 12.25f)}, 19, 11, 22, 14},
    };

    // vFace can only be a condition (if_comp, break_comp, setp_comp, cmp), so its value is found
    // by comparing it with thresholds: x = vFace > T, y = vFace == T, z = vFace < -T,
    // w = vFace == -T
    const char *cmpSrc =
        "ps_3_0\ndef c0, 0, 1, -1, 0\ndcl vFace\nmov r0, c0.x\nif_gt vFace, c1.x\nmov r0.x, "
        "c0.y\nendif\nif_eq vFace, c1.y\nmov r0.y, c0.y\nendif\nif_lt vFace, c1.z\nmov r0.z, "
        "c0.y\nendif\nif_eq vFace, c1.w\nmov r0.w, c0.y\nendif\nmov oC0, r0\n";
    // the other forms: cmp (>= 0), setp, and if_gt against 0
    const char *otherSrc =
        "ps_3_0\ndef c0, 0, 1, -1, 0\ndcl vFace\ncmp r0.x, vFace, c0.y, c0.z\nsetp_gt p0, vFace, "
        "c0.x\nmov r0.y, c0.z\n(p0) mov r0.y, c0.y\nif_ge vFace, c0.x\nmov r0.z, c0.y\nelse\nmov "
        "r0.z, c0.z\nendif\nmov r0.w, c0.y\nmov oC0, r0\n";

    struct Mode
    {
      const char *name;
      DWORD cull, fill;
    };
    const Mode modes[] = {
        {"cull none", D3DCULL_NONE, D3DFILL_SOLID},
        {"cull CW", D3DCULL_CW, D3DFILL_SOLID},
        {"cull CCW", D3DCULL_CCW, D3DFILL_SOLID},
        {"cull none, wireframe", D3DCULL_NONE, D3DFILL_WIREFRAME},
        {"cull none, point fill", D3DCULL_NONE, D3DFILL_POINT},
    };

    const float thresholds[] = {0.0f, 0.5f, 0.999f, 1.0f, 1.001f, 2.0f, 1000.0f, 1e6f};

    P("threshold T: per region the pixels' (vFace > T, vFace == T, vFace < -T, vFace == -T)\n");
    for(const Mode &m : modes)
    {
      P("%s:\n", m.name);
      for(int pass = -1; pass < (int)(sizeof(thresholds) / sizeof(thresholds[0])); pass++)
      {
        ResetState();
        Bind(t);
        dev->SetRenderState(D3DRS_CULLMODE, m.cull);
        dev->SetRenderState(D3DRS_FILLMODE, m.fill);
        IDirect3DPixelShader9 *ps;
        if(pass < 0)
        {
          ps = PS(otherSrc);
          P("  cmp / setp_gt 0 / if_ge 0 (1 or -1 each):\n");
        }
        else
        {
          float T = thresholds[pass];
          V4 c1 = {T, T, -T, -T};
          dev->SetPixelShaderConstantF(1, &c1.x, 1);
          ps = PS(cmpSrc);
          P("  T = %g:\n", T);
        }
        std::vector<HRESULT> hrs;
        for(const Region &r : regions)
          hrs.push_back(Draw(ps, r.v, r.prim, true));
        std::vector<V4> a = Read(t.rt[0]);
        for(size_t i = 0; i < regions.size(); i++)
        {
          const Region &r = regions[i];
          std::map<std::string, int> values;
          for(UINT y = r.y0; y < r.y1; y++)
            for(UINT x = r.x0; x < r.x1; x++)
            {
              const V4 &v = a[y * W + x];
              if(Unwritten(v))
                continue;
              char buf[128];
              snprintf(buf, sizeof(buf), "%g %g %g %g", v.x, v.y, v.z, v.w);
              values[buf]++;
            }
          P("    %s (draw %08x):", r.name, (UINT)hrs[i]);
          if(values.empty())
            P(" not drawn");
          for(const auto &kv : values)
            P(" [%s] x%d", kv.first.c_str(), kv.second);
          P("\n");
        }
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////////////////////////
  // C: ps_1_x ranges, texcoord, cnd

  // one pixel's output of a shader drawn over a 4x4 target with the given constants c0..c7,
  // t0 and v0 on every vertex, and an optional texture in s0
  V4 RunOne(const std::string &src, const V4 *consts, int numConsts, V4 t0, V4 v0,
            IDirect3DBaseTexture9 *tex = NULL, HRESULT *hrOut = NULL)
  {
    static Targets t;
    if(!t.rt[0])
      t = MakeTargets(4, 4, 1);
    ResetState();
    Bind(t);
    if(tex)
      dev->SetTexture(0, tex);
    if(numConsts)
      dev->SetPixelShaderConstantF(0, &consts[0].x, numConsts);
    auto c = [&](float sx, float sy) {
      Vert r = At(sx, sy, 4.0f, 4.0f);
      r.t[0] = t0;
      r.c[0] = v0;
      return r;
    };
    std::vector<Vert> v;
    PushQuad(v, c(0, 0), c(4, 0), c(0, 4), c(4, 4));
    HRESULT hr = DrawSrc(src, v);
    if(hrOut)
      *hrOut = hr;
    dev->SetTexture(0, NULL);
    if(FAILED(hr))
      return {NAN, NAN, NAN, NAN};
    return Read(t.rt[0])[5];
  }

  void PS1x()
  {
    Open("c_ps1x");
    P("PixelShader1xMaxValue %.9g, MaxTextureRepeat %u, PixelShaderVersion %08x\n",
      caps.PixelShader1xMaxValue, (UINT)caps.MaxTextureRepeat, (UINT)caps.PixelShaderVersion);
    P("PS20Caps: Caps %08x (gradient instructions %d, predication %d, arbitrary swizzle %d, no "
      "dependent read limit %d, no texture instruction limit %d), DynamicFlowControlDepth %d, "
      "NumTemps %d, StaticFlowControlDepth %d, NumInstructionSlots %d; "
      "MaxPShaderInstructionsExecuted %u, MaxPixelShader30InstructionSlots %u\n",
      (UINT)caps.PS20Caps.Caps, (caps.PS20Caps.Caps & D3DPS20CAPS_GRADIENTINSTRUCTIONS) ? 1 : 0,
      (caps.PS20Caps.Caps & D3DPS20CAPS_PREDICATION) ? 1 : 0,
      (caps.PS20Caps.Caps & D3DPS20CAPS_ARBITRARYSWIZZLE) ? 1 : 0,
      (caps.PS20Caps.Caps & D3DPS20CAPS_NODEPENDENTREADLIMIT) ? 1 : 0,
      (caps.PS20Caps.Caps & D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT) ? 1 : 0,
      caps.PS20Caps.DynamicFlowControlDepth, caps.PS20Caps.NumTemps,
      caps.PS20Caps.StaticFlowControlDepth, caps.PS20Caps.NumInstructionSlots,
      (UINT)caps.MaxPShaderInstructionsExecuted, (UINT)caps.MaxPixelShader30InstructionSlots);
    P("mapped results are out = in * 0.125 + 0.5 (c6, c7), decoded back as (out - 0.5) * 8\n");

    const V4 map6 = {0.125f, 0.125f, 0.125f, 0.125f}, map7 = {0.5f, 0.5f, 0.5f, 0.5f};
    auto dec = [](V4 v) {
      return V4{(v.x - 0.5f) * 8.0f, (v.y - 0.5f) * 8.0f, (v.z - 0.5f) * 8.0f, (v.w - 0.5f) * 8.0f};
    };

    struct Test
    {
      const char *name;
      std::string body;     // after the version line
      std::vector<V4> c;    // c0...
      V4 t0, v0;
      bool mapped;
      std::vector<const char *> versions;
    };

    const V4 z = {0, 0, 0, 0};
    const std::vector<const char *> v11_13 = {"ps_1_1", "ps_1_2", "ps_1_3"};
    const std::vector<const char *> v11_14 = {"ps_1_1", "ps_1_2", "ps_1_3", "ps_1_4"};
    const float third = 1.0f / 3.0f;
    std::vector<Test> tests = {
        {"constant, raw", "mov r0, c0\n", {{2, -2, 0.25f, 0.75f}}, z, z, false, v11_14},
        {"constant, mapped", "mov r1, c0\nmad r0, r1, c6, c7\n", {{2, -2, 0.25f, 0.75f}}, z, z, true, v11_14},
        {"constant 1.5 -0.3 0.6 -1.5, mapped",
         "mov r1, c0\nmad r0, r1, c6, c7\n",
         {{1.5f, -0.3f, 0.6f, -1.5f}},
         z,
         z,
         true,
         v11_14},
        {"add, raw (1.5 -1.5 0.6 1.2)", "add r0, c0, c0\n", {{0.75f, -0.75f, 0.3f, 0.6f}}, z, z, false, v11_14},
        {"add, mapped (1.5 -1.5 0.6 1.2)",
         "add r1, c0, c0\nmad r0, r1, c6, c7\n",
         {{0.75f, -0.75f, 0.3f, 0.6f}},
         z,
         z,
         true,
         v11_14},
        {"x4 once then /8 (0.45 -0.45 0.225 -0.225)",
         "mul_x4 r1, c0, c1\nmul r1, r1, c2\nmad r0, r1, c6, c7\n",
         {{0.9f, -0.9f, 0.45f, -0.45f}, {1, 1, 1, 1}, {0.125f, 0.125f, 0.125f, 0.125f}},
         z,
         z,
         true,
         v11_14},
        {"x4 twice then /64 (0.225 -0.225 0.1125 -0.1125)",
         "mul_x4 r1, c0, c1\nmul_x4 r1, r1, c1\nmul r1, r1, c2\nmul r1, r1, c2\nmad r0, r1, c6, "
         "c7\n",
         {{0.9f, -0.9f, 0.45f, -0.45f}, {1, 1, 1, 1}, {0.125f, 0.125f, 0.125f, 0.125f}},
         z,
         z,
         true,
         v11_14},
        {"x4 three times then /512 (0.1125 -0.1125 0.05625 -0.05625)",
         "mul_x4 r1, c0, c1\nmul_x4 r1, r1, c1\nmul_x4 r1, r1, c1\nmul r1, r1, c2\nmul r1, r1, "
         "c2\nmul r1, r1, c2\nmad r0, r1, c6, c7\n",
         {{0.9f, -0.9f, 0.45f, -0.45f}, {1, 1, 1, 1}, {0.125f, 0.125f, 0.125f, 0.125f}},
         z,
         z,
         true,
         v11_14},
        {"x4 five times then /4096 (0.225 -0.225 0.1125 -0.1125)",
         "mul_x4 r1, c0, c1\nmul_x4 r1, r1, c1\nmul_x4 r1, r1, c1\nmul_x4 r1, r1, c1\nmul_x4 r1, "
         "r1, c1\nmul r1, r1, c3\nmul r1, r1, c3\nmad r0, r1, c6, c7\n",
         {{0.9f, -0.9f, 0.45f, -0.45f},
          {1, 1, 1, 1},
          {0.125f, 0.125f, 0.125f, 0.125f},
          {1.0f / 64, 1.0f / 64, 1.0f / 64, 1.0f / 64}},
         z,
         z,
         true,
         v11_13},
        {"x8 four times then /4096 (0.9 -0.9 0.45 -0.45)",
         "mul_x8 r1, c0, c1\nmul_x8 r1, r1, c1\nmul_x8 r1, r1, c1\nmul_x8 r1, r1, c1\nmul r1, r1, "
         "c3\nmul r1, r1, c3\nmad r0, r1, c6, c7\n",
         {{0.9f, -0.9f, 0.45f, -0.45f},
          {1, 1, 1, 1},
          {0.125f, 0.125f, 0.125f, 0.125f},
          {1.0f / 64, 1.0f / 64, 1.0f / 64, 1.0f / 64}},
         z,
         z,
         true,
         {"ps_1_4"}},
        {"x4 to 171993 (above 65504) then /262144 (0.656 0.656 0.0410 0.0410, or 0.250 if "
         "clamped at 65504)",
         "mul_x4 r1, c0, c1\nmul_x4 r1, r1, r1\nmul_x4 r1, r1, r1\nmul_x4 r1, r1, c1\nmul_x4 r1, "
         "r1, c1\nmul r1, r1, c4\nmul r1, r1, c4\nmad r0, r1, c6, c7\n",
         {{0.9f, -0.9f, 0.45f, -0.45f},
          {1, 1, 1, 1},
          {0, 0, 0, 0},
          {0, 0, 0, 0},
          {1.0f / 512, 1.0f / 512, 1.0f / 512, 1.0f / 512}},
         z,
         z,
         true,
         v11_13},
        {"x8 to 1375949 (above 65504) then /2097152 (0.656 0.656 0.0410 0.0410, or 0.0312 if "
         "clamped at 65504)",
         "mul_x8 r1, c0, c1\nmul_x8 r1, r1, r1\nmul_x8 r1, r1, r1\nmul r1, r1, c3\nmul r1, r1, "
         "c3\nmul r1, r1, c3\nmul_d8 r1, r1, c1\nmad r0, r1, c6, c7\n",
         {{0.9f, -0.9f, 0.45f, -0.45f},
          {1, 1, 1, 1},
          {0.125f, 0.125f, 0.125f, 0.125f},
          {1.0f / 64, 1.0f / 64, 1.0f / 64, 1.0f / 64}},
         z,
         z,
         true,
         {"ps_1_4"}},
        {"precision: c0 * c0, raw (1/9 0.01 0.49 0.998001)",
         "mul r0, c0, c0\n",
         {{third, 0.1f, 0.7f, 0.999f}},
         z,
         z,
         false,
         v11_14},
        {"precision: c0 * c1, raw (0.1234567 * 0.7654321 ...)",
         "mul r0, c0, c1\n",
         {{0.1234567f, 0.00390625f, 1.0f / 1024, 0.5f + 1.0f / 4096},
          {0.7654321f, 0.999f, 0.999f, 0.999f}},
         z,
         z,
         false,
         v11_14},
        {"_bx2 source",
         "mov r1, c0\nmov r0, r1_bx2\nmad r0, r0, c6, c7\n",
         {{0.9f, -0.3f, 0.6f, 1.5f}},
         z,
         z,
         true,
         v11_13},
        {"_bias source",
         "mov r1, c0\nmov r0, r1_bias\nmad r0, r0, c6, c7\n",
         {{0.9f, -0.3f, 0.6f, 1.5f}},
         z,
         z,
         true,
         v11_13},
        {"1-x source",
         "mov r1, c0\nmov r0, 1-r1\nmad r0, r0, c6, c7\n",
         {{0.9f, -0.3f, 0.6f, 1.5f}},
         z,
         z,
         true,
         v11_13},
        {"negate source",
         "mov r1, c0\nmov r0, -r1\nmad r0, r0, c6, c7\n",
         {{0.9f, -0.3f, 0.6f, 1.5f}},
         z,
         z,
         true,
         v11_13},
        {"_x2 dest",
         "mov r1, c0\nmov_x2 r0, r1\nmad r0, r0, c6, c7\n",
         {{0.9f, -0.3f, 0.6f, 1.5f}},
         z,
         z,
         true,
         v11_14},
        {"_d2 dest",
         "mov r1, c0\nmov_d2 r0, r1\nmad r0, r0, c6, c7\n",
         {{0.9f, -0.3f, 0.6f, 1.5f}},
         z,
         z,
         true,
         v11_14},
        {"_sat dest",
         "mov r1, c0\nmov_sat r0, r1\nmad r0, r0, c6, c7\n",
         {{0.9f, -0.3f, 0.6f, 1.5f}},
         z,
         z,
         true,
         v11_14},
        {"_x2 source (1_4)",
         "mov r1, c0\nmov r0, r1_x2\nmad r0, r0, c6, c7\n",
         {{0.9f, -0.3f, 0.6f, 1.5f}},
         z,
         z,
         true,
         {"ps_1_4"}},
        {"_d8 dest (1_4)",
         "mov r1, c0\nmov_d8 r0, r1\nmad r0, r0, c6, c7\n",
         {{0.9f, -0.3f, 0.6f, 1.5f}},
         z,
         z,
         true,
         {"ps_1_4"}},
        {"_bx2 of texcoord (0.9 -0.3 0.6 1.5)",
         "texcoord t0\nmov r0, t0_bx2\nmad r0, r0, c6, c7\n",
         {},
         {0.9f, -0.3f, 0.6f, 1.5f},
         z,
         true,
         v11_13},
        {"texcoord t0, mapped (-0.5 0.3 1.7 3)",
         "texcoord t0\nmad r0, t0, c6, c7\n",
         {},
         {-0.5f, 0.3f, 1.7f, 3.0f},
         z,
         true,
         v11_13},
        {"texcoord t0, mapped (2.5 -2 0.999 0.0001)",
         "texcoord t0\nmad r0, t0, c6, c7\n",
         {},
         {2.5f, -2.0f, 0.999f, 0.0001f},
         z,
         true,
         v11_13},
        {"texcoord t0, raw (0.123456789 1/3 1e-5 0.999)",
         "texcoord t0\nmov r0, t0\n",
         {},
         {0.123456789f, third, 1e-5f, 0.999f},
         z,
         false,
         v11_13},
        {"texcrd, mapped (-0.5 0.3 1.7 3)",
         "texcrd r1.rgb, t0.xyz\nmad r0.rgb, r1, c6, c7\nmov r0.a, c7\n",
         {},
         {-0.5f, 0.3f, 1.7f, 3.0f},
         z,
         true,
         {"ps_1_4"}},
        {"texcrd, mapped (2.5 -2 0.999 0.0001)",
         "texcrd r1.rgb, t0.xyz\nmad r0.rgb, r1, c6, c7\nmov r0.a, c7\n",
         {},
         {2.5f, -2.0f, 0.999f, 0.0001f},
         z,
         true,
         {"ps_1_4"}},
        {"v0, mapped (-0.5 1.7 0.123456789 1/3)",
         "mad r0, v0, c6, c7\n",
         {},
         z,
         {-0.5f, 1.7f, 0.123456789f, third},
         true,
         v11_14},
        {"v0, raw (0.123456789 1/3 1e-5 0.999)",
         "mov r0, v0\n",
         {},
         z,
         {0.123456789f, third, 1e-5f, 0.999f},
         false,
         v11_14},
        {"cmp with -0 0 -1e-30 1e-30",
         "mov r1, c0\ncmp r0, r1, c1, c2\n",
         {{-0.0f, 0.0f, -1e-30f, 1e-30f}, {0.75f, 0.75f, 0.75f, 0.75f}, {0.25f, 0.25f, 0.25f, 0.25f}},
         z,
         z,
         false,
         {"ps_1_2", "ps_1_3", "ps_1_4"}},
    };

    // sampling a float texture holding values outside 0..1
    IDirect3DTexture9Ptr tex;
    dev->CreateTexture(1, 1, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &tex, NULL);
    {
      D3DLOCKED_RECT lr = {};
      tex->LockRect(0, &lr, NULL, 0);
      float texel[4] = {2.0f, -1.0f, 0.3f, 3.5f};
      memcpy(lr.pBits, texel, 16);
      tex->UnlockRect(0);
    }

    for(const Test &t : tests)
    {
      for(const char *ver : t.versions)
      {
        std::vector<V4> c(8, z);
        for(size_t i = 0; i < t.c.size(); i++)
          c[i] = t.c[i];
        c[6] = map6;
        c[7] = map7;
        HRESULT hr = S_OK;
        V4 o = RunOne(std::string(ver) + "\n" + t.body, c.data(), 8, t.t0, t.v0, NULL, &hr);
        P("%s %s: draw %08x raw %.9g %.9g %.9g %.9g", ver, t.name, (UINT)hr, o.x, o.y, o.z, o.w);
        if(t.mapped)
        {
          V4 d = dec(o);
          P(" decoded %.9g %.9g %.9g %.9g", d.x, d.y, d.z, d.w);
        }
        P("\n");
      }
    }

    for(const char *ver : v11_14)
    {
      std::vector<V4> c(8, z);
      c[6] = map6;
      c[7] = map7;
      std::string src = std::string(ver) + "\n" +
                        (strcmp(ver, "ps_1_4") ? "tex t0\nmad r0, t0, c6, c7\n"
                                               : "texld r0, t0\nmad r0, r0, c6, c7\n");
      HRESULT hr = S_OK;
      V4 o = RunOne(src, c.data(), 8, {0.5f, 0.5f, 0, 1}, z, tex, &hr);
      V4 d = dec(o);
      P("%s float texture (2 -1 0.3 3.5) sampled, mapped: draw %08x raw %.9g %.9g %.9g %.9g "
        "decoded %.9g %.9g %.9g %.9g\n",
        ver, (UINT)hr, o.x, o.y, o.z, o.w, d.x, d.y, d.z, d.w);
    }

    // what an unbound sampler returns, in each model
    {
      const char *unbound[] = {
          "ps_1_1\ntex t1\nmov r0, t1\n",
          "ps_1_4\ntexld r1, t0\nmov r0, r1\n",
          "ps_2_0\ndcl t0\ndcl_2d s1\ntexld r0, t0, s1\nmov oC0, r0\n",
          "ps_3_0\ndcl_texcoord0 v0\ndcl_2d s1\ntexld r0, v0, s1\nmov oC0, r0\n",
          "ps_3_0\ndcl_texcoord0 v0\ndcl_cube s1\ntexld r0, v0, s1\nmov oC0, r0\n",
          "ps_3_0\ndcl_texcoord0 v0\ndcl_volume s1\ntexld r0, v0, s1\nmov oC0, r0\n",
      };
      for(const char *src : unbound)
      {
        HRESULT hr = S_OK;
        V4 o = RunOne(src, NULL, 0, {0.5f, 0.5f, 0.5f, 1}, z, NULL, &hr);
        std::string first(src, strchr(src, '\n'));
        P("unbound sampler 1, %s (%s): draw %08x %.9g %.9g %.9g %.9g\n", first.c_str(),
          strstr(src, "cube") ? "cube" : (strstr(src, "volume") ? "volume" : "2d"), (UINT)hr, o.x,
          o.y, o.z, o.w);
      }
    }

    // cnd: which side of 0.5 is which
    std::vector<float> as = {0.0f, 1.0f, -0.5f, 0.5f};
    for(int k = 1; k <= 24; k++)
    {
      as.push_back(0.5f - ldexpf(1.0f, -k));
      as.push_back(0.5f + ldexpf(1.0f, -k));
    }
    as.push_back(FromBits(Bits(0.5f) - 1));
    as.push_back(FromBits(Bits(0.5f) + 1));
    P("cnd: the condition's value, then the output per version (0.75 = took src1, 0.25 = src2)\n");
    for(float a : as)
    {
      P("cnd %.9g (0x%08x):", a, Bits(a));
      for(const char *ver : v11_14)
      {
        std::vector<V4> c(8, z);
        c[0] = {a, a, a, a};
        c[1] = {0.75f, 0.75f, 0.75f, 0.75f};
        c[2] = {0.25f, 0.25f, 0.25f, 0.25f};
        std::string src = std::string(ver) + "\n" +
                          (strcmp(ver, "ps_1_4") ? "mov r0, c0\ncnd r0, r0.a, c1, c2\n"
                                                 : "mov r1, c0\ncnd r0, r1, c1, c2\n");
        HRESULT hr = S_OK;
        V4 o = RunOne(src, c.data(), 8, z, z, NULL, &hr);
        P(" %s %.9g %.9g %.9g %.9g", ver, o.x, o.y, o.z, o.w);
        if(FAILED(hr))
          P(" (draw %08x)", (UINT)hr);
      }
      P("\n");
    }

    // the same cnd with the condition from an interpolated colour and texcoord, to see if they're
    // compared at the same precision
    P("cnd with the condition from v0.a / t0.w (texcoord):\n");
    for(float a : as)
    {
      P("cnd %.9g:", a);
      for(const char *ver : v11_13)
      {
        std::vector<V4> c(8, z);
        c[1] = {0.75f, 0.75f, 0.75f, 0.75f};
        c[2] = {0.25f, 0.25f, 0.25f, 0.25f};
        V4 o1 = RunOne(std::string(ver) + "\nmov r0, v0\ncnd r0, r0.a, c1, c2\n", c.data(), 8, z,
                       {a, a, a, a});
        V4 o2 = RunOne(std::string(ver) + "\ntexcoord t0\nmov r0, t0\ncnd r0, r0.a, c1, c2\n",
                       c.data(), 8, {a, a, a, a}, z);
        P(" %s v0 %.9g t0 %.9g", ver, o1.x, o2.x);
      }
      P("\n");
    }
  }

  //////////////////////////////////////////////////////////////////////////////////////////////
  // D: colour inputs

  void Colours()
  {
    Open("d_colour");
    const V4 z = {0, 0, 0, 0};
    const float third = 1.0f / 3.0f;
    const V4 sets[] = {
        {-0.5f, 1.7f, 0.123456789f, third},
        {0.5f + 1.0f / 1024, 1e-5f, 0.999f, 2.0f / 255 + 0.0001f},
        {1.0f / 255, 254.5f / 255, 0.75f, 0.0009765625f},
    };
    struct Model
    {
      const char *name;
      const char *src;
    };
    const Model models[] = {
        {"ps_1_1 v0 raw", "ps_1_1\nmov r0, v0\n"},
        {"ps_1_4 v0 raw", "ps_1_4\nmov r0, v0\n"},
        {"ps_2_0 v0 (vs_2_0 oD0)", "ps_2_0\ndcl v0\nmov oC0, v0\n"},
        {"ps_2_0 t0 (vs_2_0 oT0)", "ps_2_0\ndcl t0\nmov oC0, t0\n"},
        {"ps_3_0 color0 (vs_3_0 color0)", "ps_3_0\ndcl_color0 v0\nmov oC0, v0\n"},
        {"ps_3_0 texcoord0 (vs_3_0 texcoord0)", "ps_3_0\ndcl_texcoord0 v0\nmov oC0, v0\n"},
    };
    P("flat values (every vertex the same), read back from an A32B32G32R32F target. The ps_2_0 "
      "and ps_1_x draws use a vs_2_0 writing oD0/oT0, the ps_3_0 ones a vs_3_0\n");
    for(const V4 &s : sets)
    {
      P("input %.9g %.9g %.9g %.9g\n", s.x, s.y, s.z, s.w);
      for(const Model &m : models)
      {
        HRESULT hr = S_OK;
        V4 o = RunOne(m.src, NULL, 0, s, s, NULL, &hr);
        P("  %s: draw %08x %.9g %.9g %.9g %.9g\n", m.name, (UINT)hr, o.x, o.y, o.z, o.w);
      }
    }

    // a gradient, and the same with perspective: is colour interpolated like a texcoord?
    const UINT W = 256, H = 4;
    Targets t = MakeTargets(W, H, 1);
    const Model grads[] = {
        {"ps_2_0 v0", "ps_2_0\ndcl v0\nmov oC0, v0\n"},
        {"ps_2_0 t0", "ps_2_0\ndcl t0\nmov oC0, t0\n"},
        {"ps_1_1 v0", "ps_1_1\nmov r0, v0\n"},
        {"ps_1_1 t0 (texcoord)", "ps_1_1\ntexcoord t0\nmov r0, t0\n"},
        {"ps_3_0 color0", "ps_3_0\ndcl_color0 v0\nmov oC0, v0\n"},
        {"ps_3_0 texcoord0", "ps_3_0\ndcl_texcoord0 v0\nmov oC0, v0\n"},
    };
    for(float wr : {1.0f, 4.0f})
    {
      auto c = [&](float sx, float sy, float w) {
        Vert r = At(sx, sy, float(W), float(H), 0.5f, w);
        float f = sx / W;
        r.t[0] = {f, 1.0f - f, 0.5f, 1.0f};
        r.c[0] = r.t[0];
        return r;
      };
      std::vector<Vert> v;
      PushQuad(v, c(0, 0, 1.0f), c(float(W), 0, wr), c(0, float(H), 1.0f), c(float(W), float(H), wr));
      std::vector<std::vector<V4>> res;
      for(const Model &m : grads)
      {
        ResetState();
        Bind(t);
        HRESULT hr = DrawSrc(m.src, v);
        res.push_back(Read(t.rt[0]));
        P("gradient 0..1 across %u pixels, right w %g: %s draw %08x\n", W, wr, m.name, (UINT)hr);
      }
      P("gradient right w %g, row 1, pixel x: value.x per model (", wr);
      for(const Model &m : grads)
        P("%s; ", m.name);
      P(")\n");
      for(UINT x = 0; x < W; x++)
      {
        P("  %u:", x);
        for(auto &r : res)
          P(" %.9g", r[W + x].x);
        P("\n");
      }
    }

    // flat shading: which vertex's colour, and does it flatten texcoords too?
    {
      const UINT FW = 16, FH = 16;
      Targets ft = MakeTargets(FW, FH, 1);
      auto c = [&](float sx, float sy, V4 col) {
        Vert r = At(sx, sy, float(FW), float(FH));
        r.t[0] = col;
        r.c[0] = col;
        return r;
      };
      std::vector<Vert> v = {c(1, 1, {1, 0, 0, 1}), c(15, 1, {0, 1, 0, 1}), c(1, 15, {0, 0, 1, 1})};
      for(DWORD shade : {D3DSHADE_FLAT, D3DSHADE_GOURAUD})
        for(const Model &m : grads)
        {
          ResetState();
          Bind(ft);
          dev->SetRenderState(D3DRS_SHADEMODE, shade);
          HRESULT hr = DrawSrc(m.src, v);
          V4 o = Read(ft.rt[0])[5 * FW + 5];
          P("%s, triangle with red, green, blue vertices, %s: draw %08x pixel 5,5 = %.9g %.9g %.9g "
            "%.9g\n",
            shade == D3DSHADE_FLAT ? "flat" : "gouraud", m.name, (UINT)hr, o.x, o.y, o.z, o.w);
        }
    }
  }

  //////////////////////////////////////////////////////////////////////////////////////////////
  // E: derivatives

  void Derivatives()
  {
    Open("e_derivatives");
    const UINT W = 16, H = 8;
    Targets t = MakeTargets(W, H, 4);
    auto c = [&](float sx, float sy) {
      Vert r = At(sx, sy, float(W), float(H));
      r.t[0] = {sx, sy, 0.0f, 1.0f};
      return r;
    };
    // f = x*y, g = x*x, h = y*y, w = x (x and y the interpolated screen position)
    const char *src =
        "ps_3_0\ndcl_texcoord0 v0\nmul r0.x, v0.x, v0.y\nmul r0.y, v0.x, v0.x\nmul r0.z, v0.y, "
        "v0.y\nmov r0.w, v0.x\ndsx r1, r0\ndsy r2, r0\ndsx r3.xy, v0\ndsy r3.zw, v0.xyxy\nmov oC0, "
        "r1\nmov oC1, r2\nmov oC2, r0\nmov oC3, r3\n";

    const char *src2x =
        "ps_2_x\ndef c0, 0, 0, 0, 0\ndcl t0\nmul r0.x, t0.x, t0.y\nmul r0.y, t0.x, t0.x\nmul r0.z, "
        "t0.y, t0.y\nmov r0.w, t0.x\ndsx r1, r0\ndsy r2, r0\ndsx r3.xy, t0\ndsy r4, t0\nmov "
        "r3.zw, r4.xyxy\nmov oC0, r1\nmov oC1, r2\nmov oC2, r0\nmov oC3, r3\n";

    struct Case
    {
      const char *name;
      std::vector<Vert> v;
      bool scissor;
    };
    std::vector<Case> cases;
    {
      Case cs = {"full quad", {}, false};
      PushQuad(cs.v, c(0, 0), c(float(W), 0), c(0, float(H)), c(float(W), float(H)));
      cases.push_back(cs);
    }
    // small triangles covering one pixel of a 2x2 quad (pixel 5,3: the bottom right of the quad at
    // 4,2), and one covering pixel 4,2 (the top left)
    cases.push_back(
        {"triangle covering only pixel 5,3", {c(5.0f, 2.8f), c(5.9f, 3.4f), c(5.0f, 3.9f)}, false});
    cases.push_back(
        {"triangle covering only pixel 4,2", {c(3.9f, 1.8f), c(4.9f, 2.4f), c(3.9f, 2.9f)}, false});
    cases.push_back(
        {"triangle covering only pixel 9,5", {c(8.9f, 4.8f), c(9.9f, 5.4f), c(8.9f, 5.9f)}, false});
    {
      Case cs = {"full quad, scissor to pixel 5,3", {}, true};
      PushQuad(cs.v, c(0, 0), c(float(W), 0), c(0, float(H)), c(float(W), float(H)));
      cases.push_back(cs);
    }

    std::vector<std::pair<const char *, Case>> runs;
    for(const Case &cs : cases)
      runs.push_back({src, cs});
    runs.push_back({src2x, cases[0]});
    runs.push_back({src2x, cases[1]});
    for(const auto &run : runs)
    {
      const Case &cs = run.second;
      ResetState();
      Bind(t);
      if(cs.scissor)
      {
        RECT r = {5, 3, 6, 4};
        dev->SetScissorRect(&r);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
      }
      HRESULT hr = DrawSrc(run.first, cs.v);
      std::vector<V4> dx = Read(t.rt[0]), dy = Read(t.rt[1]), val = Read(t.rt[2]), dv = Read(t.rt[3]);
      P("%s, %s (draw %08x). f = x*y, g = x*x, h = y*y, w = x, with x and y the interpolated "
        "screen position\n",
        run.first == src ? "ps_3_0" : "ps_2_x", cs.name, (UINT)hr);
      P("pixel x y: value (f g h w) | dsx (f g h w) | dsy (f g h w) | dsx(xy) dsy(xy)\n");
      for(UINT y = 0; y < H; y++)
        for(UINT x = 0; x < W; x++)
        {
          UINT i = y * W + x;
          if(Unwritten(val[i]))
            continue;
          P("%u %u: %.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g | %.9g %.9g "
            "%.9g %.9g\n",
            x, y, val[i].x, val[i].y, val[i].z, val[i].w, dx[i].x, dx[i].y, dx[i].z, dx[i].w,
            dy[i].x, dy[i].y, dy[i].z, dy[i].w, dv[i].x, dv[i].y, dv[i].z, dv[i].w);
        }
    }
  }

  //////////////////////////////////////////////////////////////////////////////////////////////
  // F: texldd against texld

  void Texldd()
  {
    Open("f_texldd");
    const UINT S = 64, LEVELS = 7;
    IDirect3DTexture9Ptr tex;
    dev->CreateTexture(S, S, LEVELS, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &tex, NULL);
    for(UINT l = 0; l < LEVELS; l++)
    {
      UINT s = S >> l;
      D3DLOCKED_RECT lr = {};
      tex->LockRect(l, &lr, NULL, 0);
      for(UINT y = 0; y < s; y++)
        for(UINT x = 0; x < s; x++)
        {
          float *p = (float *)((byte *)lr.pBits + y * lr.Pitch) + x * 4;
          p[0] = float(l);
          p[1] = float((x + y) & 1);
          p[2] = (float(x) + 0.5f) / float(s);
          p[3] = (float(y) + 0.5f) / float(s);
        }
      tex->UnlockRect(l);
    }

    const UINT W = 64, H = 64;
    Targets t = MakeTargets(W, H, 4);
    Targets one = MakeTargets(W, H, 1);

    struct Mapping
    {
      const char *name;
      // uv of the four corners, and w of the right hand side
      float uv[4][2];
      float wr;
    };
    const float r30c = cosf(0.5235988f) * 2.5f, r30s = sinf(0.5235988f) * 2.5f;
    const Mapping maps[] = {
        {"one texel per pixel", {{0, 0}, {1, 0}, {0, 1}, {1, 1}}, 1.0f},
        {"scale 3.7", {{0, 0}, {3.7f, 0}, {0, 3.7f}, {3.7f, 3.7f}}, 1.0f},
        {"u scale 8, v scale 1", {{0, 0}, {8, 0}, {0, 1}, {8, 1}}, 1.0f},
        {"rotated 30 degrees, scale 2.5",
         {{0, 0}, {r30c, r30s}, {-r30s, r30c}, {r30c - r30s, r30s + r30c}},
         1.0f},
        {"perspective, right w 5, scale 4", {{0, 0}, {4, 0}, {0, 4}, {4, 4}}, 5.0f},
        {"scale 40", {{0, 0}, {40, 0}, {0, 40}, {40, 40}}, 1.0f},
        {"mirrored, u from 2 to 0", {{2, 0}, {0, 0}, {2, 2}, {0, 2}}, 1.0f},
        {"subtexel offset 0.3, scale 1.3",
         {{0.3f / 64, 0.1f / 64}, {1.3f, 0.1f / 64}, {0.3f / 64, 1.3f}, {1.3f, 1.3f}},
         1.0f},
    };

    struct Config
    {
      const char *name;
      DWORD mag, min, mip, aniso, maxmip;
      float bias;
    };
    const Config configs[] = {
        {"trilinear", D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, 1, 0, 0.0f},
        {"point, point mip", D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_POINT, 1, 0, 0.0f},
        {"bilinear, point mip", D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_POINT, 1, 0, 0.0f},
        {"anisotropic 4, linear mip", D3DTEXF_LINEAR, D3DTEXF_ANISOTROPIC, D3DTEXF_LINEAR, 4, 0, 0.0f},
        {"anisotropic 16, linear mip", D3DTEXF_ANISOTROPIC, D3DTEXF_ANISOTROPIC, D3DTEXF_LINEAR, 16,
         0, 0.0f},
        {"trilinear, bias 0.5", D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, 1, 0, 0.5f},
        {"trilinear, maxmiplevel 2", D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, 1, 2, 0.0f},
    };

    auto setSampler = [&](const Config &cf) {
      dev->SetTexture(0, tex);
      dev->SetSamplerState(0, D3DSAMP_MAGFILTER, cf.mag);
      dev->SetSamplerState(0, D3DSAMP_MINFILTER, cf.min);
      dev->SetSamplerState(0, D3DSAMP_MIPFILTER, cf.mip);
      dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, cf.aniso);
      dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, cf.maxmip);
      dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, *(DWORD *)&cf.bias);
    };

    const char *pass1 =
        "ps_3_0\ndef c0, 0, 0, 0, 0\ndcl_texcoord0 v0\ndcl_2d s0\ntexld r0, v0, s0\ndsx r1, "
        "v0\ndsy r2, v0\ntexldd r3, v0, s0, r1, r2\nmov oC0, r0\nmov r4, c0\nmov r4.xy, v0\nmov "
        "r4.zw, r1.xyxy\nmov oC1, r4\nmov r5, c0\nmov r5.xy, r2\nmov oC2, r5\nmov oC3, r3\n";
    const char *pass2 =
        "ps_3_0\ndcl_2d s0\nmov r0, c0\nmov r1, c1\nmov r2, c2\ntexldd r3, r0, s0, r1, r2\nmov "
        "oC0, "
        "r3\n";

    P("texture: 64x64, 7 levels, A32B32G32R32F, texel = (level, checker, u centre, v centre)\n");
    P("pass 1 (one draw, ps_3_0): texld, and texldd with dsx/dsy of the same coordinate. "
      "pass 2: a one pixel draw per pixel, texldd with pass 1's coordinate and derivatives as "
      "constants\n");

    FILE *diffs = fopen((outDir + "/f_texldd_diffs.txt").c_str(), "w");

    for(const Config &cf : configs)
      for(const Mapping &m : maps)
      {
        auto c = [&](float sx, float sy, int corner, float w) {
          Vert r = At(sx, sy, float(W), float(H), 0.5f, w);
          r.t[0] = {m.uv[corner][0], m.uv[corner][1], 0.0f, 1.0f};
          return r;
        };
        std::vector<Vert> v;
        PushQuad(v, c(0, 0, 0, 1.0f), c(float(W), 0, 1, m.wr), c(0, float(H), 2, 1.0f),
                 c(float(W), float(H), 3, m.wr));
        ResetState();
        Bind(t);
        setSampler(cf);
        HRESULT hr = DrawSrc(pass1, v);
        std::vector<V4> tld = Read(t.rt[0]), uvdx = Read(t.rt[1]), dyv = Read(t.rt[2]),
                        tldd = Read(t.rt[3]);

        // pass 2
        ResetState();
        Bind(one);
        setSampler(cf);
        IDirect3DPixelShader9 *ps2 = PS(pass2);
        HRESULT hr2 = S_OK;
        for(UINT y = 0; y < H; y++)
          for(UINT x = 0; x < W; x++)
          {
            UINT i = y * W + x;
            V4 cs[3] = {{uvdx[i].x, uvdx[i].y, 0, 0},
                        {uvdx[i].z, uvdx[i].w, 0, 0},
                        {dyv[i].x, dyv[i].y, 0, 0}};
            dev->SetPixelShaderConstantF(0, &cs[0].x, 3);
            auto cp = [&](float sx, float sy) { return At(sx, sy, float(W), float(H)); };
            std::vector<Vert> q;
            float x0 = float(x) - 0.25f, y0 = float(y) - 0.25f;
            PushQuad(q, cp(x0, y0), cp(x0 + 1, y0), cp(x0, y0 + 1), cp(x0 + 1, y0 + 1));
            HRESULT h = Draw(ps2, q, D3DPT_TRIANGLELIST, true);
            if(FAILED(h))
              hr2 = h;
          }
        std::vector<V4> cdd = Read(one.rt[0]);

        int sameDD = 0, sameConst = 0, n = 0, written = 0;
        float maxDD = 0.0f, maxConst = 0.0f, maxLodDD = 0.0f, maxLodConst = 0.0f;
        for(UINT i = 0; i < W * H; i++)
        {
          n++;
          auto diff = [](const V4 &a, const V4 &b) {
            float d = fabsf(a.x - b.x);
            d = fmaxf(d, fabsf(a.y - b.y));
            d = fmaxf(d, fabsf(a.z - b.z));
            d = fmaxf(d, fabsf(a.w - b.w));
            return d;
          };
          float d1 = diff(tld[i], tldd[i]), d2 = diff(tld[i], cdd[i]);
          if(d1 == 0.0f)
            sameDD++;
          if(d2 == 0.0f)
            sameConst++;
          maxDD = fmaxf(maxDD, d1);
          maxConst = fmaxf(maxConst, d2);
          maxLodDD = fmaxf(maxLodDD, fabsf(tld[i].x - tldd[i].x));
          maxLodConst = fmaxf(maxLodConst, fabsf(tld[i].x - cdd[i].x));
          if((d1 != 0.0f || d2 != 0.0f) && diffs && ++written <= 64)
            fprintf(diffs,
                    "%s | %s | pixel %u %u uv %.9g %.9g dx %.9g %.9g dy %.9g %.9g: texld %.9g %.9g "
                    "%.9g %.9g texldd %.9g %.9g %.9g %.9g const %.9g %.9g %.9g %.9g\n",
                    cf.name, m.name, i % W, i / W, uvdx[i].x, uvdx[i].y, uvdx[i].z, uvdx[i].w,
                    dyv[i].x, dyv[i].y, tld[i].x, tld[i].y, tld[i].z, tld[i].w, tldd[i].x,
                    tldd[i].y, tldd[i].z, tldd[i].w, cdd[i].x, cdd[i].y, cdd[i].z, cdd[i].w);
        }
        float lodMin = 1e9f, lodMax = -1e9f;
        for(const V4 &s : tld)
        {
          lodMin = fminf(lodMin, s.x);
          lodMax = fmaxf(lodMax, s.x);
        }
        P("%s | %s: draws %08x %08x, texld level blend %.4g..%.4g. texldd in the shader: %d/%d "
          "the same, max diff %.9g (level %.9g). one pixel texldd from constants: %d/%d the "
          "same, max diff %.9g (level %.9g)\n",
          cf.name, m.name, (UINT)hr, (UINT)hr2, lodMin, lodMax, sameDD, n, maxDD, maxLodDD,
          sameConst, n, maxConst, maxLodConst);
      }

    if(diffs)
      fclose(diffs);
  }

  //////////////////////////////////////////////////////////////////////////////////////////////
  // G: transcendentals and other arithmetic

  struct Rng
  {
    uint32_t s = 0x12345678;
    uint32_t Next()
    {
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      return s;
    }
    float Uniform(float a, float b) { return a + (b - a) * float(Next() >> 8) / 16777216.0f; }
    // a random float with a random exponent in [lo, hi] and random mantissa
    float Log(int lo, int hi, bool sign)
    {
      int e = lo + int(Next() % uint32_t(hi - lo + 1));
      float m = 1.0f + float(Next() >> 9) / 8388608.0f;
      float f = ldexpf(m, e);
      if(sign && (Next() & 1))
        f = -f;
      return f;
    }
  };

  void Arithmetic()
  {
    Open("g_summary");
    const UINT W = 256, H = 32;
    Targets t = MakeTargets(W, H, 2);
    IDirect3DTexture9Ptr tex;
    dev->CreateTexture(W, H, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &tex, NULL);

    const float specials[] = {0.0f,
                              -0.0f,
                              1.0f,
                              -1.0f,
                              2.0f,
                              0.5f,
                              3.0f,
                              -3.0f,
                              FromBits(1),
                              FromBits(0x007fffff),
                              FLT_MIN,
                              -FLT_MIN,
                              FLT_MAX,
                              -FLT_MAX,
                              INFINITY,
                              -INFINITY,
                              NAN,
                              100.0f,
                              -100.0f,
                              1e10f,
                              1e-10f,
                              127.0f,
                              128.0f,
                              129.0f,
                              -126.0f,
                              -127.0f,
                              -149.0f,
                              -150.0f,
                              FromBits(Bits(1.0f) - 1),
                              FromBits(Bits(1.0f) + 1),
                              3.14159265f,
                              -3.14159265f,
                              6.28318531f,
                              1.57079633f};

    struct Op
    {
      const char *name;
      const char *body;
      int kind;    // input distribution
    };
    enum
    {
      K_UNARY,
      K_EXP,
      K_TRIG,
      K_POW,
      K_BINARY,
      K_MAD,
      K_VEC,
    };
    const Op ops[] = {
        {"rcp", "rcp r1.x, r0.x\n", K_UNARY},
        {"rsq", "rsq r1.x, r0.x\n", K_UNARY},
        {"log", "log r1.x, r0.x\n", K_UNARY},
        {"exp", "exp r1.x, r0.x\n", K_EXP},
        {"pow", "pow r1.x, r0.x, r0.y\n", K_POW},
        {"sincos", "sincos r1.xy, r0.x\n", K_TRIG},
        {"rcp_pp", "rcp_pp r1.x, r0.x\n", K_UNARY},
        {"rsq_pp", "rsq_pp r1.x, r0.x\n", K_UNARY},
        {"log_pp", "log_pp r1.x, r0.x\n", K_UNARY},
        {"exp_pp", "exp_pp r1.x, r0.x\n", K_EXP},
        {"pow_pp", "pow_pp r1.x, r0.x, r0.y\n", K_POW},
        {"sincos_pp", "sincos_pp r1.xy, r0.x\n", K_TRIG},
        {"mul_pp", "mul_pp r1.x, r0.x, r0.y\n", K_BINARY},
        {"mul", "mul r1.x, r0.x, r0.y\n", K_BINARY},
        {"add", "add r1.x, r0.x, r0.y\n", K_BINARY},
        {"mad", "mad r1.x, r0.x, r0.y, r0.z\n", K_MAD},
        {"mul_add", "mul r2.x, r0.x, r0.y\nadd r1.x, r2.x, r0.z\n", K_MAD},
        {"min", "min r1.x, r0.x, r0.y\n", K_BINARY},
        {"max", "max r1.x, r0.x, r0.y\n", K_BINARY},
        {"cmp", "cmp r1.x, r0.x, c0.x, c0.y\n", K_UNARY},
        {"frc", "frc r1.x, r0.x\n", K_UNARY},
        {"dp3", "dp3 r1.x, r0, r0.yzxw\n", K_VEC},
        {"dp4", "dp4 r1.x, r0, r0.yzwx\n", K_VEC},
        {"dp2add", "dp2add r1.x, r0, r0.zwzw, r0.z\n", K_VEC},
        {"nrm", "nrm r1.xyz, r0\n", K_VEC},
        {"lrp", "lrp r1.x, r0.x, r0.y, r0.z\n", K_VEC},
    };

    for(const Op &op : ops)
    {
      Rng rng;
      std::vector<V4> in;
      auto add = [&](float x, float y, float z, float w) { in.push_back({x, y, z, w}); };
      switch(op.kind)
      {
        case K_UNARY:
          for(float s : specials)
            add(s, 1.0f, 0.0f, 0.0f);
          for(int i = 0; i < 2048; i++)
            add(rng.Log(-60, 60, true), 0, 0, 0);
          for(int i = 0; i < 1024; i++)
            add(rng.Uniform(1.0f, 2.0f), 0, 0, 0);
          for(int i = 0; i < 1024; i++)
            add(rng.Uniform(-1.0f, 1.0f), 0, 0, 0);
          for(int i = 0; i < 256; i++)
            add(rng.Log(-149, -127, true), 0, 0, 0);
          break;
        case K_EXP:
          for(float s : specials)
            add(s, 0, 0, 0);
          for(int i = 0; i < 2048; i++)
            add(rng.Uniform(-130.0f, 130.0f), 0, 0, 0);
          for(int i = 0; i < 2048; i++)
            add(rng.Uniform(-1.0f, 1.0f), 0, 0, 0);
          for(int i = 0; i < 512; i++)
            add(rng.Uniform(-8.0f, 8.0f), 0, 0, 0);
          break;
        case K_TRIG:
          for(float s : specials)
            add(s, 0, 0, 0);
          for(int i = 0; i < 2048; i++)
            add(rng.Uniform(-3.14159265f, 3.14159265f), 0, 0, 0);
          for(int i = 0; i < 1024; i++)
            add(rng.Uniform(-100.0f, 100.0f), 0, 0, 0);
          for(int i = 0; i < 512; i++)
            add(rng.Log(-20, 20, true), 0, 0, 0);
          for(int k = -8; k <= 8; k++)
            add(float(k) * 1.57079633f, 0, 0, 0);
          break;
        case K_POW:
          for(float s : specials)
            for(float e : {0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 2.2f, INFINITY, NAN})
              add(s, e, 0, 0);
          for(int i = 0; i < 2048; i++)
            add(rng.Log(-10, 10, true), rng.Uniform(-8.0f, 8.0f), 0, 0);
          for(int i = 0; i < 1024; i++)
            add(rng.Uniform(0.0f, 1.0f), rng.Uniform(0.0f, 64.0f), 0, 0);
          for(int i = 0; i < 512; i++)
            add(rng.Uniform(0.0f, 1.0f), 2.2f, 0, 0);
          break;
        case K_BINARY:
          for(float a : specials)
            for(float b : {0.0f, -0.0f, 1.0f, -1.0f, INFINITY, -INFINITY, NAN, FromBits(1), 0.5f})
              add(a, b, 0, 0);
          for(int i = 0; i < 2048; i++)
            add(rng.Log(-60, 60, true), rng.Log(-60, 60, true), 0, 0);
          for(int i = 0; i < 1024; i++)
            add(rng.Uniform(-4.0f, 4.0f), rng.Uniform(-4.0f, 4.0f), 0, 0);
          for(int i = 0; i < 256; i++)
            add(rng.Log(-149, -120, true), rng.Uniform(0.25f, 4.0f), 0, 0);
          break;
        case K_MAD:
          for(int i = 0; i < 2048; i++)
          {
            float x = rng.Uniform(-4.0f, 4.0f), y = rng.Uniform(-4.0f, 4.0f);
            add(x, y, -(x * y), 0);
          }
          for(int i = 0; i < 2048; i++)
            add(rng.Uniform(-4.0f, 4.0f), rng.Uniform(-4.0f, 4.0f), rng.Uniform(-4.0f, 4.0f), 0);
          for(int i = 0; i < 256; i++)
            add(rng.Log(-140, -120, true), rng.Uniform(0.25f, 4.0f), rng.Log(-140, -120, true), 0);
          break;
        case K_VEC:
          add(0, 0, 0, 0);
          add(1, 0, 0, 0);
          add(3, 4, 0, 0);
          add(1e-30f, 0, 0, 0);
          add(1e30f, 1e30f, 0, 0);
          add(INFINITY, 0, 0, 0);
          add(NAN, 1, 1, 1);
          for(int i = 0; i < 4096; i++)
            add(rng.Uniform(-4.0f, 4.0f), rng.Uniform(-4.0f, 4.0f), rng.Uniform(-4.0f, 4.0f),
                rng.Uniform(-4.0f, 4.0f));
          break;
      }
      if(in.size() > W * H)
        in.resize(W * H);
      size_t n = in.size();
      std::vector<V4> padded = in;
      padded.resize(W * H, V4{1, 1, 1, 1});

      D3DLOCKED_RECT lr = {};
      tex->LockRect(0, &lr, NULL, 0);
      for(UINT y = 0; y < H; y++)
        memcpy((byte *)lr.pBits + y * lr.Pitch, &padded[y * W], W * 16);
      tex->UnlockRect(0);

      std::string src = std::string(
                            "ps_3_0\ndef c0, 1, 0, 0.5, 0\ndcl_texcoord0 v0.xy\ndcl_2d s0\ntexld "
                            "r0, v0, s0\nmov r1, c0.y\n") +
                        op.body + "mov oC0, r1\nmov oC1, r0\n";
      auto c = [&](float sx, float sy) {
        Vert r = At(sx, sy, float(W), float(H));
        r.t[0] = {(sx + 0.25f) / W, (sy + 0.25f) / H, 0, 1};
        return r;
      };
      std::vector<Vert> v;
      PushQuad(v, c(-0.25f, -0.25f), c(W - 0.25f, -0.25f), c(-0.25f, H - 0.25f),
               c(W - 0.25f, H - 0.25f));
      ResetState();
      Bind(t);
      dev->SetTexture(0, tex);
      dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      HRESULT hr = DrawSrc(src, v);
      std::vector<V4> res = Read(t.rt[0]), echo = Read(t.rt[1]);

      FILE *f = fopen((outDir + "/g_" + op.name + ".txt").c_str(), "w");
      fprintf(f,
              "%s (draw %08x): input x y z w | echo differs? | gpu result (x y z) | ref1 | ulps | "
              "ref2 | ulps\n",
              op.name, (UINT)hr);
      int echoBad = 0;
      // stats per reference: over finite results
      struct Stat
      {
        int n = 0, exact = 0, le1 = 0, le2 = 0, le4 = 0, le16 = 0, specialDiff = 0;
        int64_t maxUlp = 0;
        float maxAbs = 0.0f;
        V4 worst = {};
      };
      std::vector<Stat> st;
      std::string pp = op.name;
      bool isPP = pp.find("_pp") != std::string::npos;
      int ppHalfMatch = 0, ppFloatMatch = 0;
      for(size_t i = 0; i < n; i++)
      {
        const V4 &x = in[i];
        const V4 &e = echo[i];
        bool eb = Bits(x.x) != Bits(e.x) || Bits(x.y) != Bits(e.y) || Bits(x.z) != Bits(e.z) ||
                  Bits(x.w) != Bits(e.w);
        if(eb)
          echoBad++;
        // refs computed from what the shader received
        float a = e.x, b = e.y, cc = e.z, d = e.w;
        float gpu = res[i].x;
        std::vector<float> ref;
        std::string base = pp.substr(0, pp.find("_pp"));
        if(base == "rcp")
          ref = {1.0f / a};
        else if(base == "rsq")
          ref = {1.0f / sqrtf(fabsf(a))};
        else if(base == "log")
          ref = {log2f(fabsf(a))};
        else if(base == "exp")
          ref = {exp2f(a)};
        else if(base == "pow")
          ref = {powf(fabsf(a), b), exp2f(b * log2f(fabsf(a)))};
        else if(base == "sincos")
          ref = {cosf(a), sinf(a)};
        else if(base == "mul")
          ref = {a * b};
        else if(base == "add")
          ref = {a + b};
        else if(base == "mad" || base == "mul_add")
          ref = {a * b + cc, fmaf(a, b, cc)};
        else if(base == "min")
          ref = {a < b ? a : b, fminf(a, b)};
        else if(base == "max")
          ref = {a >= b ? a : b, fmaxf(a, b)};
        else if(base == "cmp")
          ref = {a >= 0.0f ? 1.0f : 0.0f};
        else if(base == "frc")
          ref = {a - floorf(a)};
        else if(base == "dp3")
          ref = {a * b + b * cc + cc * a, fmaf(cc, a, fmaf(b, cc, a * b))};
        else if(base == "dp4")
          ref = {a * b + b * cc + cc * d + d * a, fmaf(d, a, fmaf(cc, d, fmaf(b, cc, a * b)))};
        else if(base == "dp2add")
          // a*c + b*d + c in different orders
          ref = {(a * cc + b * d) + cc, fmaf(b, d, fmaf(a, cc, cc)), fmaf(a, cc, fmaf(b, d, cc)),
                 fmaf(b, d, a * cc) + cc};
        else if(base == "nrm")
        {
          float l = a * a + b * b + cc * cc;
          float lf = fmaf(cc, cc, fmaf(b, b, a * a));
          ref = {a * (1.0f / sqrtf(l)), a / sqrtf(l), a * (1.0f / sqrtf(lf))};
        }
        else if(base == "lrp")
          ref = {
              a * (b - cc) + cc,       fmaf(a, b - cc, cc),           fmaf(a, b, fmaf(-a, cc, cc)),
              a * b + (1.0f - a) * cc, fmaf(a, b, -fmaf(a, cc, -cc)), fmaf(a, b, (1.0f - a) * cc)};
        while(ref.size() < 1)
          ref.push_back(NAN);
        const size_t NR = ref.size();
        if(st.size() < NR)
          st.resize(NR);
        std::vector<int64_t> u(NR, 0);
        for(size_t r = 0; r < NR; r++)
        {
          // sincos: x is cos, y is sin
          float gv = (base == "sincos" && r == 1) ? res[i].y : gpu;
          u[r] = Ulps(gv, ref[r]);
          auto odd = [](float v) { return !isfinite(v) || (v != 0.0f && fabsf(v) < FLT_MIN); };
          int used =
              op.kind == K_VEC
                  ? 4
                  : (op.kind == K_MAD ? 3 : ((op.kind == K_POW || op.kind == K_BINARY) ? 2 : 1));
          bool special = odd(ref[r]) || odd(gv) || odd(a) || (used > 1 && odd(b)) ||
                         (used > 2 && odd(cc)) || (used > 3 && odd(d));
          if(special)
          {
            if(u[r] != 0)
              st[r].specialDiff++;
            continue;
          }
          st[r].n++;
          if(u[r] == 0)
            st[r].exact++;
          if(u[r] <= 1)
            st[r].le1++;
          if(u[r] <= 2)
            st[r].le2++;
          if(u[r] <= 4)
            st[r].le4++;
          if(u[r] <= 16)
            st[r].le16++;
          if(u[r] > st[r].maxUlp)
          {
            st[r].maxUlp = u[r];
            st[r].worst = e;
          }
          st[r].maxAbs = fmaxf(st[r].maxAbs, fabsf(gv - ref[r]));
        }
        if(isPP)
        {
          float h = FromHalf(ToHalf(ref[0]));
          if(Bits(gpu) == Bits(h))
            ppHalfMatch++;
          if(Bits(gpu) == Bits(ref[0]))
            ppFloatMatch++;
        }
        fprintf(f, "%.9g %.9g %.9g %.9g |%s| %.9g %.9g %.9g (%08x)", x.x, x.y, x.z, x.w,
                eb ? " ECHO DIFFERS" : "", res[i].x, res[i].y, res[i].z, Bits(res[i].x));
        for(size_t r = 0; r < NR; r++)
          fprintf(f, " | %.9g (%08x) %lld", ref[r], Bits(ref[r]),
                  (long long)(u[r] == INT64_MAX ? -1 : u[r]));
        fprintf(f, "\n");
      }
      fclose(f);
      P("%s: draw %08x, %zu inputs, %d echoes differ from the input\n", op.name, (UINT)hr, n,
        echoBad);
      for(size_t r = 0; r < st.size(); r++)
      {
        if(st[r].n == 0 && st[r].specialDiff == 0)
          continue;
        const Stat &s = st[r];
        P("  ref%d: %d finite normal: exact %d, <=1 ulp %d, <=2 %d, <=4 %d, <=16 %d, max %lld ulps "
          "(input %.9g %.9g %.9g %.9g), max abs %.9g; %d special cases differ\n",
          int(r + 1), s.n, s.exact, s.le1, s.le2, s.le4, s.le16, (long long)s.maxUlp, s.worst.x,
          s.worst.y, s.worst.z, s.worst.w, s.maxAbs, s.specialDiff);
      }
      if(isPP)
        P("  _pp: %d of %zu equal the fp32 reference rounded to fp16, %d the fp32 reference\n",
          ppHalfMatch, n, ppFloatMatch);
    }
  }

  //////////////////////////////////////////////////////////////////////////////////////////////
  // H: reading inputs back, and which fragment

  void Fragments()
  {
    Open("h_fragments");
    P("NumSimultaneousRTs %u, PrimitiveMiscCaps %08x (MRTINDEPENDENTBITDEPTHS %d, "
      "MRTPOSTPIXELSHADERBLENDING %d)\n",
      (UINT)caps.NumSimultaneousRTs, (UINT)caps.PrimitiveMiscCaps,
      (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) ? 1 : 0,
      (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING) ? 1 : 0);
    for(D3DFORMAT f : {D3DFMT_A32B32G32R32F, D3DFMT_A16B16G16R16F})
    {
      HRESULT a = d3d->CheckDeviceFormat(
          D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, presentParams.BackBufferFormat,
          D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, f);
      P("format %d post pixel shader blending %08x\n", (int)f, (UINT)a);
    }

    // alpha test at the threshold, with 8-bit and float targets
    {
      std::vector<float> alphas = {127.0f / 255, 127.5f / 255, 128.0f / 255,
                                   128.5f / 255, 129.0f / 255, 0.5f};
      for(int k = 9; k <= 24; k++)
      {
        alphas.push_back(128.0f / 255 - ldexpf(1.0f, -k));
        alphas.push_back(128.0f / 255 + ldexpf(1.0f, -k));
        alphas.push_back(0.5f + ldexpf(1.0f, -k));
        alphas.push_back(0.5f - ldexpf(1.0f, -k));
      }
      alphas.push_back(FromBits(Bits(128.0f / 255) - 1));
      alphas.push_back(FromBits(Bits(128.0f / 255) + 1));
      std::sort(alphas.begin(), alphas.end());
      P("alpha test, ALPHAREF 128: alpha, then per target format and function 1 = passed\n");
      const D3DFORMAT fmts[] = {D3DFMT_A8R8G8B8, D3DFMT_A16B16G16R16F, D3DFMT_A32B32G32R32F};
      std::vector<Targets> ts;
      for(D3DFORMAT f : fmts)
        ts.push_back(MakeTargets(4, 4, 1, f));
      for(float a : alphas)
      {
        P("%.9g (0x%08x, *255 = %.9g):", a, Bits(a), a * 255.0f);
        for(size_t fi = 0; fi < ts.size(); fi++)
          for(DWORD func : {D3DCMP_GREATEREQUAL, D3DCMP_EQUAL, D3DCMP_GREATER})
          {
            ResetState();
            Bind(ts[fi]);
            dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            dev->SetRenderState(D3DRS_ALPHAREF, 128);
            dev->SetRenderState(D3DRS_ALPHAFUNC, func);
            V4 c0 = {1, 1, 1, a};
            dev->SetPixelShaderConstantF(0, &c0.x, 1);
            auto c = [&](float sx, float sy) { return At(sx, sy, 4.0f, 4.0f); };
            std::vector<Vert> v;
            PushQuad(v, c(0, 0), c(4, 0), c(0, 4), c(4, 4));
            HRESULT hr = DrawSrc("ps_2_0\nmov oC0, c0\n", v);
            V4 o = Read(ts[fi].rt[0])[5];
            P(" %s%s:%d", fi == 0 ? "8bit" : (fi == 1 ? "f16" : "f32"),
              func == D3DCMP_GREATEREQUAL ? "GE" : (func == D3DCMP_EQUAL ? "EQ" : "GT"),
              FAILED(hr) ? -1 : (o.x == 1.0f ? 1 : 0));
          }
        P("\n");
      }
    }

    struct Frag
    {
      int id;
      float z, alpha;
      bool kill;
    };
    struct Scenario
    {
      const char *name;
      BOOL zenable;
      DWORD zfunc;
      BOOL zwrite;
      BOOL alphatest;
      std::vector<Frag> frags;
    };
    const Scenario scenarios[] = {
        {"LESS, nearest second",
         TRUE,
         D3DCMP_LESS,
         TRUE,
         FALSE,
         {{1, 0.5f, 1, false}, {2, 0.3f, 1, false}, {3, 0.7f, 1, false}}},
        {"LESS, nearest fails alpha test",
         TRUE,
         D3DCMP_LESS,
         TRUE,
         TRUE,
         {{1, 0.5f, 1, false}, {2, 0.3f, 0.2f, false}, {3, 0.7f, 1, false}}},
        {"LESS, nearest killed",
         TRUE,
         D3DCMP_LESS,
         TRUE,
         FALSE,
         {{1, 0.5f, 1, false}, {2, 0.3f, 1, true}, {3, 0.7f, 1, false}}},
        {"LESS, nearest first and fails alpha test",
         TRUE,
         D3DCMP_LESS,
         TRUE,
         TRUE,
         {{2, 0.3f, 0.2f, false}, {1, 0.5f, 1, false}, {3, 0.7f, 1, false}}},
        {"no depth test",
         FALSE,
         D3DCMP_LESS,
         TRUE,
         FALSE,
         {{1, 0.5f, 1, false}, {2, 0.3f, 1, false}, {3, 0.7f, 1, false}}},
        {"no depth test, last killed",
         FALSE,
         D3DCMP_LESS,
         TRUE,
         FALSE,
         {{1, 0.5f, 1, false}, {2, 0.3f, 1, false}, {3, 0.7f, 1, true}}},
        {"no depth test, last fails alpha test",
         FALSE,
         D3DCMP_LESS,
         TRUE,
         TRUE,
         {{1, 0.5f, 1, false}, {2, 0.3f, 1, false}, {3, 0.7f, 0.2f, false}}},
        {"LESSEQUAL, same depth, last fails alpha test",
         TRUE,
         D3DCMP_LESSEQUAL,
         TRUE,
         TRUE,
         {{1, 0.5f, 1, false}, {2, 0.5f, 1, false}, {3, 0.5f, 0.2f, false}}},
        {"LESS, no depth write, last killed",
         TRUE,
         D3DCMP_LESS,
         FALSE,
         FALSE,
         {{1, 0.5f, 1, false}, {2, 0.3f, 1, false}, {3, 0.7f, 1, true}}},
    };

    const UINT W = 8, H = 8;
    // t0 = (id / 16, z, gradient, alpha), t1 = kill (texkill if negative), t2 = an interpolated
    // value to compare between methods
    auto build = [&](const Scenario &s) {
      std::vector<Vert> v;
      for(const Frag &f : s.frags)
      {
        auto c = [&](float sx, float sy) {
          Vert r = At(sx, sy, float(W), float(H), f.z);
          r.t[0] = {f.id / 16.0f, f.z, 0.0f, f.alpha};
          float k = f.kill ? -1.0f : 1.0f;
          r.t[1] = {k, k, k, k};
          r.t[2] = {sx / W + f.id, sy / H, sx * sy / 64.0f, 1.0f};
          return r;
        };
        PushQuad(v, c(0, 0), c(float(W), 0), c(0, float(H)), c(float(W), float(H)));
      }
      return v;
    };

    struct Variant
    {
      const char *name;
      const char *orig, *origMRT, *repl;
    };
    const Variant variants[] = {
        {"ps_2_x (dsx appended)", "ps_2_x\ndcl t0\ndcl t1\ndcl t2\ntexkill t1\nmov oC0, t0\n",
         "ps_2_x\ndcl t0\ndcl t1\ndcl t2\ntexkill t1\nmov oC0, t0\nmov oC1, t0\ndsx r0, "
         "t2\nmov oC2, t2\nmov oC3, r0\n",
         "ps_2_x\ndcl t0\ndcl t2\nmov oC0, t0\nmov oC1, t0\nmov oC2, t2\n"},
        {"ps_2_0", "ps_2_0\ndcl t0\ndcl t1\ndcl t2\ntexkill t1\nmov oC0, t0\n",
         "ps_2_0\ndcl t0\ndcl t1\ndcl t2\ntexkill t1\nmov oC0, t0\nmov oC1, t0\nmov oC2, t2\n",
         "ps_2_0\ndcl t0\ndcl t2\nmov oC0, t0\nmov oC1, t0\nmov oC2, t2\n"},
        {"ps_3_0",
         "ps_3_0\ndcl_texcoord0 v0\ndcl_texcoord1 v1\ndcl_texcoord2 v2\ntexkill v1\nmov oC0, v0\n",
         "ps_3_0\ndcl_texcoord0 v0\ndcl_texcoord1 v1\ndcl_texcoord2 v2\ntexkill v1\nmov oC0, "
         "v0\nmov oC1, v0\nmov oC2, v2\n",
         "ps_3_0\ndcl_texcoord0 v0\ndcl_texcoord2 v2\nmov oC0, v0\nmov oC1, v0\nmov oC2, v2\n"},
    };

    auto setState = [&](const Scenario &s, bool alpha) {
      dev->SetRenderState(D3DRS_ZENABLE, s.zenable);
      dev->SetRenderState(D3DRS_ZFUNC, s.zfunc);
      dev->SetRenderState(D3DRS_ZWRITEENABLE, s.zwrite);
      dev->SetRenderState(D3DRS_ALPHATESTENABLE, alpha && s.alphatest);
      dev->SetRenderState(D3DRS_ALPHAREF, 128);
      dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    };

    const UINT px = 3, py = 5, pi = py * W + px;
    auto idOf = [&](const V4 &v) {
      if(Unwritten(v))
        return -1;
      return int(floorf(v.x * 16.0f + 0.5f));
    };

    Targets truthT = MakeTargets(W, H, 1, D3DFMT_A8R8G8B8, true);
    Targets replT = MakeTargets(W, H, 3, D3DFMT_A32B32G32R32F, true);
    Targets mrt8 = MakeTargets(W, H, 4, D3DFMT_A8R8G8B8, true);
    Targets mrtF = MakeTargets(W, H, 4, D3DFMT_A32B32G32R32F, true);

    P("which fragment: id of the fragment each method reads at pixel %u,%u\n", px, py);
    P("  truth = the original drawn alone to A8R8G8B8 with its depth/alpha state\n");
    P("  replacement = a shader writing only the inputs, to float targets, with the draw's depth "
      "state and alpha test off\n");
    P("  original+MRT 8bit = the original with its inputs written to oC1/oC2, oC0 to A8R8G8B8, "
      "alpha test on\n");
    P("  original+MRT float = the same with oC0 to A32B32G32R32F\n");
    P("  depth EQUAL = the replacement after the original, ZFUNC EQUAL, no depth write\n");
    for(const Variant &var : variants)
      for(const Scenario &s : scenarios)
      {
        std::vector<Vert> v = build(s);
        bool sm3 = !strncmp(var.name, "ps_3_0", 6);

        ResetState();
        Bind(truthT);
        setState(s, true);
        HRESULT h0 = Draw(PS(var.orig), v, D3DPT_TRIANGLELIST, sm3);
        int truth = idOf(Read(truthT.rt[0])[pi]);

        ResetState();
        Bind(replT);
        setState(s, false);
        HRESULT h1 = Draw(PS(var.repl), v, D3DPT_TRIANGLELIST, sm3);
        std::vector<V4> r1 = Read(replT.rt[1]), r1g = Read(replT.rt[2]);
        int repl = idOf(r1[pi]);

        ResetState();
        HRESULT hb8 = Bind(mrt8);
        setState(s, true);
        HRESULT h2 = Draw(PS(var.origMRT), v, D3DPT_TRIANGLELIST, sm3);
        std::vector<V4> r2 = Read(mrt8.rt[1]), r2g = Read(mrt8.rt[2]), r2d = Read(mrt8.rt[3]);
        int m8 = idOf(r2[pi]);
        int m8c = idOf(Read(mrt8.rt[0])[pi]);

        ResetState();
        Bind(mrtF);
        setState(s, true);
        HRESULT h3 = Draw(PS(var.origMRT), v, D3DPT_TRIANGLELIST, sm3);
        std::vector<V4> r3 = Read(mrtF.rt[1]), r3g = Read(mrtF.rt[2]);
        int mf = idOf(r3[pi]);

        // depth EQUAL after the original
        int eq = -2;
        HRESULT h4 = S_OK;
        if(s.zenable)
        {
          ResetState();
          Bind(replT);
          setState(s, true);
          Draw(PS(var.orig), v, D3DPT_TRIANGLELIST, sm3);
          dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(7, 7, 7, 7), 1.0f, 0);
          setState(s, false);
          dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
          dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
          h4 = Draw(PS(var.repl), v, D3DPT_TRIANGLELIST, sm3);
          eq = idOf(Read(replT.rt[1])[pi]);
        }

        P("%s, %s: truth %d | replacement %d | original+MRT 8bit %d (its oC0 %d) | original+MRT "
          "float %d | depth EQUAL %d   (draws %08x %08x bind %08x %08x %08x %08x)\n",
          var.name, s.name, truth, repl, m8, m8c, mf, eq, (UINT)h0, (UINT)h1, (UINT)hb8, (UINT)h2,
          (UINT)h3, (UINT)h4);
        if(strstr(var.name, "dsx"))
          P("    appended dsx(t2) at the pixel: %.9g %.9g %.9g\n", r2d[pi].x, r2d[pi].y, r2d[pi].z);
        const V4 &a = r1g[pi], &b = r2g[pi], &c = r3g[pi];
        P("    interpolated t2 at the pixel: replacement %.9g %.9g %.9g | original+MRT 8bit %.9g "
          "%.9g %.9g | float %.9g %.9g %.9g\n",
          a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z);
      }
  }

  int main()
  {
    if(!Init())
      return 3;

    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    assemble = d3dcompiler ? (pD3DAssemble)GetProcAddress(d3dcompiler, "D3DAssemble") : NULL;
    if(!assemble)
      return 4;

    char *o = getenv("PROBE_OUT");
    outDir = o ? o : ".";

    dev->GetDeviceCaps(&caps);
    D3DADAPTER_IDENTIFIER9 ident = {};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident);
    TEST_LOG("%s driver %u.%u.%u.%u", ident.Description, HIWORD(ident.DriverVersion.HighPart),
             LOWORD(ident.DriverVersion.HighPart), HIWORD(ident.DriverVersion.LowPart),
             LOWORD(ident.DriverVersion.LowPart));

    const D3DVERTEXELEMENT9 el[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        {0, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
        {0, 64, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3},
        {0, 80, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 96, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1},
        D3DDECL_END()};
    dev->CreateVertexDeclaration(el, &decl);

    std::vector<DWORD> code = Assemble(
        "vs_2_0\ndcl_position v0\ndcl_texcoord0 v1\ndcl_texcoord1 v2\ndcl_texcoord2 "
        "v3\ndcl_texcoord3 v4\ndcl_color0 v5\ndcl_color1 v6\nmov oPos, v0\nmov oT0, v1\nmov oT1, "
        "v2\nmov oT2, v3\nmov oT3, v4\nmov oD0, v5\nmov oD1, v6\n");
    if(code.empty() || FAILED(dev->CreateVertexShader(code.data(), &vs2)))
      return 5;
    code = Assemble(
        "vs_3_0\ndcl_position v0\ndcl_texcoord0 v1\ndcl_texcoord1 v2\ndcl_texcoord2 "
        "v3\ndcl_texcoord3 v4\ndcl_color0 v5\ndcl_color1 v6\ndcl_position o0\ndcl_texcoord0 "
        "o1\ndcl_texcoord1 o2\ndcl_texcoord2 o3\ndcl_texcoord3 o4\ndcl_color0 o5\ndcl_color1 "
        "o6\nmov o0, v0\nmov o1, v1\nmov o2, v2\nmov o3, v3\nmov o4, v4\nmov o5, v5\nmov o6, v6\n");
    if(code.empty() || FAILED(dev->CreateVertexShader(code.data(), &vs3)))
      return 6;

    {
      Targets st = MakeTargets(1, 1, 1);
      Bind(st);
      sentinel = Read(st.rt[0])[0].x;
      TEST_LOG("cleared float target holds %.9g (0x%08x)", sentinel, Bits(sentinel));
    }

    const char *only = getenv("PROBE_ONLY");
    auto want = [&](const char *s) { return !only || strchr(only, s[0]); };

    if(want("a"))
      Centre();
    if(want("b"))
      Face();
    if(want("c"))
      PS1x();
    if(want("d"))
      Colours();
    if(want("e"))
      Derivatives();
    if(want("f"))
      Texldd();
    if(want("g"))
      Arithmetic();
    if(want("h"))
      Fragments();

    if(out)
      fclose(out);
    out = NULL;
    TEST_LOG("done");
    return 0;
  }
};

REGISTER_TEST();
