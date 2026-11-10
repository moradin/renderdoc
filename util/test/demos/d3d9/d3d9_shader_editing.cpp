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

#include "d3d9_test.h"

#define CHECK_HR(expr)                                    \
  do                                                      \
  {                                                       \
    HRESULT check_hr = (expr);                            \
    if(FAILED(check_hr))                                  \
      TEST_ERROR("%s failed: %x", #expr, (UINT)check_hr); \
  } while(0)

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

// Nine draws into a 288x32 render target ("Scene"), one per 32x32 cell, with shaders of every
// model and every way a shader gets bound on replay:
// - "Frame Start": vs_3_0 + ps_3_0, bound at the end of the previous frame, so they're in the
//   bindings the frame starts with.
// - "HLSL 3_0": the same vs_3_0, and a ps_3_0 that "Shared" uses too.
// - "HLSL 2_0": vs_2_0 + ps_2_0.
// - "SM1": vs_1_1 + ps_1_1, assembled.
// - "ps_1_4": the same vs_1_1, and ps_1_4, assembled.
// - "State Block": vs_2_0 + another ps_2_0, from a state block recorded before the frame.
// - "Shared": vs_3_0 and the ps_3_0 "HLSL 3_0" uses.
// - "FFP Pixel": vs_2_0 with fixed function pixel processing.
// - "In Frame": vs_2_0 + a ps_2_0 created before the draw and released at the end of the frame,
//   so it's created again each time the frame is replayed.
//
// Natively, before the first frame, the scene is drawn as it is and then once for each edit in
// Edits, with the edited shader in place of the original wherever it's bound, and what the GPU drew
// at a few pixels of each cell is written to D3D9_Shader_Editing_reference.txt. The edits are
// written to files, and listed in D3D9_Shader_Editing_edits.txt with the draw whose shader they
// replace, for util/test/tests/D3D9/D3D9_Shader_Editing.py to build and replace them with.
RD_TEST(D3D9_Shader_Editing, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Draws with shaders of every model, bound in every way replay binds them, and a reference of "
      "what the GPU draws with each of a list of edits in place of the original.";

  static const UINT CellSize = 32;
  static const UINT NumDraws = 9;
  static const UINT SceneWidth = CellSize * NumDraws;
  static const UINT SceneHeight = CellSize;

  const std::string VS3 = R"EOSHADER(
float4 rect : register(c1);
float4 misc : register(c2);
bool flip : register(b0);

struct VIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };

VOut main(VIn i)
{
  VOut o;
  o.pos = float4(i.pos * rect.zw + rect.xy, 0.5f, 1.0f);
  o.uv = i.uv;
  o.col = i.col * misc;
  if(flip)
    o.uv.x = 1.0f - o.uv.x;
  return o;
}
)EOSHADER";

  // moved right by a quarter of the quad, with the colour's channels swapped
  const std::string VS3Edit = R"EOSHADER(
float4 rect : register(c1);
float4 misc : register(c2);
bool flip : register(b0);

struct VIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };

VOut main(VIn i)
{
  VOut o;
  o.pos = float4(i.pos * rect.zw + rect.xy, 0.5f, 1.0f);
  o.pos.x += 0.25f * rect.z;
  o.uv = i.uv;
  o.col = i.col.bgra * misc;
  if(flip)
    o.uv.x = 1.0f - o.uv.x;
  return o;
}
)EOSHADER";

  const std::string PS3A = R"EOSHADER(
sampler2D tex : register(s0);
float4 tint : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0, float face : VFACE) : COLOR0
{
  float4 t = tex2D(tex, uv);
  float4 r = t * col * tint;
  if(face < 0.0f)
    r = 1.0f - r;
  return r;
}
)EOSHADER";

  const std::string PS3AEdit = R"EOSHADER(
sampler2D tex : register(s0);
float4 tint : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
  float4 t = tex2D(tex, uv);
  return float4(t.bgr * tint.rgb + col.rgb * 0.25f, 1.0f);
}
)EOSHADER";

  const std::string PS3B = R"EOSHADER(
