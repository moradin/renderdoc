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

#include <functional>
#include "d3d9_test.h"

#define CHECK_HR(expr)                                    \
  do                                                      \
  {                                                       \
    HRESULT check_hr = (expr);                            \
    if(FAILED(check_hr))                                  \
      TEST_ERROR("%s failed: %x", #expr, (UINT)check_hr); \
  } while(0)

RD_TEST(D3D9_Pixel_History, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Tests pixel history. Each batch draws D3D11_Pixel_History's draws (the shared PixelHistory "
      "vertices, with the same markers where D3D9 has the feature), then D3D9's own writes: clears "
      "with rects and with the viewport and scissor, ColorFill, StretchRect, UpdateSurface, a "
      "fixed function draw and an indexed one. Run natively, its third frame records each probe "
      "pixel before the batch and after every event (colour, depth and stencil, and for draws "
      "whether they rasterise anything there) in D3D9_Pixel_History_reference.txt in its tmp "
      "folder.";

  // the vertices' colours, as D3D11_Pixel_History's ProcessColor: negative is an overflow
  const std::string common = R"EOSHADER(

#define ALPHA_ADD 1.75
#define DISCARD_X 150

float4 ProcessColor(float4 col)
{
  float4 ret = col;
  if(col.x < 0.0f) ret.x = 100000.0f;
  if(col.y < 0.0f) ret.y = 100000.0f;
  if(col.z < 0.0f) ret.z = 100000.0f;
  return ret;
}

)EOSHADER";

  const std::string vertex = R"EOSHADER(

float4 main(float3 pos : POSITION, float4 col : COLOR0, out float4 outCol : TEXCOORD0) : POSITION
{
  outCol = ProcessColor(col);
  return float4(pos, 1.0f);
}

)EOSHADER";

  // D3D9's VPOS is the pixel's integer coordinates, D3D11's SV_Position its centre: this discards
  // the column D3D11's does
  const std::string pixel = R"EOSHADER(

float4 main(float4 col : TEXCOORD0, float2 vpos : VPOS) : COLOR0
{
  if(abs(vpos.x - DISCARD_X) < 0.5f)
    discard;
  if(col.x > 10000 && col.y > 10000 && col.z > 10000)
    discard;
  return col + float4(0, 0, 0, ALPHA_ADD);
}

)EOSHADER";

  // D3D9 has no draws without pixel output: "Unbound Shader" draws with a vs_2_0 shader and the
  // fixed function pixel pipeline instead
  const std::string vertex2 = R"EOSHADER(

float4 main(float3 pos : POSITION, float4 col : COLOR0, out float4 outCol : COLOR0) : POSITION
{
  outCol = col;
  return float4(pos, 1.0f);
}

)EOSHADER";

  // the native run's own shaders: a constant colour for the coverage queries, as pixel history's
  const std::string whitePixel = R"EOSHADER(

float4 main() : COLOR0
{
  return float4(1, 1, 1, 1);
}

)EOSHADER";

  // a quad over the whole target, in clip space
  const std::string quadVertex = R"EOSHADER(

float4 main(float4 pos : POSITION) : POSITION
{
  return pos;
}

)EOSHADER";

  // an INTZ depth stencil's depth. c0.xy: 1 / its size
  const std::string depthPixel = R"EOSHADER(

sampler2D depth : register(s0);
float4 invSize : register(c0);

float4 main(float2 vpos : VPOS) : COLOR0
{
  return tex2D(depth, (vpos + 0.5f) * invSize.xy).rrrr;
}

)EOSHADER";

  // one bit of stencil, added up with blending
  const std::string bitPixel = R"EOSHADER(

float4 bit : register(c0);

