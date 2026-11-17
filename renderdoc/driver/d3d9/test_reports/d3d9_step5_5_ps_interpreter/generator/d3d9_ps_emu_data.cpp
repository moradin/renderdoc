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

// a temporary generator for the pixel shader interpreter's unit tests (step 5.5): compiles HLSL
// pixel shaders with D3DCompile for ps_2_0, ps_2_a (ps_2_x) and ps_3_0, runs each natively on a
// 4x4 float target, reads back the inputs each pixel saw (with a shader declaring the same inputs,
// which step 5.4 found reads them bit for bit) and the outputs, and writes them all to a header
// for d3d9_ps_emulate.cpp. Writes to GEN_OUT.

#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "d3d9_test.h"

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);
typedef HRESULT(WINAPI *pD3DCompile)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                     const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                     LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
                                     ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

RD_TEST(D3D9_PS_Emu_Data, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Generates the pixel shader interpreter's test data";

  static const UINT W = 4, H = 4, NumPixels = W * H;

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

  pD3DAssemble assemble = NULL;
  pD3DCompile compile = NULL;
  FILE *out = NULL;
  IDirect3DVertexDeclaration9Ptr decl;
  IDirect3DVertexShader9Ptr vs2, vs3;
  IDirect3DSurface9Ptr rt[4];
  IDirect3DTexture9Ptr tex;
  float sentinel = 0.0f;

  void P(const char *fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
  }

  static uint32_t Bits(float f)
  {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
  }

  std::vector<DWORD> Blob(ID3DBlob * b)
  {
    const DWORD *p = (const DWORD *)b->GetBufferPointer();
    return std::vector<DWORD>(p, p + b->GetBufferSize() / 4);
  }

  std::vector<DWORD> Assemble(const std::string &src)
  {
    ID3DBlobPtr code, errors;
    if(FAILED(assemble(src.c_str(), src.size(), NULL, NULL, NULL, 0, &code, &errors)) || !code)
    {
      TEST_LOG("assemble failed: %s", errors ? (const char *)errors->GetBufferPointer() : "?");
      return {};
    }
    return Blob(code);
  }

  std::vector<DWORD> Compile(const std::string &src, const char *profile)
  {
    ID3DBlobPtr code, errors;
    if(FAILED(compile(src.c_str(), src.size(), NULL, NULL, NULL, "main", profile, 0, 0, &code,
                      &errors)) ||
       !code)
    {
      std::string err = errors ? (const char *)errors->GetBufferPointer() : "?";
      for(char &c : err)
        if(c == '\n')
          c = ' ';
      TEST_LOG("compile %s failed: %s", profile, err.c_str());
      return {};
    }
    return Blob(code);
  }

  // the texture the shaders sample: 16x16, one level, texel (x, y) = (x, y, x * y / 8, 1 - x / 16)
  void MakeTexture()
  {
    dev->CreateTexture(16, 16, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &tex, NULL);
    D3DLOCKED_RECT lr = {};
    tex->LockRect(0, &lr, NULL, 0);
    for(UINT y = 0; y < 16; y++)
    {
      float *row = (float *)((byte *)lr.pBits + y * lr.Pitch);
      for(UINT x = 0; x < 16; x++)
      {
        row[x * 4 + 0] = float(x);
        row[x * 4 + 1] = float(y);
        row[x * 4 + 2] = float(x * y) / 8.0f;
        row[x * 4 + 3] = 1.0f - float(x) / 16.0f;
      }
    }
    tex->UnlockRect(0);
  }

  void ResetState()
  {
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xf);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE2, 0xf);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE3, 0xf);
    dev->SetTexture(0, tex);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    for(int i = 0; i < 4; i++)
      dev->SetRenderTarget(i, rt[i]);
    dev->SetDepthStencilSurface(NULL);
    D3DVIEWPORT9 vp = {0, 0, W, H, 0.0f, 1.0f};
    dev->SetViewport(&vp);

    // the constants the shaders read
    const float k[8][4] = {
        {1.5f, -0.75f, 2.25f, 0.5f},      {0.1f, 0.2f, -0.3f, 0.4f},    {0.8f, -0.6f, 0.0f, 0.3f},
        {0.6f, 0.8f, 0.1f, -0.2f},        {-0.25f, 0.5f, 0.9f, 1.1f},   {0.05f, -1.2f, 0.4f, 2.0f},
        {3.0f, 1.0f / 3.0f, -7.5f, 0.0f}, {-1.0f, 0.125f, 6.0f, -2.5f},
    };
    dev->SetPixelShaderConstantF(0, &k[0][0], 8);
    const int n[4] = {3, 0, 1, 0};
    dev->SetPixelShaderConstantI(0, n, 1);
    const BOOL b[2] = {TRUE, FALSE};
    dev->SetPixelShaderConstantB(0, b, 2);
  }

  // one triangle covering the target: screen (0, 0), (8, 0) and (0, 8), with every input an
  // affine function of the screen position. t3 puts the pixel centres at texel centres of the
  // 16x16 texture, with w = 2 for texldp.
  static Vert At(float sx, float sy)
  {
    Vert v = {};
    v.pos = {sx * 2.0f / W - 1.0f, 1.0f - sy * 2.0f / H, 0.5f, 1.0f};
    v.t[0] = {0.37f * sx - 1.21f, -0.53f * sy + 0.77f, 0.29f * sx + 0.41f * sy - 1.13f,
              1.7f - 0.61f * sx};
    v.t[1] = {-0.83f * sx + 1.4f, 0.71f * sy - 1.9f, 0.23f * (sx - sy) + 0.5f, 2.3f - 0.35f * sy};
    v.t[2] = {0.15f + 0.2f * sx, 0.3f + 0.17f * sy, 0.05f + 0.31f * sx + 0.07f * sy,
              0.9f - 0.08f * sx};
    v.t[3] = {(sx + 0.125f) / 4.0f, (sy + 0.125f) / 4.0f, 0.5f, 2.0f};
    v.c[0] = {0.1f + 0.2f * sx, 0.9f - 0.15f * sy, 0.5f + 0.1f * (sx - sy), 0.75f};
    v.c[1] = {0.3f * sx - 0.4f, 0.2f + 0.1f * sy, 1.3f - 0.2f * sx, 0.6f};
    return v;
  }

  void Draw(IDirect3DPixelShader9 * ps, bool sm3, bool ccw)
  {
    Vert v[3] = {At(0, 0), At(8, 0), At(0, 8)};
    // clockwise on screen, or not
    if(ccw)
      std::swap(v[1], v[2]);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(7, 7, 7, 7), 1.0f, 0);
    dev->SetVertexDeclaration(decl);
    dev->SetVertexShader(sm3 ? vs3 : vs2);
    dev->SetPixelShader(ps);
    dev->BeginScene();
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(Vert));
    dev->EndScene();
  }

  std::vector<V4> Read(int target)
  {
    std::vector<V4> ret(NumPixels);
    IDirect3DSurface9Ptr read;
    dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &read, NULL);
    D3DLOCKED_RECT lr = {};
    if(FAILED(dev->GetRenderTargetData(rt[target], read)) ||
       FAILED(read->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
      TEST_LOG("readback failed");
      return ret;
    }
    for(UINT y = 0; y < H; y++)
      memcpy(&ret[y * W], (const byte *)lr.pBits + y * lr.Pitch, W * sizeof(V4));
    read->UnlockRect();
    return ret;
  }

  IDirect3DPixelShader9Ptr Shader(const std::vector<DWORD> &code)
  {
    IDirect3DPixelShader9Ptr ps;
    if(code.empty() || FAILED(dev->CreatePixelShader(code.data(), &ps)))
      TEST_LOG("CreatePixelShader failed");
    return ps;
  }

  void Array(const char *name, const std::vector<uint32_t> &v)
  {
    P("static const uint32_t %s[] = {", name);
    for(size_t i = 0; i < v.size(); i++)
      P("%s0x%08x,", (i % 8) == 0 ? "\n    " : " ", v[i]);
    P("\n};\n\n");
  }

  // reads back each pixel's TEXCOORD0-3 and COLOR0-1, and for ps_3_0 vFace, as the shaders see
  // them: [pixel][6][4], then the face per pixel
  void Inputs(const char *name, bool sm3, bool ccw)
  {
    std::vector<uint32_t> data(NumPixels * 6 * 4);
    std::vector<uint32_t> face(NumPixels, Bits(1.0f));
    std::string tc =
        sm3 ? "ps_3_0\ndcl_texcoord0 v0\ndcl_texcoord1 v1\ndcl_texcoord2 v2\ndcl_texcoord3 "
              "v3\nmov oC0, v0\nmov oC1, v1\nmov oC2, v2\nmov oC3, v3\n"
            : "ps_2_0\ndcl t0\ndcl t1\ndcl t2\ndcl t3\nmov oC0, t0\nmov oC1, t1\nmov oC2, t2\nmov "
              "oC3, t3\n";
    std::string col =
        sm3 ? "ps_3_0\ndef c0, 1, -1, 0, 0\ndcl_color0 v0\ndcl_color1 v1\ndcl vFace\nmov oC0, "
              "v0\nmov oC1, v1\nmov r0, c0.x\nif_lt vFace, c0.z\nmov r0, c0.y\nendif\nmov oC2, r0\n"
            : "ps_2_0\ndcl v0\ndcl v1\nmov oC0, v0\nmov oC1, v1\n";
    IDirect3DPixelShader9Ptr tcps = Shader(Assemble(tc)), colps = Shader(Assemble(col));
    Draw(tcps, sm3, ccw);
    for(int t = 0; t < 4; t++)
    {
      std::vector<V4> v = Read(t);
      for(UINT p = 0; p < NumPixels; p++)
        memcpy(&data[(p * 6 + t) * 4], &v[p], 16);
    }
    Draw(colps, sm3, ccw);
    for(int t = 0; t < 2; t++)
    {
      std::vector<V4> v = Read(t);
      for(UINT p = 0; p < NumPixels; p++)
        memcpy(&data[(p * 6 + 4 + t) * 4], &v[p], 16);
    }
    if(sm3)
    {
      std::vector<V4> v = Read(2);
      for(UINT p = 0; p < NumPixels; p++)
        face[p] = Bits(v[p].x);
    }
    Array(name, data);
    Array((std::string(name) + "_face").c_str(), face);
  }

  struct Source
  {
    const char *name;
    // which of ps_2_0, ps_2_a and ps_3_0 it compiles for
    bool profiles[3];
    uint32_t numTargets;
    const char *hlsl;
  };

  int main()
  {
    if(!Init())
      return 3;

    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    assemble = d3dcompiler ? (pD3DAssemble)GetProcAddress(d3dcompiler, "D3DAssemble") : NULL;
    compile = d3dcompiler ? (pD3DCompile)GetProcAddress(d3dcompiler, "D3DCompile") : NULL;
    if(!assemble || !compile)
      return 4;

    const char *o = getenv("GEN_OUT");
    out = fopen(o ? o : "d3d9_ps_test_data.h", "w");
    if(!out)
      return 5;

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
      return 6;
    code = Assemble(
        "vs_3_0\ndcl_position v0\ndcl_texcoord0 v1\ndcl_texcoord1 v2\ndcl_texcoord2 "
        "v3\ndcl_texcoord3 v4\ndcl_color0 v5\ndcl_color1 v6\ndcl_position o0\ndcl_texcoord0 "
        "o1\ndcl_texcoord1 o2\ndcl_texcoord2 o3\ndcl_texcoord3 o4\ndcl_color0 o5\ndcl_color1 "
        "o6\nmov o0, v0\nmov o1, v1\nmov o2, v2\nmov o3, v3\nmov o4, v4\nmov o5, v5\nmov o6, v6\n");
    if(code.empty() || FAILED(dev->CreateVertexShader(code.data(), &vs3)))
      return 7;

    for(int i = 0; i < 4; i++)
      dev->CreateRenderTarget(W, H, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt[i],
                              NULL);
    MakeTexture();
    ResetState();

    // what a cleared target holds: a pixel holding it in every component of every target was
    // discarded
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(7, 7, 7, 7), 1.0f, 0);
    sentinel = Read(0)[0].x;

    P("// Generated by the D3D9_PS_Emu_Data generator in\n"
      "// test_reports/d3d9_step5_5_ps_interpreter/generator, on %s (driver %u.%u.%u.%u).\n"
      "// The HLSL shaders below were compiled by D3DCompile (d3dcompiler_47) and run natively on "
      "a\n"
      "// %ux%u A32B32G32R32F target covered by one triangle, with the inputs each pixel saw "
      "read\n"
      "// back by a shader declaring the same inputs. Pixels are in rows, top to bottom.\n\n",
      ident.Description, HIWORD(ident.DriverVersion.HighPart), LOWORD(ident.DriverVersion.HighPart),
      HIWORD(ident.DriverVersion.LowPart), LOWORD(ident.DriverVersion.LowPart), W, H);
    P("#pragma once\n\nnamespace PSTestData\n{\nconst uint32_t Width = %u, Height = %u;\n\n", W, H);
    P("// what a discarded pixel's targets hold\nconst uint32_t Discarded = 0x%08x;\n\n",
      Bits(sentinel));
    P("// c0-c7, i0, b0 and b1\n"
      "const float FloatConsts[8][4] = {\n"
      "    {1.5f, -0.75f, 2.25f, 0.5f},     {0.1f, 0.2f, -0.3f, 0.4f},    {0.8f, -0.6f, 0.0f, "
      "0.3f},\n"
      "    {0.6f, 0.8f, 0.1f, -0.2f},       {-0.25f, 0.5f, 0.9f, 1.1f},   {0.05f, -1.2f, 0.4f, "
      "2.0f},\n"
      "    {3.0f, 1.0f / 3.0f, -7.5f, 0.0f}, {-1.0f, 0.125f, 6.0f, -2.5f},\n};\n"
      "const int32_t IntConsts[4] = {3, 0, 1, 0};\n"
      "const uint32_t BoolConsts[2] = {1, 0};\n\n");
    P("// s0: 16x16 A32B32G32R32F, one level, point sampled with wrap addressing\n"
      "inline void Texel(uint32_t x, uint32_t y, float *texel)\n{\n"
      "  texel[0] = float(x);\n  texel[1] = float(y);\n  texel[2] = float(x * y) / 8.0f;\n"
      "  texel[3] = 1.0f - float(x) / 16.0f;\n}\n\n");
    P("// each pixel's TEXCOORD0-3 and COLOR0-1, as float bits, and its vFace. The ps_2_x inputs "
      "come\n// through a vs_2_0, which clamps the colours; the ps_3_0 ones through a vs_3_0, "
      "drawn clockwise\n// and counter-clockwise.\n");
    Inputs("InputsSM2", false, false);
    Inputs("InputsSM3", true, false);
    Inputs("InputsSM3CCW", true, true);

    const std::string common = R"(
