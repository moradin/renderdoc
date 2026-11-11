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

// a temporary probe: what texldl returns natively for each sampler filter, address mode, LOD and
// texture type. Writes one file per configuration to PROBE_OUT: the texture's texels as the GPU
// reads them, then a line per sample with its coordinate and the result.

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <map>
#include <string>
#include "../win32/win32_window.h"
#include "d3d9_test.h"

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

RD_TEST(D3D9_Texldl_Probe, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Probe of texldl filtering";

  pD3DAssemble assemble = NULL;
  std::string outDir;

  std::vector<DWORD> Assemble(const std::string &src)
  {
    ID3DBlobPtr code, errors;
    if(FAILED(assemble(src.c_str(), src.size(), NULL, NULL, NULL, 0, &code, &errors)))
    {
      TEST_LOG("assemble failed: %s", errors ? (const char *)errors->GetBufferPointer() : "");
      return {};
    }
    const DWORD *p = (const DWORD *)code->GetBufferPointer();
    return std::vector<DWORD>(p, p + code->GetBufferSize() / 4);
  }

  static float Hash(uint32_t a)
  {
    a ^= a >> 16;
    a *= 0x7feb352dU;
    a ^= a >> 15;
    a *= 0x846ca68bU;
    a ^= a >> 16;
    return float(a & 0xffff) / 65536.0f;
  }

  static uint16_t ToHalf(float f)
  {
    uint32_t u;
    memcpy(&u, &f, 4);
    uint32_t sign = (u >> 16) & 0x8000;
    int32_t e = int32_t((u >> 23) & 0xff) - 127 + 15;
    uint32_t m = u & 0x7fffff;
    if(e <= 0)
      return uint16_t(sign);
    if(e >= 31)
      return uint16_t(sign | 0x7c00);
    return uint16_t(sign | (e << 10) | (m >> 13));
  }

  static float FromHalf(uint16_t h)
  {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t e = (h >> 10) & 0x1f;
    uint32_t m = h & 0x3ff;
    uint32_t u = 0;
    if(e == 0)
      u = sign;
    else
      u = sign | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
  }

  // the value written for each texel, before the format's precision: x, y, a layer/level code
  // and a hash. For UNORM formats everything is scaled into 0..1.
  struct Texel
  {
    float v[4];
  };

  // writes a texel into memory in the format and returns what the GPU reads
  static Texel Store(D3DFORMAT fmt, byte *dst, Texel t)
  {
    Texel ret = {};
    switch(fmt)
    {
      case D3DFMT_A32B32G32R32F:
        memcpy(dst, t.v, 16);
        ret = t;
        break;
      case D3DFMT_A16B16G16R16F:
        for(int c = 0; c < 4; c++)
        {
          uint16_t h = ToHalf(t.v[c]);
          memcpy(dst + c * 2, &h, 2);
          ret.v[c] = FromHalf(h);
        }
        break;
      case D3DFMT_R32F:
        memcpy(dst, t.v, 4);
        ret.v[0] = t.v[0];
        ret.v[1] = ret.v[2] = ret.v[3] = 1.0f;
        break;
      case D3DFMT_A8R8G8B8:
      {
        byte b[4];
        for(int c = 0; c < 4; c++)
        {
          float f = t.v[c] < 0.0f ? 0.0f : (t.v[c] > 1.0f ? 1.0f : t.v[c]);
          b[c] = byte(f * 255.0f + 0.5f);
          ret.v[c] = float(b[c]) / 255.0f;
        }
        // stored B, G, R, A
        dst[0] = b[2];
        dst[1] = b[1];
        dst[2] = b[0];
        dst[3] = b[3];
        break;
      }
      default: break;
    }
    return ret;
  }

  static UINT Bpp(D3DFORMAT fmt)
  {
    switch(fmt)
    {
      case D3DFMT_A32B32G32R32F: return 16;
      case D3DFMT_A16B16G16R16F: return 8;
      default: return 4;
    }
  }

  static const char *FmtName(D3DFORMAT fmt)
  {
    switch(fmt)
    {
      case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
      case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
      case D3DFMT_R32F: return "R32F";
      case D3DFMT_A8R8G8B8: return "A8R8G8B8";
      default: return "?";
    }
  }

  struct Tex
  {
    IDirect3DBaseTexture9Ptr tex;
    std::string kind;    // 2d, cube, volume
    D3DFORMAT fmt;
    // per level: width, height, depth (faces for cube) and the texels as read
    struct Level
    {
      UINT w, h, d;
      std::vector<Texel> texels;
    };
    std::vector<Level> levels;
  };

  Texel Make(D3DFORMAT fmt, UINT x, UINT y, UINT z, UINT l, UINT w, UINT h)
  {
    Texel t;
    if(fmt == D3DFMT_A8R8G8B8)
    {
      // spread over 0..1
      t.v[0] = (float(x) + 0.5f) / float(w);
      t.v[1] = (float(y) + 0.5f) / float(h);
      t.v[2] = float(z * 4 + l) / 32.0f;
      t.v[3] = Hash(x * 7919 + y * 104729 + z * 1299709 + l * 15485863);
    }
    else
    {
      t.v[0] = float(x);
      t.v[1] = float(y);
      t.v[2] = float(z * 10 + l * 100);
      t.v[3] = Hash(x * 7919 + y * 104729 + z * 1299709 + l * 15485863);
    }
    return t;
  }

  bool Create2D(IDirect3DDevice9 * d, D3DFORMAT fmt, UINT w, UINT h, UINT levels, Tex & out,
                bool weights = false)
  {
    IDirect3DTexture9Ptr t;
    if(FAILED(d->CreateTexture(w, h, levels, 0, fmt, D3DPOOL_MANAGED, &t, NULL)))
      return false;
    out.tex = t;
    out.kind = "2d";
    out.fmt = fmt;
    for(UINT l = 0; l < levels; l++)
    {
      UINT lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
      Tex::Level lev = {lw, lh, 1, {}};
      D3DLOCKED_RECT lr = {};
      t->LockRect(l, &lr, NULL, 0);
      for(UINT y = 0; y < lh; y++)
        for(UINT x = 0; x < lw; x++)
        {
          Texel v = Make(fmt, x, y, 0, l, lw, lh);
          if(weights)
          {
            // 0 and 1, to read the weights straight off
            v.v[0] = float(x & 1);
            v.v[1] = float(y & 1);
            v.v[2] = float(l & 1);
            v.v[3] = 1.0f;
          }
          lev.texels.push_back(Store(fmt, (byte *)lr.pBits + y * lr.Pitch + x * Bpp(fmt), v));
        }
      t->UnlockRect(l);
      out.levels.push_back(lev);
    }
    return true;
  }

  bool CreateCube(IDirect3DDevice9 * d, D3DFORMAT fmt, UINT size, UINT levels, Tex & out)
  {
    IDirect3DCubeTexture9Ptr t;
    if(FAILED(d->CreateCubeTexture(size, levels, 0, fmt, D3DPOOL_MANAGED, &t, NULL)))
      return false;
    out.tex = t;
    out.kind = "cube";
    out.fmt = fmt;
    for(UINT l = 0; l < levels; l++)
    {
      UINT s = size >> l ? size >> l : 1;
      Tex::Level lev = {s, s, 6, {}};
      for(UINT f = 0; f < 6; f++)
      {
        D3DLOCKED_RECT lr = {};
        t->LockRect((D3DCUBEMAP_FACES)f, l, &lr, NULL, 0);
        for(UINT y = 0; y < s; y++)
          for(UINT x = 0; x < s; x++)
            lev.texels.push_back(Store(fmt, (byte *)lr.pBits + y * lr.Pitch + x * Bpp(fmt),
                                       Make(fmt, x, y, f, l, s, s)));
        t->UnlockRect((D3DCUBEMAP_FACES)f, l);
      }
      out.levels.push_back(lev);
    }
    return true;
  }

  bool CreateVolume(IDirect3DDevice9 * d, D3DFORMAT fmt, UINT size, UINT levels, Tex & out)
  {
    IDirect3DVolumeTexture9Ptr t;
    if(FAILED(d->CreateVolumeTexture(size, size, size, levels, 0, fmt, D3DPOOL_MANAGED, &t, NULL)))
      return false;
    out.tex = t;
    out.kind = "volume";
    out.fmt = fmt;
    for(UINT l = 0; l < levels; l++)
    {
      UINT s = size >> l ? size >> l : 1;
      Tex::Level lev = {s, s, s, {}};
      D3DLOCKED_BOX lb = {};
      t->LockBox(l, &lb, NULL, 0);
      for(UINT z = 0; z < s; z++)
        for(UINT y = 0; y < s; y++)
          for(UINT x = 0; x < s; x++)
            lev.texels.push_back(
                Store(fmt, (byte *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch + x * Bpp(fmt),
                      Make(fmt, x, y, z, l, s, s)));
      t->UnlockBox(l);
      out.levels.push_back(lev);
    }
    return true;
  }

  struct Coord
  {
    float v[4];
  };

  // draws one point per coordinate into a float target and reads back what texldl returned
  void Sample(IDirect3DDevice9 * d, const char *devName, const std::string &name, const Tex &tex,
              const std::vector<std::pair<D3DSAMPLERSTATETYPE, DWORD>> &states,
              const std::vector<Coord> &coords)
  {
    const char *dcl = tex.kind == "cube" ? "dcl_cube s0\n"
                                         : (tex.kind == "volume" ? "dcl_volume s0\n" : "dcl_2d s0\n");
    static std::map<std::string, IDirect3DVertexShader9Ptr> vsCache;
    std::string key = std::string(devName) + tex.kind;
    IDirect3DVertexShader9Ptr vs = vsCache[key];
    if(!vs)
    {
      std::vector<DWORD> vt = Assemble(
          std::string("vs_3_0\ndcl_position v0\ndcl_texcoord v1\n") + dcl +
          "dcl_position o0\ndcl_texcoord o1\ntexldl r0, v1, s0\nmov o0, v0\nmov o1, r0\n");
      if(vt.empty() || FAILED(d->CreateVertexShader(vt.data(), &vs)))
      {
        TEST_LOG("%s: couldn't create the VS", devName);
        return;
      }
      vsCache[key] = vs;
    }
    IDirect3DPixelShader9Ptr ps;
    {
      std::vector<DWORD> pt = Assemble("ps_3_0\ndcl_texcoord v0\nmov oC0, v0\n");
      d->CreatePixelShader(pt.data(), &ps);
    }

    const UINT W = 64;
    const UINT H = UINT((coords.size() + W - 1) / W);
    IDirect3DSurface9Ptr rt, read, old;
    d->CreateRenderTarget(W, H, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    d->CreateOffscreenPlainSurface(W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &read, NULL);
    d->GetRenderTarget(0, &old);

    D3DVERTEXELEMENT9 el[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};
    IDirect3DVertexDeclaration9Ptr decl;
    d->CreateVertexDeclaration(el, &decl);

    std::vector<float> verts;
    for(size_t i = 0; i < coords.size(); i++)
    {
      // a quarter pixel off the centre, so the point covers one pixel whichever way centres are
      float px = float(i % W) + 0.25f, py = float(i / W) + 0.25f;
      verts.push_back(px * 2.0f / float(W) - 1.0f);
      verts.push_back(1.0f - py * 2.0f / float(H));
      verts.push_back(0.5f);
      verts.push_back(1.0f);
      for(int c = 0; c < 4; c++)
        verts.push_back(coords[i].v[c]);
    }

    d->SetRenderTarget(0, rt);
    d->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(7, 7, 7, 7), 1.0f, 0);
    D3DVIEWPORT9 vp = {0, 0, W, H, 0.0f, 1.0f};
    d->SetViewport(&vp);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    float size = 1.0f;
    d->SetRenderState(D3DRS_POINTSIZE, *(DWORD *)&size);
    d->SetVertexDeclaration(decl);
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    d->SetTexture(D3DVERTEXTEXTURESAMPLER0, tex.tex);
    // defaults first, then the configuration's
    const std::pair<D3DSAMPLERSTATETYPE, DWORD> defaults[] = {
        {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP},  {D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP},
        {D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP},  {D3DSAMP_BORDERCOLOR, 0},
        {D3DSAMP_MAGFILTER, D3DTEXF_POINT},    {D3DSAMP_MINFILTER, D3DTEXF_POINT},
        {D3DSAMP_MIPFILTER, D3DTEXF_NONE},     {D3DSAMP_MIPMAPLODBIAS, 0},
        {D3DSAMP_MAXMIPLEVEL, 0},              {D3DSAMP_MAXANISOTROPY, 1},
        {D3DSAMP_SRGBTEXTURE, 0},
    };
    for(const auto &s : defaults)
      d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, s.first, s.second);
    for(const auto &s : states)
      d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, s.first, s.second);
    d->BeginScene();
    HRESULT hr = d->DrawPrimitiveUP(D3DPT_POINTLIST, (UINT)coords.size(), verts.data(),
                                    sizeof(float) * 8);
    d->EndScene();
    d->SetRenderTarget(0, old);
    d->SetVertexShader(NULL);
    d->SetPixelShader(NULL);
    d->SetTexture(D3DVERTEXTEXTURESAMPLER0, NULL);

    const char *only = getenv("PROBE_ONLY");
    if(only && name.find(only) == std::string::npos)
      return;
    std::string path = outDir + "/" + devName + "_" + name + ".txt";
    FILE *f = fopen(path.c_str(), "w");
    if(!f)
      return;
    fprintf(f, "name %s\ndevice %s\nkind %s\nformat %s\ndraw %08x\n", name.c_str(), devName,
            tex.kind.c_str(), FmtName(tex.fmt), (UINT)hr);
    for(const auto &s : states)
      fprintf(f, "state %d %u\n", (int)s.first, (UINT)s.second);
    for(size_t l = 0; l < tex.levels.size(); l++)
    {
      const Tex::Level &lev = tex.levels[l];
      fprintf(f, "level %zu %u %u %u\n", l, lev.w, lev.h, lev.d);
      for(const Texel &t : lev.texels)
        fprintf(f, "t %.9g %.9g %.9g %.9g\n", t.v[0], t.v[1], t.v[2], t.v[3]);
    }

    D3DLOCKED_RECT lr = {};
    int missing = 0;
    if(SUCCEEDED(d->GetRenderTargetData(rt, read)) &&
       SUCCEEDED(read->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
      for(size_t i = 0; i < coords.size(); i++)
      {
        const float *o = (const float *)((const byte *)lr.pBits + (i / W) * lr.Pitch) + (i % W) * 4;
        const float clear = 7.0f / 255.0f;
        if(o[0] == clear && o[1] == clear && o[2] == clear && o[3] == clear)
          missing++;
        fprintf(f, "s %.9g %.9g %.9g %.9g = %.9g %.9g %.9g %.9g\n", coords[i].v[0],
                coords[i].v[1], coords[i].v[2], coords[i].v[3], o[0], o[1], o[2], o[3]);
      }
      read->UnlockRect();
    }
    fclose(f);
    TEST_LOG("%s %s: %zu samples, draw %08x, %d not written", devName, name.c_str(), coords.size(),
             (UINT)hr, missing);
  }

  void Run(IDirect3DDevice9 * d, const char *devName)
  {
    D3DCAPS9 caps = {};
    d->GetDeviceCaps(&caps);
    TEST_LOG("%s: VertexTextureFilterCaps %08x, MaxVertexBlendMatrixIndex %u", devName,
             (UINT)caps.VertexTextureFilterCaps, (UINT)caps.MaxVertexBlendMatrixIndex);

    const D3DFORMAT fmts[] = {D3DFMT_A32B32G32R32F, D3DFMT_A16B16G16R16F, D3DFMT_R32F,
                              D3DFMT_A8R8G8B8};
    for(D3DFORMAT fmt : fmts)
    {
      HRESULT a = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                         presentParams.BackBufferFormat,
                                         D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, fmt);
      HRESULT b = d3d->CheckDeviceFormat(
          D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, presentParams.BackBufferFormat,
          D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, fmt);
      HRESULT c = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                         presentParams.BackBufferFormat, D3DUSAGE_QUERY_FILTER,
                                         D3DRTYPE_TEXTURE, fmt);
      TEST_LOG("%s: %s vertex texture %08x, filtered vertex texture %08x, filtered %08x", devName,
               FmtName(fmt), (UINT)a, (UINT)b, (UINT)c);
    }

    // the coordinates: u and v across the texture's edges, and LODs around each level
    const float us[] = {-0.3f, -0.05f, 0.0f, 0.03f, 0.1f, 0.37f, 0.5f, 0.61f, 0.95f, 1.0f, 1.07f, 1.6f};
    const float vs[] = {-0.2f, 0.0f, 0.06f, 0.3f, 0.55f, 0.9f, 1.0f, 1.12f};
    const float lods[] = {-1.0f, -0.6f, -0.5f, -0.25f, -0.01f, 0.0f, 0.01f,
                          0.1f,  0.25f, 0.4f,  0.5f,   0.6f,   0.75f, 0.99f,
                          1.0f,  1.3f,  1.5f,  2.0f,   2.6f,   3.0f,  5.0f};
    std::vector<Coord> c2d;
    for(float lod : lods)
      for(float v : vs)
        for(float u : us)
          c2d.push_back({{u, v, 0.0f, lod}});

    // the weights: across one texel gap in 1/512 steps, at LOD 0 and between levels 0 and 1
    std::vector<Coord> cw;
    for(int i = 0; i <= 512; i++)
    {
      float u = 0.25f + float(i) / 1024.0f;
      cw.push_back({{u, 0.25f, 0.0f, 0.0f}});
    }
    for(int i = 0; i <= 512; i++)
      cw.push_back({{0.25f, 0.25f, 0.0f, float(i) / 512.0f}});

    typedef std::vector<std::pair<D3DSAMPLERSTATETYPE, DWORD>> States;
    auto filters = [](DWORD mag, DWORD mn, DWORD mip) {
      return States{{D3DSAMP_MAGFILTER, mag}, {D3DSAMP_MINFILTER, mn}, {D3DSAMP_MIPFILTER, mip}};
    };
    auto plus = [](States a, const States &b) {
      a.insert(a.end(), b.begin(), b.end());
      return a;
    };
    const char *fn[] = {"N", "P", "L", "A"};

    Tex t2d;
    if(!Create2D(d, D3DFMT_A32B32G32R32F, 8, 4, 4, t2d))
    {
      TEST_LOG("%s: no float texture", devName);
      return;
    }

    // every filter combination
    for(DWORD mag : {D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_ANISOTROPIC})
      for(DWORD mn : {D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_ANISOTROPIC})
        for(DWORD mip : {D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR})
          Sample(d, devName, std::string("f2d_") + fn[mag] + fn[mn] + fn[mip], t2d,
                 filters(mag, mn, mip), c2d);

    const States lll = filters(D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR);
    const States ppp = filters(D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_POINT);
    const char *an[] = {"", "wrap", "mirror", "clamp", "border", "mirroronce"};
    for(DWORD a = D3DTADDRESS_WRAP; a <= D3DTADDRESS_MIRRORONCE; a++)
    {
      States s = {{D3DSAMP_ADDRESSU, a}, {D3DSAMP_ADDRESSV, a},
                  {D3DSAMP_BORDERCOLOR, D3DCOLOR_ARGB(255, 51, 102, 204)}};
      Sample(d, devName, std::string("addr_") + an[a] + "_LLL", t2d, plus(lll, s), c2d);
      Sample(d, devName, std::string("addr_") + an[a] + "_LLN", t2d,
             plus(filters(D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE), s), c2d);
    }
    Sample(d, devName, "addr_clampU_borderV_LLL", t2d,
           plus(lll, {{D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
                      {D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER},
                      {D3DSAMP_BORDERCOLOR, D3DCOLOR_ARGB(128, 255, 0, 64)}}),
           c2d);

    for(DWORD mml : {1u, 2u, 3u, 7u})
    {
      std::string n = "maxmip" + std::to_string(mml);
      Sample(d, devName, n + "_LLL", t2d, plus(lll, {{D3DSAMP_MAXMIPLEVEL, mml}}), c2d);
      Sample(d, devName, n + "_PPP", t2d, plus(ppp, {{D3DSAMP_MAXMIPLEVEL, mml}}), c2d);
      Sample(d, devName, n + "_LLN", t2d,
             plus(filters(D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE), {{D3DSAMP_MAXMIPLEVEL, mml}}),
             c2d);
      Sample(d, devName, n + "_PLL", t2d,
             plus(filters(D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_LINEAR),
                  {{D3DSAMP_MAXMIPLEVEL, mml}}),
             c2d);
    }
    for(float bias : {0.5f, -0.7f, 1.25f})
    {
      DWORD b;
      memcpy(&b, &bias, 4);
      char n[32];
      snprintf(n, sizeof(n), "bias%g", bias);
      Sample(d, devName, std::string(n) + "_LLL", t2d, plus(lll, {{D3DSAMP_MIPMAPLODBIAS, b}}), c2d);
      Sample(d, devName, std::string(n) + "_PPP", t2d, plus(ppp, {{D3DSAMP_MIPMAPLODBIAS, b}}), c2d);
      Sample(d, devName, std::string(n) + "_PLP", t2d,
             plus(filters(D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_POINT),
                  {{D3DSAMP_MIPMAPLODBIAS, b}, {D3DSAMP_MAXMIPLEVEL, 1}}),
             c2d);
    }

    Tex tw;
    if(Create2D(d, D3DFMT_A32B32G32R32F, 2, 2, 2, tw, true))
    {
      Sample(d, devName, "weights_LLL", tw, lll, cw);
      Sample(d, devName, "weights_PPL", tw, filters(D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_LINEAR), cw);
    }

    // other formats
    for(D3DFORMAT fmt : {D3DFMT_A16B16G16R16F, D3DFMT_R32F, D3DFMT_A8R8G8B8})
    {
      Tex t;
      if(!Create2D(d, fmt, 8, 4, 4, t))
      {
        TEST_LOG("%s: couldn't create %s", devName, FmtName(fmt));
        continue;
      }
      Sample(d, devName, std::string("fmt_") + FmtName(fmt) + "_LLL", t, lll, c2d);
      Sample(d, devName, std::string("fmt_") + FmtName(fmt) + "_PPP", t, ppp, c2d);
      if(fmt == D3DFMT_A8R8G8B8)
      {
        Sample(d, devName, "fmt_A8R8G8B8_srgb_LLL", t, plus(lll, {{D3DSAMP_SRGBTEXTURE, 1}}), c2d);
        Sample(d, devName, "fmt_A8R8G8B8_srgb_PPP", t, plus(ppp, {{D3DSAMP_SRGBTEXTURE, 1}}), c2d);
      }
      if(fmt == D3DFMT_A8R8G8B8)
      {
        Tex w8;
        if(Create2D(d, fmt, 2, 2, 2, w8, true))
          Sample(d, devName, "weights_A8R8G8B8_LLL", w8, lll, cw);
      }
    }

    // cube: directions across face edges and corners
    Tex tc;
    if(CreateCube(d, D3DFMT_A32B32G32R32F, 4, 3, tc))
    {
      std::vector<Coord> cc;
      const float a[] = {-1.0f, -0.97f, -0.8f, -0.3f, 0.0f, 0.1f, 0.62f, 0.9f, 0.99f, 1.0f};
      for(float lod : {0.0f, 0.3f, 0.5f, 1.0f, 1.7f, 4.0f, -0.5f})
        for(int major = 0; major < 3; major++)
          for(float sign : {1.0f, -1.0f})
            for(float s : a)
              for(float t : a)
              {
                Coord co = {};
                co.v[major] = sign;
                co.v[(major + 1) % 3] = s;
                co.v[(major + 2) % 3] = t;
                co.v[3] = lod;
                cc.push_back(co);
              }
      // a few with a longer major axis
      for(float lod : {0.0f, 1.0f})
        for(float s : a)
          cc.push_back({{2.5f, s * 2.5f, -0.4f * 2.5f, lod}});
      Sample(d, devName, "cube_LLL", tc, lll, cc);
      Sample(d, devName, "cube_LLN", tc, filters(D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE), cc);
      Sample(d, devName, "cube_PPP", tc, ppp, cc);
      Sample(d, devName, "cube_PPL", tc, filters(D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_LINEAR), cc);
      Sample(d, devName, "cube_LLL_wrap_ignored", tc,
             plus(lll, {{D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER},
                        {D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER},
                        {D3DSAMP_ADDRESSW, D3DTADDRESS_BORDER},
                        {D3DSAMP_BORDERCOLOR, D3DCOLOR_ARGB(255, 51, 102, 204)}}),
             cc);
    }

    Tex tv;
    if(CreateVolume(d, D3DFMT_A32B32G32R32F, 4, 3, tv))
    {
      std::vector<Coord> cv;
      const float a[] = {-0.1f, 0.0f, 0.06f, 0.3f, 0.5f, 0.77f, 1.0f, 1.2f};
      for(float lod : {0.0f, 0.4f, 1.0f, 1.6f, -1.0f, -0.001f, 0.001f, 0.004f, -0.0f})
        for(float w : a)
          for(float v : a)
            for(float u : a)
              cv.push_back({{u, v, w, lod}});
      Sample(d, devName, "vol_LLL", tv, lll, cv);
      Sample(d, devName, "vol_PPP", tv, ppp, cv);
      Sample(d, devName, "vol_LLN", tv, filters(D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE), cv);
      // which of MAGFILTER and MINFILTER at a LOD of 0, with and without MAXMIPLEVEL
      for(DWORD mml : {0u, 1u})
      {
        std::string n = mml ? "_maxmip1" : "";
        for(DWORD mip : {D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR})
        {
          Sample(d, devName, std::string("vol_LP") + fn[mip] + n, tv,
                 plus(filters(D3DTEXF_LINEAR, D3DTEXF_POINT, mip), {{D3DSAMP_MAXMIPLEVEL, mml}}), cv);
          Sample(d, devName, std::string("vol_PL") + fn[mip] + n, tv,
                 plus(filters(D3DTEXF_POINT, D3DTEXF_LINEAR, mip), {{D3DSAMP_MAXMIPLEVEL, mml}}), cv);
          Sample(d, devName, std::string("f2d_LP") + fn[mip] + n, t2d,
                 plus(filters(D3DTEXF_LINEAR, D3DTEXF_POINT, mip), {{D3DSAMP_MAXMIPLEVEL, mml}}), c2d);
          Sample(d, devName, std::string("f2d_PL") + fn[mip] + n + "_b", t2d,
                 plus(filters(D3DTEXF_POINT, D3DTEXF_LINEAR, mip), {{D3DSAMP_MAXMIPLEVEL, mml}}), c2d);
        }
      }
      Sample(d, devName, "vol_LPN_border_clamp_mirroronce_maxmip1", tv,
             plus(filters(D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTEXF_NONE),
                  {{D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER},
                   {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
                   {D3DSAMP_ADDRESSW, D3DTADDRESS_MIRRORONCE},
                   {D3DSAMP_BORDERCOLOR, D3DCOLOR_ARGB(255, 51, 102, 204)},
                   {D3DSAMP_MAXMIPLEVEL, 1}}),
             cv);
      Sample(d, devName, "vol_LLL_clamp_mirror_border", tv,
             plus(lll, {{D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
                        {D3DSAMP_ADDRESSV, D3DTADDRESS_MIRROR},
                        {D3DSAMP_ADDRESSW, D3DTADDRESS_BORDER},
                        {D3DSAMP_BORDERCOLOR, D3DCOLOR_ARGB(255, 51, 102, 204)}}),
             cv);
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

    char *out = getenv("PROBE_OUT");
    outDir = out ? out : ".";

    Run(dev, "hw");

    Win32Window *win = (Win32Window *)MakeWindow(screenWidth, screenHeight, "sw");
    D3DPRESENT_PARAMETERS pp = presentParams;
    pp.hDeviceWindow = win->wnd;
    IDirect3DDevice9Ptr sw;
    if(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, win->wnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &sw)))
      Run(sw, "sw");
    sw = NULL;
    delete win;

    return 0;
  }
};

REGISTER_TEST();
