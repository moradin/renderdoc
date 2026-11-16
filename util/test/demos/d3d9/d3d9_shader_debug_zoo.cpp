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

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

RD_TEST(D3D9_Shader_Debug_Zoo, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Draws with a vertex shader of each model to debug (vs_1_1, vs_2_0, vs_2_x and vs_3_0), "
      "each in its own marker, plus an instanced vs_3_0 draw with a base vertex, a vs_2_x draw "
      "with nested calls, a vs_3_0 draw whose constant table has structs, arrays and matrices, and "
      "a fixed function draw, which can't be debugged.";

  // vs_1_1 tokens, hand assembled since D3DAssemble doesn't take vs_1_1
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

  // the arithmetic and the fixed output registers: m4x4, relative addressing on a0, expp, logp,
  // lit, dst, frc, the comparisons, oFog and oPts
  // where each instruction starts, to find one the runtime rejects
  static std::vector<DWORD> VS11(std::vector<size_t> *starts = NULL)
  {
    const DWORD X = 0x00, Y = 0x55, Z = 0xAA, W = 0xFF;
    std::vector<DWORD> t = {0xFFFE0101};
    auto op = [&t, starts](DWORD opcode, std::initializer_list<DWORD> params) {
      if(starts)
        starts->push_back(t.size());
      t.push_back(opcode);
      t.insert(t.end(), params.begin(), params.end());
    };
    // dcl_position v0, dcl_color v1, dcl_texcoord v2
    op(31, {0x80000000 | 0, Dst(RInput, 0)});
    op(31, {0x80000000 | 10, Dst(RInput, 1)});
    op(31, {0x80000000 | 5, Dst(RInput, 2)});
    // m4x4 oPos, v0, c0
    op(20, {Dst(RRast, 0), Src(RInput, 0), Src(RConst, 0)});
    // mov r3, c20: (0.5, 2.5, -1, 0.25)
    op(1, {Dst(RTemp, 3), Src(RConst, 20)});
    // mov a0.x, r3.y: rounds down to 2. Another instruction comes before a0 is read.
    op(1, {Dst(RAddr, 0, 0x1), Src(RTemp, 3, Y)});
    // dp3 r4.x, v0, v0
    op(8, {Dst(RTemp, 4, 0x1), Src(RInput, 0), Src(RInput, 0)});
    // mul r2, v1, c[a0.x + 4]
    op(5, {Dst(RTemp, 2), Src(RInput, 1), Src(RConst, 4) | 0x2000});
    // mad r2, r2, r3.x, r3.w
    op(4, {Dst(RTemp, 2), Src(RTemp, 2), Src(RTemp, 3, X), Src(RTemp, 3, W)});
    // add r4.x, r4.x, r3.w; rsq r4.y, r4.x; rcp r4.z, r4.x; mov r4.w, r3.x
    op(2, {Dst(RTemp, 4, 0x1), Src(RTemp, 4, X), Src(RTemp, 3, W)});
    op(7, {Dst(RTemp, 4, 0x2), Src(RTemp, 4, X)});
    op(6, {Dst(RTemp, 4, 0x4), Src(RTemp, 4, X)});
    op(1, {Dst(RTemp, 4, 0x8), Src(RTemp, 3, X)});
    // expp r5, r3.y; logp r6, r3.y
    op(78, {Dst(RTemp, 5), Src(RTemp, 3, Y)});
    op(79, {Dst(RTemp, 6), Src(RTemp, 3, Y)});
    // lit r7, c7; dst r8, r4, c8
    op(16, {Dst(RTemp, 7), Src(RConst, 7)});
    op(17, {Dst(RTemp, 8), Src(RTemp, 4), Src(RConst, 8)});
    // frc r9.xy, v2
    op(19, {Dst(RTemp, 9, 0x3), Src(RInput, 2)});
    // min r10, r2, r3.x; max r10, r10, r3.z
    op(10, {Dst(RTemp, 10), Src(RTemp, 2), Src(RTemp, 3, X)});
    op(11, {Dst(RTemp, 10), Src(RTemp, 10), Src(RTemp, 3, Z)});
    // slt r11, v0, r3.w; sge r0, v0, r3.w
    op(12, {Dst(RTemp, 11), Src(RInput, 0), Src(RTemp, 3, W)});
    op(13, {Dst(RTemp, 0), Src(RInput, 0), Src(RTemp, 3, W)});
    // add oD0, r10, r11; mov oD1, r7
    op(2, {Dst(RAttr, 0), Src(RTemp, 10), Src(RTemp, 11)});
    op(1, {Dst(RAttr, 1), Src(RTemp, 7)});
    // mov oT0, v2; mad oT1, r4.xyzx, r9.xyxy, r8; mul oT2, r5, r6; add oT3, r0, -r3
    op(1, {Dst(RTex, 0), Src(RInput, 2)});
    op(4, {Dst(RTex, 1), Src(RTemp, 4, 0x24), Src(RTemp, 9, 0x44), Src(RTemp, 8)});
    op(5, {Dst(RTex, 2), Src(RTemp, 5), Src(RTemp, 6)});
    op(2, {Dst(RTex, 3), Src(RTemp, 0), Src(RTemp, 3) | (1 << 24)});
    // mov oFog, r4.y, with the full mask and a replicated source as fxc writes it; mov oPts, r3.y
    op(1, {Dst(RRast, 1), Src(RTemp, 4, Y)});
    op(1, {Dst(RRast, 2), Src(RTemp, 3, Y)});
    t.push_back(0x0000FFFF);
    return t;
  }

  // static flow control: a loop by i0 over a relatively addressed array, a branch on b0, sincos,
  // pow, normalize
  const char *VS20 = R"EOSHADER(

float4x4 wvp : register(c0);
float4 lights[4] : register(c10);
float4 params : register(c20);
bool useParams : register(b0);

struct vin
{
  float3 pos : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

struct vout
{
  float4 pos : POSITION;
  float4 col : COLOR0;
  float4 tc : TEXCOORD0;
  float3 n : TEXCOORD1;
};

vout main(vin IN)
{
  vout OUT;
  OUT.pos = mul(float4(IN.pos, 1), wvp);
  float3 dir = normalize(IN.pos + float3(0.1f, 0.2f, 0.3f));
  float3 acc = 0;
  // 4 is the count the app sets in i0 (see CompileLoop)
  [loop] for(int i = 0; i < 4; i++)
    acc += lights[i].xyz * saturate(dot(dir, lights[i].xyz));
  if(useParams)
    acc *= params.xyz;
  float s, c;
  sincos(IN.uv.x * 6.0f, s, c);
  OUT.col = float4(acc, 1) * IN.col;
  OUT.tc = float4(s, c, pow(abs(IN.uv.y) + 0.5f, params.w), frac(IN.uv.x * 3.7f));
  OUT.n = dir;
  return OUT;
}

)EOSHADER";

  // dynamic flow control: predicates, setp, if_lt, break_gt and a call
  const char *VS2X = R"EOSHADER(
vs_2_x
dcl_position v0
dcl_color v1
dcl_texcoord v2
defi i0, 3, 0, 1, 0
def c20, 0.5, 1, 0, -1
m4x4 oPos, v0, c0
mov r0, c20.z
setp_gt p0, v2.xyxy, c20.x
loop aL, i0
  add r0, r0, c10[aL]
  break_gt r0.x, c20.y
endloop
if_lt v0.x, c20.z
  call l0
else
  mov r1, c20.y
endif
(p0) mov r0.yz, c20.w
(!p0.x) add r0.x, r0.x, c20.y
mov oD0, r0
mov oT0, r1
ret
label l0
mov r1, v1
ret
)EOSHADER";

  // calls: rep around a call to l1, which calls l3, then callnz on b0
  const char *VS2XCalls = R"EOSHADER(
