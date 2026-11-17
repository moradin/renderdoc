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

// a temporary generator for the ps_1_x interpreter's unit tests (step 5.6): assembles ps_1_1 to
// ps_1_4 shaders with D3DAssemble, runs each natively on a 4x4 float target with a texture on each
// of stages 0-5, reads back the inputs each pixel saw (with a ps_2_0 shader declaring them) and
// r0, and writes them all to a header for d3d9_ps_emulate.cpp. Writes to GEN_OUT.

#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "d3d9_test.h"

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

RD_TEST(D3D9_PS1_Emu_Data, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Generates the ps_1_x interpreter's test data";

  static const UINT W = 4, H = 4, NumPixels = W * H, NumStages = 6, TexSize = 16;

  struct V4
  {
    float x, y, z, w;
  };

  struct Vert
  {
    V4 pos;
    V4 t[6];
    V4 c[2];
  };

  pD3DAssemble assemble = NULL;
  FILE *out = NULL;
  IDirect3DVertexDeclaration9Ptr decl;
  IDirect3DVertexShader9Ptr vs;
  IDirect3DSurface9Ptr rt[4];
  IDirect3DTexture9Ptr tex[NumStages];
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

  std::vector<DWORD> Assemble(const std::string &src)
  {
    ID3DBlobPtr code, errors;
    if(FAILED(assemble(src.c_str(), src.size(), NULL, NULL, NULL, 0, &code, &errors)) || !code)
    {
      std::string err = errors ? (const char *)errors->GetBufferPointer() : "?";
      for(char &c : err)
        if(c == '\n')
          c = ' ';
      TEST_LOG("assemble failed: %s", err.c_str());
      return {};
    }
    const DWORD *p = (const DWORD *)code->GetBufferPointer();
    return std::vector<DWORD>(p, p + code->GetBufferSize() / 4);
  }

// the texel each stage's texture holds, every value at a texel centre so that a texel read as a
// coordinate (texreg2ar, texbem's offsets) lands on one too. Written out as is in the header.
#define TEXEL_FUNCTION                                                    \
  "inline void Texel(uint32_t stage, uint32_t x, uint32_t y, float *t)\n" \
  "{\n"                                                                   \
  "  t[0] = float((x + 3 * stage) % 16) / 16.0f + 1.0f / 32.0f;\n"        \
  "  t[1] = float((y + 5 * stage) % 16) / 16.0f + 1.0f / 32.0f;\n"        \
  "  t[2] = float((x * y + 7 * stage) % 16) / 16.0f + 1.0f / 32.0f;\n"    \
  "  t[3] = float((15 - x + 2 * stage) % 16) / 16.0f + 1.0f / 32.0f;\n"   \
  "}\n"

  static void Texel(uint32_t stage, uint32_t x, uint32_t y, float *t)
  {
    t[0] = float((x + 3 * stage) % 16) / 16.0f + 1.0f / 32.0f;
    t[1] = float((y + 5 * stage) % 16) / 16.0f + 1.0f / 32.0f;
    t[2] = float((x * y + 7 * stage) % 16) / 16.0f + 1.0f / 32.0f;
    t[3] = float((15 - x + 2 * stage) % 16) / 16.0f + 1.0f / 32.0f;
  }

  // D3DTSS_BUMPENVMAT00, 01, 10, 11 per stage: even, so offsets of texel centres stay on them
  static void BumpMat(uint32_t stage, float *m)
  {
    m[0] = 2.0f;
    m[1] = -4.0f;
    m[2] = 2.0f * float(stage);
    m[3] = 4.0f;
  }
  static float LumScale(uint32_t stage)
  {
    return 2.0f + float(stage);
  }
  static float LumOffset(uint32_t stage)
  {
    return -0.75f;
  }

  void MakeTextures()
  {
    for(UINT s = 0; s < NumStages; s++)
    {
      dev->CreateTexture(TexSize, TexSize, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &tex[s],
                         NULL);
      D3DLOCKED_RECT lr = {};
      tex[s]->LockRect(0, &lr, NULL, 0);
      for(UINT y = 0; y < TexSize; y++)
      {
        float *row = (float *)((byte *)lr.pBits + y * lr.Pitch);
        for(UINT x = 0; x < TexSize; x++)
          Texel(s, x, y, row + x * 4);
      }
      tex[s]->UnlockRect(0);
    }
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
    for(UINT s = 0; s < NumStages; s++)
    {
      dev->SetTexture(s, tex[s]);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
      dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      float m[4];
      BumpMat(s, m);
      dev->SetTextureStageState(s, D3DTSS_BUMPENVMAT00, Bits(m[0]));
      dev->SetTextureStageState(s, D3DTSS_BUMPENVMAT01, Bits(m[1]));
      dev->SetTextureStageState(s, D3DTSS_BUMPENVMAT10, Bits(m[2]));
      dev->SetTextureStageState(s, D3DTSS_BUMPENVMAT11, Bits(m[3]));
      dev->SetTextureStageState(s, D3DTSS_BUMPENVLSCALE, Bits(LumScale(s)));
      dev->SetTextureStageState(s, D3DTSS_BUMPENVLOFFSET, Bits(LumOffset(s)));
      dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
    for(int i = 0; i < 4; i++)
      dev->SetRenderTarget(i, rt[i]);
    dev->SetDepthStencilSurface(NULL);
    D3DVIEWPORT9 vp = {0, 0, W, H, 0.0f, 1.0f};
    dev->SetViewport(&vp);

    // the constants the shaders read, some outside ps_1_x's [-1, 1]
    const float k[8][4] = {
        {1.5f, -0.75f, 2.25f, 0.5f},      {0.1f, 0.2f, -0.3f, 0.4f},    {0.8f, -0.6f, 0.0f, 0.3f},
        {0.6f, 0.8f, 0.1f, -0.2f},        {-0.25f, 0.5f, 0.9f, 1.1f},   {0.05f, -1.2f, 0.4f, 2.0f},
        {3.0f, 1.0f / 3.0f, -7.5f, 0.0f}, {-1.0f, 0.125f, 6.0f, -2.5f},
    };
    dev->SetPixelShaderConstantF(0, &k[0][0], 8);
  }

  // one triangle covering the target: screen (0, 0), (8, 0) and (0, 8), with every input an
  // affine function of the screen position
  static Vert At(float sx, float sy)
  {
    Vert v = {};
    v.pos = {sx * 2.0f / W - 1.0f, 1.0f - sy * 2.0f / H, 0.5f, 1.0f};
    // texel centres in x and y, x negative for the left two columns, w for the bottom row
    v.t[0] = {(4.0f * sx + 0.5f) / 16.0f - 0.5f, (4.0f * sy + 2.5f) / 16.0f, 0.3f + 0.1f * sx,
              0.6f - 0.25f * sy};
    // crossing 0 and 1 in every component, only w negative in the right column
    v.t[1] = {0.37f * sx - 0.21f, -0.53f * sy + 1.27f, 0.29f * sx + 0.41f * sy - 0.43f,
              1.3f - 0.45f * sx};
    // texel centres as they are, and divided by z
    v.t[2] = {3.0f * (4.0f * sx + 0.5f) / 16.0f, 3.0f * (4.0f * sy + 1.5f) / 16.0f, 3.0f, 4.0f};
    v.t[3] = {-0.83f * sx + 1.4f, 0.71f * sy - 0.9f, 0.23f * (sx - sy) + 0.5f, 0.35f - 0.2f * sy};
    v.t[4] = {0.15f + 0.2f * sx, 0.3f + 0.17f * sy, 0.05f + 0.31f * sx + 0.07f * sy,
              0.9f - 0.08f * sx};
    v.t[5] = {2.5f - 1.1f * sx, -2.0f + 0.9f * sy, 0.5f + 0.25f * sx, 1.5f - 0.3f * sy};
    v.c[0] = {0.1f + 0.2f * sx, 0.9f - 0.15f * sy, 0.5f + 0.1f * (sx - sy), 0.75f};
    v.c[1] = {0.3f * sx - 0.4f, 0.2f + 0.1f * sy, 1.3f - 0.2f * sx, 0.6f};
    return v;
  }

  void Draw(IDirect3DPixelShader9 * ps)
  {
    Vert v[3] = {At(0, 0), At(8, 0), At(0, 8)};
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(7, 7, 7, 7), 1.0f, 0);
    dev->SetVertexDeclaration(decl);
    dev->SetVertexShader(vs);
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

  // reads back each pixel's TEXCOORD0-5 and COLOR0-1 as a ps_2_0 shader sees them: [pixel][8][4]
  void Inputs()
  {
    std::vector<uint32_t> data(NumPixels * 8 * 4);
    const char *shaders[3] = {
        "ps_2_0\ndcl t0\ndcl t1\ndcl t2\ndcl t3\nmov oC0, t0\nmov oC1, t1\nmov oC2, t2\nmov oC3, "
        "t3\n",
        "ps_2_0\ndcl t4\ndcl t5\nmov oC0, t4\nmov oC1, t5\n",
        "ps_2_0\ndcl v0\ndcl v1\nmov oC0, v0\nmov oC1, v1\n",
    };
    const int first[3] = {0, 4, 6}, count[3] = {4, 2, 2};
    for(int s = 0; s < 3; s++)
    {
      IDirect3DPixelShader9Ptr ps = Shader(Assemble(shaders[s]));
      Draw(ps);
      for(int t = 0; t < count[s]; t++)
      {
        std::vector<V4> v = Read(t);
        for(UINT p = 0; p < NumPixels; p++)
          memcpy(&data[(p * 8 + first[s] + t) * 4], &v[p], 16);
      }
    }
    Array("Inputs", data);
  }

  struct Source
  {
    const char *name;
    const char *asm_;
    // a stage with D3DTSS_TEXTURETRANSFORMFLAGS set, and the flags
    uint32_t projStage;
    uint32_t projFlags;
  };

  int main()
  {
    if(!Init())
      return 3;

    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    assemble = d3dcompiler ? (pD3DAssemble)GetProcAddress(d3dcompiler, "D3DAssemble") : NULL;
    if(!assemble)
      return 4;

    const char *o = getenv("GEN_OUT");
    out = fopen(o ? o : "d3d9_ps1_test_data.h", "w");
    if(!out)
      return 5;

    D3DADAPTER_IDENTIFIER9 ident = {};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident);
    TEST_LOG("%s driver %u.%u.%u.%u", ident.Description, HIWORD(ident.DriverVersion.HighPart),
             LOWORD(ident.DriverVersion.HighPart), HIWORD(ident.DriverVersion.LowPart),
             LOWORD(ident.DriverVersion.LowPart));

    D3DVERTEXELEMENT9 el[10] = {};
    el[0] = {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0};
    for(BYTE t = 0; t < 6; t++)
      el[1 + t] = {
          0, WORD(16 + 16 * t), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,
          t};
    el[7] = {0, 112, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0};
    el[8] = {0, 128, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1};
    el[9] = D3DDECL_END();
    dev->CreateVertexDeclaration(el, &decl);

    std::vector<DWORD> code = Assemble(
        "vs_2_0\ndcl_position v0\ndcl_texcoord0 v1\ndcl_texcoord1 v2\ndcl_texcoord2 "
        "v3\ndcl_texcoord3 v4\ndcl_texcoord4 v5\ndcl_texcoord5 v6\ndcl_color0 v7\ndcl_color1 "
        "v8\nmov oPos, v0\nmov oT0, v1\nmov oT1, v2\nmov oT2, v3\nmov oT3, v4\nmov oT4, v5\nmov "
        "oT5, v6\nmov oD0, v7\nmov oD1, v8\n");
    if(code.empty() || FAILED(dev->CreateVertexShader(code.data(), &vs)))
      return 6;

    for(int i = 0; i < 4; i++)
      dev->CreateRenderTarget(W, H, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt[i],
                              NULL);
    MakeTextures();
    ResetState();

    // what a cleared target holds: a pixel holding it in every component was discarded
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(7, 7, 7, 7), 1.0f, 0);
    sentinel = Read(0)[0].x;

    P("// Generated by the D3D9_PS1_Emu_Data generator in\n"
      "// test_reports/d3d9_step5_6_ps1_interpreter/generator, on %s (driver %u.%u.%u.%u).\n"
      "// The ps_1_x shaders below were assembled by D3DAssemble (d3dcompiler_47) and run "
      "natively on a\n"
      "// %ux%u A32B32G32R32F target covered by one triangle, with the inputs each pixel saw "
      "read back\n"
      "// by a ps_2_0 shader. Pixels are in rows, top to bottom.\n\n",
      ident.Description, HIWORD(ident.DriverVersion.HighPart), LOWORD(ident.DriverVersion.HighPart),
      HIWORD(ident.DriverVersion.LowPart), LOWORD(ident.DriverVersion.LowPart), W, H);
    P("#pragma once\n\nnamespace PS1TestData\n{\nconst uint32_t Width = %u, Height = %u;\n\n", W, H);
    P("// what a discarded pixel's target holds\nconst uint32_t Discarded = 0x%08x;\n\n",
      Bits(sentinel));
    P("// c0-c7, as SetPixelShaderConstantF set them\n"
      "const float FloatConsts[8][4] = {\n"
      "    {1.5f, -0.75f, 2.25f, 0.5f},     {0.1f, 0.2f, -0.3f, 0.4f},    {0.8f, -0.6f, 0.0f, "
      "0.3f},\n"
      "    {0.6f, 0.8f, 0.1f, -0.2f},       {-0.25f, 0.5f, 0.9f, 1.1f},   {0.05f, -1.2f, 0.4f, "
      "2.0f},\n"
      "    {3.0f, 1.0f / 3.0f, -7.5f, 0.0f}, {-1.0f, 0.125f, 6.0f, -2.5f},\n};\n\n");
    P("// stages 0-5 each have a 16x16 A32B32G32R32F texture, one level, point sampled with wrap "
      "addressing\nconst uint32_t NumStages = %u, TexSize = %u;\n%s\n",
      NumStages, TexSize, TEXEL_FUNCTION);
    P("// D3DTSS_BUMPENVMAT00, 01, 10, 11, BUMPENVLSCALE and BUMPENVLOFFSET per stage\n"
      "const float BumpEnv[%u][6] = {\n",
      NumStages);
    for(UINT s = 0; s < NumStages; s++)
    {
      float m[4];
      BumpMat(s, m);
      const float vals[6] = {m[0], m[1], m[2], m[3], LumScale(s), LumOffset(s)};
      P("    {");
      for(int i = 0; i < 6; i++)
      {
        char num[32];
        snprintf(num, sizeof(num), "%.9g", vals[i]);
        P("%s%s%sf", i ? ", " : "", num, strpbrk(num, ".e") ? "" : ".0");
      }
      P("},\n");
    }
    P("};\n\n");
    P("// each pixel's TEXCOORD0-5 and COLOR0-1 as float bits, through a vs_2_0 (which clamps the "
      "colours)\n");
    Inputs();

    const Source sources[] = {
        {"arith_1_1",
         "ps_1_1\n"
         "tex t0\n"
         "texcoord t1\n"
         "mul_x2 r0.rgb, t0, v0_bias\n"
         "+add_d2 r0.a, v1_bx2, c1\n"
         "mad r1, t1, c0, -v1\n"
         "dp3_x4 r1.rgb, t1_bx2, c7\n"
         "+mov r1.a, 1-v0\n"
         "lrp_sat r0.rgb, v1, r0, r1\n"
         "+sub_x2 r0.a, r0, c4\n"
         "cnd r0, r0.a, r0, r1\n"},
        {"texcoord_1_1", "ps_1_1\ntexcoord t1\ntexcoord t2\nmov r0, t1\n"},
        {"texkill_1_1", "ps_1_1\ntexkill t0\nmov r0, v0\n"},
        {"texkill_w_1_1", "ps_1_1\ntexkill t1\nmov r0, v0\n"},
        {"texbem_1_1", "ps_1_1\ntex t0\ntexbem t2, t0\nmov r0, t2\n"},
        {"texbeml_1_1",
         "ps_1_1\ndef c6, 0.25, 0.25, 0.25, 0.25\ndef c7, 0.5, 0.5, 0.5, 0.5\ntex t0\ntexbeml t2, "
         "t0\nmad r0, t2, c6, c7\n"},
        {"texreg2_1_1",
         "ps_1_1\ntex t0\ntexreg2ar t1, t0\ntexreg2gb t2, t0\nmov r0.rgb, t1\n+mov "
         "r0.a, t2\n"},
        {"m3x2tex_1_1",
         "ps_1_1\ntex t0\ntexm3x2pad t1, t0_bx2\ntexm3x2tex t2, t0_bx2\nmov r0, t2\n"},
        {"m3x3tex_1_1",
         "ps_1_1\ntex t0\ntexm3x3pad t1, t0\ntexm3x3pad t2, t0\ntexm3x3tex t3, t0\nmov r0, t3\n"},
        {"m3x3spec_1_1",
         "ps_1_1\ntex t0\ntexm3x3pad t1, t0\ntexm3x3pad t2, t0\ntexm3x3spec t3, "
         "t0, c0\nmov r0, t3\n"},
        {"m3x3spec_c3_1_1",
         "ps_1_1\ntex t0\ntexm3x3pad t1, t0\ntexm3x3pad t2, t0\ntexm3x3spec "
         "t3, t0, c3\nmov r0, t3\n"},
        {"m3x3vspec_1_1",
         "ps_1_1\ntex t0\ntexm3x3pad t1, t0\ntexm3x3pad t2, t0\ntexm3x3vspec t3, "
         "t0\nmov r0, t3\n"},
        {"coissue_1_1", "ps_1_1\ntex t0\nmov r0, v0\nmul r0.rgb, t0, c3\n+mov r0.a, r0.b\n"},
        {"arith_1_2",
         "ps_1_2\ntex t0\ntexcoord t1\ncmp r0, t1_bias, t0, v0\ndp4 r1, t0, c4\nmad r0.rgb, r1, "
         "c3, r0\n+mul r0.a, r1.b, v1\n"},
        {"m3x3_1_2",
         "ps_1_2\ndef c6, 0.125, 0.125, 0.125, 0.125\ndef c7, 0.5, 0.5, 0.5, 0.5\ntex t0\n"
         "texm3x3pad t1, t0\ntexm3x3pad t2, t0\ntexm3x3 t3, t0\nmad r0, t3, c6, c7\n"},
        {"dp3_1_2",
         "ps_1_2\ndef c6, 0.125, 0.125, 0.125, 0.125\ndef c7, 0.5, 0.5, 0.5, 0.5\ntex t0\n"
         "texdp3 t1, t0\ntexdp3tex t2, t0\nmad r0.rgb, t1, c6, c7\n+mov r0.a, t2\n"},
        {"reg2rgb_1_2", "ps_1_2\ntex t0\ntexreg2rgb t3, t0\nmov r0, t3\n"},
        {"proj_none_1_1", "ps_1_1\ntex t2\nmov r0, t2\n"},
        {"proj_count3_1_1", "ps_1_1\ntex t2\nmov r0, t2\n", 2, D3DTTFF_COUNT3 | D3DTTFF_PROJECTED},
        {"proj_count4_1_1", "ps_1_1\ntex t2\nmov r0, t2\n", 2, D3DTTFF_COUNT4 | D3DTTFF_PROJECTED},
        {"proj_only_1_1", "ps_1_1\ntex t2\nmov r0, t2\n", 2, D3DTTFF_PROJECTED},
        {"proj_count3_1_4", "ps_1_4\ntexld r2, t2\nmov r0, r2\n", 2,
         D3DTTFF_COUNT3 | D3DTTFF_PROJECTED},
        {"arith_1_4",
         "ps_1_4\ndef c6, 0.5, -0.5, 0.25, 0.125\ntexcrd r1.rgb, t2\ntexcrd r2.rgb, t2\nphase\n"
         "texld r0, t0\ntexld r4, r1\ntexld r2, r2_dz\nmul_x2 r3, r0, c0\nadd r0.rgb, r3, v0\n"
         "+mov r0.a, r1.b\ncnd r0.rgb, r1, r0, r4\nmad_d2 r0, r0, c6, r2\n"},
        {"texcrd_1_4",
         "ps_1_4\ndef c6, 0.125, 0.125, 0.125, 0.125\ndef c7, 0.5, 0.5, 0.5, 0.5\ntexcrd r0.rgb, "
         "t5\nmad r0.rgb, r0, c6, c7\n+mov r0.a, c7\n"},
        {"texcrd_dw_1_4",
         "ps_1_4\ndef c6, 0.125, 0.125, 0.125, 0.125\ndef c7, 0.5, 0.5, 0.5, 0.5\ntexcrd r0.rg, "
         "t5_dw.xyw\nmad r0.rg, r0, c6, c7\nmov r0.ba, c7\n"},
        {"texld_dw_1_4", "ps_1_4\ntexld r3, t2_dw.xyw\nmov r0, r3\n"},
        {"bem_1_4",
         "ps_1_4\ntexld r0, t0\ntexcrd r1.rgb, t2\ntexcrd r2.rgb, t4\nbem r2.rg, r1, "
         "r0\nphase\ntexld r2, r2\nmov r0, r2\n"},
        {"texkill_1_4", "ps_1_4\ntexcrd r1.rgb, t1\nphase\ntexkill r1\ntexkill t0\nmov r0, v0\n"},
        {"coissue_1_4",
         "ps_1_4\ndef c6, 0.25, 0.5, 0.75, 1\ntexld r0, t0\nmov r1, v0\nmul r1.rgb, r0, c6\n+mov "
         "r1.a, r1.b\nmov r0, r1\n"},
    };

    P("struct Case\n{\n  const char *name;\n  const char *source;\n"
      "  const uint32_t *code;\n  uint32_t codeDWORDs;\n"
      "  // a stage with D3DTSS_TEXTURETRANSFORMFLAGS set, and the flags (0 for none)\n"
      "  uint32_t projStage, projFlags;\n"
      "  // r0 per pixel, as float bits\n"
      "  const uint32_t *outputs;\n};\n\n");

    std::string cases;
    for(const Source &s : sources)
    {
      std::vector<DWORD> bc = Assemble(s.asm_);
      if(bc.empty())
      {
        TEST_LOG("%s: didn't assemble", s.name);
        continue;
      }
      IDirect3DPixelShader9Ptr ps = Shader(bc);
      if(!ps)
      {
        TEST_LOG("%s: not created", s.name);
        continue;
      }

      ResetState();
      if(s.projFlags)
        dev->SetTextureStageState(s.projStage, D3DTSS_TEXTURETRANSFORMFLAGS, s.projFlags);
      Draw(ps);
      std::vector<V4> v = Read(0);
      ResetState();

      std::vector<uint32_t> outs(NumPixels * 4);
      memcpy(outs.data(), v.data(), NumPixels * 16);

      std::string escaped;
      for(char c : std::string(s.asm_))
      {
        if(c == '\n')
          escaped += "\\n\"\n    \"";
        else
          escaped += c;
      }
      P("static const char %s_asm[] =\n    \"%s\";\n\n", s.name, escaped.c_str());
      Array((std::string(s.name) + "_code").c_str(), std::vector<uint32_t>(bc.begin(), bc.end()));
      Array((std::string(s.name) + "_out").c_str(), outs);
      char line[1024];
      snprintf(line, sizeof(line),
               "    {\"%s\", %s_asm, %s_code, sizeof(%s_code) / 4, %u, 0x%x, %s_out},\n", s.name,
               s.name, s.name, s.name, s.projStage, s.projFlags, s.name);
      cases += line;
      TEST_LOG("%s: %zu DWORDs", s.name, bc.size());
    }

    P("static const Case Cases[] = {\n%s};\n\n};    // namespace PS1TestData\n", cases.c_str());
    fclose(out);
    out = NULL;
    TEST_LOG("done");
    return 0;
  }
};

REGISTER_TEST();