sampler2D tex : register(s0);
float4 tint : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
  return tex2D(tex, uv * 2.0f) * tint + col * 0.25f;
}
)EOSHADER";

  const std::string PS3BEditAsm = R"EOSHADER(
ps_3_0
def c10, 0.5, 0.25, 0, 1
dcl_texcoord v0.xy
dcl_color v1
dcl_2d s0
texld r0, v0, s0
mul r0, r0, c0
mad oC0, v1, c10.x, r0
)EOSHADER";

  const std::string VS2 = R"EOSHADER(
float4 rect : register(c1);
float4 misc : register(c2);

struct VIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };

VOut main(VIn i)
{
  VOut o;
  o.pos = float4(i.pos * rect.zw + rect.xy, 0.5f, 1.0f);
  o.uv = i.uv;
  o.col = i.col * misc;
  return o;
}
)EOSHADER";

  const std::string VS2Edit = R"EOSHADER(
float4 rect : register(c1);
float4 misc : register(c2);

struct VIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };

VOut main(VIn i)
{
  VOut o;
  o.pos = float4(i.pos * rect.zw + rect.xy, 0.5f, 1.0f);
  o.uv = i.uv * 2.0f;
  o.col = i.col.gbra * misc;
  return o;
}
)EOSHADER";

  const std::string PS2 = R"EOSHADER(
sampler2D tex : register(s0);
float4 tint : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
  return tex2D(tex, uv) * col + tint * 0.25f;
}
)EOSHADER";

  const std::string PS2Edit = R"EOSHADER(
sampler2D tex : register(s0);
float4 tint : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
  return tex2D(tex, uv).gbra * col;
}
)EOSHADER";

  const std::string PS2SB = R"EOSHADER(
float4 tint : register(c0);

float4 main(float4 col : COLOR0) : COLOR0
{
  return col * tint;
}
)EOSHADER";

  const std::string PS2SBEdit = R"EOSHADER(
float4 tint : register(c0);

float4 main(float4 col : COLOR0) : COLOR0
{
  return col.gbra * tint + 0.125f;
}
)EOSHADER";

  const std::string VS11 = R"EOSHADER(vs_1_1
dcl_position v0
dcl_texcoord v1
dcl_color v2
def c10, 0.5, 1, 0.25, 0
mad oPos.xy, v0, c1.zwzw, c1.xyxy
mov oPos.zw, c10.xy
mov oT0.xy, v1
mul oD0, v2, c2
)EOSHADER";

  // moved right by a quarter of the quad, with the colour's channels swapped
  const std::string VS11Edit = R"EOSHADER(vs_1_1
dcl_position v0
dcl_texcoord v1
dcl_color v2
def c10, 0.5, 1, 0.25, 0
mov r2, c10
mad r0.xy, v0, c1.zwzw, c1.xyxy
mul r1.x, c1.z, r2.z
add oPos.x, r0.x, r1.x
mov oPos.y, r0.y
mov oPos.zw, c10.xy
mov oT0.xy, v1
mov oD0, v2.zyxw
)EOSHADER";

  const std::string PS11 = R"EOSHADER(ps_1_1
tex t0
mul_x2 r0.rgb, t0, v0
+mov r0.a, c0.a
)EOSHADER";

  const std::string PS11Edit = R"EOSHADER(ps_1_1
tex t0
add r0.rgb, t0, v0_bx2
+mul r0.a, t0.a, c0.a
)EOSHADER";

  const std::string PS14 = R"EOSHADER(ps_1_4
texcrd r1.rgb, t0
phase
texld r0, t0
texld r2, r1
mul r0, r0, v0
add_sat r0.rgb, r0, r2_x2
+mov r0.a, c0.a
)EOSHADER";

  // d3dcompiler_47 can't compile ps_1_4, an HLSL edit of one is ps_2_0
  const std::string PS14Edit = R"EOSHADER(
sampler2D tex : register(s0);
float4 tint : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
  return saturate(tex2D(tex, uv) * tint + col * 0.5f);
}
)EOSHADER";

  const std::string PSFrame = R"EOSHADER(
sampler2D tex : register(s0);
float4 tint : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
  return tex2D(tex, uv.yx) * tint;
}
)EOSHADER";

  const std::string PSFrameEdit = R"EOSHADER(