vs_2_x
dcl_position v0
dcl_color v1
defi i1, 3, 0, 0, 0
def c20, 0, 0.25, 0.5, 1
m4x4 oPos, v0, c0
mov r0, c20.x
rep i1
  call l1
endrep
callnz l2, b0
mov oD0, r0
mov oT0, v1
ret
label l1
add r0, r0, c20.y
call l3
ret
label l2
mul r0, r0, c20.z
ret
label l3
add r0.x, r0.x, v1.x
ret
)EOSHADER";

  // a constant table with a row_major matrix, an array of structs, a matrix the compiler only
  // allocates two columns of, and a bool array. 1 / zero is an infinity.
  const char *VS30Names = R"EOSHADER(

struct Light
{
  float4 colour;
  float3 dir;
  float range;
};

float4x4 wvp : register(c0);
row_major float4x3 world : register(c4);
Light lights[2] : register(c8);
float4x4 texMat : register(c14);
float zero : register(c18);
bool flags[2] : register(b0);

struct vin
{
  float3 pos : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

struct vout
{
  float4 pos : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  float extra : TEXCOORD1;
};

vout main(vin IN)
{
  vout OUT;
  float3 wp = mul(float4(IN.pos, 1), world);
  OUT.pos = mul(float4(wp, 1), wvp);
  float3 c = 0;
  for(int i = 0; i < 2; i++)
    c += lights[i].colour.rgb * saturate(dot(normalize(wp + 1), lights[i].dir) * lights[i].range);
  if(flags[1])
    c = c.bgr;
  OUT.col = float4(c, IN.col.a);
  OUT.uv = mul(float4(IN.uv, 0, 1), texMat).xy;
  OUT.extra = min(1.0f / zero, 4.0f);
  return OUT;
}

)EOSHADER";

  // a dynamic loop with relative addressing on aL and a break, texldl from a vertex texture
  const char *VS30 = R"EOSHADER(

