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

#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include "../win32/win32_window.h"
#include "d3d9_test.h"

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

RD_TEST(D3D9_Mesh_Output, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Draws with fixed function vertex processing (transforms, each light type, texture "
      "coordinate generation, vertex blending, pretransformed vertices, point sizes) and with "
      "vs_1_1, vs_2_0 and vs_3_0 shaders (flow control, relative addressing, texldl), instancing, "
      "DrawPrimitiveUP and DrawIndexedPrimitiveUP, each in its own marker. Before drawing it works "
      "out what each draw's vertices should be after vertex processing, with ProcessVertices on a "
      "software vertex processing device where it can and in C++ otherwise, and writes that to "
      "D3D9_Mesh_Output_reference.txt in the demo's tmp folder (--tmp, or tmp next to the "
      "executable; _reference_rdoc.txt under RenderDoc) for util/test/d3d9_mesh_check.py and the "
      "D3D9_Mesh_Output test.";

  typedef std::map<std::string, std::vector<float>> Outputs;

  struct M
  {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    operator const D3DMATRIX *() const { return (const D3DMATRIX *)m; }
  };

  static M Mul(const M &a, const M &b)
  {
    M r;
    for(int i = 0; i < 4; i++)
      for(int j = 0; j < 4; j++)
        r.m[i * 4 + j] = a.m[i * 4 + 0] * b.m[0 * 4 + j] + a.m[i * 4 + 1] * b.m[1 * 4 + j] +
                         a.m[i * 4 + 2] * b.m[2 * 4 + j] + a.m[i * 4 + 3] * b.m[3 * 4 + j];
    return r;
  }

  static M Translate(float x, float y, float z)
  {
    M r;
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
  }

  static M Scale(float x, float y, float z)
  {
    M r;
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    return r;
  }

  static M RotY(float a)
  {
    M r;
    r.m[0] = cosf(a);
    r.m[2] = -sinf(a);
    r.m[8] = sinf(a);
    r.m[10] = cosf(a);
    return r;
  }

  // D3DXMatrixPerspectiveFovLH
  static M Perspective(float fovy, float aspect, float zn, float zf)
  {
    M r;
    float ys = 1.0f / tanf(fovy * 0.5f), xs = ys / aspect;
    r.m[0] = xs;
    r.m[5] = ys;
    r.m[10] = zf / (zf - zn);
    r.m[11] = 1.0f;
    r.m[14] = -zn * zf / (zf - zn);
    r.m[15] = 0.0f;
    return r;
  }

  static M Transpose(const M &a)
  {
    M r;
    for(int i = 0; i < 4; i++)
      for(int j = 0; j < 4; j++)
        r.m[i * 4 + j] = a.m[j * 4 + i];
    return r;
  }

  static void Transform(const float *v, const M &m, float *out)
  {
    for(int c = 0; c < 4; c++)
      out[c] = v[0] * m.m[c] + v[1] * m.m[4 + c] + v[2] * m.m[8 + c] + v[3] * m.m[12 + c];
  }

  // a draw, its state, and how to get its reference
  struct Case
  {
    std::string name;

    std::vector<byte> verts;
    UINT stride = 0;
    // instance data, in stream 1
    std::vector<byte> verts1;
    UINT stride1 = 0;
    DWORD fvf = 0;
    std::vector<D3DVERTEXELEMENT9> decl;
    std::vector<uint16_t> indices;

    std::vector<DWORD> vsTokens;
    // whether the draw has a vertex shader, so it's left out if the shader didn't compile
    bool shader = false;

    // sets the draw's state on a device
    std::function<void(IDirect3DDevice9 *)> state;

    enum Kind
    {
      Draw,
      Indexed,
      UP,
      IndexedUP,
    } kind = Draw;
    D3DPRIMITIVETYPE prim = D3DPT_TRIANGLELIST;
    UINT start = 0, primCount = 0;
    INT baseVertex = 0;
    UINT instances = 1;
    bool vertexTexture = false;

    // the outputs ProcessVertices gives, or empty to use reference()
    std::vector<std::string> pvOutputs;
    // the expected outputs of a vertex, for draws ProcessVertices can't do, in clip space
    std::function<void(UINT inst, UINT vert, Outputs &out)> reference;

    // vs_3_0 output registers to read back from the GPU (WriteHardwareOutputs): each vertex is
    // drawn on its own with a ps_3_0 that declares v0 as the vertex shader declares the register,
    // and writes v0 to a float render target. Each semantic is then the components its mask
    // names, in order.
    struct HWRegister
    {
      std::string psDcls;
      // the semantic's key in the reference, and its mask
      std::vector<std::pair<std::string, std::string>> semantics;
    };
    std::vector<HWRegister> hwRegisters;
    // if not 0, the GPU's values only have to be this close (texldl with filtering, whose
    // arithmetic isn't reproduced exactly), and the C++ reference has to match as well
    float hwTolerance = 0.0f;
  };

  struct Bound
  {
    IDirect3DVertexBuffer9Ptr vb, vb1;
    IDirect3DIndexBuffer9Ptr ib;
    IDirect3DVertexDeclaration9Ptr decl;
    IDirect3DVertexShader9Ptr vs;
  };

  static UINT VertexCount(D3DPRIMITIVETYPE prim, UINT count)
  {
    switch(prim)
    {
      case D3DPT_POINTLIST: return count;
      case D3DPT_LINELIST: return count * 2;
      case D3DPT_LINESTRIP: return count + 1;
      case D3DPT_TRIANGLELIST: return count * 3;
      default: return count + 2;
    }
  }

  // the vertex each row of a draw reads
  static std::vector<UINT> Rows(const Case &c)
  {
    std::vector<UINT> ret;
    UINT n = VertexCount(c.prim, c.primCount);
    for(UINT i = 0; i < n; i++)
    {
      switch(c.kind)
      {
        case Case::Draw: ret.push_back(c.start + i); break;
        case Case::Indexed: ret.push_back(UINT(c.indices[c.start + i] + c.baseVertex)); break;
        case Case::UP: ret.push_back(i); break;
        case Case::IndexedUP: ret.push_back(c.indices[i]); break;
      }
    }
    return ret;
  }

  template <typename T>
  static void Append(std::vector<byte> & v, const T &t)
  {
    const byte *b = (const byte *)&t;
    v.insert(v.end(), b, b + sizeof(T));
  }

  // what every draw starts from, so state doesn't leak between them
  static void ResetState(IDirect3DDevice9 * d, UINT w, UINT h)
  {
    D3DVIEWPORT9 vp = {0, 0, w, h, 0.0f, 1.0f};
    d->SetViewport(&vp);
    d->SetVertexShader(NULL);
    d->SetPixelShader(NULL);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    d->SetRenderState(D3DRS_CLIPPING, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    d->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
    d->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
    d->SetRenderState(D3DRS_COLORVERTEX, TRUE);
    d->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    d->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
    d->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    d->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    d->SetRenderState(D3DRS_AMBIENT, 0);
    d->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    d->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    d->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    d->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    d->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);
    // the defaults, which the "FFP point size" case changes
    float one = 1.0f, zero = 0.0f, maxSize = 64.0f;
    d->SetRenderState(D3DRS_POINTSIZE, *(DWORD *)&one);
    d->SetRenderState(D3DRS_POINTSIZE_MIN, *(DWORD *)&one);
    d->SetRenderState(D3DRS_POINTSIZE_MAX, *(DWORD *)&maxSize);
    d->SetRenderState(D3DRS_POINTSCALE_A, *(DWORD *)&one);
    d->SetRenderState(D3DRS_POINTSCALE_B, *(DWORD *)&zero);
    d->SetRenderState(D3DRS_POINTSCALE_C, *(DWORD *)&zero);
    d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
    for(DWORD i = 0; i < 8; i++)
    {
      d->LightEnable(i, FALSE);
      // the defaults: stage 0 modulates the texture and the diffuse colour, the rest are disabled
      d->SetTextureStageState(i, D3DTSS_COLOROP, i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
      d->SetTextureStageState(i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      d->SetTextureStageState(i, D3DTSS_COLORARG2, D3DTA_CURRENT);
      d->SetTextureStageState(i, D3DTSS_ALPHAOP, i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
      d->SetTextureStageState(i, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      d->SetTextureStageState(i, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
      d->SetTextureStageState(i, D3DTSS_TEXCOORDINDEX, i);
      d->SetTextureStageState(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
      d->SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + i), M());
    }
    for(UINT i = 0; i < 4; i++)
      d->SetTransform(D3DTS_WORLDMATRIX(i), M());
    d->SetTransform(D3DTS_VIEW, M());
    d->SetTransform(D3DTS_PROJECTION, M());
    d->SetStreamSourceFreq(0, 1);
    d->SetStreamSourceFreq(1, 1);
    d->SetStreamSource(1, NULL, 0, 0);
    // the vertex samplers' defaults, which the texldl cases change
    for(DWORD s = D3DVERTEXTEXTURESAMPLER0; s <= D3DVERTEXTEXTURESAMPLER3; s++)
    {
      d->SetTexture(s, NULL);
      d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
      d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
      d->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
      d->SetSamplerState(s, D3DSAMP_BORDERCOLOR, 0);
      d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      d->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
      d->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
      d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
  }

  bool Bind(IDirect3DDevice9 * d, const Case &c, bool software, Bound &b)
  {
    const DWORD usage = software ? D3DUSAGE_SOFTWAREPROCESSING : 0;
    const D3DPOOL pool = software ? D3DPOOL_SYSTEMMEM : D3DPOOL_MANAGED;

    auto makeVB = [&](const std::vector<byte> &data, IDirect3DVertexBuffer9Ptr &vb) {
      if(data.empty())
        return true;
      if(FAILED(d->CreateVertexBuffer((UINT)data.size(), usage, 0, pool, &vb, NULL)))
        return false;
      void *p = NULL;
      vb->Lock(0, 0, &p, 0);
      memcpy(p, data.data(), data.size());
      vb->Unlock();
      return true;
    };

    // ProcessVertices needs the UP draws' data in a buffer
    if((c.kind != Case::UP && c.kind != Case::IndexedUP) || software)
      if(!makeVB(c.verts, b.vb) || !makeVB(c.verts1, b.vb1))
        return false;

    if(c.kind == Case::Indexed)
    {
      UINT size = UINT(c.indices.size() * sizeof(uint16_t));
      if(FAILED(d->CreateIndexBuffer(size, usage, D3DFMT_INDEX16, pool, &b.ib, NULL)))
        return false;
      void *p = NULL;
      b.ib->Lock(0, 0, &p, 0);
      memcpy(p, c.indices.data(), size);
      b.ib->Unlock();
    }

    if(!c.decl.empty())
    {
      std::vector<D3DVERTEXELEMENT9> el = c.decl;
      el.push_back(D3DDECL_END());
      if(FAILED(d->CreateVertexDeclaration(el.data(), &b.decl)))
        return false;
    }

    if(!c.vsTokens.empty() && FAILED(d->CreateVertexShader(c.vsTokens.data(), &b.vs)))
      return false;

    return true;
  }

  void Apply(IDirect3DDevice9 * d, const Case &c, const Bound &b)
  {
    ResetState(d, screenWidth, screenHeight);
    if(c.state)
      c.state(d);
    if(b.decl)
      d->SetVertexDeclaration(b.decl);
    else
      d->SetFVF(c.fvf);
    d->SetVertexShader(b.vs);
    if(b.vb)
      d->SetStreamSource(0, b.vb, 0, c.stride);
    if(b.vb1)
      d->SetStreamSource(1, b.vb1, 0, c.stride1);
    if(b.ib)
      d->SetIndices(b.ib);
    if(c.instances > 1)
    {
      d->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | c.instances);
      d->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
    }
  }

  void Draw(IDirect3DDevice9 * d, const Case &c)
  {
    switch(c.kind)
    {
      case Case::Draw: d->DrawPrimitive(c.prim, c.start, c.primCount); break;
      case Case::Indexed:
      {
        UINT maxV = 0;
        for(uint16_t i : c.indices)
          maxV = std::max<UINT>(maxV, i);
        d->DrawIndexedPrimitive(c.prim, c.baseVertex, 0, maxV + 1, c.start, c.primCount);
        break;
      }
      case Case::UP: d->DrawPrimitiveUP(c.prim, c.primCount, c.verts.data(), c.stride); break;
      case Case::IndexedUP:
      {
        UINT maxV = 0;
        for(uint16_t i : c.indices)
          maxV = std::max<UINT>(maxV, i);
        d->DrawIndexedPrimitiveUP(c.prim, 0, maxV + 1, c.primCount, c.indices.data(),
                                  D3DFMT_INDEX16, c.verts.data(), c.stride);
        break;
      }
    }
  }

  // the destination of ProcessVertices: XYZRHW, PSIZE, DIFFUSE, SPECULAR and 8 4D coordinates
  struct PVVertex
  {
    float pos[4];
    float psize;
    D3DCOLOR diffuse, specular;
    float tex[8][4];
  };

  static const DWORD PVFVF =
      (DWORD)(D3DFVF_XYZRHW | D3DFVF_PSIZE | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX8 |
              D3DFVF_TEXCOORDSIZE4(0) | D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE4(2) |
              D3DFVF_TEXCOORDSIZE4(3) | D3DFVF_TEXCOORDSIZE4(4) | D3DFVF_TEXCOORDSIZE4(5) |
              D3DFVF_TEXCOORDSIZE4(6) | D3DFVF_TEXCOORDSIZE4(7));

  static std::vector<float> Color(D3DCOLOR c)
  {
    return {float((c >> 16) & 0xFF) / 255.0f, float((c >> 8) & 0xFF) / 255.0f,
            float(c & 0xFF) / 255.0f, float(c >> 24) / 255.0f};
  }

  // writes a draw's reference: ProcessVertices on the software device, or the case's own
  void WriteReference(FILE * f, IDirect3DDevice9 * sw, const Case &c)
  {
    std::vector<UINT> rows = Rows(c);
    std::vector<std::array<float, 2>> screen;
    fprintf(f, "draw %s\n", c.name.c_str());

    if(c.pvOutputs.empty())
    {
      fprintf(f, "space clip\n");
      for(UINT inst = 0; inst < c.instances; inst++)
      {
        for(size_t r = 0; r < rows.size(); r++)
        {
          Outputs out;
          c.reference(inst, rows[r], out);
          fprintf(f, "row %u %u", inst, (UINT)r);
          for(auto it = out.begin(); it != out.end(); ++it)
          {
            fprintf(f, " %s=", it->first.c_str());
            for(size_t i = 0; i < it->second.size(); i++)
              fprintf(f, "%s%.9g", i ? "," : "", it->second[i]);
          }
          fprintf(f, "\n");
        }
      }
      WriteHardwareOutputs(f, c, rows);
      fprintf(f, "end\n");
      return;
    }

    fprintf(f, "space screen\n");

    Bound b;
    if(!sw || !Bind(sw, c, true, b))
    {
      TEST_WARN("Couldn't set up %s on the software vertex processing device", c.name.c_str());
      fprintf(f, "error couldn't set up the draw for ProcessVertices\nend\n");
      return;
    }
    Apply(sw, c, b);

    UINT minV = ~0U, maxV = 0;
    for(UINT v : rows)
    {
      minV = std::min(minV, v);
      maxV = std::max(maxV, v);
    }
    const UINT count = maxV - minV + 1;

    IDirect3DVertexBuffer9Ptr dest;
    HRESULT hr = sw->CreateVertexBuffer(count * sizeof(PVVertex), D3DUSAGE_SOFTWAREPROCESSING,
                                        PVFVF, D3DPOOL_SYSTEMMEM, &dest, NULL);
    if(SUCCEEDED(hr))
    {
      void *p = NULL;
      dest->Lock(0, 0, &p, 0);
      memset(p, 0, count * sizeof(PVVertex));
      dest->Unlock();
      hr = sw->ProcessVertices(minV, 0, count, dest, NULL, 0);
    }
    if(FAILED(hr))
    {
      TEST_WARN("ProcessVertices failed for %s: %x", c.name.c_str(), hr);
      fprintf(f, "error ProcessVertices failed %x\nend\n", hr);
      return;
    }

    PVVertex *pv = NULL;
    dest->Lock(0, 0, (void **)&pv, D3DLOCK_READONLY);
    for(size_t r = 0; r < rows.size(); r++)
    {
      const PVVertex &v = pv[rows[r] - minV];
      screen.push_back({v.pos[0], v.pos[1]});
      Outputs out;
      out["POSITION.0"] = {v.pos[0], v.pos[1], v.pos[2], v.pos[3]};
      for(const std::string &o : c.pvOutputs)
      {
        if(o == "COLOR.0")
          out[o] = Color(v.diffuse);
        else if(o == "COLOR.1")
        {
          // the alpha is the fog
          std::vector<float> s = Color(v.specular);
          s.resize(3);
          out[o] = s;
        }
        else if(o == "FOG.0")
          out[o] = {Color(v.specular)[3]};
        else if(o == "PSIZE.0")
          out[o] = {v.psize};
        else if(o.compare(0, 9, "TEXCOORD.") == 0)
        {
          int t = atoi(o.c_str() + 9);
          out[o] = {v.tex[t][0], v.tex[t][1], v.tex[t][2], v.tex[t][3]};
        }
      }
      fprintf(f, "row 0 %u", (UINT)r);
      for(auto it = out.begin(); it != out.end(); ++it)
      {
        fprintf(f, " %s=", it->first.c_str());
        for(size_t i = 0; i < it->second.size(); i++)
          fprintf(f, "%s%.9g", i ? "," : "", it->second[i]);
      }
      fprintf(f, "\n");
    }
    dest->Unlock();
    WriteHardware(f, c, rows, screen);
    fprintf(f, "end\n");
  }

  // a render target to draw single vertices into, and a copy of it to read
  IDirect3DSurface9Ptr hwTarget, hwRead;

  struct Pixel
  {
    int x, y;
    DWORD c;
  };

  // draws vertex v of a case as a point on its own, with extra state, and returns the pixels it
  // covers. The extra state has to make the alpha 1, which tells them from the cleared ones.
  std::vector<Pixel> DrawVertex(const Case &c, const Bound &b, UINT v,
                                std::function<void(IDirect3DDevice9 *)> extra)
  {
    std::vector<Pixel> ret;
    IDirect3DSurface9Ptr old;
    dev->GetRenderTarget(0, &old);
    dev->ColorFill(hwTarget, NULL, 0);
    dev->SetRenderTarget(0, hwTarget);
    Apply(dev, c, b);
    extra(dev);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->BeginScene();
    dev->DrawPrimitive(D3DPT_POINTLIST, v, 1);
    dev->EndScene();
    dev->SetRenderTarget(0, old);

    D3DLOCKED_RECT lr = {};
    if(FAILED(dev->GetRenderTargetData(hwTarget, hwRead)) ||
       FAILED(hwRead->LockRect(&lr, NULL, D3DLOCK_READONLY)))
      return ret;
    for(int y = 0; y < screenHeight; y++)
    {
      const DWORD *row = (const DWORD *)((const byte *)lr.pBits + y * lr.Pitch);
      for(int x = 0; x < screenWidth; x++)
        if((row[x] >> 24) == 0xFF)
          ret.push_back({x, y, row[x]});
    }
    hwRead->UnlockRect();
    return ret;
  }

  // what the GPU draws for the vertices of lit draws (their colours) and scaled points (their
  // sizes), each vertex drawn as a point on its own. ProcessVertices is the runtime's software
  // vertex processing, so this is what the mesh output is checked against where they differ.
  void WriteHardware(FILE * f, const Case &c, const std::vector<UINT> &rows,
                     const std::vector<std::array<float, 2>> &screen)
  {
    if(!hwTarget || c.kind == Case::UP || c.kind == Case::IndexedUP || c.instances > 1 ||
       !c.vsTokens.empty())
      return;

    Bound b;
    if(!Bind(dev, c, false, b))
      return;

    DWORD lighting = FALSE, pointScale = FALSE;
    Apply(dev, c, b);
    dev->GetRenderState(D3DRS_LIGHTING, &lighting);
    dev->GetRenderState(D3DRS_POINTSCALEENABLE, &pointScale);
    const bool points = c.prim == D3DPT_POINTLIST && pointScale;
    if(!lighting && !points)
      return;

    auto colourOf = [](DWORD p) {
      return std::vector<float>{float((p >> 16) & 0xFF) / 255.0f, float((p >> 8) & 0xFF) / 255.0f,
                                float(p & 0xFF) / 255.0f};
    };

    for(size_t r = 0; r < rows.size(); r++)
    {
      Outputs out;
      if(lighting)
      {
        // the colour comes from one stage 0 argument, with a 1x1 point
        auto pass = [&](DWORD arg, BOOL specular) {
          return DrawVertex(c, b, rows[r], [arg, specular](IDirect3DDevice9 *d) {
            float one = 1.0f;
            d->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);
            d->SetRenderState(D3DRS_POINTSIZE, *(DWORD *)&one);
            d->SetRenderState(D3DRS_POINTSIZE_MIN, *(DWORD *)&one);
            d->SetRenderState(D3DRS_SPECULARENABLE, specular);
            d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF000000);
            d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            d->SetTextureStageState(0, D3DTSS_COLORARG1, arg);
          });
        };
        std::vector<Pixel> diffuse = pass(D3DTA_DIFFUSE, FALSE);
        std::vector<Pixel> alpha = pass(D3DTA_DIFFUSE | D3DTA_ALPHAREPLICATE, FALSE);
        // black from the texture factor, and the specular colour added after the stages
        std::vector<Pixel> specular = pass(D3DTA_TFACTOR, TRUE);
        if(diffuse.size() == 1 && alpha.size() == 1 && specular.size() == 1)
        {
          out["COLOR.0"] = colourOf(diffuse[0].c);
          out["COLOR.0"].push_back(colourOf(alpha[0].c)[0]);
          out["COLOR.1"] = colourOf(specular[0].c);
        }
        else
        {
          TEST_WARN("%s row %u: the point at (%.1f, %.1f) covers %u, %u and %u pixels, not 1",
                    c.name.c_str(), (UINT)r, screen[r][0], screen[r][1], (UINT)diffuse.size(),
                    (UINT)alpha.size(), (UINT)specular.size());
        }
      }
      if(points)
      {
        std::vector<Pixel> px = DrawVertex(c, b, rows[r], [](IDirect3DDevice9 *d) {
          d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
          d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
          d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        });
        if(!px.empty())
        {
          int x0 = px[0].x, x1 = px[0].x, y0 = px[0].y, y1 = px[0].y;
          for(const Pixel &p : px)
          {
            x0 = std::min(x0, p.x);
            x1 = std::max(x1, p.x);
            y0 = std::min(y0, p.y);
            y1 = std::max(y1, p.y);
          }
          out["PSIZE.0"] = {float(std::max(x1 - x0 + 1, y1 - y0 + 1))};
        }
      }
      if(out.empty())
        continue;
      fprintf(f, "hw 0 %u", (UINT)r);
      for(auto it = out.begin(); it != out.end(); ++it)
      {
        fprintf(f, " %s=", it->first.c_str());
        for(size_t i = 0; i < it->second.size(); i++)
          fprintf(f, "%s%.9g", i ? "," : "", it->second[i]);
      }
      fprintf(f, "\n");
    }
  }

  pD3DAssemble assemble = NULL;

  std::vector<DWORD> Assemble(const std::string &src)
  {
    ID3DBlobPtr code, errors;
    if(!assemble || FAILED(assemble(src.c_str(), src.size(), NULL, NULL, NULL, 0, &code, &errors)))
    {
      TEST_ERROR("Couldn't assemble:\n%s\n%s", src.c_str(),
                 errors ? (const char *)errors->GetBufferPointer() : "");
      return {};
    }
    const DWORD *p = (const DWORD *)code->GetBufferPointer();
    return std::vector<DWORD>(p, p + code->GetBufferSize() / 4);
  }

  // A reference for texldl with filtering, following what the GPU's hardware vertex processing
  // was measured to do (see the D3D9 driver's STATUS.md, "texldl filtering"). The GPU's own
  // arithmetic isn't reproduced exactly (its weights vary by a 256th or two), so its values are
  // compared with a tolerance.
  struct RefTexture
  {
    enum Kind
    {
      Tex2D,
      Cube,
      Volume,
    } kind = Tex2D;
    struct Level
    {
      int w = 1, h = 1, d = 1;
      // x fastest, then y, then the slice or face, as the sampler reads them
      std::vector<std::array<float, 4>> texels;
    };
    std::vector<Level> levels;
    DWORD address[3] = {D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP};
    float border[4] = {};
    DWORD mag = D3DTEXF_POINT, min = D3DTEXF_POINT, mip = D3DTEXF_NONE;
    DWORD maxMip = 0;
    float bias = 0.0f;
  };

  static int RefAddress(int i, int size, DWORD mode)
  {
    switch(mode)
    {
      case D3DTADDRESS_WRAP: i %= size; return i < 0 ? i + size : i;
      case D3DTADDRESS_MIRROR:
      {
        int p = i % (2 * size);
        if(p < 0)
          p += 2 * size;
        return p < size ? p : 2 * size - 1 - p;
      }
      case D3DTADDRESS_MIRRORONCE:
        if(i < 0)
          i = -i - 1;
        return i < size ? i : size - 1;
      case D3DTADDRESS_BORDER: return i < 0 || i >= size ? -1 : i;
      default: return i < 0 ? 0 : (i >= size ? size - 1 : i);
    }
  }

  // the texels along one axis and their weights; -1 is the border colour
  static int RefTaps(float u, int size, DWORD mode, bool linear, int idx[2], float w[2])
  {
    if(!linear)
    {
      float a = u;
      if(mode == D3DTADDRESS_WRAP)
      {
        a = u - floorf(u);
      }
      else if(mode == D3DTADDRESS_MIRROR)
      {
        float t = u - 2.0f * floorf(u * 0.5f);
        a = t > 1.0f ? 2.0f - t : t;
      }
      else if(mode == D3DTADDRESS_MIRRORONCE)
      {
        a = fabsf(u) < 1.0f ? fabsf(u) : 1.0f;
      }
      else if(mode != D3DTADDRESS_BORDER)
      {
        a = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
      }
      int i = (int)floorf(a * float(size));
      if(mode == D3DTADDRESS_BORDER && (i < 0 || i >= size))
        idx[0] = -1;
      else
        idx[0] = i < 0 ? 0 : (i >= size ? size - 1 : i);
      w[0] = 1.0f;
      return 1;
    }

    // texel centres are at half texels, and the weight keeps 8 bits, rounded to the nearest
    const float p = u * float(size) - 0.5f;
    const float base = floorf(p);
    const float f = floorf((p - base) * 256.0f + 0.5f) / 256.0f;
    idx[0] = RefAddress((int)base, size, mode);
    idx[1] = RefAddress((int)base + 1, size, mode);
    w[0] = 1.0f - f;
    w[1] = f;
    return 2;
  }

  static std::array<float, 4> RefSampleLevel(const RefTexture &t, int level, const float *c,
                                             bool linear)
  {
    const RefTexture::Level &l = t.levels[level];
    int idx[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    float w[3][2] = {{1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}};
    int n[3] = {1, 1, 1};

    if(t.kind == RefTexture::Cube)
    {
      // the largest axis picks the face, z then y then x when two are as long, and the face's
      // edges clamp
      const float ax = fabsf(c[0]), ay = fabsf(c[1]), az = fabsf(c[2]);
      int face;
      float sc, tc, ma;
      if(az >= ax && az >= ay)
      {
        face = c[2] >= 0.0f ? 4 : 5;
        sc = c[2] >= 0.0f ? c[0] : -c[0];
        tc = -c[1];
        ma = az;
      }
      else if(ay >= ax)
      {
        face = c[1] >= 0.0f ? 2 : 3;
        sc = c[0];
        tc = c[1] >= 0.0f ? c[2] : -c[2];
        ma = ay;
      }
      else
      {
        face = c[0] >= 0.0f ? 0 : 1;
        sc = c[0] >= 0.0f ? -c[2] : c[2];
        tc = -c[1];
        ma = ax;
      }
      n[0] = RefTaps((sc / ma + 1.0f) * 0.5f, l.w, D3DTADDRESS_CLAMP, linear, idx[0], w[0]);
      n[1] = RefTaps((tc / ma + 1.0f) * 0.5f, l.h, D3DTADDRESS_CLAMP, linear, idx[1], w[1]);
      idx[2][0] = face;
    }
    else
    {
      n[0] = RefTaps(c[0], l.w, t.address[0], linear, idx[0], w[0]);
      n[1] = RefTaps(c[1], l.h, t.address[1], linear, idx[1], w[1]);
      if(t.kind == RefTexture::Volume)
        n[2] = RefTaps(c[2], l.d, t.address[2], linear, idx[2], w[2]);
    }

    std::array<float, 4> ret = {};
    for(int z = 0; z < n[2]; z++)
      for(int y = 0; y < n[1]; y++)
        for(int x = 0; x < n[0]; x++)
        {
          const float wt = w[0][x] * w[1][y] * w[2][z];
          const int tx = idx[0][x], ty = idx[1][y], tz = idx[2][z];
          const float *v = t.border;
          if(tx >= 0 && ty >= 0 && tz >= 0)
            v = t.levels[level].texels[(tz * l.h + ty) * l.w + tx].data();
          for(int k = 0; k < 4; k++)
            ret[k] += wt * v[k];
        }
    return ret;
  }

  static std::array<float, 4> RefSample(const RefTexture &t, const float *c)
  {
    // the LOD keeps 8 bits of fraction, rounded down
    float lod = c[3] + t.bias;
    lod = floorf(lod * 256.0f) / 256.0f;
    const auto linear = [](DWORD f) { return f > D3DTEXF_POINT; };

    // the LOD clamped to MAXMIPLEVEL picks the filter
    const int last = (int)t.levels.size() - 1;
    const float lo = float((int)t.maxMip < last ? (int)t.maxMip : last);
    lod = lod < lo ? lo : (lod > float(last) ? float(last) : lod);
    const bool lin = linear(lod <= 0.0f ? t.mag : t.min);

    // without a mip filter it's level 0 whatever MAXMIPLEVEL is
    if(t.mip == D3DTEXF_NONE)
      return RefSampleLevel(t, 0, c, lin);

    if(t.mip == D3DTEXF_POINT)
      return RefSampleLevel(t, (int)floorf(lod + 0.5f), c, lin);

    const int l0 = (int)floorf(lod);
    const float f = lod - float(l0);
    std::array<float, 4> a = RefSampleLevel(t, l0, c, lin);
    if(f > 0.0f && l0 < last)
    {
      std::array<float, 4> b = RefSampleLevel(t, l0 + 1, c, lin);
      for(int k = 0; k < 4; k++)
        a[k] = a[k] * (1.0f - f) + b[k] * f;
    }
    return a;
  }

  // a 1x1 float render target for WriteHardwareOutputs, and a copy of it to read
  IDirect3DSurface9Ptr hwFloatTarget, hwFloatRead;

  // draws vertex v of a case as a point into hwFloatTarget, with a ps_3_0 that has the
  // declarations dcl (e.g. "dcl_texcoord1 v0.xy") and writes v0's mask to the same components,
  // zero elsewhere
  bool ReadVertexOutput(const Case &c, const Bound &b, UINT v, const std::string &dcl,
                        const char *mask, float out[4])
  {
    std::vector<DWORD> tokens = Assemble("ps_3_0\ndef c0, 0, 0, 0, 0\n" + dcl +
                                         "\nmov r0, c0\nmov r0." + mask + ", v0\nmov oC0, r0\n");
    IDirect3DPixelShader9Ptr ps;
    if(tokens.empty() || FAILED(dev->CreatePixelShader(tokens.data(), &ps)))
      return false;

    IDirect3DSurface9Ptr old;
    dev->GetRenderTarget(0, &old);
    dev->SetRenderTarget(0, hwFloatTarget);
    // something no output here gives
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(7, 7, 7, 7), 1.0f, 0);
    Apply(dev, c, b);
    D3DVIEWPORT9 vp = {0, 0, 1, 1, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetPixelShader(ps);
    // a point big enough to cover the one pixel wherever the vertex is in the viewport
    float size = 4.0f, one = 1.0f;
    dev->SetRenderState(D3DRS_POINTSIZE, *(DWORD *)&size);
    dev->SetRenderState(D3DRS_POINTSIZE_MIN, *(DWORD *)&one);
    dev->BeginScene();
    dev->DrawPrimitive(D3DPT_POINTLIST, v, 1);
    dev->EndScene();
    dev->SetRenderTarget(0, old);
    dev->SetPixelShader(NULL);

    D3DLOCKED_RECT lr = {};
    if(FAILED(dev->GetRenderTargetData(hwFloatTarget, hwFloatRead)) ||
       FAILED(hwFloatRead->LockRect(&lr, NULL, D3DLOCK_READONLY)))
      return false;
    // A32B32G32R32F is stored R, G, B, A
    memcpy(out, lr.pBits, sizeof(float) * 4);
    hwFloatRead->UnlockRect();
    return true;
  }

  // the outputs of a vs_3_0 case as the GPU gives them to a pixel shader that declares its inputs
  // as the vertex shader declares its outputs
  void WriteHardwareOutputs(FILE * f, const Case &c, const std::vector<UINT> &rows)
  {
    if(c.hwRegisters.empty())
      return;

    Bound b;
    if(!hwFloatTarget || c.kind != Case::Indexed || c.instances > 1 || !Bind(dev, c, false, b))
    {
      TEST_ERROR("Couldn't read %s's outputs back from the GPU", c.name.c_str());
      return;
    }

    if(c.hwTolerance > 0.0f)
      fprintf(f, "hwfloat %g\n", c.hwTolerance);
    else
      fprintf(f, "hwfloat\n");
    for(size_t r = 0; r < rows.size(); r++)
    {
      fprintf(f, "hw 0 %u", (UINT)r);
      for(const Case::HWRegister &reg : c.hwRegisters)
      {
        float val[4] = {};
        if(!ReadVertexOutput(c, b, rows[r], reg.psDcls, "xyzw", val))
        {
          TEST_ERROR("Couldn't read %s back from the GPU", reg.psDcls.c_str());
          continue;
        }
        for(const auto &sem : reg.semantics)
        {
          fprintf(f, " %s=", sem.first.c_str());
          for(size_t i = 0; i < sem.second.size(); i++)
            fprintf(f, "%s%.9g", i ? "," : "", val[strchr("xyzw", sem.second[i]) - "xyzw"]);
        }
      }
      fprintf(f, "\n");
    }
  }

  // vs_1_1 tokens: register types, opcodes and parameters
  enum
  {
    RTemp = 0,
    RInput = 1,
    RConst = 2,
    RAddr = 3,
    RRast = 4,
    RAttr = 5,
    RTex = 6,
  };

  static DWORD Reg(DWORD type, DWORD idx)
  {
    return 0x80000000 | (type << 28) | idx;
  }
  static DWORD Dst(DWORD type, DWORD idx, DWORD mask = 0xF)
  {
    return Reg(type, idx) | (mask << 16);
  }
  static DWORD Src(DWORD type, DWORD idx, DWORD swz = 0xE4)
  {
    return Reg(type, idx) | (swz << 16);
  }

  // a vs_1_1 token stream, with where each instruction starts, to find which one the runtime
  // rejects
  struct Tokens
  {
    std::vector<DWORD> t;
    std::vector<size_t> starts;
    std::vector<std::string> names;
  };

  static Tokens VS11Tokens()
  {
    const DWORD X = 0x00, Y = 0x55, Z = 0xAA;
    Tokens ret;
    std::vector<DWORD> &t = ret.t;
    t.push_back(0xFFFE0101);
    auto op = [&ret](const char *name, DWORD opcode, std::initializer_list<DWORD> params) {
      ret.starts.push_back(ret.t.size());
      ret.names.push_back(name);
      ret.t.push_back(opcode);
      ret.t.insert(ret.t.end(), params.begin(), params.end());
    };
    op("dcl_position v0", 31, {0x80000000 | 0, Dst(RInput, 0)});
    op("dcl_color v1", 31, {0x80000000 | 10, Dst(RInput, 1)});
    op("dcl_texcoord v2", 31, {0x80000000 | 5, Dst(RInput, 2)});
    // c0-c3: the transposed world view projection
    op("m4x4 oPos, v0, c0", 20, {Dst(RRast, 0), Src(RInput, 0), Src(RConst, 0)});
    // a0.x rounds down. Another instruction comes before a0 is read.
    op("mov a0.x, v2.x", 1, {Dst(RAddr, 0, 0x1), Src(RInput, 2, X)});
    op("mov oD0, v1", 1, {Dst(RAttr, 0), Src(RInput, 1)});
    op("mov oT0, c5[a0.x]", 1, {Dst(RTex, 0), Src(RConst, 5) | 0x2000});
    // expp, logp, lit and dst write temporaries, which are then moved to the outputs
    op("expp r3, v2.y", 78, {Dst(RTemp, 3), Src(RInput, 2, Y)});
    op("mov oT1, r3", 1, {Dst(RTex, 1), Src(RTemp, 3)});
    op("logp r4, v2.y", 79, {Dst(RTemp, 4), Src(RInput, 2, Y)});
    op("mov oT2, r4", 1, {Dst(RTex, 2), Src(RTemp, 4)});
    op("lit r5, c8", 16, {Dst(RTemp, 5), Src(RConst, 8)});
    op("mov oT3, r5", 1, {Dst(RTex, 3), Src(RTemp, 5)});
    // vs_1_1 reads one constant per instruction
    op("mov r1, c9", 1, {Dst(RTemp, 1), Src(RConst, 9)});
    op("dst r6, r1, c10", 17, {Dst(RTemp, 6), Src(RTemp, 1), Src(RConst, 10)});
    op("mov oT4, r6", 1, {Dst(RTex, 4), Src(RTemp, 6)});
    op("frc r0.xy, v0", 19, {Dst(RTemp, 0, 0x3), Src(RInput, 0)});
    op("rcp r0.z, v2.z", 6, {Dst(RTemp, 0, 0x4), Src(RInput, 2, Z)});
    op("rsq r0.w, v2.z", 7, {Dst(RTemp, 0, 0x8), Src(RInput, 2, Z)});
    op("mov oT5, r0", 1, {Dst(RTex, 5), Src(RTemp, 0)});
    // oFog is written with the full mask and a replicated source, as fxc writes it. The .x mask
    // alone is rejected.
    op("mov oFog, v2.z", 1, {Dst(RRast, 1), Src(RInput, 2, Z)});
    op("mov r2, c12", 1, {Dst(RTemp, 2), Src(RConst, 12)});
    op("mad oD1, v1, c11, r2", 4, {Dst(RAttr, 1), Src(RInput, 1), Src(RConst, 11), Src(RTemp, 2)});
    t.push_back(0x0000FFFF);
    return ret;
  }

  std::vector<DWORD> VS11()
  {
    Tokens tokens = VS11Tokens();

    IDirect3DVertexShader9Ptr vs;
    if(SUCCEEDED(dev->CreateVertexShader(tokens.t.data(), &vs)))
      return tokens.t;

    // find the first instruction the runtime rejects: every shader has to write oPos, so the
    // shortest one tried ends at the m4x4
    for(size_t n = 4; n <= tokens.starts.size(); n++)
    {
      std::vector<DWORD> prefix(
          tokens.t.begin(),
          tokens.t.begin() + (n < tokens.starts.size() ? tokens.starts[n] : tokens.t.size() - 1));
      prefix.push_back(0x0000FFFF);
      vs = NULL;
      if(FAILED(dev->CreateVertexShader(prefix.data(), &vs)))
      {
        TEST_ERROR("The vs_1_1 shader is rejected at instruction %u, '%s'", (uint32_t)n,
                   tokens.names[n - 1].c_str());
        return {};
      }
    }
    TEST_ERROR("The vs_1_1 shader is rejected, but every part of it is accepted");
    return {};
  }

  std::vector<DWORD> CompileVS(const char *src, const char *profile)
  {
    ID3DBlobPtr blob = Compile(src, "main", profile);
    if(!blob)
      return {};
    const DWORD *p = (const DWORD *)blob->GetBufferPointer();
    return std::vector<DWORD>(p, p + blob->GetBufferSize() / 4);
  }

  int main()
  {
    if(!Init())
      return 3;

    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    assemble = d3dcompiler ? (pD3DAssemble)GetProcAddress(d3dcompiler, "D3DAssemble") : NULL;
    if(!assemble)
      TEST_ERROR("Couldn't load D3DAssemble, leaving out the assembled shaders");

    const float W = (float)screenWidth, H = (float)screenHeight;
    const M view = Translate(0.0f, 0.0f, 5.0f);
    const M proj = Perspective(1.0471976f, W / H, 1.0f, 20.0f);
    const M viewProj = Mul(view, proj);

    D3DCAPS9 caps = {};
    dev->GetDeviceCaps(&caps);

    std::vector<Case> cases;

    // a 2x2 grid of quads with normals facing the camera
    struct PNCT
    {
      float pos[3];
      float nrm[3];
      D3DCOLOR col;
      float uv[2];
    };
    std::vector<byte> grid;
    for(int y = 0; y < 3; y++)
    {
      for(int x = 0; x < 3; x++)
      {
        float fx = float(x) - 1.0f, fy = float(y) - 1.0f;
        float n[3] = {fx * 0.3f, fy * 0.3f, -1.0f};
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + 1.0f);
        PNCT v = {{fx, fy, fx * 0.25f},
                  {n[0] / l, n[1] / l, n[2] / l},
                  D3DCOLOR_ARGB(200, 50 + x * 80, 255 - y * 100, 128),
                  {x * 0.5f, y * 0.5f}};
        Append(grid, v);
      }
    }
    std::vector<uint16_t> gridIdx;
    for(uint16_t y = 0; y < 2; y++)
    {
      for(uint16_t x = 0; x < 2; x++)
      {
        uint16_t i = y * 3 + x;
        for(uint16_t k :
            {i, uint16_t(i + 3), uint16_t(i + 1), uint16_t(i + 1), uint16_t(i + 3), uint16_t(i + 4)})
          gridIdx.push_back(k);
      }
    }

    // fixed function, transform only, with DrawPrimitive from vertex 3
    {
      Case c;
      c.name = "FFP transform";
      c.verts = grid;
      c.stride = sizeof(PNCT);
      c.fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
      c.kind = Case::Draw;
      c.prim = D3DPT_TRIANGLESTRIP;
      c.start = 3;
      c.primCount = 4;
      c.state = [view, proj](IDirect3DDevice9 *d) {
        d->SetTransform(D3DTS_WORLD, Mul(RotY(0.3f), Translate(0.5f, 0.0f, 1.0f)));
        d->SetTransform(D3DTS_VIEW, view);
        d->SetTransform(D3DTS_PROJECTION, proj);
      };
      c.pvOutputs = {"COLOR.0", "TEXCOORD.0"};
      cases.push_back(c);
    }

    // lights, indexed. Each uses all of the grid.
    auto lit = [&](const char *name, D3DLIGHT9 light, bool localViewer,
                   std::function<void(IDirect3DDevice9 *)> extra) {
      Case c;
      c.name = name;
      c.verts = grid;
      c.stride = sizeof(PNCT);
      c.fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
      c.indices = gridIdx;
      c.kind = Case::Indexed;
      c.primCount = 8;
      c.state = [view, proj, light, localViewer, extra](IDirect3DDevice9 *d) {
        d->SetTransform(D3DTS_WORLD, Translate(0.0f, 0.0f, 0.5f));
        d->SetTransform(D3DTS_VIEW, view);
        d->SetTransform(D3DTS_PROJECTION, proj);
        d->SetRenderState(D3DRS_LIGHTING, TRUE);
        d->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
        d->SetRenderState(D3DRS_LOCALVIEWER, localViewer);
        d->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(30, 20, 10));
        D3DMATERIAL9 mat = {};
        mat.Diffuse = {0.8f, 0.6f, 0.4f, 0.9f};
        mat.Ambient = {0.5f, 0.5f, 0.5f, 1.0f};
        mat.Specular = {0.7f, 0.7f, 0.7f, 1.0f};
        mat.Emissive = {0.05f, 0.0f, 0.1f, 1.0f};
        mat.Power = 8.0f;
        d->SetMaterial(&mat);
        d->SetLight(0, &light);
        d->LightEnable(0, TRUE);
        if(extra)
          extra(d);
      };
      c.pvOutputs = {"COLOR.0", "COLOR.1"};
      cases.push_back(c);
    };

    {
      D3DLIGHT9 l = {};
      l.Type = D3DLIGHT_DIRECTIONAL;
      l.Diffuse = {1.0f, 0.9f, 0.8f, 1.0f};
      l.Specular = {1.0f, 1.0f, 1.0f, 1.0f};
      l.Ambient = {0.1f, 0.1f, 0.2f, 1.0f};
      l.Direction = {0.3f, -0.4f, 1.0f};
      // the diffuse colour from the vertex
      lit("FFP directional light", l, false, NULL);

      D3DLIGHT9 p = {};
      p.Type = D3DLIGHT_POINT;
      p.Diffuse = {0.2f, 1.0f, 0.5f, 1.0f};
      p.Specular = {1.0f, 0.5f, 0.5f, 1.0f};
      p.Position = {0.5f, 0.5f, -1.5f};
      p.Range = 3.5f;
      p.Attenuation0 = 0.5f;
      p.Attenuation1 = 0.2f;
      p.Attenuation2 = 0.05f;
      // the material's colours, and the local viewer
      lit("FFP point light", p, true, [](IDirect3DDevice9 *d) {
        d->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
      });

      D3DLIGHT9 s = {};
      s.Type = D3DLIGHT_SPOT;
      s.Diffuse = {1.0f, 1.0f, 0.3f, 1.0f};
      s.Specular = {0.5f, 0.5f, 1.0f, 1.0f};
      s.Ambient = {0.2f, 0.0f, 0.0f, 1.0f};
      s.Position = {0.0f, 0.0f, -2.0f};
      s.Direction = {0.1f, 0.05f, 1.0f};
      s.Range = 10.0f;
      s.Attenuation0 = 1.0f;
      s.Theta = 0.4f;
      s.Phi = 1.2f;
      s.Falloff = 1.5f;
      // ambient from the vertex colour, normals normalized
      lit("FFP spot light", s, true, [](IDirect3DDevice9 *d) {
        d->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
        d->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
      });
    }

    // texture coordinate generation and transforms, with range fog
    {
      Case c;
      c.name = "FFP texgen and fog";
      c.verts = grid;
      c.stride = sizeof(PNCT);
      c.fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
      c.indices = gridIdx;
      c.kind = Case::Indexed;
      c.primCount = 8;
      c.state = [view, proj](IDirect3DDevice9 *d) {
        d->SetTransform(D3DTS_WORLD, RotY(-0.4f));
        d->SetTransform(D3DTS_VIEW, view);
        d->SetTransform(D3DTS_PROJECTION, proj);
        d->SetRenderState(D3DRS_LOCALVIEWER, FALSE);
        d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        d->SetTransform(D3DTS_TEXTURE0, Scale(0.5f, 0.25f, 2.0f));
        // set 0 with a translation (a 2D coordinate's is the third row), projected
        M t = Scale(2.0f, 3.0f, 1.0f);
        t.m[8] = 0.25f;
        t.m[9] = -0.5f;
        t.m[10] = 2.0f;
        d->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU | 0);
        d->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3 | D3DTTFF_PROJECTED);
        d->SetTransform(D3DTS_TEXTURE1, t);
        d->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL);
        d->SetTextureStageState(3, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_SPHEREMAP);
        d->SetTextureStageState(4, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
        // stages after a disabled one output no texture coordinate, so 1-4 pass the colour on
        for(DWORD i = 1; i <= 4; i++)
        {
          d->SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
          d->SetTextureStageState(i, D3DTSS_COLORARG1, D3DTA_CURRENT);
          d->SetTextureStageState(i, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
          d->SetTextureStageState(i, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
        }
        d->SetRenderState(D3DRS_FOGENABLE, TRUE);
        d->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
        d->SetRenderState(D3DRS_RANGEFOGENABLE, TRUE);
        float start = 3.0f, end = 7.0f;
        d->SetRenderState(D3DRS_FOGSTART, *(DWORD *)&start);
        d->SetRenderState(D3DRS_FOGEND, *(DWORD *)&end);
      };
      c.pvOutputs = {"COLOR.0",    "TEXCOORD.0", "TEXCOORD.1", "TEXCOORD.2",
                     "TEXCOORD.3", "TEXCOORD.4", "FOG.0"};
      cases.push_back(c);
    }

    // points with scaled sizes and exponential fog
    {
      struct PS
      {
        float pos[3];
        float size;
      };
      Case c;
      c.name = "FFP point size";
      for(int i = 0; i < 6; i++)
        Append(c.verts, PS{{float(i) * 0.4f - 1.0f, 0.5f, float(i) * 0.8f}, 2.0f + float(i)});
      c.stride = sizeof(PS);
      c.fvf = D3DFVF_XYZ | D3DFVF_PSIZE;
      c.prim = D3DPT_POINTLIST;
      c.primCount = 6;
      c.state = [view, proj](IDirect3DDevice9 *d) {
        d->SetTransform(D3DTS_VIEW, view);
        d->SetTransform(D3DTS_PROJECTION, proj);
        d->SetRenderState(D3DRS_POINTSCALEENABLE, TRUE);
        // with the 400x300 window the sizes are about 8.3, 10.9, 12.8, 14.3, 15 and 15.8, so
        // the first is clamped to the minimum and the last three to the maximum
        float a = 1.0f, b = 2.0f, cc = 200.0f, mn = 9.0f, mx = 14.0f;
        d->SetRenderState(D3DRS_POINTSCALE_A, *(DWORD *)&a);
        d->SetRenderState(D3DRS_POINTSCALE_B, *(DWORD *)&b);
        d->SetRenderState(D3DRS_POINTSCALE_C, *(DWORD *)&cc);
        d->SetRenderState(D3DRS_POINTSIZE_MIN, *(DWORD *)&mn);
        d->SetRenderState(D3DRS_POINTSIZE_MAX, *(DWORD *)&mx);
        d->SetRenderState(D3DRS_FOGENABLE, TRUE);
        d->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_EXP);
        float density = 0.15f;
        d->SetRenderState(D3DRS_FOGDENSITY, *(DWORD *)&density);
      };
      // ProcessVertices doesn't write the point size: it's measured on the hardware instead
      c.pvOutputs = {"COLOR.0", "FOG.0"};
      cases.push_back(c);
    }

    // vertex blending with two weights (three matrices), and indexed if the device can
    {
      struct PB
      {
        float pos[3];
        float w[2];
        float nrm[3];
      };
      struct PBI
      {
        float pos[3];
        float w[2];
        DWORD idx;
        float nrm[3];
      };
      for(int indexed = 0; indexed < 2; indexed++)
      {
        if(indexed && caps.MaxVertexBlendMatrixIndex < 7)
        {
          TEST_LOG("Indexed vertex blending isn't supported (MaxVertexBlendMatrixIndex %u)",
                   caps.MaxVertexBlendMatrixIndex);
          continue;
        }
        Case c;
        c.name = indexed ? "FFP indexed vertex blending" : "FFP vertex blending";
        for(int i = 0; i < 6; i++)
        {
          float x = float(i % 3) - 1.0f, y = float(i / 3) - 0.5f;
          float w0 = float(i) / 6.0f, w1 = 0.5f - float(i) * 0.05f;
          float n[3] = {0.0f, 0.6f, -0.8f};
          if(indexed)
            Append(c.verts,
                   PBI{{x, y, 0.0f}, {w0, w1}, DWORD(0x00010702 + (i % 3)), {n[0], n[1], n[2]}});
          else
            Append(c.verts, PB{{x, y, 0.0f}, {w0, w1}, {n[0], n[1], n[2]}});
        }
        c.stride = indexed ? sizeof(PBI) : sizeof(PB);
        c.fvf = indexed ? (D3DFVF_XYZB3 | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_NORMAL)
                        : (D3DFVF_XYZB2 | D3DFVF_NORMAL);
        c.prim = D3DPT_TRIANGLESTRIP;
        c.primCount = 4;
        c.state = [view, proj, indexed](IDirect3DDevice9 *d) {
          d->SetTransform(D3DTS_VIEW, view);
          d->SetTransform(D3DTS_PROJECTION, proj);
          d->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_2WEIGHTS);
          d->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, indexed ? TRUE : FALSE);
          d->SetRenderState(D3DRS_LIGHTING, TRUE);
          D3DMATERIAL9 mat = {};
          mat.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
          d->SetMaterial(&mat);
          D3DLIGHT9 l = {};
          l.Type = D3DLIGHT_DIRECTIONAL;
          l.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
          l.Direction = {0.0f, 0.0f, 1.0f};
          d->SetLight(0, &l);
          d->LightEnable(0, TRUE);
          for(UINT m = 0; m < 8; m++)
            d->SetTransform(D3DTS_WORLDMATRIX(m),
                            Mul(RotY(float(m) * 0.2f),
                                Translate(float(m) * 0.1f - 0.3f, float(m % 3) * 0.2f, 0.5f)));
        };
        c.pvOutputs = {"COLOR.0"};
        cases.push_back(c);
      }
    }

    // pretransformed vertices, drawn with DrawPrimitiveUP. ProcessVertices doesn't take them, so
    // the reference is the input.
    {
      struct TV
      {
        float pos[4];
        D3DCOLOR col;
        float uv[2];
      };
      Case c;
      c.name = "FFP XYZRHW DrawPrimitiveUP";
      std::vector<TV> tv;
      for(int i = 0; i < 4; i++)
        tv.push_back({{W * (0.05f + 0.1f * (i & 1)), H * (0.7f + 0.1f * (i >> 1)), 0.25f * i,
                       1.0f / (1.0f + i)},
                      D3DCOLOR_ARGB(255, 60 * i, 255, 0),
                      {float(i), 0.5f}});
      for(const TV &v : tv)
        Append(c.verts, v);
      c.stride = sizeof(TV);
      c.fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
      c.kind = Case::UP;
      c.prim = D3DPT_TRIANGLESTRIP;
      c.primCount = 2;
      c.reference = [tv](UINT, UINT v, Outputs &out) {
        out["POSITIONT.0"] = {tv[v].pos[0], tv[v].pos[1], tv[v].pos[2], tv[v].pos[3]};
        out["COLOR.0"] = Color(tv[v].col);
        out["TEXCOORD.0"] = {tv[v].uv[0], tv[v].uv[1]};
      };
      cases.push_back(c);
    }

    // fixed function DrawIndexedPrimitiveUP
    {
      Case c;
      c.name = "FFP DrawIndexedPrimitiveUP";
      c.verts = grid;
      c.stride = sizeof(PNCT);
      c.fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
      c.indices = gridIdx;
      c.kind = Case::IndexedUP;
      c.primCount = 8;
      c.state = [viewProj](IDirect3DDevice9 *d) {
        d->SetTransform(D3DTS_WORLD, Scale(0.5f, 0.5f, 0.5f));
        d->SetTransform(D3DTS_PROJECTION, viewProj);
      };
      c.pvOutputs = {"COLOR.0", "TEXCOORD.0"};
      cases.push_back(c);
    }

    // vs_1_1, hand assembled, indexed with a base vertex and start index
    {
      struct V11
      {
        float pos[3];
        D3DCOLOR col;
        float t[3];
      };
      Case c;
      c.name = "vs_1_1";
      for(int i = 0; i < 2; i++)
        Append(c.verts, V11{{9.0f, 9.0f, 9.0f}, 0, {0, 0, 0}});
      for(int i = 0; i < 9; i++)
      {
        float x = float(i % 3) - 1.0f, y = float(i / 3) - 1.0f;
        Append(c.verts, V11{{x, y, 0.2f * x},
                            D3DCOLOR_ARGB(255, 25 * i, 100, 200),
                            {float(i % 3) + 0.3f, float(i) * 0.37f - 1.0f, 0.5f + float(i)}});
      }
      c.stride = sizeof(V11);
      c.decl = {
          {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
          {0, 16, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      };
      c.indices = {0, 0, 0};
      c.indices.insert(c.indices.end(), gridIdx.begin(), gridIdx.end());
      c.kind = Case::Indexed;
      c.start = 3;
      c.baseVertex = 2;
      c.primCount = 8;
      c.vsTokens = VS11();
      c.shader = true;
      M wvp = Transpose(Mul(RotY(0.2f), viewProj));
      c.state = [wvp](IDirect3DDevice9 *d) {
        d->SetVertexShaderConstantF(0, wvp.m, 4);
        const float consts[] = {
            1.0f, 0.0f,  0.0f,  1.0f,     // c5
            0.0f, 1.0f,  0.0f,  0.5f,     // c6
            0.0f, 0.0f,  1.0f,  0.25f,    // c7
            0.5f, 0.8f,  0.0f,  3.0f,     // c8: lit
            2.0f, 3.0f,  4.0f,  5.0f,     // c9: dst
            6.0f, 7.0f,  8.0f,  9.0f,     // c10
            0.5f, 0.25f, 2.0f,  1.0f,     // c11
            0.1f, 0.2f,  -0.3f, 0.0f,     // c12
        };
        d->SetVertexShaderConstantF(5, consts, 8);
      };
      c.pvOutputs = {"COLOR.0",    "COLOR.1",    "FOG.0",      "TEXCOORD.0", "TEXCOORD.1",
                     "TEXCOORD.2", "TEXCOORD.3", "TEXCOORD.4", "TEXCOORD.5"};
      cases.push_back(c);
    }

    // vs_2_0 with static flow control: a bool branch and a loop by an integer constant
    {
      Case c;
      c.name = "vs_2_0 flow control";
      c.verts = grid;
      c.stride = sizeof(PNCT);
      c.decl = {
          {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      };
      c.prim = D3DPT_TRIANGLESTRIP;
      c.primCount = 7;
      c.shader = true;
      c.vsTokens = CompileLoop(R"(
row_major float4x4 mvp : register(c0);
float4 tint[2] : register(c4);
bool useTint : register(b0);
bool other : register(b1);

struct VOut
{
  float4 pos : POSITION;
  float4 col : COLOR0;
  float4 uv : TEXCOORD0;
  float4 uv1 : TEXCOORD1;
};

VOut main(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
  VOut o;
  o.pos = mul(pos, mvp);
  float4 acc = 0;
  // 3 is the count the app sets in i0 (see CompileLoop). The loop counter isn't used in
  // arithmetic, which vs_2_0 can't do.
  float k = 1.0f;
  [loop] for(int i = 0; i < 3; i++)
  {
    acc += float4(uv, 0.25f, 0.5f) * k;
    k += 1.0f;
  }
  if(useTint)
    o.col = saturate(tint[0] + acc * 0.1f);
  else
    o.col = float4(1, 0, 1, 1);
  if(other)
    o.col.w = 0.25f;
  float s, cs;
  sincos(pos.x, s, cs);
  o.uv = float4(acc.xy, s, cs);
  o.uv1 = float4(rsqrt(abs(pos.y) + 1), log2(abs(pos.x) + 2), exp2(pos.y), frac(pos.x * 3.7f));
  return o;
}
)",
                               "vs_2_0");
      M wvp = viewProj;
      c.state = [wvp](IDirect3DDevice9 *d) {
        d->SetVertexShaderConstantF(0, wvp.m, 4);
        const float tint[] = {0.2f, 0.3f, 0.4f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        d->SetVertexShaderConstantF(4, tint, 2);
        const BOOL b[] = {TRUE, FALSE};
        d->SetVertexShaderConstantB(0, b, 2);
        const int i[] = {3, 0, 1, 0};
        d->SetVertexShaderConstantI(0, i, 1);
      };
      c.pvOutputs = {"COLOR.0", "TEXCOORD.0", "TEXCOORD.1"};
      cases.push_back(c);
    }

    // vs_3_0: a loop over a relatively addressed array, and texldl from a vertex texture
    const float texels[4][4] = {{0.1f, 0.2f, 0.3f, 0.4f},
                                {0.5f, 0.6f, 0.7f, 0.8f},
                                {-0.3f, 1.5f, 2.0f, 0.0f},
                                {0.9f, 0.0f, 0.5f, 1.0f}};
    IDirect3DTexture9Ptr vtex;
    const bool vtexOK = SUCCEEDED(d3d->CheckDeviceFormat(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, presentParams.BackBufferFormat,
        D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A32B32G32R32F));
    if(vtexOK &&
       SUCCEEDED(dev->CreateTexture(2, 2, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &vtex, NULL)))
    {
      D3DLOCKED_RECT lr = {};
      vtex->LockRect(0, &lr, NULL, 0);
      for(int y = 0; y < 2; y++)
        for(int x = 0; x < 2; x++)
        {
          // A32B32G32R32F is stored R, G, B, A
          float *t = (float *)((byte *)lr.pBits + y * lr.Pitch) + x * 4;
          memcpy(t, texels[y * 2 + x], sizeof(float) * 4);
        }
      vtex->UnlockRect(0);
    }
    else
    {
      TEST_WARN("A32B32G32R32F vertex textures aren't supported, skipping texldl");
    }

    if(vtex)
    {
      Case c;
      c.name = "vs_3_0 loops and texldl";
      c.verts = grid;
      c.stride = sizeof(PNCT);
      c.decl = {
          {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      };
      c.indices = gridIdx;
      c.kind = Case::Indexed;
      c.primCount = 8;
      c.vertexTexture = true;
      c.shader = true;
      c.vsTokens = CompileLoop(R"(
row_major float4x4 mvp : register(c0);
float4 offsets[8] : register(c4);
sampler2D heights : register(s0);

struct VOut
{
  float4 pos : POSITION;
  float4 data : TEXCOORD0;
  float4 h : TEXCOORD1;
};

VOut main(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
  VOut o;
  float4 acc = 0;
  // 5 is the count the app sets in i0 (see CompileLoop). The loop counter only indexes, as aL
  // can.
  float k = 1.0f;
  [loop] for(int i = 0; i < 5; i++)
  {
    acc += offsets[i] * k;
    k += 1.0f;
  }
  // the grid's uvs are 0, 0.5 and 1, so this samples at 0.2, 0.45 and 0.7: away from the
  // edges between the 2x2 texture's texels
  float4 h = tex2Dlod(heights, float4(uv * 0.5f + 0.2f, 0, 0));
  float4 p = pos;
  p.z += h.x * 0.5f;
  o.pos = mul(p, mvp);
  o.data = acc;
  o.h = h;
  return o;
}
)",
                               "vs_3_0");
      float offsets[8][4];
      for(int i = 0; i < 8; i++)
        for(int k = 0; k < 4; k++)
          offsets[i][k] = float(i) * 0.1f + float(k) * 0.01f;
      M wvp = viewProj;
      IDirect3DTexture9 *tex = vtex;
      c.state = [wvp, offsets, tex](IDirect3DDevice9 *d) {
        d->SetVertexShaderConstantF(0, wvp.m, 4);
        d->SetVertexShaderConstantF(4, &offsets[0][0], 8);
        const int i[] = {5, 0, 1, 0};
        d->SetVertexShaderConstantI(0, i, 1);
        d->SetTexture(D3DVERTEXTEXTURESAMPLER0, tex);
        d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      };
      std::vector<byte> verts = grid;
      c.reference = [verts, offsets, texels, wvp](UINT, UINT v, Outputs &out) {
        const PNCT &p = ((const PNCT *)verts.data())[v];
        float acc[4] = {};
        for(int i = 0; i < 5; i++)
          for(int k = 0; k < 4; k++)
            acc[k] += offsets[i][k] * float(i + 1);
        // point sampling the 2x2 texture
        int tx = int(floorf((p.uv[0] * 0.5f + 0.2f) * 2.0f));
        int ty = int(floorf((p.uv[1] * 0.5f + 0.2f) * 2.0f));
        const float *h = texels[ty * 2 + tx];
        float pos[4] = {p.pos[0], p.pos[1], p.pos[2] + h[0] * 0.5f, 1.0f}, clip[4];
        Transform(pos, wvp, clip);
        out["POSITION.0"] = {clip[0], clip[1], clip[2], clip[3]};
        out["TEXCOORD.0"] = {acc[0], acc[1], acc[2], acc[3]};
        out["TEXCOORD.1"] = {h[0], h[1], h[2], h[3]};
      };
      cases.push_back(c);
    }

    // instancing: 3 instances of a strip, offset and scaled by stream 1
    {
      struct IV
      {
        float pos[3];
        D3DCOLOR col;
      };
      struct ID
      {
        float offs[4];
      };
      Case c;
      c.name = "vs_3_0 instancing";
      std::vector<IV> iv;
      for(int i = 0; i < 4; i++)
        iv.push_back({{float(i & 1) * 0.5f, float(i >> 1) * 0.5f, 0.0f},
                      D3DCOLOR_ARGB(255, 255 * (i & 1), 255 * (i >> 1), 128)});
      std::vector<ID> id = {
          {{-1.5f, 0.0f, 0.0f, 1.0f}}, {{0.0f, 0.5f, 0.5f, 0.5f}}, {{1.0f, -1.0f, 1.0f, 2.0f}}};
      for(const IV &v : iv)
        Append(c.verts, v);
      for(const ID &v : id)
        Append(c.verts1, v);
      c.stride = sizeof(IV);
      c.stride1 = sizeof(ID);
      c.decl = {
          {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
          {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
      };
      c.indices = {0, 1, 2, 2, 1, 3};
      c.kind = Case::Indexed;
      c.primCount = 2;
      c.instances = 3;
      c.shader = true;
      c.vsTokens = CompileVS(R"(
row_major float4x4 mvp : register(c0);

struct VOut
{
  float4 pos : POSITION;
  float4 col : COLOR0;
  float4 inst : TEXCOORD0;
};

VOut main(float3 pos : POSITION, float4 col : COLOR0, float4 inst : TEXCOORD1)
{
  VOut o;
  o.pos = mul(float4(pos * inst.w + inst.xyz, 1), mvp);
  o.col = col;
  o.inst = inst;
  return o;
}
)",
                             "vs_3_0");
      M wvp = viewProj;
      c.state = [wvp](IDirect3DDevice9 *d) { d->SetVertexShaderConstantF(0, wvp.m, 4); };
      c.reference = [iv, id, wvp](UINT inst, UINT v, Outputs &out) {
        const float *o = id[inst].offs;
        float pos[4] = {iv[v].pos[0] * o[3] + o[0], iv[v].pos[1] * o[3] + o[1],
                        iv[v].pos[2] * o[3] + o[2], 1.0f},
              clip[4];
        Transform(pos, wvp, clip);
        out["POSITION.0"] = {clip[0], clip[1], clip[2], clip[3]};
        out["COLOR.0"] = Color(iv[v].col);
        out["TEXCOORD.0"] = {o[0], o[1], o[2], o[3]};
      };
      cases.push_back(c);
    }

    // vs_3_0 with two or more semantics in one output register, which fxc doesn't do but
    // hand-written shaders and other compilers can. Assembled. Every output register is
    // r0 * a + b, with r0 = (u, v, x, y) of the vertex and a and b literals of its own.
    const float packA[5][4] = {{1.0f, 2.0f, 3.0f, 4.0f},
                               {0.25f, 0.125f, 0.0625f, 0.2f},
                               {-1.0f, 0.5f, 1.5f, -2.0f},
                               {3.0f, -3.0f, 0.75f, 1.25f},
                               {0.5f, -0.5f, 0.1f, 0.15f}};
    const float packB[5][4] = {{0.1f, 0.2f, 0.3f, 0.4f},
                               {0.5f, 0.6f, 0.4f, 0.5f},
                               {10.0f, 20.0f, 30.0f, 40.0f},
                               {-5.0f, -6.0f, -7.0f, -8.0f},
                               {0.25f, 0.5f, 0.3f, 0.6f}};
    IDirect3DPixelShader9Ptr packedPS;
    {
      Case c;
      c.name = "vs_3_0 packed outputs";
      c.verts = grid;
      c.stride = sizeof(PNCT);
      c.decl = {
          {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      };
      c.indices = gridIdx;
      c.kind = Case::Indexed;
      c.primCount = 8;
      c.shader = true;

      std::string vs =
          "vs_3_0\n"
          "dcl_position v0\n"
          "dcl_texcoord v1\n"
          "dcl_position o0\n"
          "dcl_texcoord0 o1.xy\n"
          "dcl_texcoord1 o1.zw\n"
          "dcl_color0 o2.xyz\n"
          "dcl_fog o2.w\n"
          "dcl_texcoord2 o3.x\n"
          "dcl_texcoord3 o3.yzw\n"
          "dcl_texcoord4 o4.xz\n"
          "dcl_texcoord5 o4.yw\n"
          "dcl_texcoord6 o5.x\n"
          "dcl_texcoord7 o5.y\n"
          "dcl_color1 o5.zw\n";
      for(int i = 0; i < 5; i++)
      {
        char line[256];
        snprintf(line, sizeof(line),
                 "def c%d, %.9g, %.9g, %.9g, %.9g\ndef c%d, %.9g, %.9g, %.9g, %.9g\n", 4 + i * 2,
                 packA[i][0], packA[i][1], packA[i][2], packA[i][3], 5 + i * 2, packB[i][0],
                 packB[i][1], packB[i][2], packB[i][3]);
        vs += line;
      }
      vs += "m4x4 o0, v0, c0\nmov r0.xy, v1\nmov r0.zw, v0.xyxy\n";
      for(int i = 0; i < 5; i++)
        vs += "mul r1, r0, c" + std::to_string(4 + i * 2) + "\nadd o" + std::to_string(i + 1) +
              ", r1, c" + std::to_string(5 + i * 2) + "\n";
      c.vsTokens = Assemble(vs);

      // and a ps_3_0 that reads some of them packed too, so the pixel shader's inputs are
      std::vector<DWORD> ps = Assemble(
          "ps_3_0\n"
          "dcl_texcoord0 v0.xy\n"
          "dcl_texcoord1 v0.zw\n"
          "dcl_texcoord2 v1.x\n"
          "dcl_texcoord3 v1.yzw\n"
          "dcl_color0 v2.xyz\n"
          "def c0, 0.1, 0.05, 0.002, 1\n"
          "mul r0, v0, c0.y\n"
          "add r0.xy, r0.xy, r0.zw\n"
          "mul r1, v1, c0.z\n"
          "add r1, r1, v2.xyzx\n"
          "add r0.xy, r0.xy, r1.xy\n"
          "mov r0.z, r1.z\n"
          "mov r0.w, c0.w\n"
          "mov oC0, r0\n");
      if(!ps.empty())
        dev->CreatePixelShader(ps.data(), &packedPS);

      M wvp = Transpose(viewProj);
      IDirect3DPixelShader9 *pps = packedPS;
      c.state = [wvp, pps](IDirect3DDevice9 *d) {
        d->SetVertexShaderConstantF(0, wvp.m, 4);
        d->SetPixelShader(pps);
      };

      std::vector<byte> verts = grid;
      M vpm = viewProj;
      c.reference = [verts, vpm, packA, packB](UINT, UINT v, Outputs &out) {
        const PNCT &p = ((const PNCT *)verts.data())[v];
        float pos[4] = {p.pos[0], p.pos[1], p.pos[2], 1.0f}, clip[4];
        Transform(pos, vpm, clip);
        out["POSITION.0"] = {clip[0], clip[1], clip[2], clip[3]};
        const float r0[4] = {p.uv[0], p.uv[1], p.pos[0], p.pos[1]};
        float o[6][4];
        for(int i = 0; i < 5; i++)
          for(int k = 0; k < 4; k++)
            o[i + 1][k] = r0[k] * packA[i][k] + packB[i][k];
        // each semantic is its components of the register, in order
        out["TEXCOORD.0"] = {o[1][0], o[1][1]};
        out["TEXCOORD.1"] = {o[1][2], o[1][3]};
        out["COLOR.0"] = {o[2][0], o[2][1], o[2][2]};
        out["FOG.0"] = {o[2][3]};
        out["TEXCOORD.2"] = {o[3][0]};
        out["TEXCOORD.3"] = {o[3][1], o[3][2], o[3][3]};
        out["TEXCOORD.4"] = {o[4][0], o[4][2]};
        out["TEXCOORD.5"] = {o[4][1], o[4][3]};
        out["TEXCOORD.6"] = {o[5][0]};
        out["TEXCOORD.7"] = {o[5][1]};
        out["COLOR.1"] = {o[5][2], o[5][3]};
      };
      // a colour can share a pixel shader's input register only if every semantic in it is
      // centroid, which doesn't change a point
      c.hwRegisters = {
          {"dcl_texcoord0 v0.xy\ndcl_texcoord1 v0.zw", {{"TEXCOORD.0", "xy"}, {"TEXCOORD.1", "zw"}}},
          {"dcl_color0_centroid v0.xyz\ndcl_fog_centroid v0.w",
           {{"COLOR.0", "xyz"}, {"FOG.0", "w"}}},
          {"dcl_texcoord2 v0.x\ndcl_texcoord3 v0.yzw", {{"TEXCOORD.2", "x"}, {"TEXCOORD.3", "yzw"}}},
          {"dcl_texcoord4 v0.xz\ndcl_texcoord5 v0.yw", {{"TEXCOORD.4", "xz"}, {"TEXCOORD.5", "yw"}}},
          {"dcl_texcoord6_centroid v0.x\ndcl_texcoord7_centroid v0.y\ndcl_color1_centroid v0.zw",
           {{"TEXCOORD.6", "x"}, {"TEXCOORD.7", "y"}, {"COLOR.1", "zw"}}},
      };
      cases.push_back(c);
    }

    // vs_3_0 texldl with filtering: four vertex textures with different sampler states, over a
    // 5x5 grid so the coordinates and LODs cross texels, mip levels and cube faces
    const bool vtexFloat =
        SUCCEEDED(d3d->CheckDeviceFormat(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, presentParams.BackBufferFormat,
            D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_CUBETEXTURE, D3DFMT_A32B32G32R32F)) &&
        SUCCEEDED(d3d->CheckDeviceFormat(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, presentParams.BackBufferFormat,
            D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_VOLUMETEXTURE, D3DFMT_A32B32G32R32F)) &&
        SUCCEEDED(d3d->CheckDeviceFormat(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, presentParams.BackBufferFormat,
            D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)) &&
        SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                         presentParams.BackBufferFormat, D3DUSAGE_QUERY_SRGBREAD,
                                         D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8));
    if(vtexOK && vtexFloat)
    {
      Case c;
      c.name = "vs_3_0 texldl filtering";

      std::vector<byte> fine;
      for(int y = 0; y < 5; y++)
      {
        for(int x = 0; x < 5; x++)
        {
          float fx = float(x) * 0.5f - 1.0f, fy = float(y) * 0.5f - 1.0f;
          PNCT v = {{fx, fy, fx * 0.1f}, {0.0f, 0.0f, -1.0f}, 0xffffffff, {x * 0.25f, y * 0.25f}};
          Append(fine, v);
        }
      }
      for(uint16_t y = 0; y < 4; y++)
      {
        for(uint16_t x = 0; x < 4; x++)
        {
          uint16_t i = y * 5 + x;
          for(uint16_t k : {i, uint16_t(i + 5), uint16_t(i + 1), uint16_t(i + 1), uint16_t(i + 5),
                            uint16_t(i + 6)})
            c.indices.push_back(k);
        }
      }
      c.verts = fine;
      c.stride = sizeof(PNCT);
      c.decl = {
          {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      };
      c.kind = Case::Indexed;
      c.primCount = 32;
      c.vertexTexture = true;
      c.shader = true;

      auto hash = [](uint32_t a) {
        a ^= a >> 16;
        a *= 0x7feb352dU;
        a ^= a >> 15;
        a *= 0x846ca68bU;
        a ^= a >> 16;
        return float(a & 0xffff) / 65535.0f;
      };

      // s0: 8x4 with 4 levels, trilinear, wrapping in u and mirrored in v, with a LOD bias
      // s1: a 4x4 cube with 3 levels, trilinear
      // s2: a 4x4x4 volume with 3 levels, linear magnification and point minification without a
      //     mip filter, border, clamp and mirror once
      // s3: 8x8 A8R8G8B8 read as sRGB, with 4 levels: point magnification, linear minification,
      //     the nearest mip level and MAXMIPLEVEL 1
      RefTexture refs[4];
      IDirect3DBaseTexture9Ptr texs[4];
      bool ok = true;
      {
        IDirect3DTexture9Ptr t;
        ok &= SUCCEEDED(
            dev->CreateTexture(8, 4, 4, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &t, NULL));
        for(UINT l = 0; ok && l < 4; l++)
        {
          RefTexture::Level lev;
          lev.w = 8 >> l;
          lev.h = std::max(1, 4 >> l);
          D3DLOCKED_RECT lr = {};
          t->LockRect(l, &lr, NULL, 0);
          for(int y = 0; y < lev.h; y++)
            for(int x = 0; x < lev.w; x++)
            {
              std::array<float, 4> v;
              for(int k = 0; k < 4; k++)
                v[k] = hash(1000 + l * 4096 + (y * lev.w + x) * 4 + k);
              memcpy((byte *)lr.pBits + y * lr.Pitch + x * 16, v.data(), 16);
              lev.texels.push_back(v);
            }
          t->UnlockRect(l);
          refs[0].levels.push_back(lev);
        }
        texs[0] = t;
        refs[0].address[0] = D3DTADDRESS_WRAP;
        refs[0].address[1] = D3DTADDRESS_MIRROR;
        refs[0].mag = refs[0].min = refs[0].mip = D3DTEXF_LINEAR;
        refs[0].bias = 0.25f;
      }
      {
        IDirect3DCubeTexture9Ptr t;
        ok &= SUCCEEDED(
            dev->CreateCubeTexture(4, 3, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &t, NULL));
        for(UINT l = 0; ok && l < 3; l++)
        {
          RefTexture::Level lev;
          lev.w = lev.h = 4 >> l;
          lev.d = 6;
          for(UINT f = 0; f < 6; f++)
          {
            D3DLOCKED_RECT lr = {};
            t->LockRect((D3DCUBEMAP_FACES)f, l, &lr, NULL, 0);
            for(int y = 0; y < lev.h; y++)
              for(int x = 0; x < lev.w; x++)
              {
                std::array<float, 4> v;
                for(int k = 0; k < 4; k++)
                  v[k] = hash(2000 + l * 4096 + f * 256 + (y * lev.w + x) * 4 + k);
                memcpy((byte *)lr.pBits + y * lr.Pitch + x * 16, v.data(), 16);
                lev.texels.push_back(v);
              }
            t->UnlockRect((D3DCUBEMAP_FACES)f, l);
          }
          refs[1].levels.push_back(lev);
        }
        texs[1] = t;
        refs[1].kind = RefTexture::Cube;
        refs[1].mag = refs[1].min = refs[1].mip = D3DTEXF_LINEAR;
      }
      {
        IDirect3DVolumeTexture9Ptr t;
        ok &= SUCCEEDED(dev->CreateVolumeTexture(4, 4, 4, 3, 0, D3DFMT_A32B32G32R32F,
                                                 D3DPOOL_MANAGED, &t, NULL));
        for(UINT l = 0; ok && l < 3; l++)
        {
          RefTexture::Level lev;
          lev.w = lev.h = lev.d = 4 >> l;
          D3DLOCKED_BOX lb = {};
          t->LockBox(l, &lb, NULL, 0);
          for(int z = 0; z < lev.d; z++)
            for(int y = 0; y < lev.h; y++)
              for(int x = 0; x < lev.w; x++)
              {
                std::array<float, 4> v;
                for(int k = 0; k < 4; k++)
                  v[k] = hash(3000 + l * 4096 + ((z * lev.h + y) * lev.w + x) * 4 + k);
                memcpy((byte *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch + x * 16, v.data(), 16);
                lev.texels.push_back(v);
              }
          t->UnlockBox(l);
          refs[2].levels.push_back(lev);
        }
        texs[2] = t;
        refs[2].kind = RefTexture::Volume;
        refs[2].address[0] = D3DTADDRESS_BORDER;
        refs[2].address[1] = D3DTADDRESS_CLAMP;
        refs[2].address[2] = D3DTADDRESS_MIRRORONCE;
        refs[2].border[0] = 0.2f;
        refs[2].border[1] = 0.4f;
        refs[2].border[2] = 0.8f;
        refs[2].border[3] = 1.0f;
        refs[2].mag = D3DTEXF_LINEAR;
        refs[2].min = D3DTEXF_POINT;
        refs[2].mip = D3DTEXF_NONE;
      }
      {
        IDirect3DTexture9Ptr t;
        ok &= SUCCEEDED(dev->CreateTexture(8, 8, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL));
        for(UINT l = 0; ok && l < 4; l++)
        {
          RefTexture::Level lev;
          lev.w = lev.h = 8 >> l;
          D3DLOCKED_RECT lr = {};
          t->LockRect(l, &lr, NULL, 0);
          for(int y = 0; y < lev.h; y++)
            for(int x = 0; x < lev.w; x++)
            {
              byte b[4];
              std::array<float, 4> v;
              for(int k = 0; k < 4; k++)
              {
                b[k] = byte(hash(4000 + l * 4096 + (y * lev.w + x) * 4 + k) * 255.0f + 0.5f);
                float f = float(b[k]) / 255.0f;
                // the colour channels are read as sRGB
                if(k < 3)
                  f = f <= 0.04045f ? f / 12.92f : powf((f + 0.055f) / 1.055f, 2.4f);
                v[k] = f;
              }
              // stored B, G, R, A
              byte *dst = (byte *)lr.pBits + y * lr.Pitch + x * 4;
              dst[0] = b[2];
              dst[1] = b[1];
              dst[2] = b[0];
              dst[3] = b[3];
              lev.texels.push_back(v);
            }
          t->UnlockRect(l);
          refs[3].levels.push_back(lev);
        }
        texs[3] = t;
        refs[3].address[0] = refs[3].address[1] = D3DTADDRESS_CLAMP;
        refs[3].mag = D3DTEXF_POINT;
        refs[3].min = D3DTEXF_LINEAR;
        refs[3].mip = D3DTEXF_POINT;
        refs[3].maxMip = 1;
      }

      // each sampler's coordinate is r0 * a + b, with r0 = (u, v, v, u) of the vertex
      const float texA[4][4] = {
          {1.3f, 1.2f, 0.0f, 2.5f},
          {2.0f, 1.0f, 2.0f, 1.5f},
          {1.4f, 1.2f, 1.6f, 2.0f},
          {0.9f, 1.1f, 0.0f, 3.0f},
      };
      const float texB[4][4] = {
          {-0.15f, -0.1f, 0.0f, -0.6f},
          {-1.0f, -0.4f, -1.0f, 0.0f},
          {-0.2f, -0.1f, -0.3f, -0.5f},
          {0.03f, -0.05f, 0.0f, -1.2f},
      };

      std::string vs =
          "vs_3_0\n"
          "dcl_position v0\n"
          "dcl_texcoord v1\n"
          "dcl_2d s0\n"
          "dcl_cube s1\n"
          "dcl_volume s2\n"
          "dcl_2d s3\n"
          "dcl_position o0\n"
          "dcl_texcoord0 o1\n"
          "dcl_texcoord1 o2\n"
          "dcl_texcoord2 o3\n"
          "dcl_texcoord3 o4\n";
      for(int i = 0; i < 4; i++)
      {
        char line[256];
        snprintf(line, sizeof(line),
                 "def c%d, %.9g, %.9g, %.9g, %.9g\ndef c%d, %.9g, %.9g, %.9g, %.9g\n", 4 + i * 2,
                 texA[i][0], texA[i][1], texA[i][2], texA[i][3], 5 + i * 2, texB[i][0], texB[i][1],
                 texB[i][2], texB[i][3]);
        vs += line;
      }
      vs += "m4x4 o0, v0, c0\nmov r0, v1.xyyx\n";
      for(int i = 0; i < 4; i++)
        vs += "mul r1, r0, c" + std::to_string(4 + i * 2) + "\nadd r1, r1, c" +
              std::to_string(5 + i * 2) + "\ntexldl r2, r1, s" + std::to_string(i) + "\nmov o" +
              std::to_string(i + 1) + ", r2\n";
      c.vsTokens = ok ? Assemble(vs) : std::vector<DWORD>();
      if(!ok)
        TEST_ERROR("Couldn't create the vertex textures for texldl filtering");

      M wvp = Transpose(viewProj);
      // the case holds the textures
      std::vector<IDirect3DBaseTexture9Ptr> tex(texs, texs + 4);
      std::vector<RefTexture> samp(refs, refs + 4);
      c.state = [wvp, tex, samp](IDirect3DDevice9 *d) {
        d->SetVertexShaderConstantF(0, wvp.m, 4);
        for(DWORD i = 0; i < 4; i++)
        {
          const DWORD s = D3DVERTEXTEXTURESAMPLER0 + i;
          const RefTexture &r = samp[i];
          d->SetTexture(s, tex[i]);
          d->SetSamplerState(s, D3DSAMP_ADDRESSU, r.address[0]);
          d->SetSamplerState(s, D3DSAMP_ADDRESSV, r.address[1]);
          d->SetSamplerState(s, D3DSAMP_ADDRESSW, r.address[2]);
          d->SetSamplerState(s, D3DSAMP_BORDERCOLOR,
                             D3DCOLOR_COLORVALUE(r.border[0], r.border[1], r.border[2], r.border[3]));
          d->SetSamplerState(s, D3DSAMP_MAGFILTER, r.mag);
          d->SetSamplerState(s, D3DSAMP_MINFILTER, r.min);
          d->SetSamplerState(s, D3DSAMP_MIPFILTER, r.mip);
          d->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, *(const DWORD *)&r.bias);
          d->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, r.maxMip);
          d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, i == 3 ? TRUE : FALSE);
        }
      };

      M vpm = viewProj;
      c.reference = [fine, vpm, texA, texB, samp](UINT, UINT v, Outputs &out) {
        const PNCT &p = ((const PNCT *)fine.data())[v];
        float pos[4] = {p.pos[0], p.pos[1], p.pos[2], 1.0f}, clip[4];
        Transform(pos, vpm, clip);
        out["POSITION.0"] = {clip[0], clip[1], clip[2], clip[3]};
        const float r0[4] = {p.uv[0], p.uv[1], p.uv[1], p.uv[0]};
        for(int i = 0; i < 4; i++)
        {
          float coord[4];
          for(int k = 0; k < 4; k++)
          {
            // as the shader does: a multiply, then an add
            float m = r0[k] * texA[i][k];
            coord[k] = m + texB[i][k];
          }
          std::array<float, 4> s = RefSample(samp[i], coord);
          out["TEXCOORD." + std::to_string(i)] = {s[0], s[1], s[2], s[3]};
        }
      };
      c.hwRegisters = {
          {"dcl_texcoord0 v0", {{"TEXCOORD.0", "xyzw"}}},
          {"dcl_texcoord1 v0", {{"TEXCOORD.1", "xyzw"}}},
          {"dcl_texcoord2 v0", {{"TEXCOORD.2", "xyzw"}}},
          {"dcl_texcoord3 v0", {{"TEXCOORD.3", "xyzw"}}},
      };
      c.hwTolerance = 0.01f;
      cases.push_back(c);
    }
    else
    {
      TEST_WARN(
          "Float cube and volume or sRGB vertex textures aren't supported, skipping texldl "
          "filtering");
    }

    // a vertex shader with DrawPrimitiveUP
    {
      Case c = cases.back();
      for(const Case &k : cases)
        if(k.name == "vs_2_0 flow control")
          c = k;
      c.name = "vs_2_0 DrawPrimitiveUP";
      c.kind = Case::UP;
      c.start = 0;
      cases.push_back(c);
    }

    for(size_t i = 0; i < cases.size();)
    {
      if(cases[i].shader && cases[i].vsTokens.empty())
      {
        TEST_ERROR("Leaving out %s: its shader didn't compile", cases[i].name.c_str());
        cases.erase(cases.begin() + i);
      }
      else
      {
        i++;
      }
    }

    // the references, from a software vertex processing device on a window of its own
    {
      // in the demo's tmp folder, where the test framework's --tmp puts it. Under RenderDoc the
      // software device and the hardware readback are captured too, so that reference is kept
      // apart from the native one the checks use.
      std::string path = tmpPath + (rdoc ? "/D3D9_Mesh_Output_reference_rdoc.txt"
                                         : "/D3D9_Mesh_Output_reference.txt");

      Win32Window *win = (Win32Window *)MakeWindow(screenWidth, screenHeight, "ProcessVertices");
      D3DPRESENT_PARAMETERS pp = presentParams;
      pp.hDeviceWindow = win->wnd;
      IDirect3DDevice9Ptr sw;
      HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, win->wnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &sw);
      if(FAILED(hr))
        TEST_WARN("Couldn't create a software vertex processing device: %x", hr);

      // for WriteHardware
      if(FAILED(dev->CreateRenderTarget(screenWidth, screenHeight, D3DFMT_A8R8G8B8,
                                        D3DMULTISAMPLE_NONE, 0, FALSE, &hwTarget, NULL)) ||
         FAILED(dev->CreateOffscreenPlainSurface(screenWidth, screenHeight, D3DFMT_A8R8G8B8,
                                                 D3DPOOL_SYSTEMMEM, &hwRead, NULL)))
      {
        TEST_WARN("Couldn't create the surfaces to read the hardware's vertices back");
        hwTarget = NULL;
      }
      // for WriteHardwareOutputs
      if(FAILED(dev->CreateRenderTarget(1, 1, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE,
                                        &hwFloatTarget, NULL)) ||
         FAILED(dev->CreateOffscreenPlainSurface(1, 1, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM,
                                                 &hwFloatRead, NULL)))
      {
        TEST_ERROR("Couldn't create the float surfaces to read vertex outputs back");
        hwFloatTarget = NULL;
      }

      FILE *f = NULL;
      fopen_s(&f, path.c_str(), "w");
      if(f)
      {
        fprintf(f, "viewport 0 0 %u %u 0 1\n", screenWidth, screenHeight);
        for(const Case &c : cases)
          WriteReference(f, sw, c);
        fclose(f);
        TEST_LOG("Wrote the reference to %s", path.c_str());
        D3DDEVICE_CREATION_PARAMETERS params = {};
        dev->GetCreationParameters(&params);
        if(params.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING)
          TEST_WARN(
              "The main device has software vertex processing, so the hw lines aren't "
              "from the hardware");
      }
      else
      {
        TEST_ERROR("Couldn't write %s", path.c_str());
      }

      sw = NULL;
      hwTarget = NULL;
      hwRead = NULL;
      hwFloatTarget = NULL;
      hwFloatRead = NULL;
      delete win;
    }

    std::vector<Bound> bound(cases.size());
    for(size_t i = 0; i < cases.size(); i++)
      if(!Bind(dev, cases[i], false, bound[i]))
        TEST_ERROR("Couldn't create the resources for %s", cases[i].name.c_str());

    while(Running())
    {
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);
      dev->BeginScene();

      for(size_t i = 0; i < cases.size(); i++)
      {
        pushMarker(cases[i].name);
        Apply(dev, cases[i], bound[i]);
        Draw(dev, cases[i]);
        popMarker();
      }

      dev->SetStreamSourceFreq(0, 1);
      dev->SetStreamSourceFreq(1, 1);
      dev->EndScene();
      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