sampler2D tex : register(s0);
float4 tint : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR0
{
  return tex2D(tex, uv) * col + float4(0.0f, 0.25f, 0.0f, 0.0f);
}
)EOSHADER";

  enum ShaderKey
  {
    Key_VS3,
    Key_VS2,
    Key_VS11,
    Key_PS3A,
    Key_PS3B,
    Key_PS2,
    Key_PS2SB,
    Key_PS11,
    Key_PS14,
    // created in the frame (frameShader), shaders[] has one for its bytecode
    Key_PSFrame,
    Key_Count,
    Key_None = Key_Count,
  };

  struct Draw
  {
    const char *marker;
    ShaderKey vs, ps;
  };

  const Draw Draws[NumDraws] = {
      {"Frame Start", Key_VS3, Key_PS3A}, {"HLSL 3_0", Key_VS3, Key_PS3B},
      {"HLSL 2_0", Key_VS2, Key_PS2},     {"SM1", Key_VS11, Key_PS11},
      {"ps_1_4", Key_VS11, Key_PS14},     {"State Block", Key_VS2, Key_PS2SB},
      {"Shared", Key_VS3, Key_PS3B},      {"FFP Pixel", Key_VS2, Key_None},
      {"In Frame", Key_VS2, Key_PSFrame},
  };

  struct Edit
  {
    const char *name;
    // the draw whose shader is replaced, and its stage
    UINT draw;
    bool vertex;
    // HLSL, D3D9Asm or D3D9Bytecode, and the HLSL profile
    const char *encoding;
    const char *profile;
    const std::string *source;
  };

  std::vector<Edit> Edits;

  // one of the two, for the key's stage
  struct Shader
  {
    IDirect3DVertexShader9Ptr vs;
    IDirect3DPixelShader9Ptr ps;
    explicit operator bool() const { return vs || ps; }
  };

  Shader shaders[Key_Count];
  Shader edited[Key_Count];

  IDirect3DVertexBuffer9Ptr vb;
  IDirect3DVertexDeclaration9Ptr decl;
  IDirect3DTexture9Ptr tex;
  IDirect3DStateBlock9Ptr stateBlock;
  IDirect3DSurface9Ptr scene;

  std::vector<DWORD> frameCode;
  IDirect3DPixelShader9Ptr frameShader;

  pD3DAssemble assemble = NULL;

  std::vector<DWORD> Assemble(const std::string &src)
  {
    std::vector<DWORD> ret;
    ID3DBlobPtr code, errors;
    HRESULT hr = assemble(src.c_str(), src.size(), "", NULL, NULL, 0, &code, &errors);
    if(FAILED(hr) || !code)
    {
      TEST_ERROR("Couldn't assemble: %s", errors ? (const char *)errors->GetBufferPointer() : "");
      return ret;
    }
    ret.resize(code->GetBufferSize() / sizeof(DWORD));
    memcpy(ret.data(), code->GetBufferPointer(), ret.size() * sizeof(DWORD));
    return ret;
  }

  std::vector<DWORD> Build(const Edit &e)
  {
    if(!strcmp(e.encoding, "D3D9Asm"))
      return Assemble(*e.source);

    std::vector<DWORD> ret;
    ID3DBlobPtr blob = Compile(*e.source, "main", e.profile);
    if(blob)
    {
      ret.resize(blob->GetBufferSize() / sizeof(DWORD));
      memcpy(ret.data(), blob->GetBufferPointer(), ret.size() * sizeof(DWORD));
    }
    return ret;
  }

  Shader CreateShader(bool vertex, const std::vector<DWORD> &code)
  {
    Shader ret;
    if(code.empty())
      return ret;
    if(vertex)
      CHECK_HR(dev->CreateVertexShader(code.data(), &ret.vs));
    else
      CHECK_HR(dev->CreatePixelShader(code.data(), &ret.ps));
    return ret;
  }

  IDirect3DVertexShader9 *VS(ShaderKey key, const Shader *replace)
  {
    return replace && replace[key] ? replace[key].vs : shaders[key].vs;
  }

  IDirect3DPixelShader9 *PS(ShaderKey key, const Shader *replace)
  {
    if(key == Key_None)
      return NULL;
    if(replace && replace[key])
      return replace[key].ps;
    return key == Key_PSFrame ? frameShader.GetInterfacePtr() : shaders[key].ps.GetInterfacePtr();
  }

  // everything a draw uses but its shaders
  void SetDrawState(UINT d)
  {
    const float x0 = float(d * CellSize + 2), y0 = 2.0f, size = float(CellSize - 4);

    // clip space: the top left corner and the size
    const float rect[4] = {
        x0 / float(SceneWidth) * 2.0f - 1.0f,
        1.0f - y0 / float(SceneHeight) * 2.0f,
        size / float(SceneWidth) * 2.0f,
        -size / float(SceneHeight) * 2.0f,
    };
    const float misc[4] = {1.0f, 0.75f + 0.03f * d, 1.0f - 0.05f * d, 1.0f};
    const float tint[4] = {0.9f - 0.1f * d, 0.5f + 0.05f * d, 0.3f + 0.08f * d, 0.6f + 0.04f * d};
    const BOOL flip = (d % 2) ? TRUE : FALSE;

    dev->SetVertexShaderConstantF(1, rect, 1);
    dev->SetVertexShaderConstantF(2, misc, 1);
    dev->SetVertexShaderConstantB(0, &flip, 1);
    dev->SetPixelShaderConstantF(0, tint, 1);

    dev->SetTexture(0, tex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    dev->SetVertexDeclaration(decl);
    dev->SetStreamSource(0, vb, 0, sizeof(float) * 4 + sizeof(DWORD));
  }

  // natively with replace, or in the frame (replace NULL) where the first draw's shaders are the
  // ones the frame starts with and the state block's are from the state block
  void DrawScene(const Shader *replace)
  {
    dev->SetRenderTarget(0, scene);
    dev->SetDepthStencilSurface(NULL);
    const D3DVIEWPORT9 view = {0, 0, SceneWidth, SceneHeight, 0.0f, 1.0f};
    dev->SetViewport(&view);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 25, 50, 75), 1.0f, 0);

    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);

    for(UINT d = 0; d < NumDraws; d++)
    {
      const Draw &draw = Draws[d];

      if(!replace)
        pushMarker(draw.marker);

      SetDrawState(d);

      if(draw.ps == Key_PSFrame)
        CHECK_HR(dev->CreatePixelShader(frameCode.data(), &frameShader));

      if(draw.ps == Key_PS2SB)
      {
        stateBlock->Apply();
        // natively the state block has the originals
        if(replace)
        {
          dev->SetVertexShader(VS(draw.vs, replace));
          dev->SetPixelShader(PS(draw.ps, replace));
        }
      }
      else if(d > 0 || replace)
      {
        dev->SetVertexShader(VS(draw.vs, replace));
        dev->SetPixelShader(PS(draw.ps, replace));
      }

      dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

      if(!replace)
        popMarker();
    }

    // the first draw's state, for the next frame to start with
    if(!replace)
      pushMarker("Next Frame Start");
    SetDrawState(0);
    dev->SetVertexShader(VS(Draws[0].vs, NULL));
    dev->SetPixelShader(PS(Draws[0].ps, NULL));
    if(!replace)
      popMarker();

    frameShader = NULL;
  }

  void WriteFile(const std::string &name, const void *data, size_t size, bool binary)
  {
    const std::string path = tmpPath + "/" + name;
    FILE *f = NULL;
    fopen_s(&f, path.c_str(), binary ? "wb" : "w");
    if(!f)
    {
      TEST_ERROR("Couldn't write %s", path.c_str());
      return;
    }
    fwrite(data, 1, size, f);
    fclose(f);
  }

  void WriteReference(FILE * f, const char *name)
  {
    IDirect3DSurface9Ptr readback;
    CHECK_HR(dev->CreateOffscreenPlainSurface(SceneWidth, SceneHeight, D3DFMT_A8R8G8B8,
                                              D3DPOOL_SYSTEMMEM, &readback, NULL));
    CHECK_HR(dev->GetRenderTargetData(scene, readback));

    D3DLOCKED_RECT locked = {};
    CHECK_HR(readback->LockRect(&locked, NULL, D3DLOCK_READONLY));
    for(UINT d = 0; d < NumDraws; d++)
    {
      const UINT x0 = d * CellSize;
      const UINT probes[][2] = {{x0 + 16, 16}, {x0 + 6, 6}, {x0 + 25, 25}, {x0 + 29, 10}};
      for(const auto &p : probes)
      {
        const DWORD c = ((DWORD *)((byte *)locked.pBits + p[1] * locked.Pitch))[p[0]];
        fprintf(f, "%s\t%u\t%u\t%f\t%f\t%f\t%f\n", name, p[0], p[1], ((c >> 16) & 0xff) / 255.0f,
                ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f, ((c >> 24) & 0xff) / 255.0f);
      }
    }
    readback->UnlockRect();
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    assemble = d3dcompiler ? (pD3DAssemble)GetProcAddress(d3dcompiler, "D3DAssemble") : NULL;
    if(!assemble)
    {
      TEST_ERROR("Couldn't load D3DAssemble");
      return 4;
    }

    Edits = {
        {"Frame start ps_3_0, HLSL", 0, false, "HLSL", "ps_3_0", &PS3AEdit},
        {"Shared ps_3_0, assembly", 6, false, "D3D9Asm", "-", &PS3BEditAsm},
        {"ps_2_0, bytecode", 2, false, "D3D9Bytecode", "ps_2_0", &PS2Edit},
        {"ps_1_1, assembly", 3, false, "D3D9Asm", "-", &PS11Edit},
        {"ps_1_4 as ps_2_0, HLSL", 4, false, "HLSL", "ps_2_0", &PS14Edit},
        {"vs_1_1, assembly", 3, true, "D3D9Asm", "-", &VS11Edit},
        {"vs_3_0, HLSL", 1, true, "HLSL", "vs_3_0", &VS3Edit},
        {"State block ps_2_0, HLSL", 5, false, "HLSL", "ps_2_0", &PS2SBEdit},
        {"vs_2_0 with FFP pixel, HLSL", 7, true, "HLSL", "vs_2_0", &VS2Edit},
        {"In-frame ps_2_0, HLSL", 8, false, "HLSL", "ps_2_0", &PSFrameEdit},
    };

    {
      const struct
      {
        ShaderKey key;
        bool vertex;
        const std::string *src;
        const char *profile;
      } originals[] = {
          {Key_VS3, true, &VS3, "vs_3_0"},      {Key_VS2, true, &VS2, "vs_2_0"},
          {Key_VS11, true, &VS11, NULL},        {Key_PS3A, false, &PS3A, "ps_3_0"},
          {Key_PS3B, false, &PS3B, "ps_3_0"},   {Key_PS2, false, &PS2, "ps_2_0"},
          {Key_PS2SB, false, &PS2SB, "ps_2_0"}, {Key_PS11, false, &PS11, NULL},
          {Key_PS14, false, &PS14, NULL},       {Key_PSFrame, false, &PSFrame, "ps_2_0"},
      };
      for(const auto &o : originals)
      {
        Edit e = {"", 0, o.vertex, o.profile ? "HLSL" : "D3D9Asm", o.profile, o.src};
        std::vector<DWORD> code = Build(e);
        if(o.key == Key_PSFrame)
          frameCode = code;
        shaders[o.key] = CreateShader(o.vertex, code);
        if(!shaders[o.key])
          return 5;
      }
    }

    // a unit quad: position, texcoord, colour
    struct Vertex
    {
      float x, y, u, v;
      DWORD col;
    };
    const Vertex quad[4] = {
        {0.0f, 0.0f, 0.0f, 0.0f, D3DCOLOR_ARGB(255, 255, 64, 32)},
        {1.0f, 0.0f, 1.0f, 0.0f, D3DCOLOR_ARGB(200, 32, 255, 64)},
        {0.0f, 1.0f, 0.0f, 1.0f, D3DCOLOR_ARGB(160, 64, 32, 255)},
        {1.0f, 1.0f, 1.0f, 1.0f, D3DCOLOR_ARGB(255, 240, 240, 200)},
    };
    vb = MakeVB(quad, sizeof(quad));

    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    CHECK_HR(dev->CreateVertexDeclaration(elements, &decl));

    {
      IDirect3DTexture9Ptr fill;
      CHECK_HR(dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &fill, NULL));
      D3DLOCKED_RECT locked = {};
      CHECK_HR(fill->LockRect(0, &locked, NULL, 0));
      for(UINT y = 0; y < 8; y++)
        for(UINT x = 0; x < 8; x++)
          ((DWORD *)((byte *)locked.pBits + y * locked.Pitch))[x] =
              D3DCOLOR_ARGB(255 - x * 16, 40 + x * 26, 230 - y * 24, ((x + y) % 2) ? 220 : 60);
      fill->UnlockRect(0);
      CHECK_HR(dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL));
      CHECK_HR(dev->UpdateTexture(fill, tex));
    }

    CHECK_HR(dev->CreateRenderTarget(SceneWidth, SceneHeight, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE,
                                     0, FALSE, &scene, NULL));

    // the state block's shaders, recorded before the frame
    CHECK_HR(dev->BeginStateBlock());
    dev->SetVertexShader(VS(Key_VS2, NULL));
    dev->SetPixelShader(PS(Key_PS2SB, NULL));
    CHECK_HR(dev->EndStateBlock(&stateBlock));

    IDirect3DSurface9Ptr backBuffer;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);

    // the native reference, before the first frame
    if(!rdoc)
    {
      const std::string refPath = tmpPath + "/D3D9_Shader_Editing_reference.txt";
      const std::string editsPath = tmpPath + "/D3D9_Shader_Editing_edits.txt";
      FILE *ref = NULL, *edits = NULL;
      fopen_s(&ref, refPath.c_str(), "w");
      fopen_s(&edits, editsPath.c_str(), "w");
      if(!ref || !edits)
      {
        TEST_ERROR("Couldn't write %s or %s", refPath.c_str(), editsPath.c_str());
        return 6;
      }

      dev->BeginScene();
      DrawScene(edited);
      dev->EndScene();
      WriteReference(ref, "None");

      for(size_t i = 0; i < Edits.size(); i++)
      {
        const Edit &e = Edits[i];
        const Draw &draw = Draws[e.draw];
        const ShaderKey key = e.vertex ? draw.vs : draw.ps;

        std::vector<DWORD> code = Build(e);
        for(Shader &s : edited)
          s = Shader();
        edited[key] = CreateShader(e.vertex, code);
        if(!edited[key])
          return 7;

        dev->BeginScene();
        DrawScene(edited);
        dev->EndScene();
        WriteReference(ref, e.name);

        std::string file = "D3D9_Shader_Editing_" + std::to_string(i);
        if(!strcmp(e.encoding, "D3D9Bytecode"))
        {
          file += ".fxo";
          WriteFile(file, code.data(), code.size() * sizeof(DWORD), true);
        }
        else
        {
          file += !strcmp(e.encoding, "HLSL") ? ".hlsl" : ".asm";
          WriteFile(file, e.source->c_str(), e.source->size(), false);
        }

        fprintf(edits, "%s\t%s\t%s\t%s\t%s\t%s\n", e.name, draw.marker, e.vertex ? "vs" : "ps",
                e.encoding, e.profile, file.c_str());
      }

      for(Shader &s : edited)
        s = Shader();

      fclose(ref);
      fclose(edits);
    }

    // the first frame starts with the first draw's shaders
    SetDrawState(0);
    dev->SetVertexShader(VS(Draws[0].vs, NULL));
    dev->SetPixelShader(PS(Draws[0].ps, NULL));

    while(Running())
    {
      dev->BeginScene();
      DrawScene(NULL);
      dev->EndScene();

      dev->SetRenderTarget(0, backBuffer);
      dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
      const RECT dst = {0, 0, LONG(SceneWidth), LONG(SceneHeight)};
      dev->StretchRect(scene, NULL, backBuffer, &dst, D3DTEXF_NONE);

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