float4x4 wvp : register(c0);
float4 offsets[4] : register(c10);
float4 params : register(c30);
sampler2D heights : register(s0);

struct vin
{
  float3 pos : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

struct vout
{
  float4 pos : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  float len : TEXCOORD1;
  float3 extra : TEXCOORD2;
};

vout main(vin IN)
{
  vout OUT;
  float4 h = tex2Dlod(heights, float4(IN.uv, 0, 0));
  float3 p = IN.pos;
  // 3 is the count the app sets in i0 (see CompileLoop)
  [loop] for(int i = 0; i < 3; i++)
  {
    p += offsets[i].xyz * h.x;
    if(p.y > params.x)
      break;
  }
  OUT.pos = mul(float4(p, 1), wvp);
  OUT.col = IN.col * h;
  OUT.uv = IN.uv * 2.0f;
  OUT.len = length(p);
  OUT.extra = cross(p, float3(0, 1, 0)) + params.yzw;
  return OUT;
}

)EOSHADER";

  // instancing: stream 1 has a per-instance offset
  const char *VS30Instanced = R"EOSHADER(

float4x4 wvp : register(c0);

struct vin
{
  float3 pos : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  float4 offset : TEXCOORD3;
};

struct vout
{
  float4 pos : POSITION;
  float4 col : COLOR0;
};

