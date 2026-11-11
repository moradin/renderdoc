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

// a temporary probe: how each depth format's levels map to fp32 oDepth values, natively. One pixel
// per level (4096x4096 for 24 bits), so every level is tested in one draw. For each formula that
// turns an integer l into a depth:
// * the shader's depth against the same formula on the CPU;
// * chain: write F(l) at pixel l, then test F(l + 1) with GREATER. A failure means F(l) and
//   F(l + 1) land on the same level, so some other level is never written;
// * draws: random fp32 depths, read back by bisection with F, then F(l) tested with EQUAL;
// * clears: 65536 single pixel Clears with various depths, read back and tested the same way.

#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "d3d9_test.h"

RD_TEST(D3D9_Depth_Probe, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Probe of depth levels";

  std::string vsSrc = R"(
float4 halfPixel : register(c0);
float4 main(float4 pos : POSITION) : POSITION { return float4(pos.xy + halfPixel.xy, pos.zw); }
)";

  // c0.zw = 1 / size; c1 = (added, threshold, step, scale); c2.xy: colour = depth * x + y
  std::string formulaSrc = R"(
sampler2D vals : register(s0);
float4 p : register(c0);
float4 f : register(c1);
float4 o : register(c2);
float4 main(float2 vpos : VPOS, out float depth : DEPTH) : COLOR0
{
  float x = tex2Dlod(vals, float4((vpos + 0.5f) * p.zw, 0, 0)).r + f.x;
  depth = (x + (x >= f.y ? f.z : 0.0f)) * f.w;
  return depth * o.x + o.y;
}
)";

  // c1.x = the bit
  std::string combineSrc = R"(
