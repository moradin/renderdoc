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

// a temporary probe: how SetLOD on a managed texture changes sampling, and whether setting
// MAXMIPLEVEL to max(MAXMIPLEVEL, LOD) on a default pool copy samples the same.

#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "d3d9_test.h"

RD_TEST(D3D9_LOD_Probe, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Probe of SetLOD";

  std::string VS = R"EOSHADER(
float4 main(float4 pos : POSITION) : POSITION { return pos; }
)EOSHADER";

  std::string VTF_VS = R"EOSHADER(
sampler2D vs : register(s0);
float4 p : register(c0);
struct v2f { float4 pos : POSITION; float4 col : COLOR0; };
v2f main(float4 pos : POSITION) { v2f o; o.pos = pos; o.col = tex2Dlod(vs, float4(0.5, 0.5, 0, p.x)); return o; }
)EOSHADER";

  std::string COL_PS = R"EOSHADER(
float4 main(float4 col : COLOR0) : COLOR0 { return col; }
)EOSHADER";

  std::string LOD_PS = R"EOSHADER(
sampler2D s : register(s0);
float4 p : register(c0);
float4 main() : COLOR0 { return tex2Dlod(s, float4(0.5, 0.5, 0, p.x)); }
)EOSHADER";

  std::string GRAD_PS = R"EOSHADER(
sampler2D s : register(s0);
float4 p : register(c0);
float4 main() : COLOR0 { return tex2Dgrad(s, float2(0.5, 0.5), float2(p.y, 0), float2(0, p.y)); }
)EOSHADER";

  std::string CUBE_PS = R"EOSHADER(
samplerCUBE s : register(s0);
float4 p : register(c0);
float4 main() : COLOR0 { return texCUBElod(s, float4(1, 0, 0, p.x)); }
)EOSHADER";

  std::string VOL_PS = R"EOSHADER(