vout main(vin IN)
{
  vout OUT;
  OUT.pos = mul(float4(IN.pos * IN.offset.w + IN.offset.xyz, 1), wvp);
  OUT.col = IN.col * IN.offset;
  return OUT;
}

)EOSHADER";

  struct FFVertex
  {
    float x, y, z, rhw;
    D3DCOLOR col;
  };

  // c0-c3 as fxc reads a column_major float4x4 with mul(v, m): a scale and a translation that put
  // the triangle in a cell of a 4x2 grid
  static void Cell(float m[16], int cell)
  {
    const float x = -0.75f + 0.5f * float(cell % 4), y = 0.5f - 1.0f * float(cell / 4);
    const float c[16] = {
        0.4f, 0.0f, 0.0f, x,       //
        0.0f, 0.6f, 0.0f, y,       //
        0.0f, 0.0f, 1.0f, 0.0f,    //
        0.0f, 0.0f, 0.0f, 1.0f,    //
    };
    memcpy(m, c, sizeof(c));
  }

  int main()
  {
    if(!Init())
      return 3;

    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    pD3DAssemble assemble =
        d3dcompiler ? (pD3DAssemble)GetProcAddress(d3dcompiler, "D3DAssemble") : NULL;
    if(!assemble)
      TEST_ERROR("Couldn't load D3DAssemble, leaving out the vs_2_x draw");

    auto createVS = [this](const std::vector<DWORD> &tokens, const char *name) {
      IDirect3DVertexShader9Ptr ret;
      if(tokens.empty() || FAILED(dev->CreateVertexShader(tokens.data(), &ret)))
        TEST_ERROR("Couldn't create the %s vertex shader", name);
      return ret;
    };

    IDirect3DVertexShader9Ptr vs11 = createVS(VS11(), "vs_1_1");
    if(!vs11)
    {
      // the first instruction the runtime rejects: every shader has to write oPos, so the
      // shortest one tried ends at the m4x4
      std::vector<size_t> starts;
      const std::vector<DWORD> t = VS11(&starts);
      for(size_t n = 4; n < starts.size(); n++)
      {
        std::vector<DWORD> prefix(t.begin(), t.begin() + starts[n]);
        prefix.push_back(0x0000FFFF);
        IDirect3DVertexShader9Ptr probe;
        if(FAILED(dev->CreateVertexShader(prefix.data(), &probe)))
        {
          TEST_ERROR("The vs_1_1 shader is rejected at instruction %u", (uint32_t)n);
          break;
        }
      }
    }
    IDirect3DVertexShader9Ptr vs20 = createVS(CompileLoop(VS20, "vs_2_0", 0), "vs_2_0");
    IDirect3DVertexShader9Ptr vs30 = createVS(CompileLoop(VS30, "vs_3_0", 0), "vs_3_0");
    IDirect3DVertexShader9Ptr vs30inst = CreateVS(Compile(VS30Instanced, "main", "vs_3_0"));
    IDirect3DVertexShader9Ptr vs30names = CreateVS(Compile(VS30Names, "main", "vs_3_0"));

    auto assembleVS = [&](const char *source, const char *name) {
      IDirect3DVertexShader9Ptr ret;
      if(!assemble)
        return ret;
      ID3DBlobPtr code, errors;
      const std::string src = source;
      if(FAILED(assemble(src.c_str(), src.size(), NULL, NULL, NULL, 0, &code, &errors)))
        TEST_ERROR("Couldn't assemble the %s shader: %s", name,
                   errors ? (const char *)errors->GetBufferPointer() : "");
      else
        ret = createVS(
            std::vector<DWORD>((const DWORD *)code->GetBufferPointer(),
                               (const DWORD *)code->GetBufferPointer() + code->GetBufferSize() / 4),
            name);
      return ret;
    };
    IDirect3DVertexShader9Ptr vs2x = assembleVS(VS2X, "vs_2_x");
    IDirect3DVertexShader9Ptr vs2xcalls = assembleVS(VS2XCalls, "vs_2_x calls");

    // vs_3_0 and ps_3_0 have to be used together
    IDirect3DPixelShader9Ptr ps = CreatePS(Compile(D3D9DefaultPixel, "main", "ps_3_0"));
    IDirect3DPixelShader9Ptr ps20 = CreatePS(Compile(D3D9DefaultPixel, "main", "ps_2_0"));

    // the triangle, then three more vertices for the indexed instanced draw, after two padding
    // vertices for its base vertex
    const DefaultA2V verts[] = {
        {Vec3f(-0.5f, -0.5f, 0.0f), Vec4f(0.0f, 1.0f, 0.0f, 1.0f), Vec2f(0.0f, 0.0f)},
        {Vec3f(0.0f, 0.5f, 0.0f), Vec4f(0.2f, 1.0f, 0.4f, 1.0f), Vec2f(0.3f, 1.0f)},
        {Vec3f(0.5f, -0.5f, 0.0f), Vec4f(0.6f, 0.8f, 0.0f, 1.0f), Vec2f(1.0f, 0.2f)},
        {Vec3f(9.0f, 9.0f, 9.0f), Vec4f(), Vec2f()},
        {Vec3f(9.0f, 9.0f, 9.0f), Vec4f(), Vec2f()},
        {Vec3f(-0.4f, -0.4f, 0.1f), Vec4f(1.0f, 0.5f, 0.0f, 1.0f), Vec2f(0.1f, 0.9f)},
        {Vec3f(0.4f, -0.4f, 0.2f), Vec4f(0.5f, 1.0f, 0.5f, 1.0f), Vec2f(0.8f, 0.7f)},
        {Vec3f(0.0f, 0.4f, 0.3f), Vec4f(0.0f, 0.5f, 1.0f, 1.0f), Vec2f(0.5f, 0.1f)},
    };
    IDirect3DVertexBuffer9Ptr vb = MakeVB(verts, sizeof(verts));

    // per instance: an offset in xyz and a scale in w
    const float instances[] = {
        -0.15f, 0.0f, 0.0f, 0.6f,    //
        0.15f,  0.1f, 0.0f, 0.4f,    //
    };
    IDirect3DVertexBuffer9Ptr instVB = MakeVB(instances, sizeof(instances));

    // three padding indices for the start index, then the three vertices from the base vertex
    const uint16_t indices[] = {7, 7, 7, 1, 0, 2};
    IDirect3DIndexBuffer9Ptr ib;
    if(FAILED(dev->CreateIndexBuffer(sizeof(indices), 0, D3DFMT_INDEX16,
                                     useEx ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &ib, NULL)))
    {
      TEST_ERROR("CreateIndexBuffer failed");
      return 4;
    }
    void *ibData = NULL;
    ib->Lock(0, 0, &ibData, 0);
    memcpy(ibData, indices, sizeof(indices));
    ib->Unlock();

    const D3DVERTEXELEMENT9 instElems[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 28, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9Ptr instDecl;
    dev->CreateVertexDeclaration(instElems, &instDecl);

    // a 4x4 vertex texture for texldl, point sampled
    IDirect3DTexture9Ptr heights;
    if(FAILED(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A32B32G32R32F,
                                 useEx ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &heights, NULL)))
    {
      TEST_ERROR("Couldn't create the vertex texture");
      return 4;
    }
    D3DLOCKED_RECT rect = {};
    heights->LockRect(0, &rect, NULL, 0);
    for(int y = 0; y < 4; y++)
    {
      float *row = (float *)((byte *)rect.pBits + y * rect.Pitch);
      for(int x = 0; x < 4; x++)
      {
        row[x * 4 + 0] = 0.25f + 0.125f * float(x);
        row[x * 4 + 1] = 0.5f + 0.1f * float(y);
        row[x * 4 + 2] = 1.0f - 0.2f * float(x);
        row[x * 4 + 3] = 1.0f;
      }
    }
    heights->UnlockRect(0);

    const float w = (float)screenWidth, h = (float)screenHeight;
    const FFVertex ffTri[3] = {
        {w * 0.80f, h * 0.95f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 0, 255)},
        {w * 0.875f, h * 0.75f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 0, 255)},
        {w * 0.95f, h * 0.95f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 0, 255)},
    };

    float m[16];

    while(Running())
    {
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      dev->BeginScene();

      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetVertexDeclaration(defaultDecl);
      dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));

      if(vs11)
      {
        pushMarker("vs_1_1");
        Cell(m, 0);
        dev->SetVertexShaderConstantF(0, m, 4);
        const float consts[] = {
            1.0f, 0.5f, 0.25f, 1.0f,     // c4
            0.5f, 0.5f, 1.0f,  1.0f,     // c5
            0.8f, 1.2f, 0.6f,  1.0f,     // c6: c[a0.x + 4]
            0.5f, 0.8f, 0.0f,  16.0f,    // c7: lit
            2.0f, 3.0f, 4.0f,  5.0f,     // c8: dst
        };
        dev->SetVertexShaderConstantF(4, consts, 5);
        const float c20[4] = {0.5f, 2.5f, -1.0f, 0.25f};
        dev->SetVertexShaderConstantF(20, c20, 1);
        dev->SetVertexShader(vs11);
        dev->SetPixelShader(ps20);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
        popMarker();
      }

      if(vs20)
      {
        pushMarker("vs_2_0");
        Cell(m, 1);
        dev->SetVertexShaderConstantF(0, m, 4);
        const float lights[] = {
            1.0f, 0.0f, 0.0f, 0.0f,    //
            0.0f, 1.0f, 0.0f, 0.0f,    //
            0.0f, 0.0f, 1.0f, 0.0f,    //
            0.5f, 0.5f, 0.5f, 0.0f,    //
        };
        dev->SetVertexShaderConstantF(10, lights, 4);
        const float params[4] = {0.9f, 0.7f, 1.1f, 2.0f};
        dev->SetVertexShaderConstantF(20, params, 1);
        const int loop[4] = {4, 0, 1, 0};
        dev->SetVertexShaderConstantI(0, loop, 1);
        const BOOL useParams = TRUE;
        dev->SetVertexShaderConstantB(0, &useParams, 1);
        dev->SetVertexShader(vs20);
        dev->SetPixelShader(ps20);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
        popMarker();
      }

      if(vs2x)
      {
        pushMarker("vs_2_x");
        Cell(m, 2);
        dev->SetVertexShaderConstantF(0, m, 4);
        const float steps[] = {
            0.25f, 0.5f, 0.75f, 1.0f,    //
            0.5f,  0.1f, 0.2f,  0.0f,    //
            0.5f,  0.3f, 0.1f,  0.0f,    //
        };
        dev->SetVertexShaderConstantF(10, steps, 3);
        dev->SetVertexShader(vs2x);
        dev->SetPixelShader(ps20);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
        popMarker();
      }

      if(vs30)
      {
        pushMarker("vs_3_0");
        Cell(m, 3);
        dev->SetVertexShaderConstantF(0, m, 4);
        const float offsets[] = {
            0.0f, 0.1f,  0.0f, 0.0f,    //
            0.1f, 0.2f,  0.0f, 0.0f,    //
            0.0f, -0.1f, 0.1f, 0.0f,    //
        };
        dev->SetVertexShaderConstantF(10, offsets, 3);
        const float params[4] = {0.4f, 0.1f, 0.2f, 0.3f};
        dev->SetVertexShaderConstantF(30, params, 1);
        const int loop[4] = {3, 0, 1, 0};
        dev->SetVertexShaderConstantI(0, loop, 1);
        dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, heights);
        dev->SetVertexShader(vs30);
        dev->SetPixelShader(ps);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
        dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, NULL);
        popMarker();
      }

      if(vs30inst)
      {
        pushMarker("vs_3_0 instanced");
        Cell(m, 4);
        dev->SetVertexShaderConstantF(0, m, 4);
        dev->SetVertexDeclaration(instDecl);
        dev->SetStreamSource(1, instVB, 0, sizeof(float) * 4);
        dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 2);
        dev->SetStreamSourceFreq(1, UINT(D3DSTREAMSOURCE_INSTANCEDATA | 1));
        dev->SetIndices(ib);
        dev->SetVertexShader(vs30inst);
        dev->SetPixelShader(ps);
        // base vertex 5, start index 3: vertices 6, 5 and 7
        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 5, 0, 3, 3, 1);
        dev->SetStreamSourceFreq(0, 1);
        dev->SetStreamSourceFreq(1, 1);
        dev->SetStreamSource(1, NULL, 0, 0);
        dev->SetVertexDeclaration(defaultDecl);
        popMarker();
      }

      if(vs2xcalls)
      {
        pushMarker("vs_2_x calls");
        Cell(m, 5);
        dev->SetVertexShaderConstantF(0, m, 4);
        const BOOL callL2 = TRUE;
        dev->SetVertexShaderConstantB(0, &callL2, 1);
        dev->SetVertexShader(vs2xcalls);
        dev->SetPixelShader(ps20);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
        popMarker();
      }

      if(vs30names)
      {
        pushMarker("vs_3_0 names");
        Cell(m, 6);
        dev->SetVertexShaderConstantF(0, m, 4);
        // world is row_major: a register per row, the translation in the last
        const float world[] = {
            1.0f,  0.0f, 0.0f, 0.0f,    //
            0.0f,  1.0f, 0.0f, 0.0f,    //
            0.0f,  0.0f, 1.0f, 0.0f,    //
            0.05f, 0.1f, 0.0f, 0.0f,    //
        };
        dev->SetVertexShaderConstantF(4, world, 4);
        // two lights: colour, dir, range
        const float lights[] = {
            1.0f,  0.5f, 0.25f, 1.0f,    //
            0.6f,  0.8f, 0.0f,  0.0f,    //
            0.75f, 0.0f, 0.0f,  0.0f,    //
            0.25f, 0.5f, 1.0f,  1.0f,    //
            0.0f,  0.6f, 0.8f,  0.0f,    //
            1.5f,  0.0f, 0.0f,  0.0f,    //
        };
        dev->SetVertexShaderConstantF(8, lights, 6);
        // texMat's first two columns: u' = 2u + 0.1, v' = 0.5v + 0.2
        const float texMat[] = {
            2.0f, 0.0f, 0.0f, 0.1f,    //
            0.0f, 0.5f, 0.0f, 0.2f,    //
        };
        dev->SetVertexShaderConstantF(14, texMat, 2);
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        dev->SetVertexShaderConstantF(18, zero, 1);
        const BOOL flags[2] = {FALSE, TRUE};
        dev->SetVertexShaderConstantB(0, flags, 2);
        dev->SetVertexShader(vs30names);
        dev->SetPixelShader(ps);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
        popMarker();
      }

      pushMarker("Fixed function");
      dev->SetVertexShader(NULL);
      dev->SetPixelShader(NULL);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
      dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
      dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, ffTri, sizeof(FFVertex));
      popMarker();

      dev->EndScene();

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