float4 main() : COLOR0
{
  return bit;
}

)EOSHADER";

  struct FFVertex
  {
    float x, y, z, rhw;
    D3DCOLOR col;
  };

  // the pixels the shared Pixel_History test looks at, then D3D9's own writes' ones
  static constexpr int NumProbes = 24;
  const int Probes[NumProbes][2] = {
      {110, 100},
      {170, 140},
      {170, 160},
      {150, 250},
      {330, 145},
      {340, 145},
      {330, 102},
      {318, 102},
      {346, 102},
      {106, 248},
      {275, 258},
      {60, 130},
      {60, 50},
      {330, 200},
      {200, 250},
      {60, 80},
      {105, 50},
      {60, 160},
      // D3D9's writes
      {30, 30},
      {52, 30},
      {60, 30},
      {330, 30},
      {370, 30},
      {355, 60},
  };

  // what D3D9_Pixel_History.py mirrors to know where the clears and copies write
  const D3DRECT ClearRects[2] = {{20, 20, 40, 40}, {320, 20, 340, 40}};
  const D3DVIEWPORT9 ClearViewport = {50, 15, 40, 30, 0.0f, 1.0f};
  const RECT ClearScissor = {55, 0, 400, 300};
  const RECT FillRect = {360, 20, 380, 40};
  const RECT StretchDest = {320, 20, 340, 40};
  const POINT UpdateDest = {25, 25};

  struct State
  {
    IDirect3DVertexShader9 *vs = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    // the vertex shader's major version, for the native run's coverage queries
    int vsModel = 3;
    DWORD cull = D3DCULL_CCW;
    BOOL zenable = TRUE;
    DWORD zfunc = D3DCMP_ALWAYS;
    BOOL zwrite = TRUE;
    BOOL stencil = FALSE;
    DWORD stencilFunc = D3DCMP_ALWAYS;
    DWORD ref = 0x55;
    DWORD readMask = 0xff;
    DWORD writeMask = 0xff;
    DWORD colourWrite = 0xf;
  };

  struct Batch
  {
    std::string name;
    IDirect3DTexture9Ptr colTex;
    IDirect3DSurface9Ptr col;
    IDirect3DTexture9Ptr depthTex;
    IDirect3DSurface9Ptr depth;
  };

  // one probe's values before the batch or after an event, natively
  struct ProbeValue
  {
    // 1 or 0 for draws, -1 for the rest
    int covered = -1;
    uint32_t rgba = 0;
    float depth = -1.0f;
    int stencil = -1;
  };

  struct Record
  {
    std::string batch;
    std::string marker;
    std::string kind;
    // whether a draw has depth or stencil on, and so writes the depth stencil
    bool depthUsed = false;
    ProbeValue probes[NumProbes];
  };

  std::vector<Record> records;
  bool recording = false;
  std::string curMarker;
  State cur;
  Batch *curBatch = NULL;

  IDirect3DVertexShader9Ptr quadVS;
  IDirect3DPixelShader9Ptr whitePS2, whitePS3, depthPS, bitPS;
  IDirect3DVertexDeclaration9Ptr quadDecl;
  IDirect3DQuery9Ptr occlusion;
  IDirect3DStateBlock9Ptr saved;
  IDirect3DSurface9Ptr colourSys, depthRT, depthSys, stencilRT, stencilSys;

  void Marker(const std::string &name)
  {
    setMarker(name);
    curMarker = name;
  }

  void SetState(const State &s)
  {
    cur = s;
    const bool hasDepth = curBatch && curBatch->depth;
    dev->SetVertexShader(s.vs);
    dev->SetPixelShader(s.ps);
    dev->SetRenderState(D3DRS_CULLMODE, s.cull);
    dev->SetRenderState(D3DRS_ZENABLE, hasDepth && s.zenable ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, s.zfunc);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, s.zwrite);
    dev->SetRenderState(D3DRS_STENCILENABLE, hasDepth && s.stencil);
    dev->SetRenderState(D3DRS_STENCILFUNC, s.stencilFunc);
    dev->SetRenderState(D3DRS_STENCILREF, s.ref);
    dev->SetRenderState(D3DRS_STENCILMASK, s.readMask);
    dev->SetRenderState(D3DRS_STENCILWRITEMASK, s.writeMask);
    dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, s.colourWrite);
  }

  // what a draw would rasterise at a probe, natively: pixel history's query
  bool Covers(const std::function<void()> &draw, int vsModel, int probe)
  {
    saved->Capture();

    dev->SetPixelShader(vsModel >= 3 ? whitePS3 : whitePS2);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    const RECT rect = {Probes[probe][0], Probes[probe][1], Probes[probe][0] + 1,
                       Probes[probe][1] + 1};
    dev->SetScissorRect(&rect);

    occlusion->Issue(D3DISSUE_BEGIN);
    draw();
    occlusion->Issue(D3DISSUE_END);

    saved->Apply();

    DWORD samples = 0;
    while(occlusion->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH) == S_FALSE)
    {
    }
    return samples > 0;
  }

  void DrawQuad()
  {
    const float quad[4][4] = {
        {-1.0f, 1.0f, 0.5f, 1.0f},
        {1.0f, 1.0f, 0.5f, 1.0f},
        {-1.0f, -1.0f, 0.5f, 1.0f},
        {1.0f, -1.0f, 0.5f, 1.0f},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
  }

  // the probes' values now, natively
  void ReadProbes(ProbeValue out[NumProbes])
  {
    saved->Capture();
    IDirect3DSurface9Ptr rt[4], ds;
    for(DWORD i = 0; i < 4; i++)
      dev->GetRenderTarget(i, &rt[i]);
    dev->GetDepthStencilSurface(&ds);

    dev->SetVertexShader(quadVS);
    dev->SetVertexDeclaration(quadDecl);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
    dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    for(DWORD i = 1; i < 4; i++)
      dev->SetRenderTarget(i, NULL);

    const bool hasDepth = curBatch->depth;
    if(hasDepth)
    {
      // stencil a bit at a time: the bit's value, added up where the stencil test passes
      dev->SetRenderTarget(0, stencilRT);
      dev->SetDepthStencilSurface(curBatch->depth);
      dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
      dev->SetPixelShader(bitPS);
      dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
      dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
      dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
      dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
      dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
      for(int b = 0; b < 8; b++)
      {
        const float bit[4] = {float(1 << b) / 255.0f, 0.0f, 0.0f, 0.0f};
        dev->SetPixelShaderConstantF(0, bit, 1);
        dev->SetRenderState(D3DRS_STENCILREF, 1 << b);
        dev->SetRenderState(D3DRS_STENCILMASK, 1 << b);
        DrawQuad();
      }
      dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);

      // depth, sampled
      dev->SetRenderTarget(0, depthRT);
      dev->SetDepthStencilSurface(NULL);
      dev->SetPixelShader(depthPS);
      const float invSize[4] = {1.0f / screenWidth, 1.0f / screenHeight, 0.0f, 0.0f};
      dev->SetPixelShaderConstantF(0, invSize, 1);
      dev->SetTexture(0, curBatch->depthTex);
      dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      DrawQuad();
      dev->SetTexture(0, NULL);
    }

    dev->EndScene();

    CHECK_HR(dev->GetRenderTargetData(curBatch->col, colourSys));
    if(hasDepth)
    {
      CHECK_HR(dev->GetRenderTargetData(stencilRT, stencilSys));
      CHECK_HR(dev->GetRenderTargetData(depthRT, depthSys));
    }

    D3DLOCKED_RECT colour = {}, stencil = {}, depth = {};
    CHECK_HR(colourSys->LockRect(&colour, NULL, D3DLOCK_READONLY));
    if(hasDepth)
    {
      CHECK_HR(stencilSys->LockRect(&stencil, NULL, D3DLOCK_READONLY));
      CHECK_HR(depthSys->LockRect(&depth, NULL, D3DLOCK_READONLY));
    }
    for(int p = 0; p < NumProbes; p++)
    {
      const int x = Probes[p][0], y = Probes[p][1];
      // B, G, R, A in memory
      const byte *c = (const byte *)colour.pBits + y * colour.Pitch + x * 4;
      out[p].rgba = (uint32_t(c[2]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[0]) << 8) | c[3];
      if(hasDepth)
      {
        const byte *s = (const byte *)stencil.pBits + y * stencil.Pitch + x * 4;
        out[p].stencil = s[2];
        memcpy(&out[p].depth, (const byte *)depth.pBits + y * depth.Pitch + x * 4, sizeof(float));
      }
    }
    colourSys->UnlockRect();
    if(hasDepth)
    {
      stencilSys->UnlockRect();
      depthSys->UnlockRect();
    }

    dev->BeginScene();

    // the targets first, since setting render target 0 resets the viewport and scissor rect
    for(DWORD i = 0; i < 4; i++)
      dev->SetRenderTarget(i, rt[i]);
    dev->SetDepthStencilSurface(ds);
    saved->Apply();
  }

  // An event of the batch. Natively, when recording, the probes' values after it, and for draws
  // whether they cover each probe.
  void Event(const char *kind, const std::function<void()> &issue)
  {
    const bool draw = !strcmp(kind, "draw");

    Record rec;
    if(recording && draw)
      for(int p = 0; p < NumProbes; p++)
        rec.probes[p].covered = Covers(issue, cur.vsModel, p) ? 1 : 0;

    issue();

    if(!recording)
      return;

    rec.batch = curBatch->name;
    rec.marker = curMarker;
    rec.kind = kind;
    rec.depthUsed = draw && curBatch->depth && (cur.zenable || cur.stencil);
    int covered[NumProbes];
    for(int p = 0; p < NumProbes; p++)
      covered[p] = rec.probes[p].covered;
    ReadProbes(rec.probes);
    for(int p = 0; p < NumProbes; p++)
      rec.probes[p].covered = covered[p];
    records.push_back(rec);
  }

  void WriteReference()
  {
    const std::string path = tmpPath + "/D3D9_Pixel_History_reference.txt";
    FILE *f = NULL;
    fopen_s(&f, path.c_str(), "w");
    if(!f)
    {
      TEST_ERROR("Couldn't write %s", path.c_str());
      return;
    }
    // E: batch, marker, kind, whether the draw uses depth or stencil. P: probe x, y, covered,
    // colour as RRGGBBAA, depth as float bits, stencil. Batch starts have kind "start".
    for(const Record &r : records)
    {
      fprintf(f, "E\t%s\t%s\t%s\t%d\n", r.batch.c_str(), r.marker.c_str(), r.kind.c_str(),
              r.depthUsed ? 1 : 0);
      for(int p = 0; p < NumProbes; p++)
      {
        uint32_t depthBits;
        memcpy(&depthBits, &r.probes[p].depth, sizeof(depthBits));
        fprintf(f, "P\t%d\t%d\t%d\t%08x\t%08x\t%d\n", Probes[p][0], Probes[p][1],
                r.probes[p].covered, r.probes[p].rgba, depthBits, r.probes[p].stencil);
      }
    }
    fclose(f);
    TEST_LOG("Wrote %zu events to %s", records.size(), path.c_str());
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    const D3DFORMAT INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
    if(FAILED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                     D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, INTZ)))
    {
      TEST_ERROR("INTZ depth stencil textures aren't supported");
      return 4;
    }

    PixelHistory::init();

    // the shared vertices, then a triangle for the indexed draw with a base vertex, around (200, 40)
    std::vector<DefaultA2V> verts = PixelHistory::vb;
    const UINT indexedFirst = (UINT)verts.size();
    verts.push_back({Vec3f(-0.05f, 0.75f, 0.4f), Vec4f(0.3f, 0.6f, 0.9f, 1.0f), Vec2f()});
    verts.push_back({Vec3f(0.0f, 0.83f, 0.4f), Vec4f(0.3f, 0.6f, 0.9f, 1.0f), Vec2f()});
    verts.push_back({Vec3f(0.05f, 0.75f, 0.4f), Vec4f(0.3f, 0.6f, 0.9f, 1.0f), Vec2f()});
    IDirect3DVertexBuffer9Ptr vb = MakeVB(verts.data(), UINT(sizeof(DefaultA2V) * verts.size()));

    IDirect3DIndexBuffer9Ptr ib;
    CHECK_HR(dev->CreateIndexBuffer(sizeof(uint16_t) * 3, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib,
                                    NULL));
    {
      uint16_t *idx = NULL;
      CHECK_HR(ib->Lock(0, 0, (void **)&idx, 0));
      idx[0] = 0;
      idx[1] = 1;
      idx[2] = 2;
      ib->Unlock();
    }

    // 300 Instances: D3D9 instances indexed draws, with a stream of per-instance data the shader
    // doesn't read
    const float instanceData[300][4] = {};
    IDirect3DVertexBuffer9Ptr instanceVB = MakeVB(instanceData, sizeof(instanceData));
    IDirect3DVertexDeclaration9Ptr instanceDecl;
    {
      const D3DVERTEXELEMENT9 elems[] = {
          {0, offsetof(DefaultA2V, pos), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
           D3DDECLUSAGE_POSITION, 0},
          {0, offsetof(DefaultA2V, col), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
           D3DDECLUSAGE_COLOR, 0},
          {0, offsetof(DefaultA2V, uv), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
           D3DDECLUSAGE_TEXCOORD, 0},
          {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
          D3DDECL_END(),
      };
      CHECK_HR(dev->CreateVertexDeclaration(elems, &instanceDecl));
    }

    IDirect3DVertexShader9Ptr vs = CreateVS(Compile(common + vertex, "main", "vs_3_0"));
    IDirect3DPixelShader9Ptr ps = CreatePS(Compile(common + pixel, "main", "ps_3_0"));
    IDirect3DVertexShader9Ptr vs2 = CreateVS(Compile(vertex2, "main", "vs_2_0"));

    quadVS = CreateVS(Compile(quadVertex, "main", "vs_3_0"));
    whitePS2 = CreatePS(Compile(whitePixel, "main", "ps_2_0"));
    whitePS3 = CreatePS(Compile(whitePixel, "main", "ps_3_0"));
    depthPS = CreatePS(Compile(depthPixel, "main", "ps_3_0"));
    bitPS = CreatePS(Compile(bitPixel, "main", "ps_3_0"));
    {
      const D3DVERTEXELEMENT9 elems[] = {
          {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          D3DDECL_END(),
      };
      CHECK_HR(dev->CreateVertexDeclaration(elems, &quadDecl));
    }

    // only the native run reads anything back
    if(!rdoc)
    {
      CHECK_HR(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &occlusion));
      CHECK_HR(dev->CreateStateBlock(D3DSBT_ALL, &saved));
      CHECK_HR(dev->CreateOffscreenPlainSurface(screenWidth, screenHeight, D3DFMT_A8R8G8B8,
                                                D3DPOOL_SYSTEMMEM, &colourSys, NULL));
      CHECK_HR(dev->CreateRenderTarget(screenWidth, screenHeight, D3DFMT_R32F, D3DMULTISAMPLE_NONE,
                                       0, FALSE, &depthRT, NULL));
      CHECK_HR(dev->CreateOffscreenPlainSurface(screenWidth, screenHeight, D3DFMT_R32F,
                                                D3DPOOL_SYSTEMMEM, &depthSys, NULL));
      CHECK_HR(dev->CreateRenderTarget(screenWidth, screenHeight, D3DFMT_A8R8G8B8,
                                       D3DMULTISAMPLE_NONE, 0, FALSE, &stencilRT, NULL));
      CHECK_HR(dev->CreateOffscreenPlainSurface(screenWidth, screenHeight, D3DFMT_A8R8G8B8,
                                                D3DPOOL_SYSTEMMEM, &stencilSys, NULL));
    }

    // the sources of the copies: yellow for StretchRect, magenta for UpdateSurface
    IDirect3DSurface9Ptr stretchSrc;
    CHECK_HR(dev->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                     &stretchSrc, NULL));
    CHECK_HR(dev->ColorFill(stretchSrc, NULL, D3DCOLOR_ARGB(255, 230, 220, 20)));
    IDirect3DSurface9Ptr updateSrc;
    CHECK_HR(dev->CreateOffscreenPlainSurface(10, 10, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                              &updateSrc, NULL));
    {
      D3DLOCKED_RECT locked = {};
      CHECK_HR(updateSrc->LockRect(&locked, NULL, 0));
      for(int y = 0; y < 10; y++)
        for(int x = 0; x < 10; x++)
          ((uint32_t *)((byte *)locked.pBits + y * locked.Pitch))[x] =
              D3DCOLOR_ARGB(200, 210, 30, 200);
      updateSrc->UnlockRect();
    }

    // pre-transformed, around (355, 60)
    const FFVertex ffQuad[4] = {
        {340.0f, 50.0f, 0.5f, 1.0f, D3DCOLOR_ARGB(255, 40, 120, 250)},
        {370.0f, 50.0f, 0.5f, 1.0f, D3DCOLOR_ARGB(255, 40, 120, 250)},
        {340.0f, 70.0f, 0.5f, 1.0f, D3DCOLOR_ARGB(255, 40, 120, 250)},
        {370.0f, 70.0f, 0.5f, 1.0f, D3DCOLOR_ARGB(255, 40, 120, 250)},
    };

    std::vector<Batch> batches;
    for(int b = 0; b < 2; b++)
    {
      Batch batch;
      batch.name = b == 0 ? "Basic" : "Colour Only";
      CHECK_HR(dev->CreateTexture(screenWidth, screenHeight, 1, D3DUSAGE_RENDERTARGET,
                                  D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &batch.colTex, NULL));
      CHECK_HR(batch.colTex->GetSurfaceLevel(0, &batch.col));
      if(b == 0)
      {
        CHECK_HR(dev->CreateTexture(screenWidth, screenHeight, 1, D3DUSAGE_DEPTHSTENCIL, INTZ,
                                    D3DPOOL_DEFAULT, &batch.depthTex, NULL));
        CHECK_HR(batch.depthTex->GetSurfaceLevel(0, &batch.depth));
      }
      batches.push_back(batch);
    }

    // D3D11_Pixel_History's states
    State base;
    base.vs = vs;
    base.ps = ps;

    State depthWriteState = base;

    State depthEqualState = base;
    depthEqualState.zwrite = FALSE;
    depthEqualState.zfunc = D3DCMP_EQUAL;

    State scissorState = base;
    scissorState.zwrite = FALSE;
    scissorState.zenable = FALSE;

    State stencilRefState = scissorState;
    stencilRefState.ref = 0x67;

    State stencilMaskState = scissorState;
    stencilMaskState.readMask = 0;
    stencilMaskState.writeMask = 0;

    State depthState = base;
    depthState.zfunc = D3DCMP_LESSEQUAL;

    State stencilWriteState = depthState;
    stencilWriteState.stencil = TRUE;

    State backgroundState = depthState;

    State noPsState = depthState;
    noPsState.vs = vs2;
    noPsState.ps = NULL;
    noPsState.vsModel = 2;
    noPsState.stencil = TRUE;
    noPsState.ref = 0x33;

    State basicState = depthState;
    basicState.stencil = TRUE;
    basicState.stencilFunc = D3DCMP_GREATER;

    State colourMaskState = depthState;
    colourMaskState.colourWrite = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN;

    State cullFrontState = depthState;
    cullFrontState.cull = D3DCULL_CW;

    State sampleColourState = depthState;

    // fixed function vertex and pixel processing
    State ffState = depthState;
    ffState.vs = NULL;
    ffState.ps = NULL;
    ffState.vsModel = 0;

    auto drawList = [&](const PixelHistory::draw &d) {
      dev->SetVertexDeclaration(defaultDecl);
      dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
      dev->DrawPrimitive(D3DPT_TRIANGLELIST, d.first, d.count / 3);
    };

    const D3DVIEWPORT9 viewport = {10,   10,  (DWORD)screenWidth - 20, (DWORD)screenHeight - 20,
                                   0.0f, 1.0f};
    const RECT scissor = {0, 0, screenWidth, screenHeight};
    const RECT scissorPass = {95, 245, 115, 253};
    const RECT scissorFail = {95, 245, 103, 253};

    while(Running())
    {
      // the third frame natively, when every frame starts from the one before
      recording = !rdoc && curFrame == 2;
      records.clear();

      dev->BeginScene();

      for(Batch &b : batches)
      {
        curBatch = &b;

        pushMarker("Batch: " + b.name);

        dev->SetRenderTarget(0, b.col);
        dev->SetDepthStencilSurface(b.depth);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

        if(recording)
        {
          Record start;
          start.batch = b.name;
          start.marker = "";
          start.kind = "start";
          ReadProbes(start.probes);
          records.push_back(start);
        }

        // D3D11 clears the render target and the depth stencil separately
        Marker("Begin RenderPass");
        Event("clear", [&]() {
          dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);
        });
        if(b.depth)
          Event("clear",
                [&]() { dev->Clear(0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0); });

        dev->SetViewport(&viewport);
        dev->SetScissorRect(&scissor);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

        pushMarker("Setup");
        {
          Marker("Depth Write");
          SetState(depthWriteState);
          Event("draw", [&]() { drawList(PixelHistory::DepthWrite); });

          Marker("Depth Equal Setup");
          Event("draw", [&]() { drawList(PixelHistory::DepthEqualSetup); });

          Marker("Unbound Shader");
          SetState(noPsState);
          Event("draw", [&]() { drawList(PixelHistory::UnboundPS); });

          Marker("Stencil Write");
          SetState(stencilWriteState);
          Event("draw", [&]() { drawList(PixelHistory::StencilWrite); });

          Marker("Background");
          SetState(backgroundState);
          Event("draw", [&]() { drawList(PixelHistory::Background); });

          Marker("Cull Front");
          SetState(cullFrontState);
          Event("draw", [&]() { drawList(PixelHistory::CullFront); });

          // D3D9 has no depth bounds test
        }
        popMarker();

        pushMarker("Stress Test");
        {
          pushMarker("Lots of Drawcalls");
          {
            Marker("300 Draws");
            SetState(depthWriteState);
            for(int d = 0; d < 300; ++d)
              Event("draw", [&]() { drawList(PixelHistory::Draws300); });
          }
          popMarker();

          Marker("300 Instances");
          Event("draw", [&]() {
            dev->SetVertexDeclaration(instanceDecl);
            dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
            dev->SetStreamSource(1, instanceVB, 0, sizeof(float) * 4);
            dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 300);
            dev->SetStreamSourceFreq(1, UINT(D3DSTREAMSOURCE_INSTANCEDATA | 1));
            dev->SetIndices(ib);
            dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, PixelHistory::Instances300.first, 0, 3, 0,
                                      1);
            dev->SetStreamSourceFreq(0, 1);
            dev->SetStreamSourceFreq(1, 1);
            dev->SetStreamSource(1, NULL, 0, 0);
          });
        }
        popMarker();

        Marker("Simple Test");
        SetState(basicState);
        Event("draw", [&]() { drawList(PixelHistory::MainTest); });

        SetState(scissorState);

        Marker("Scissor Fail");
        dev->SetScissorRect(&scissorFail);
        Event("draw", [&]() { drawList(PixelHistory::ScissorFail); });

        Marker("Scissor Pass");
        dev->SetScissorRect(&scissorPass);
        Event("draw", [&]() { drawList(PixelHistory::ScissorPass); });

        dev->SetScissorRect(&scissor);

        Marker("Stencil Ref");
        SetState(stencilRefState);
        Event("draw", [&]() { drawList(PixelHistory::StencilRef); });

        Marker("Stencil Mask");
        SetState(stencilMaskState);
        Event("draw", [&]() { drawList(PixelHistory::StencilMask); });

        Marker("Depth Test");
        SetState(depthState);
        Event("draw", [&]() { drawList(PixelHistory::DepthTest); });

        // D3D9 has no sample index, so this is drawn with the usual shader
        Marker("Sample Colouring");
        SetState(sampleColourState);
        Event("draw", [&]() { drawList(PixelHistory::SampleColour); });

        Marker("Depth Equal Fail");
        SetState(depthEqualState);
        Event("draw", [&]() { drawList(PixelHistory::DepthEqualFail); });

        // INTZ has D24's depth
        Marker("Depth Equal Pass");
        SetState(depthEqualState);
        Event("draw", [&]() { drawList(PixelHistory::DepthEqualPass24); });

        Marker("Colour Masked");
        SetState(colourMaskState);
        Event("draw", [&]() { drawList(PixelHistory::ColourMask); });

        Marker("Overflowing");
        SetState(backgroundState);
        Event("draw", [&]() { drawList(PixelHistory::OverflowingDraw); });

        Marker("Per-Fragment discarding");
        SetState(backgroundState);
        Event("draw", [&]() { drawList(PixelHistory::PerFragDiscard); });

        // D3D9 has no pixel shaders without output, so no "No Output Shader"

        pushMarker("D3D9 Writes");
        {
          Marker("Clear Rects");
          Event("clear", [&]() {
            dev->Clear(2, ClearRects, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 230, 25, 25), 1.0f, 0);
          });

          Marker("Clear Viewport Scissor");
          dev->SetViewport(&ClearViewport);
          dev->SetScissorRect(&ClearScissor);
          Event("clear", [&]() {
            dev->Clear(0, NULL, D3DCLEAR_TARGET | (b.depth ? D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL : 0),
                       D3DCOLOR_ARGB(255, 25, 230, 25), 0.25f, 0x42);
          });
          dev->SetViewport(&viewport);
          dev->SetScissorRect(&scissor);

          Marker("ColorFill");
          Event("clear",
                [&]() { dev->ColorFill(b.col, &FillRect, D3DCOLOR_ARGB(255, 25, 230, 230)); });

          Marker("StretchRect");
          Event("copy", [&]() {
            const RECT src = {0, 0, 20, 20};
            dev->StretchRect(stretchSrc, &src, b.col, &StretchDest, D3DTEXF_POINT);
          });

          Marker("UpdateSurface");
          Event("copy", [&]() {
            HRESULT hr = dev->UpdateSurface(updateSrc, NULL, b.col, &UpdateDest);
            if(FAILED(hr))
              TEST_ERROR("UpdateSurface into the render target failed: %x", hr);
          });

          Marker("Fixed Function");
          SetState(ffState);
          Event("draw", [&]() {
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, ffQuad, sizeof(FFVertex));
          });

          Marker("Indexed");
          SetState(backgroundState);
          Event("draw", [&]() {
            dev->SetVertexDeclaration(defaultDecl);
            dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
            dev->SetIndices(ib);
            dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, indexedFirst, 0, 3, 0, 1);
          });
        }
        popMarker();

        popMarker();
      }

      dev->EndScene();

      if(recording)
        WriteReference();

      // the basic batch on screen
      IDirect3DSurface9Ptr bb;
      dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
      dev->StretchRect(batches[0].col, NULL, bb, NULL, D3DTEXF_POINT);

      Present();
    }

    curBatch = NULL;

    return 0;
  }
};

REGISTER_TEST();