sampler2D lo : register(s0);
sampler2D passed : register(s1);
float4 p : register(c0);
float4 f : register(c1);
float4 main(float2 vpos : VPOS) : COLOR0
{
  float2 uv = (vpos + 0.5f) * p.zw;
  return tex2Dlod(lo, float4(uv, 0, 0)).r + (tex2Dlod(passed, float4(uv, 0, 0)).r > 0.5f ? f.x : 0.0f);
}
)";

  IDirect3DVertexShader9Ptr vs;
  IDirect3DPixelShader9Ptr formulaPS, combinePS;
  IDirect3DVertexDeclaration9Ptr decl;

  UINT W = 0, H = 0;
  IDirect3DTexture9Ptr vals, valsSys, lo[2], passed;
  IDirect3DSurface9Ptr loSurf[2], passedSurf, readSys;

  struct Formula
  {
    const char *name;
    float threshold, step, scale;
  };

  void Upload(const std::vector<float> &data)
  {
    D3DLOCKED_RECT lr = {};
    valsSys->LockRect(0, &lr, NULL, 0);
    for(UINT y = 0; y < H; y++)
      memcpy((byte *)lr.pBits + lr.Pitch * y, data.data() + y * W, W * 4);
    valsSys->UnlockRect(0);
    dev->UpdateTexture(valsSys, vals);
  }

  std::vector<float> Read(IDirect3DSurface9 * rt)
  {
    dev->GetRenderTargetData(rt, readSys);
    std::vector<float> ret(W * H);
    D3DLOCKED_RECT lr = {};
    readSys->LockRect(&lr, NULL, D3DLOCK_READONLY);
    for(UINT y = 0; y < H; y++)
      memcpy(ret.data() + y * W, (byte *)lr.pBits + lr.Pitch * y, W * 4);
    readSys->UnlockRect();
    return ret;
  }

  void Draw(IDirect3DPixelShader9 * ps, IDirect3DSurface9 * rt, IDirect3DSurface9 * ds,
            IDirect3DBaseTexture9 * t0, IDirect3DBaseTexture9 * t1, const float *c1,
            const float *c2, bool zenable, D3DCMPFUNC zfunc, bool zwrite, bool clearTarget)
  {
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(ds);
    D3DVIEWPORT9 vp = {0, 0, W, H, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    if(clearTarget)
      dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    dev->SetRenderState(D3DRS_ZENABLE, zenable ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, zfunc);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, zwrite);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    const float half[4] = {-1.0f / float(W), 1.0f / float(H), 0, 0};
    const float pc[4] = {0, 0, 1.0f / float(W), 1.0f / float(H)};
    const float zero[4] = {};
    dev->SetVertexShader(vs);
    dev->SetVertexShaderConstantF(0, half, 1);
    dev->SetPixelShader(ps);
    dev->SetPixelShaderConstantF(0, pc, 1);
    dev->SetPixelShaderConstantF(1, c1 ? c1 : zero, 1);
    dev->SetPixelShaderConstantF(2, c2 ? c2 : zero, 1);
    dev->SetTexture(0, t0);
    dev->SetTexture(1, t1);
    dev->SetVertexDeclaration(decl);
    const float quad[] = {
        -1, -1, 0.5f, 1, -1, 1, 0.5f, 1, 1, -1, 0.5f, 1, 1, 1, 0.5f, 1,
    };
    dev->BeginScene();
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 16);
    dev->EndScene();
    dev->SetTexture(0, NULL);
    dev->SetTexture(1, NULL);
  }

  // the formula on the CPU, in fp32 with round to nearest
  static float CpuF(const Formula &f, uint32_t l)
  {
    volatile float x = float(l);
    volatile float add = x >= f.threshold ? f.step : 0.0f;
    volatile float s = x + add;
    volatile float d = s * f.scale;
    return d;
  }

  // the integer the bisection finds, with F, for each pixel
  std::vector<float> Bisect(const Formula &f, IDirect3DSurface9 *ds, int bits)
  {
    dev->ColorFill(loSurf[0], NULL, 0);
    int cur = 0;
    const float test[4] = {0, 1, 0, 0};
    for(int b = bits - 1; b >= 0; b--)
    {
      const float bit = float(1u << b);
      const float c1[4] = {bit, f.threshold, f.step, f.scale};
      Draw(formulaPS, passedSurf, ds, lo[cur], NULL, c1, test, true, D3DCMP_LESSEQUAL, false, true);
      const float cc[4] = {bit, 0, 0, 0};
      Draw(combinePS, loSurf[1 - cur], NULL, lo[cur], passed, cc, NULL, false, D3DCMP_ALWAYS,
           false, false);
      cur = 1 - cur;
    }
    return Read(loSurf[cur]);
  }

  static uint32_t Bits(float f)
  {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
  }

  uint32_t rng = 12345;
  uint32_t Rand()
  {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  }

  // EQUAL with F(l) at each pixel, after uploading l; returns the failures among the first n
  uint32_t EqualFails(const Formula &f, IDirect3DSurface9 *ds, const std::vector<float> &l, uint32_t n,
                      std::vector<uint32_t> &where)
  {
    Upload(l);
    const float c1[4] = {0, f.threshold, f.step, f.scale};
    const float test[4] = {0, 1, 0, 0};
    Draw(formulaPS, passedSurf, ds, vals, NULL, c1, test, true, D3DCMP_EQUAL, false, true);
    std::vector<float> p = Read(passedSurf);
    uint32_t fails = 0;
    for(uint32_t i = 0; i < n; i++)
    {
      if(p[i] < 0.5f)
      {
        fails++;
        if(where.size() < 12)
          where.push_back(i);
      }
    }
    return fails;
  }

  // the level index models: round to nearest even, and round half up, of d * max
  static uint32_t ModelRNE(float d, double maxv)
  {
    double x = double(d) * maxv;
    return (uint32_t)nearbyint(x);
  }
  static uint32_t ModelHalfUp(float d, double maxv) { return (uint32_t)floor(double(d) * maxv + 0.5); }

  void Run(D3DFORMAT fmt, const char *name, int bits, bool tex)
  {
    const uint32_t count = 1u << bits;
    W = count >= 4096 * 4096 ? 4096 : 256;
    H = count / W;
    const uint32_t maxL = count - 1;
    const double maxv = double(maxL);

    IDirect3DSurface9Ptr ds;
    IDirect3DTexture9Ptr dsTex;
    HRESULT hr;
    if(tex)
    {
      hr = dev->CreateTexture(W, H, 1, D3DUSAGE_DEPTHSTENCIL, fmt, D3DPOOL_DEFAULT, &dsTex, NULL);
      if(SUCCEEDED(hr))
        hr = dsTex->GetSurfaceLevel(0, &ds);
    }
    else
    {
      hr = dev->CreateDepthStencilSurface(W, H, fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL);
    }
    if(FAILED(hr))
    {
      TEST_LOG("%s: can't create (%08x)", name, hr);
      return;
    }

    vals = valsSys = lo[0] = lo[1] = passed = NULL;
    loSurf[0] = loSurf[1] = passedSurf = readSys = NULL;
    dev->CreateTexture(W, H, 1, 0, D3DFMT_R32F, D3DPOOL_DEFAULT, &vals, NULL);
    dev->CreateTexture(W, H, 1, 0, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &valsSys, NULL);
    for(int i = 0; i < 2; i++)
    {
      dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &lo[i], NULL);
      lo[i]->GetSurfaceLevel(0, &loSurf[i]);
    }
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &passed, NULL);
    passed->GetSurfaceLevel(0, &passedSurf);
    dev->CreateOffscreenPlainSurface(W, H, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &readSys, NULL);

    for(int s = 0; s < 2; s++)
    {
      for(DWORD st : {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER})
        dev->SetSamplerState(s, (D3DSAMPLERSTATETYPE)st, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }

    TEST_LOG("==== %s: %u levels, %ux%u", name, count, W, H);

    const Formula formulas[] = {
        // the current one: l * fp32(1 / max)
        {"A l*(1/max)", 1e30f, 0.0f, 1.0f / float(maxL)},
        // (l + (l >= half ? 1 : 0)) * 2^-bits, exact in fp32
        {"B (l+[l>=half])*2^-n", float(1u << (bits - 1)), 1.0f, ldexpf(1.0f, -bits)},
        // l * 2^-bits, exact
        {"C l*2^-n", 1e30f, 0.0f, ldexpf(1.0f, -bits)},
    };
    const Formula identity = {"identity", 1e30f, 0.0f, 1.0f};

    std::vector<float> idx(count);
    for(uint32_t i = 0; i < count; i++)
      idx[i] = float(i);

    // the random depths, and the clear depths, the same for every formula
    std::vector<float> rnd(count);
    {
      rng = 12345;
      const float specials[] = {0.0f, 1.0f, 0.25f, 0.375f, 0.5f, 0.75f, 1.0f / 3.0f, 2.0f / 3.0f};
      for(uint32_t i = 0; i < count; i++)
      {
        if(i < 8)
          rnd[i] = specials[i];
        else if(i % 3 == 0)
          rnd[i] = float(Rand() >> 8) / 16777216.0f;
        else if(i % 3 == 1)
        {
          // any fp32 bit pattern in [0, 1]
          uint32_t u = Rand() % 0x3F800001u;
          memcpy(&rnd[i], &u, 4);
        }
        else
        {
          // around a level's centre or edge
          uint32_t k = Rand() % count;
          double c = (double(k) + ((Rand() & 1) ? 0.5 : 0.0)) / maxv;
          float f = float(c);
          uint32_t u = Bits(f) + (Rand() % 5) - 2;
          memcpy(&f, &u, 4);
          rnd[i] = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        }
      }
    }
    const uint32_t nClears = (count < 65536u ? count : 65536u);
    std::vector<float> clr(nClears);
    for(uint32_t i = 0; i < nClears; i++)
      clr[i] = rnd[i];

    for(const Formula &f : formulas)
    {
      TEST_LOG("-- formula %s (threshold %g, step %g, scale %.9g / %08x)", f.name, f.threshold,
               f.step, f.scale, Bits(f.scale));

      // the shader's depths
      {
        Upload(idx);
        const float c1[4] = {0, f.threshold, f.step, f.scale};
        const float verify[4] = {1, 0, 0, 0};
        Draw(formulaPS, passedSurf, NULL, vals, NULL, c1, verify, false, D3DCMP_ALWAYS, false, true);
        std::vector<float> d = Read(passedSurf);
        uint32_t bad = 0;
        for(uint32_t i = 0; i < count; i++)
        {
          if(Bits(d[i]) != Bits(CpuF(f, i)))
          {
            if(bad < 4)
              TEST_LOG("  shader depth differs at l=%u: gpu %08x cpu %08x", i, Bits(d[i]),
                       Bits(CpuF(f, i)));
            bad++;
          }
        }
        TEST_LOG("  shader vs CPU: %u of %u differ", bad, count);
      }

      // chain
      {
        Upload(idx);
        dev->SetRenderTarget(0, passedSurf);
        dev->SetDepthStencilSurface(ds);
        dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 0.0f, 0);
        const float c1[4] = {0, f.threshold, f.step, f.scale};
        const float c1next[4] = {1, f.threshold, f.step, f.scale};
        const float test[4] = {0, 1, 0, 0};
        Draw(formulaPS, passedSurf, ds, vals, NULL, c1, test, true, D3DCMP_ALWAYS, true, true);
        Draw(formulaPS, passedSurf, ds, vals, NULL, c1next, test, true, D3DCMP_GREATER, false, true);
        std::vector<float> p = Read(passedSurf);
        uint32_t dups = 0;
        uint32_t buckets[16] = {};
        std::string first;
        for(uint32_t i = 0; i + 1 < count; i++)
        {
          if(p[i] < 0.5f)
          {
            dups++;
            buckets[i * 16ULL / count]++;
            if(dups <= 8)
              first += " " + std::to_string(i);
          }
        }
        TEST_LOG("  chain: F(l+1) not above F(l) for %u l of %u; first:%s", dups, count - 1,
                 first.c_str());
        std::string b;
        for(int i = 0; i < 16; i++)
          b += " " + std::to_string(buckets[i]);
        TEST_LOG("  chain failures per 1/16 of the range:%s", b.c_str());
        Draw(formulaPS, passedSurf, ds, vals, NULL, c1, test, true, D3DCMP_EQUAL, false, true);
        p = Read(passedSurf);
        uint32_t eq = 0;
        for(uint32_t i = 0; i < count; i++)
          eq += p[i] < 0.5f ? 1 : 0;
        TEST_LOG("  F(l) EQUAL to what it wrote: %u failures", eq);
      }

      // random draws
      {
        Upload(rnd);
        dev->SetRenderTarget(0, passedSurf);
        dev->SetDepthStencilSurface(ds);
        dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 0.0f, 0);
        const float noout[4] = {0, 1, 0, 0};
        const float idc[4] = {0, identity.threshold, identity.step, identity.scale};
        Draw(formulaPS, passedSurf, ds, vals, NULL, idc, noout, true, D3DCMP_ALWAYS, true, true);
        std::vector<float> l = Bisect(f, ds, bits);
        std::vector<uint32_t> where;
        uint32_t fails = EqualFails(f, ds, l, count, where);
        uint32_t rne = 0, hu = 0;
        for(uint32_t i = 0; i < count; i++)
        {
          rne += uint32_t(l[i]) != ModelRNE(rnd[i], maxv) ? 1 : 0;
          hu += uint32_t(l[i]) != ModelHalfUp(rnd[i], maxv) ? 1 : 0;
        }
        TEST_LOG("  draws: F(l) not EQUAL after read back: %u of %u", fails, count);
        for(uint32_t w : where)
          TEST_LOG("    pixel %u: depth %.9g (%08x) l %u (%06x) RNE %u", w, rnd[w], Bits(rnd[w]),
                   uint32_t(l[w]), uint32_t(l[w]), ModelRNE(rnd[w], maxv));
        TEST_LOG("  draws: l differs from round-to-nearest-even(d*max) %u, from floor(d*max+0.5) %u",
                 rne, hu);
        for(uint32_t i = 0; i < 8; i++)
          TEST_LOG("    drawn %.9g: l %u (%06x)", rnd[i], uint32_t(l[i]), uint32_t(l[i]));
      }

      // clears
      {
        dev->SetRenderTarget(0, passedSurf);
        dev->SetDepthStencilSurface(ds);
        D3DVIEWPORT9 vp = {0, 0, W, H, 0.0f, 1.0f};
        dev->SetViewport(&vp);
        dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        for(uint32_t i = 0; i < nClears; i++)
        {
          D3DRECT r = {LONG(i % W), LONG(i / W), LONG(i % W + 1), LONG(i / W + 1)};
          dev->Clear(1, &r, D3DCLEAR_ZBUFFER, 0, clr[i], 0);
        }
        std::vector<float> l = Bisect(f, ds, bits);
        std::vector<uint32_t> where;
        uint32_t fails = EqualFails(f, ds, l, nClears, where);
        uint32_t rne = 0, hu = 0;
        for(uint32_t i = 0; i < nClears; i++)
        {
          rne += uint32_t(l[i]) != ModelRNE(clr[i], maxv) ? 1 : 0;
          hu += uint32_t(l[i]) != ModelHalfUp(clr[i], maxv) ? 1 : 0;
        }
        TEST_LOG("  clears: F(l) not EQUAL after read back: %u of %u", fails, nClears);
        for(uint32_t w : where)
          TEST_LOG("    pixel %u: clear %.9g (%08x) l %u (%06x) RNE %u", w, clr[w], Bits(clr[w]),
                   uint32_t(l[w]), uint32_t(l[w]), ModelRNE(clr[w], maxv));
        TEST_LOG("  clears: l differs from round-to-nearest-even(d*max) %u, from floor(d*max+0.5) %u",
                 rne, hu);
        for(uint32_t i = 0; i < 8; i++)
          TEST_LOG("    cleared %.9g: l %u (%06x)", clr[i], uint32_t(l[i]), uint32_t(l[i]));
      }
    }

    // the levels formula A lands on, read back with B (whose l is the level's index)
    {
      const Formula &a = formulas[0], &b = formulas[1];
      Upload(idx);
      dev->SetRenderTarget(0, passedSurf);
      dev->SetDepthStencilSurface(ds);
      dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 0.0f, 0);
      const float c1[4] = {0, a.threshold, a.step, a.scale};
      const float noout[4] = {0, 1, 0, 0};
      Draw(formulaPS, passedSurf, ds, vals, NULL, c1, noout, true, D3DCMP_ALWAYS, true, true);
      std::vector<float> lv = Bisect(b, ds, bits);
      std::vector<uint8_t> hit(count);
      uint32_t below = 0, same = 0, above = 0;
      for(uint32_t i = 0; i < count; i++)
      {
        uint32_t v = uint32_t(lv[i]);
        if(v < count)
          hit[v] = 1;
        if(v < i)
          below++;
        else if(v == i)
          same++;
        else
          above++;
      }
      TEST_LOG("-- A's l against the level it lands on: lands below l %u, on l %u, above l %u", below,
               same, above);
      for(uint32_t v = 0; v < count; v++)
        if(!hit[v])
          TEST_LOG("  level %u (%06x) is never reached by A; B's depth for it %.9g (%08x)", v, v,
                   CpuF(b, v), Bits(CpuF(b, v)));
    }
  }

  int main()
  {
    TEST_LOG("start");
    if(!Init())
      return 3;
    TEST_LOG("init done");

    vs = CreateVS(Compile(vsSrc, "main", "vs_3_0"));
    formulaPS = CreatePS(Compile(formulaSrc, "main", "ps_3_0"));
    combinePS = CreatePS(Compile(combineSrc, "main", "ps_3_0"));
    D3DVERTEXELEMENT9 el[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END(),
    };
    dev->CreateVertexDeclaration(el, &decl);
    TEST_LOG("shaders %p %p %p", (void *)vs.GetInterfacePtr(), (void *)formulaPS.GetInterfacePtr(),
             (void *)combinePS.GetInterfacePtr());

    D3DADAPTER_IDENTIFIER9 id = {};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    TEST_LOG("adapter %s, driver %u.%u.%u.%u", id.Description, HIWORD(id.DriverVersion.HighPart),
             LOWORD(id.DriverVersion.HighPart), HIWORD(id.DriverVersion.LowPart),
             LOWORD(id.DriverVersion.LowPart));

    struct F
    {
      D3DFORMAT fmt;
      const char *name;
      int bits;
      bool tex;
    } fmts[] = {
        {D3DFMT_D24S8, "D24S8", 24, false},
        {D3DFMT_D24X8, "D24X8", 24, false},
        {(D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), "INTZ", 24, true},
        {D3DFMT_D24X4S4, "D24X4S4", 24, false},
        {D3DFMT_D32, "D32 (24 bits)", 24, false},
        {D3DFMT_D16, "D16", 16, false},
        {D3DFMT_D15S1, "D15S1", 15, false},
    };
    for(const F &f : fmts)
    {
      HRESULT hr = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                          D3DUSAGE_DEPTHSTENCIL,
                                          f.tex ? D3DRTYPE_TEXTURE : D3DRTYPE_SURFACE, f.fmt);
      if(FAILED(hr))
      {
        TEST_LOG("==== %s: not supported", f.name);
        continue;
      }
      Run(f.fmt, f.name, f.bits, f.tex);
    }

    return 0;
  }
};

REGISTER_TEST();