struct PSIn
{
  float4 t0 : TEXCOORD0;
  float4 t1 : TEXCOORD1;
  float4 t2 : TEXCOORD2;
  float4 t3 : TEXCOORD3;
  float4 c0 : COLOR0;
  float4 c1 : COLOR1;
};
struct PSOut
{
  float4 o0 : COLOR0;
  float4 o1 : COLOR1;
  float4 o2 : COLOR2;
  float4 o3 : COLOR3;
};
float4 k[8] : register(c0);
int4 n : register(i0);
bool b0 : register(b0);
bool b1 : register(b1);
sampler2D s0 : register(s0);
)";

    const Source sources[] = {
        {"arith",
         {true, true, true},
         4,
         R"(
PSOut main(PSIn i)
{
  PSOut o;
  o.o0 = i.t0 * k[0] + i.t1;
  o.o0.w = dot(i.t0.xyz, i.t2.xyz);
  o.o1 = float4(normalize(i.t1.xyz), dot(i.t0, i.t1));
  o.o2 = float4(cross(i.t0.xyz, i.t2.xyz), dot(i.t0.xy, i.t1.zw) + i.t2.w);
  o.o3 = lerp(i.t0, i.t1, i.c0.x) + frac(i.t2 * 3.7) + min(i.t0, i.t1) - max(i.t2, k[1]) +
         saturate(i.t0 * 0.5) + abs(i.t1) * (i.t0 > i.t1.yxwz ? i.c1 : i.c0);
  return o;
}
)"},
        {"transcendental",
         {true, true, true},
         3,
         R"(
PSOut main(PSIn i)
{
  PSOut o;
  o.o0 = float4(1.0 / i.t0.x, rsqrt(abs(i.t1.y)), exp2(i.t0.z), log2(i.t2.x));
  o.o1 = float4(pow(i.t2.y, i.t1.w), exp(i.t1.x), log(i.t2.z), sqrt(i.t2.w));
  o.o2 = float4(sin(i.t1.x * 2.5), cos(i.t0.y * 3.0), length(i.t1.xyz), distance(i.t0.xy, i.t1.xy));
  o.o3 = 0;
  return o;
}
)"},
        {"trig",
         {false, true, true},
         2,
         R"(
PSOut main(PSIn i)
{
  PSOut o;
  o.o0 = float4(sin(i.t1.z), cos(i.t0.w), tan(i.t0.x), atan2(i.t0.y, i.t1.x));
  o.o1 = float4(asin(i.c0.x), acos(i.c0.y - 0.5), sinh(i.t1.y), cosh(i.t0.z));
  o.o2 = 0;
  o.o3 = 0;
  return o;
}
)"},
        {"matrix",
         {true, true, true},
         2,
         R"(
float4x4 m : register(c2);
PSOut main(PSIn i)
{
  PSOut o;
  o.o0 = mul(i.t0, m);
  o.o1 = float4(mul(i.t1.xyz, (float3x3)m), mul(i.t2, (float4x2)m).x);
  o.o2 = 0;
  o.o3 = 0;
  return o;
}
)"},
        {"staticflow",
         {false, false, true},
         2,
         R"(
PSOut main(PSIn i)
{
  PSOut o;
  float4 acc = i.t2;
  [loop] for(int j = 0; j < n.x; j++)
    acc = acc * k[0] + i.t0;
  if(b0)
    acc.z += 1;
  if(b1)
    acc.w += 10;
  o.o0 = acc;
  o.o1 = i.t0.x > i.t1.x ? i.t1 : i.c0;
  o.o2 = 0;
  o.o3 = 0;
  return o;
}
)"},
        {"flow",
         {false, false, true},
         2,
         R"(
PSOut main(PSIn i)
{
  PSOut o;
  float4 acc = 0;
  [loop] for(int j = 0; j < n.x; j++)
  {
    acc += i.t0 * (float)j + i.t2;
    [branch] if(acc.x > i.t1.x)
      break;
  }
  [branch] if(i.t2.x > 0.4)
    acc.y = i.t2.y * 3;
  else
    acc.y = i.c0.z;
  if(b0)
    acc.z += 1;
  if(b1)
    acc.z += 10;
  o.o0 = acc;
  o.o1 = acc.x > 0 ? i.t1 : -i.t1;
  o.o2 = 0;
  o.o3 = 0;
  return o;
}
)"},
        {"deriv",
         {false, true, true},
         2,
         R"(
PSOut main(PSIn i)
{
  PSOut o;
  o.o0 = ddx(i.t0 * i.t1.x);
  o.o1 = ddy(i.t2 * i.t0.y) + ddx(i.c0);
  o.o2 = 0;
  o.o3 = 0;
  return o;
}
)"},
        {"texture",
         {true, true, true},
         4,
         R"(
PSOut main(PSIn i)
{
  PSOut o;
  o.o0 = tex2D(s0, i.t3.xy);
  o.o1 = tex2Dproj(s0, i.t3);
  o.o2 = tex2Dbias(s0, float4(i.t3.xy, 0, i.t1.x));
  float4 d = tex2D(s0, i.t3.yx);
  o.o3 = tex2D(s0, (d.xy + 0.5) / 16) * k[0];
  return o;
}
)"},
        {"texgrad",
         {false, true, true},
         1,
         R"(
float4 main(PSIn i) : COLOR0
{
  return tex2Dgrad(s0, i.t3.xy, ddx(i.t0.xy), ddy(i.t1.xy) * 3);
}
)"},
        {"texlod",
         {false, false, true},
         1,
         R"(
float4 main(PSIn i) : COLOR0
{
  return tex2Dlod(s0, float4(i.t3.xy, 0, i.t2.x));
}
)"},
        {"clip",
         {true, true, true},
         1,
         R"(
float4 main(PSIn i) : COLOR0
{
  clip(i.t0.x + i.t1.y + 2.5);
  clip(i.t2 - 0.3);
  return i.t0;
}
)"},
        {"vface",
         {false, false, true},
         2,
         R"(
void main(PSIn i, float face : VFACE, float2 p : VPOS, out float4 o0 : COLOR0, out float4 o1 : COLOR1)
{
  o0 = face > 0 ? i.t0 + p.xyxy : i.t1 - p.yxyx;
  o1 = ddx(p.x * p.y * i.t0) + ddy(p.xyxy);
}
)"},
    };

    const char *profiles[3] = {"ps_2_0", "ps_2_a", "ps_3_0"};

    P("struct Case\n{\n  const char *name;\n  const char *profile;\n  const char *hlsl;\n"
      "  const uint32_t *code;\n  uint32_t codeDWORDs;\n"
      "  // InputsSM2, InputsSM3 or InputsSM3CCW, and its _face\n"
      "  const uint32_t *inputs;\n  const uint32_t *face;\n"
      "  uint32_t numTargets;\n  // [pixel][target][4], as float bits\n"
      "  const uint32_t *outputs;\n};\n\n");

    std::string cases;
    for(const Source &s : sources)
    {
      std::string hlsl = common + s.hlsl;
      std::string escaped;
      for(char c : std::string(s.hlsl))
      {
        if(c == '\n')
          escaped += "\\n\"\n    \"";
        else if(c == '"')
          escaped += "\\\"";
        else
          escaped += c;
      }
      P("static const char %s_hlsl[] =\n    \"%s\";\n\n", s.name, escaped.c_str());

      for(int pr = 0; pr < 3; pr++)
      {
        if(!s.profiles[pr])
          continue;
        std::vector<DWORD> bc = Compile(hlsl, profiles[pr]);
        if(bc.empty())
          continue;
        IDirect3DPixelShader9Ptr ps = Shader(bc);
        if(!ps)
          continue;

        const bool sm3 = pr == 2;
        for(int ccw = 0; ccw < (strcmp(s.name, "vface") == 0 ? 2 : 1); ccw++)
        {
          std::string id = std::string(s.name) + "_" + profiles[pr] + (ccw ? "_ccw" : "");
          Draw(ps, sm3, ccw != 0);
          std::vector<uint32_t> outs(NumPixels * s.numTargets * 4);
          for(uint32_t t = 0; t < s.numTargets; t++)
          {
            std::vector<V4> v = Read(t);
            for(UINT p = 0; p < NumPixels; p++)
              memcpy(&outs[(p * s.numTargets + t) * 4], &v[p], 16);
          }
          if(ccw == 0)
            Array((id + "_code").c_str(), std::vector<uint32_t>(bc.begin(), bc.end()));
          Array((id + "_out").c_str(), outs);
          std::string codeName = std::string(s.name) + "_" + profiles[pr] + "_code";
          const char *inputs = !sm3 ? "InputsSM2" : (ccw ? "InputsSM3CCW" : "InputsSM3");
          char line[1024];
          snprintf(line, sizeof(line),
                   "    {\"%s\", \"%s\", %s_hlsl, %s, sizeof(%s) / 4, %s, %s_face, %u, %s_out},\n",
                   id.c_str(), profiles[pr], s.name, codeName.c_str(), codeName.c_str(), inputs,
                   inputs, s.numTargets, id.c_str());
          cases += line;
          TEST_LOG("%s: %zu DWORDs", id.c_str(), bc.size());
        }
      }
    }

    P("static const Case Cases[] = {\n%s};\n\n};    // namespace PSTestData\n", cases.c_str());
    fclose(out);
    out = NULL;
    TEST_LOG("done");
    return 0;
  }
};

REGISTER_TEST();