sampler3D s : register(s0);
float4 p : register(c0);
float4 main() : COLOR0 { return tex3Dlod(s, float4(0.5, 0.5, 0.5, p.x)); }
)EOSHADER";

  static DWORD Texel(UINT level, UINT x, UINT y, UINT z)
  {
    DWORD r = 0x10 + 0x20 * level;
    DWORD g = ((x ^ y ^ z) & 1) ? 0xFF : 0;
    return 0xFF000000 | (r << 16) | (g << 8);
  }

  void Fill2D(IDirect3DTexture9 *tex)
  {
    for(UINT l = 0; l < tex->GetLevelCount(); l++)
    {
      D3DSURFACE_DESC desc;
      tex->GetLevelDesc(l, &desc);
      D3DLOCKED_RECT lr;
      tex->LockRect(l, &lr, NULL, 0);
      for(UINT y = 0; y < desc.Height; y++)
        for(UINT x = 0; x < desc.Width; x++)
          ((DWORD *)((byte *)lr.pBits + y * lr.Pitch))[x] = Texel(l, x, y, 0);
      tex->UnlockRect(l);
    }
  }

  void FillCube(IDirect3DCubeTexture9 *tex)
  {
    for(UINT f = 0; f < 6; f++)
      for(UINT l = 0; l < tex->GetLevelCount(); l++)
      {
        D3DSURFACE_DESC desc;
        tex->GetLevelDesc(l, &desc);
        D3DLOCKED_RECT lr;
        tex->LockRect((D3DCUBEMAP_FACES)f, l, &lr, NULL, 0);
        for(UINT y = 0; y < desc.Height; y++)
          for(UINT x = 0; x < desc.Width; x++)
            ((DWORD *)((byte *)lr.pBits + y * lr.Pitch))[x] = Texel(l, x, y, 0);
        tex->UnlockRect((D3DCUBEMAP_FACES)f, l);
      }
  }

  void FillVol(IDirect3DVolumeTexture9 *tex)
  {
    for(UINT l = 0; l < tex->GetLevelCount(); l++)
    {
      D3DVOLUME_DESC desc;
      tex->GetLevelDesc(l, &desc);
      D3DLOCKED_BOX lb;
      tex->LockBox(l, &lb, NULL, 0);
      for(UINT z = 0; z < desc.Depth; z++)
        for(UINT y = 0; y < desc.Height; y++)
          for(UINT x = 0; x < desc.Width; x++)
            ((DWORD *)((byte *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch))[x] =
                Texel(l, x, y, z);
      tex->UnlockBox(l);
    }
  }

  IDirect3DSurface9Ptr rt, sys;
  IDirect3DVertexDeclaration9Ptr decl;
  static const UINT W = 32, H = 256;

  void DrawPixel(UINT x, UINT y)
  {
    D3DVIEWPORT9 vp = {x, y, 1, 1, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    float quad[] = {-1, -1, 0.5f, 1, 1, -1, 0.5f, 1, -1, 1, 0.5f, 1, 1, 1, 0.5f, 1};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(float) * 4);
  }

  std::vector<DWORD> ReadBack()
  {
    std::vector<DWORD> ret(W * H);
    dev->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr;
    sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
    for(UINT y = 0; y < H; y++)
      memcpy(&ret[y * W], (byte *)lr.pBits + y * lr.Pitch, W * 4);
    sys->UnlockRect();
    return ret;
  }

  void Begin()
  {
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(NULL);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, 0x00ABCDEF, 1.0f, 0);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetVertexDeclaration(decl);
    dev->BeginScene();
  }

  struct Case
  {
    DWORD lod, maxMip, mipFilter;
    float bias;
  };

  // samples tex (bound to sampler) with every case, managed with SetLOD, or with MAXMIPLEVEL raised
  // to the LOD when emulate is set (tex is then a default pool texture whose SetLOD does nothing)
  std::vector<DWORD> Run(IDirect3DBaseTexture9 *tex, DWORD sampler, bool emulate,
                         const std::vector<Case> &cases, const std::vector<float> &lods,
                         const std::vector<float> &grads, IDirect3DVertexShader9 *vsLod,
                         IDirect3DPixelShader9 *psLod, IDirect3DPixelShader9 *psGrad)
  {
    Begin();
    dev->SetTexture(sampler, tex);
    dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    for(UINT c = 0; c < cases.size(); c++)
    {
      const Case &cs = cases[c];
      DWORD levels = tex->GetLevelCount();
      if(emulate)
      {
        DWORD lod = cs.lod < levels ? cs.lod : levels - 1;
        dev->SetSamplerState(sampler, D3DSAMP_MAXMIPLEVEL, cs.maxMip > lod ? cs.maxMip : lod);
      }
      else
      {
        tex->SetLOD(cs.lod);
        dev->SetSamplerState(sampler, D3DSAMP_MAXMIPLEVEL, cs.maxMip);
      }
      dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, cs.mipFilter);
      dev->SetSamplerState(sampler, D3DSAMP_MIPMAPLODBIAS, *(DWORD *)&cs.bias);
      UINT x = 0;
      for(float lod : lods)
      {
        float p[4] = {lod, 0, 0, 0};
        if(vsLod)
        {
          dev->SetVertexShaderConstantF(0, p, 1);
          dev->SetVertexShader(vsLod);
        }
        else
        {
          dev->SetPixelShaderConstantF(0, p, 1);
          dev->SetPixelShader(psLod);
        }
        DrawPixel(x++, c);
      }
      for(float lambda : grads)
      {
        float p[4] = {0, powf(2.0f, lambda) / 64.0f, 0, 0};
        dev->SetPixelShaderConstantF(0, p, 1);
        dev->SetPixelShader(psGrad);
        DrawPixel(x++, c);
      }
    }
    dev->EndScene();
    dev->SetTexture(sampler, NULL);
    if(!emulate)
      tex->SetLOD(0);
    return ReadBack();
  }

  void Report(const char *what, const std::vector<Case> &cases, const std::vector<float> &lods,
              const std::vector<float> &grads, const std::vector<DWORD> &a,
              const std::vector<DWORD> &b)
  {
    int diff = 0;
    UINT n = UINT(lods.size() + grads.size());
    for(UINT c = 0; c < cases.size(); c++)
    {
      std::string line, eline;
      for(UINT x = 0; x < n; x++)
      {
        DWORD va = a[c * W + x], vb = b[c * W + x];
        // level as 0x10 + 0x20 * level in red, checker in green (0x7F/0x80 = linear, mag)
        char buf[64];
        sprintf(buf, " %02x/%02x", (va >> 16) & 0xFF, (va >> 8) & 0xFF);
        line += buf;
        if(va != vb)
        {
          diff++;
          sprintf(buf, " [x%u %08x vs %08x]", x, va, vb);
          eline += buf;
        }
      }
      TEST_LOG("%s LOD %u MAXMIP %u MIPF %u bias %.1f:%s", what, cases[c].lod, cases[c].maxMip,
               cases[c].mipFilter, cases[c].bias, line.c_str());
      if(!eline.empty())
        TEST_LOG("  emulation differs:%s", eline.c_str());
    }
    TEST_LOG("%s: %d of %u samples differ between SetLOD and MAXMIPLEVEL", what, diff,
             UINT(cases.size()) * n);
  }

  int main()
  {
    if(!Init())
      return 3;

    dev->CreateRenderTarget(W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                              D3DDECL_END()};
    dev->CreateVertexDeclaration(el, &decl);

    IDirect3DVertexShader9Ptr vs = CreateVS(Compile(VS, "main", "vs_3_0"));
    IDirect3DVertexShader9Ptr vtf = CreateVS(Compile(VTF_VS, "main", "vs_3_0"));
    IDirect3DPixelShader9Ptr colps = CreatePS(Compile(COL_PS, "main", "ps_3_0"));
    IDirect3DPixelShader9Ptr lodps = CreatePS(Compile(LOD_PS, "main", "ps_3_0"));
    IDirect3DPixelShader9Ptr gradps = CreatePS(Compile(GRAD_PS, "main", "ps_3_0"));
    IDirect3DPixelShader9Ptr cubeps = CreatePS(Compile(CUBE_PS, "main", "ps_3_0"));
    IDirect3DPixelShader9Ptr volps = CreatePS(Compile(VOL_PS, "main", "ps_3_0"));

    // 64x64, 7 levels, managed and default (dynamic, as replay makes it)
    IDirect3DTexture9Ptr managed, def;
    dev->CreateTexture(64, 64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managed, NULL);
    dev->CreateTexture(64, 64, 0, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &def, NULL);
    Fill2D(managed);
    Fill2D(def);
    TEST_LOG("levels %u %u", managed->GetLevelCount(), def->GetLevelCount());

    DWORD ret = managed->SetLOD(3);
    TEST_LOG("managed SetLOD(3) returned %u, GetLOD %u", ret, managed->GetLOD());
    ret = managed->SetLOD(9);
    TEST_LOG("managed SetLOD(9) returned %u, GetLOD %u", ret, managed->GetLOD());
    ret = managed->SetLOD(0);
    TEST_LOG("managed SetLOD(0) returned %u, GetLOD %u", ret, managed->GetLOD());
    ret = def->SetLOD(2);
    TEST_LOG("default SetLOD(2) returned %u, GetLOD %u", ret, def->GetLOD());
    def->SetLOD(0);

    std::vector<Case> cases;
    DWORD lodList[] = {0, 1, 2, 3, 6, 9};
    DWORD maxList[] = {0, 1, 2, 4};
    DWORD mipfList[] = {D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR};
    float biasList[] = {0.0f, 1.0f};
    for(DWORD l : lodList)
      for(DWORD m : maxList)
        for(DWORD f : mipfList)
          for(float b : biasList)
            cases.push_back({l, m, f, b});

    std::vector<float> lods = {-1.0f, 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 6.0f, 8.0f};
    std::vector<float> grads = {-1.0f, 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 6.0f};

    dev->SetVertexShader(vs);
    std::vector<DWORD> a = Run(managed, 0, false, cases, lods, grads, NULL, lodps, gradps);
    std::vector<DWORD> b = Run(def, 0, true, cases, lods, grads, NULL, lodps, gradps);
    Report("2D", cases, lods, grads, a, b);

    // cube 32, 6 levels, and volume 16, 5 levels: explicit LODs only
    std::vector<float> nograds;
    IDirect3DCubeTexture9Ptr cubeM, cubeD;
    dev->CreateCubeTexture(32, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cubeM, NULL);
    dev->CreateCubeTexture(32, 0, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cubeD, NULL);
    FillCube(cubeM);
    FillCube(cubeD);
    a = Run(cubeM, 0, false, cases, lods, nograds, NULL, cubeps, NULL);
    b = Run(cubeD, 0, true, cases, lods, nograds, NULL, cubeps, NULL);
    Report("cube", cases, lods, nograds, a, b);

    IDirect3DVolumeTexture9Ptr volM, volD;
    dev->CreateVolumeTexture(16, 16, 16, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volM, NULL);
    dev->CreateVolumeTexture(16, 16, 16, 0, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                             &volD, NULL);
    FillVol(volM);
    FillVol(volD);
    a = Run(volM, 0, false, cases, lods, nograds, NULL, volps, NULL);
    b = Run(volD, 0, true, cases, lods, nograds, NULL, volps, NULL);
    Report("volume", cases, lods, nograds, a, b);

    // vertex texture fetch, if A8R8G8B8 can be a vertex texture
    HRESULT hr = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                        D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE,
                                        D3DFMT_A8R8G8B8);
    TEST_LOG("A8R8G8B8 vertex texture: %08x", hr);
    if(SUCCEEDED(hr))
    {
      dev->SetPixelShader(colps);
      a = Run(managed, D3DVERTEXTEXTURESAMPLER0, false, cases, lods, nograds, vtf, NULL, NULL);
      dev->SetPixelShader(colps);
      b = Run(def, D3DVERTEXTEXTURESAMPLER0, true, cases, lods, nograds, vtf, NULL, NULL);
      Report("vertex", cases, lods, nograds, a, b);
    }

    TEST_LOG("probe done");
    return 0;
  }
};

REGISTER_TEST();
