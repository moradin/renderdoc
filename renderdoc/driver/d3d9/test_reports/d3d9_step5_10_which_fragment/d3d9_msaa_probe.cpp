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

// a temporary probe for pixel shader debugging (step 5.10): what the GPU does natively with
// multisampled targets. Which multisample types the formats have, whether a resolve gives a
// pixel's value exactly, whether drawing sample 0 alone (MULTISAMPLEMASK 1) into a cleared target
// resolves to the value over the sample count, where the samples are, where centroid inputs are
// evaluated, whether multisampled depth stencils can be copied, and what occlusion queries count.
// Writes msaa.txt to PROBE_OUT.

#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "d3d9_test.h"

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

RD_TEST(D3D9_MSAA_Probe, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Probe of multisampled pixel shading";

  struct Vert
  {
    float x, y, z, rhw;
    float t[4];
  };

  static const UINT W = 16, H = 16;
  FILE *out = NULL;
  pD3DAssemble assemble = NULL;

  void P(const char *fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fflush(out);
  }

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

  // the value at (x, y) of a resolve of rt, all four components in v4
  float ReadResolved(IDirect3DSurface9 * rt, IDirect3DSurface9 * resolve, IDirect3DSurface9 * sys,
                     UINT x, UINT y, float *v4 = NULL)
  {
    dev->EndScene();
    HRESULT hr = dev->StretchRect(rt, NULL, resolve, NULL, D3DTEXF_NONE);
    if(SUCCEEDED(hr))
      hr = dev->GetRenderTargetData(resolve, sys);
    float v[4] = {-12345.0f, -12345.0f, -12345.0f, -12345.0f};
    D3DLOCKED_RECT lr = {};
    if(SUCCEEDED(hr) && SUCCEEDED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
      memcpy(v, (const byte *)lr.pBits + y * lr.Pitch + x * 16, sizeof(v));
      sys->UnlockRect();
    }
    dev->BeginScene();
    if(v4)
      memcpy(v4, v, sizeof(v));
    return v[0];
  }

  // a rectangle from (x0, y0) to (x1, y1) in D3D9's pixel coordinates (centres at integers), every
  // vertex's t0 the same, or t0 = (x, y, 0, 1) with value NULL
  void Rect(float x0, float y0, float x1, float y1, const float *value, float z = 0.5f)
  {
    Vert v[4] = {
        {x0, y0, z, 1.0f},
        {x1, y0, z, 1.0f},
        {x0, y1, z, 1.0f},
        {x1, y1, z, 1.0f},
    };
    for(Vert &vt : v)
    {
      if(value)
        memcpy(vt.t, value, sizeof(vt.t));
      else
      {
        vt.t[0] = vt.x;
        vt.t[1] = vt.y;
        vt.t[2] = 0.0f;
        vt.t[3] = 1.0f;
      }
    }
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vert));
  }

  void Tri(const float pts[3][2])
  {
    Vert v[3] = {};
    for(int i = 0; i < 3; i++)
    {
      v[i] = {pts[i][0], pts[i][1], 0.5f, 1.0f, {pts[i][0], pts[i][1], 0.0f, 1.0f}};
    }
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert));
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
    out = fopen((std::string(dir ? dir : ".") + "/msaa.txt").c_str(), "w");
    if(!out)
      return 5;

    D3DADAPTER_IDENTIFIER9 ident = {};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident);
    P("adapter %s, driver %u.%u.%u.%u\n", ident.Description, HIWORD(ident.DriverVersion.HighPart),
      LOWORD(ident.DriverVersion.HighPart), HIWORD(ident.DriverVersion.LowPart),
      LOWORD(ident.DriverVersion.LowPart));

    // 1: the multisample types each format has, and their quality levels
    const D3DFORMAT fmts[] = {D3DFMT_A32B32G32R32F, D3DFMT_A16B16G16R16F, D3DFMT_A8R8G8B8,
                              D3DFMT_D24S8};
    const char *fmtNames[] = {"A32B32G32R32F", "A16B16G16R16F", "A8R8G8B8", "D24S8"};
    P("multisample types (quality levels), windowed:\n");
    for(int f = 0; f < 4; f++)
    {
      P("  %s:", fmtNames[f]);
      for(int t = 1; t <= 16; t++)
      {
        DWORD q = 0;
        if(SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, fmts[f],
                                                     TRUE, D3DMULTISAMPLE_TYPE(t), &q)))
          P(" %s(%u)", t == 1 ? "NONMASKABLE" : std::to_string(t).c_str(), q);
      }
      P("\n");
    }

    IDirect3DPixelShader9Ptr psT0 = PS("ps_2_0\ndcl t0\nmov oC0, t0\n");
    IDirect3DPixelShader9Ptr psCentroid = PS("ps_2_0\ndcl_centroid t0\nmov oC0, t0\n");
    if(!psT0 || !psCentroid)
      return 6;

    IDirect3DSurface9Ptr backBuffer, backDepth;
    dev->GetRenderTarget(0, &backBuffer);
    dev->GetDepthStencilSurface(&backDepth);

    for(int samples : {2, 4, 8})
    {
      const D3DMULTISAMPLE_TYPE type = D3DMULTISAMPLE_TYPE(samples);
      IDirect3DSurface9Ptr rt, resolve, sys, dsA, dsB;
      HRESULT hr = dev->CreateRenderTarget(W, H, D3DFMT_A32B32G32R32F, type, 0, FALSE, &rt, NULL);
      if(SUCCEEDED(hr))
        hr = dev->CreateRenderTarget(W, H, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE,
                                     &resolve, NULL);
      if(SUCCEEDED(hr))
        hr = dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys,
                                              NULL);
      if(SUCCEEDED(hr))
        hr = dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, type, 0, FALSE, &dsA, NULL);
      if(SUCCEEDED(hr))
        hr = dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, type, 0, FALSE, &dsB, NULL);
      P("\n%ux MSAA, A32B32G32R32F and D24S8 quality 0: create %08x\n", samples, (UINT)hr);
      if(FAILED(hr))
        continue;

      dev->BeginScene();
      dev->SetRenderTarget(0, rt);
      dev->SetDepthStencilSurface(NULL);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0));
      dev->SetVertexShader(NULL);
      dev->SetPixelShader(psT0);
      const DWORD all = 0xFFFFFFFF;

      // 2: a resolve of fully covered pixels, and of sample 0 alone
      const float values[] = {1.0f / 3.0f, 0.1f,  3.14159274f, -2.5e-20f, 1.0e-37f,
                              1.0e-40f,    -0.0f, 65504.5f,    1.0e30f,   0.7f};
      int exactAll = 0, exactOne = 0;
      for(float v : values)
      {
        const float val[4] = {v, v, v, v};
        dev->SetRenderState(D3DRS_MULTISAMPLEMASK, all);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        Rect(-0.5f, -0.5f, W - 0.5f, H - 0.5f, val);
        const float a = ReadResolved(rt, resolve, sys, 5, 5);

        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 1);
        Rect(-0.5f, -0.5f, W - 0.5f, H - 0.5f, val);
        const float b = ReadResolved(rt, resolve, sys, 5, 5);
        const float back = b * float(samples);
        exactAll += Bits(a) == Bits(v);
        exactOne += Bits(back) == Bits(v);
        P("  %-14.9g (%08x): all samples resolve to %.9g (%08x), sample 0 alone to %.9g (%08x), "
          "times %d %.9g (%08x)\n",
          v, Bits(v), a, Bits(a), b, Bits(b), samples, back, Bits(back));
      }
      P("  all samples exact: %d of %d; sample 0 alone times %d exact: %d of %d\n", exactAll,
        (int)(sizeof(values) / sizeof(values[0])), samples, exactOne,
        (int)(sizeof(values) / sizeof(values[0])));

      // 3: where each sample is in pixel (5, 5), whose centre is (5, 5): the left or top edge of a
      // rectangle swept across it in 1/64ths, the first edge that leaves the sample out
      const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      for(int s = 0; s < samples; s++)
      {
        float pos[2] = {-1.0f, -1.0f};
        for(int axis = 0; axis < 2; axis++)
        {
          dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 1U << s);
          for(int step = 0; step <= 64; step++)
          {
            const float e = 4.5f + float(step) / 64.0f;
            dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
            if(axis == 0)
              Rect(e, -0.5f, W - 0.5f, H - 0.5f, one);
            else
              Rect(-0.5f, e, W - 0.5f, H - 0.5f, one);
            if(ReadResolved(rt, resolve, sys, 5, 5) == 0.0f)
            {
              pos[axis] = e - 5.0f;
              break;
            }
          }
        }
        P("  sample %d is left out from an edge at %+.4f, %+.4f of the centre\n", s, pos[0], pos[1]);
      }

      // 4: where the shader is evaluated in a pixel partly covered: t0 is the position. A
      // triangle covering the pixel's bottom right corner; each covered sample read alone.
      const float corner[3][2] = {{5.3f, 4.4f}, {5.6f, 5.6f}, {4.4f, 5.6f}};
      for(int centroid = 0; centroid < 2; centroid++)
      {
        dev->SetPixelShader(centroid ? psCentroid : psT0);
        P("  %s t0 in a pixel whose bottom right a triangle covers:",
          centroid ? "centroid" : "plain");
        for(int s = 0; s < samples; s++)
        {
          dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 1U << s);
          dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
          Tri(corner);
          float t[4];
          ReadResolved(rt, resolve, sys, 5, 5, t);
          if(t[3] != 0.0f)
            P(" sample %d covered, t0 (%.6f, %.6f)", s, t[0] * float(samples), t[1] * float(samples));
        }
        P("\n");
      }
      dev->SetPixelShader(psT0);

      // 6: where a fully covered pixel is shaded, with every sample written: t0 is (x, y, x, y),
      // and the shader writes (4 dx^2, 4 dy^2, 2 dx + 0.5, 2 dy + 0.5) of its offset from the
      // centre (5, 5). Shaded once at the centre the resolve is (0, 0, 0.5, 0.5); shaded at each
      // sample, the first two are 4 times the mean squared offset of the samples.
      IDirect3DPixelShader9Ptr psSq =
          PS("ps_2_0\ndef c0, 5, 5, 5, 5\ndef c1, 4, 4, 0, 0\ndef c2, 0, 0, 2, 2\n"
             "def c3, 0, 0, 0.5, 0.5\ndcl t0\nadd r0, t0, -c0\nmul r1, r0, r0\nmul r1, r1, c1\n"
             "mad r1, r0, c2, r1\nadd r1, r1, c3\nmov oC0, r1\n");
      for(D3DFORMAT fmt : {D3DFMT_A32B32G32R32F, D3DFMT_A8R8G8B8})
      {
        const bool f32 = fmt == D3DFMT_A32B32G32R32F;
        IDirect3DSurface9Ptr srt, sres, ssys;
        HRESULT shr = dev->CreateRenderTarget(W, H, fmt, type, 0, FALSE, &srt, NULL);
        if(SUCCEEDED(shr))
          shr = dev->CreateRenderTarget(W, H, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &sres, NULL);
        if(SUCCEEDED(shr))
          shr = dev->CreateOffscreenPlainSurface(W, H, fmt, D3DPOOL_SYSTEMMEM, &ssys, NULL);
        if(FAILED(shr) || !psSq)
        {
          P("  shading rate on %s: create %08x\n", f32 ? "A32B32G32R32F" : "A8R8G8B8", (UINT)shr);
          continue;
        }
        auto readPixel = [&](float v[4]) {
          dev->EndScene();
          dev->StretchRect(srt, NULL, sres, NULL, D3DTEXF_NONE);
          dev->GetRenderTargetData(sres, ssys);
          D3DLOCKED_RECT lr = {};
          ssys->LockRect(&lr, NULL, D3DLOCK_READONLY);
          const byte *p = (const byte *)lr.pBits + 5 * lr.Pitch;
          if(f32)
          {
            memcpy(v, p + 5 * 16, 16);
          }
          else
          {
            const byte *t = p + 5 * 4;
            v[0] = t[2] / 255.0f;
            v[1] = t[1] / 255.0f;
            v[2] = t[0] / 255.0f;
            v[3] = t[3] / 255.0f;
          }
          ssys->UnlockRect();
          dev->BeginScene();
        };
        Vert quad[4] = {
            {-0.5f, -0.5f, 0.5f, 1.0f, {-0.5f, -0.5f, -0.5f, -0.5f}},
            {W - 0.5f, -0.5f, 0.5f, 1.0f, {W - 0.5f, -0.5f, W - 0.5f, -0.5f}},
            {-0.5f, H - 0.5f, 0.5f, 1.0f, {-0.5f, H - 0.5f, -0.5f, H - 0.5f}},
            {W - 0.5f, H - 0.5f, 0.5f, 1.0f, {W - 0.5f, H - 0.5f, W - 0.5f, H - 0.5f}},
        };
        dev->SetRenderTarget(0, srt);
        dev->SetPixelShader(psSq);
        float v[4];
        dev->SetRenderState(D3DRS_MULTISAMPLEMASK, all);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vert));
        readPixel(v);
        P("  shading rate on %s, every sample written: (%.6f, %.6f, %.6f, %.6f)\n",
          f32 ? "A32B32G32R32F" : "A8R8G8B8", v[0], v[1], v[2], v[3]);
        for(int s = 0; s < samples; s++)
        {
          dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 1U << s);
          dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
          dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vert));
          readPixel(v);
          P("    sample %d alone, times %d: (%.6f, %.6f, %.6f, %.6f), so the offset (%+.4f, "
            "%+.4f)\n",
            s, samples, v[0] * samples, v[1] * samples, v[2] * samples, v[3] * samples,
            (v[2] * samples - 0.5f) / 2.0f, (v[3] * samples - 0.5f) / 2.0f);
        }
        // 7: the same with MULTISAMPLEANTIALIAS FALSE: where it's shaded, and whether every sample
        // gets the value (the resolve is then exact)
        dev->SetRenderState(D3DRS_MULTISAMPLEMASK, all);
        dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vert));
        readPixel(v);
        P("  MULTISAMPLEANTIALIAS FALSE on %s: (%.9g, %.9g, %.9g, %.9g)\n",
          f32 ? "A32B32G32R32F" : "A8R8G8B8", v[0], v[1], v[2], v[3]);
        dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);

        dev->SetRenderState(D3DRS_MULTISAMPLEMASK, all);
        dev->SetRenderTarget(0, rt);
      }
      dev->SetPixelShader(psT0);

      // 7: MULTISAMPLEANTIALIAS FALSE: values resolved exactly, coverage of an edge swept across
      // the pixel, a triangle covering part of it, and the depth test against a multisampled depth
      dev->SetRenderState(D3DRS_MULTISAMPLEMASK, all);
      dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
      {
        int exact = 0;
        for(float v : values)
        {
          const float val[4] = {v, v, v, v};
          dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
          Rect(-0.5f, -0.5f, W - 0.5f, H - 0.5f, val);
          exact += Bits(ReadResolved(rt, resolve, sys, 5, 5)) == Bits(v);
        }
        P("  MULTISAMPLEANTIALIAS FALSE: %d of %d values resolve exactly\n", exact,
          (int)(sizeof(values) / sizeof(values[0])));
        for(int axis = 0; axis < 2; axis++)
        {
          P("  MULTISAMPLEANTIALIAS FALSE, %s edge swept across the pixel, resolved:",
            axis ? "top" : "left");
          float last = -1.0f;
          for(int step = 0; step <= 64; step++)
          {
            const float e = 4.5f + float(step) / 64.0f;
            dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
            if(axis == 0)
              Rect(e, -0.5f, W - 0.5f, H - 0.5f, one);
            else
              Rect(-0.5f, e, W - 0.5f, H - 0.5f, one);
            const float r = ReadResolved(rt, resolve, sys, 5, 5);
            if(r != last)
              P(" from %+.4f %.4f", e - 5.0f, r);
            last = r;
          }
          P("\n");
        }
        float t[4];
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        Tri(corner);
        ReadResolved(rt, resolve, sys, 5, 5, t);
        P("  MULTISAMPLEANTIALIAS FALSE, the triangle over the bottom right: t0 (%.6f, %.6f), w "
          "%.6f\n",
          t[0], t[1], t[3]);
        // the triangle's centre left out: (4.6, 4.6)-(5.4, 4.6)-(5.4, 4.9), above the centre
        const float above[3][2] = {{4.6f, 4.6f}, {5.4f, 4.6f}, {5.4f, 4.9f}};
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        Tri(above);
        ReadResolved(rt, resolve, sys, 5, 5, t);
        P("  MULTISAMPLEANTIALIAS FALSE, a triangle over samples but not the centre: w %.6f\n", t[3]);
        dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        Tri(above);
        ReadResolved(rt, resolve, sys, 5, 5, t);
        P("  the same with MULTISAMPLEANTIALIAS TRUE: w %.6f (the covered fraction), t0 (%.6f, "
          "%.6f) "
          "times that\n",
          t[3], t[0] / (t[3] ? t[3] : 1.0f), t[1] / (t[3] ? t[3] : 1.0f));
      }
      dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);

      // 5: copying a multisampled depth stencil, and what an occlusion query counts
      IDirect3DQuery9Ptr query;
      dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query);
      dev->SetDepthStencilSurface(dsA);
      dev->SetRenderState(D3DRS_MULTISAMPLEMASK, all);
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
      dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
      Rect(-0.5f, -0.5f, 7.5f, H - 0.5f, one, 0.3f);
      dev->SetDepthStencilSurface(dsB);
      dev->Clear(0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
      dev->SetDepthStencilSurface(NULL);
      dev->EndScene();
      hr = dev->StretchRect(dsA, NULL, dsB, NULL, D3DTEXF_NONE);
      dev->BeginScene();
      dev->SetDepthStencilSurface(dsB);
      auto count = [&](DWORD mask) {
        dev->SetRenderState(D3DRS_MULTISAMPLEMASK, mask);
        query->Issue(D3DISSUE_BEGIN);
        Rect(-0.5f, -0.5f, W - 0.5f, H - 0.5f, one, 0.5f);
        query->Issue(D3DISSUE_END);
        DWORD n = 0;
        while(query->GetData(&n, sizeof(n), D3DGETDATA_FLUSH) == S_FALSE)
        {
        }
        return n;
      };
      dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      const DWORD nAll = count(all), nOne = count(1);
      dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
      const DWORD nOff = count(all);
      dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
      P("  with MULTISAMPLEANTIALIAS FALSE the draw passes %u samples\n", nOff);
      P("  depth stencil StretchRect %08x; a %ux%u draw at z 0.5 against the copy (the left half "
        "at 0.3) passes %u samples, %u with MULTISAMPLEMASK 1\n",
        (UINT)hr, W, H, nAll, nOne);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      dev->SetRenderState(D3DRS_MULTISAMPLEMASK, all);
      dev->SetRenderTarget(0, backBuffer);
      dev->SetDepthStencilSurface(backDepth);
      dev->EndScene();
    }

    fclose(out);
    out = NULL;
    TEST_LOG("done");
    return 0;
  }
};

REGISTER_TEST();
