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

RD_TEST(D3D9_Shader_Debug_Zoo, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Draws with a vertex shader of each model to debug (vs_1_1, vs_2_0, vs_2_x and vs_3_0), "
      "each in its own marker, plus an instanced vs_3_0 draw with a base vertex, a vs_2_x draw "
      "with nested calls, a vs_3_0 draw whose constant table has structs, arrays and matrices, and "
      "a fixed function draw, which can't be debugged. Then pixel shader draws to A32B32G32R32F "
      "targets (ps_2_0, ps_2_x with derivatives and two targets, ps_3_0 with vPos, vFace, a loop "
      "and oDepth), transcendentals, and lookups: texld, texldp, texldb, texldd and texldl, of "
      "mipmapped 2D, cube and volume textures, anisotropic, with a border colour, sRGB, SetLOD, a "
      "D24S8 shadow map drawn in the frame, and samplers with nothing bound. A ps_1_1 draw can't "
      "be debugged yet. Natively, what the GPU wrote at a few pixels of each is written to "
      "D3D9_Shader_Debug_Zoo_pixels.txt.";

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

  // The pixel draws. Each fills a cell of the float targets with two triangles whose vertices
  // have different w, so the inputs are interpolated with perspective. The vertex shader is the
  // same HLSL for vs_2_0 and vs_3_0. c0 and c1 map uv, so the lookups' coordinates are inputs as
  // the rasterizer gave them; they're the identity unless a draw says otherwise.
  const char *PixelVS = R"EOSHADER(
float4 uvRow0 : register(c0);
float4 uvRow1 : register(c1);

struct a2v
{
  float4 pos : POSITION;
  float4 col : COLOR0;
  float4 uv : TEXCOORD0;
};

struct v2f
{
  float4 pos : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  float4 uv1 : TEXCOORD1;
  float3 uv2 : TEXCOORD2;
};

v2f main(a2v IN)
{
  v2f OUT;
  OUT.pos = IN.pos;
  OUT.col = IN.col;
  OUT.uv = float2(dot(float3(IN.uv.xy, 1.0f), uvRow0.xyz), dot(float3(IN.uv.xy, 1.0f), uvRow1.xyz));
  OUT.uv1 = IN.uv * 2.0f - 0.5f;
  OUT.uv2 = IN.uv.zwx + IN.col.xyz;
  return OUT;
}
)EOSHADER";

  // arithmetic on colours and texture coordinates, with constants
  const char *PS20 = R"EOSHADER(
float4 k0 : register(c0);
float4 k1 : register(c1);

struct v2f
{
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  float4 uv1 : TEXCOORD1;
  float3 uv2 : TEXCOORD2;
};

float4 main(v2f IN) : COLOR0
{
  float4 a = IN.col * k0 + float4(IN.uv, IN.uv.x * IN.uv.y, 1.0f);
  float d = dot(IN.uv1, k1);
  return a * d + float4(IN.uv2, frac(IN.uv1.w * 3.0f));
}
)EOSHADER";

  // derivatives, of a product of inputs and of an input, and two targets
  const char *PS2X = R"EOSHADER(
float4 k0 : register(c0);

struct v2f
{
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  float4 uv1 : TEXCOORD1;
  float3 uv2 : TEXCOORD2;
};

struct f2a
{
  float4 c0 : COLOR0;
  float4 c1 : COLOR1;
};

f2a main(v2f IN)
{
  f2a OUT;
  float4 v = IN.uv1 * IN.uv1.yxwz + IN.col;
  OUT.c0 = v + ddx(v) * 16.0f;
  OUT.c1 = ddy(v) * 16.0f + ddx(IN.uv2.xyzz) * k0;
  return OUT;
}
)EOSHADER";

  // vPos, vFace (one triangle of the draw is clockwise, the other not), a loop, a branch,
  // derivatives, two targets and oDepth
  const char *PS30 = R"EOSHADER(
float4 k0 : register(c0);

struct v2f
{
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  float4 uv1 : TEXCOORD1;
  float3 uv2 : TEXCOORD2;
  float face : VFACE;
  float2 vpos : VPOS;
};

struct f2a
{
  float4 c0 : COLOR0;
  float4 c1 : COLOR1;
  float depth : DEPTH;
};

f2a main(v2f IN)
{
  f2a OUT;
  float4 acc = IN.col;
  [loop] for(int i = 0; i < 3; i++)
    acc = acc * k0 + IN.uv1;
  [branch] if(IN.uv.x > 0.5f)
    acc.xy += IN.vpos * 0.001f;
  OUT.c0 = IN.face > 0 ? acc : -acc;
  OUT.c1 = float4(ddx(IN.uv2.x), ddy(IN.uv2.y), IN.vpos.x, IN.vpos.y);
  OUT.depth = saturate(IN.uv.y * 0.5f + 0.25f);
  return OUT;
}
)EOSHADER";

  // a point sampled 4x4 float texture
  const char *PS30Texture = R"EOSHADER(
sampler2D tex : register(s0);

struct v2f
{
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

float4 main(v2f IN) : COLOR0
{
  return tex2D(tex, IN.uv) * IN.col;
}
)EOSHADER";

  // The lookups: the same v2f for all of them
  const char *SampleV2F = R"EOSHADER(
struct v2f
{
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  float4 uv1 : TEXCOORD1;
  float3 uv2 : TEXCOORD2;
};

struct f2a
{
  float4 c0 : COLOR0;
  float4 c1 : COLOR1;
};
)EOSHADER";

  // rcp, rsq, exp, log, sincos, and pow (which fxc writes as log, mul and exp)
  const char *PS30Transcendentals = R"EOSHADER(
f2a main(v2f IN)
{
  f2a OUT;
  OUT.c0 = float4(1.0f / (IN.col.x + 0.1f), rsqrt(IN.uv1.y * IN.uv1.y + 0.01f), exp2(IN.uv1.z),
                  log2(abs(IN.uv1.x) + 0.01f));
  float s, c;
  sincos(IN.uv2.x * 5.0f, s, c);
  OUT.c1 = float4(s, c, pow(abs(IN.uv1.w) + 0.1f, IN.col.z * 3.0f), 0.0f);
  return OUT;
}
)EOSHADER";

  // the transcendentals of inputs as they are, assembled, since HLSL's sincos reduces its argument
  // to [-pi, pi] first
  const char *PS30TranscendentalsAsm = R"EOSHADER(
ps_3_0
def c0, 0, 0, 0, 0
dcl_color v0
dcl_texcoord1 v1
dcl_texcoord2 v2
mov r0, c0
rcp r0.x, v0.x
rsq r0.y, v1.y
exp r0.z, v1.z
log r0.w, v1.x
mov oC0, r0
mov r1, c0
sincos r1.xy, v2.x
mov r2, v0
pow r1.z, v1.w, r2.z
mov oC1, r1
)EOSHADER";

  // texld, texldp and texldb, on a mipmapped texture sampled trilinearly
  const char *PS20Lookups = R"EOSHADER(
sampler2D tex : register(s0);

f2a main(v2f IN)
{
  f2a OUT;
  OUT.c0 = tex2D(tex, IN.uv);
  OUT.c1 = tex2Dproj(tex, float4(IN.uv1.xy, 0.0f, IN.col.w + 0.5f)) +
           tex2Dbias(tex, float4(IN.uv, 0.0f, IN.uv1.w));
  return OUT;
}
)EOSHADER";

  // texldd, with derivatives from dsx and dsy, scaled
  const char *PS2XGradients = R"EOSHADER(
sampler2D tex : register(s0);

f2a main(v2f IN)
{
  f2a OUT;
  OUT.c0 = tex2Dgrad(tex, IN.uv, ddx(IN.uv) * 3.0f, ddy(IN.uv) * 0.5f);
  OUT.c1 = tex2Dgrad(tex, IN.uv1.xy, ddy(IN.uv1.xy), ddx(IN.uv1.xy));
  return OUT;
}
)EOSHADER";

  // one lookup in c0, and IN.uv in c1 (for the anisotropic, sRGB and SetLOD draws)
  const char *PS30Lookup2D = R"EOSHADER(
sampler2D tex : register(s0);

f2a main(v2f IN)
{
  f2a OUT;
  OUT.c0 = tex2D(tex, IN.uv);
  OUT.c1 = float4(IN.uv, 0.0f, 1.0f);
  return OUT;
}
)EOSHADER";

  const char *PS30Cube = R"EOSHADER(
samplerCUBE tex : register(s0);

f2a main(v2f IN)
{
  f2a OUT;
  OUT.c0 = texCUBE(tex, IN.uv2 - float3(0.9f, 0.7f, 1.0f));
  OUT.c1 = texCUBEbias(tex, float4(IN.uv1.zxy, IN.uv1.w));
  return OUT;
}
)EOSHADER";

  const char *PS30Volume = R"EOSHADER(
sampler3D tex : register(s0);

f2a main(v2f IN)
{
  f2a OUT;
  OUT.c0 = tex3D(tex, IN.uv1.xyz);
  OUT.c1 = tex3Dlod(tex, float4(IN.uv2, IN.uv1.w * 2.0f));
  return OUT;
}
)EOSHADER";

  // a border colour, with uv outside [0, 1], and texldl
  const char *PS30Border = R"EOSHADER(
sampler2D tex : register(s0);

f2a main(v2f IN)
{
  f2a OUT;
  OUT.c0 = tex2D(tex, IN.uv);
  OUT.c1 = tex2Dlod(tex, float4(IN.uv, 0.0f, IN.uv1.w * 2.0f));
  return OUT;
}
)EOSHADER";

  // a D24S8 texture: the GPU compares z with the depth, and filters the results. Its depth is u,
  // so comparing with v passes on one side of the diagonal, with filtered results near it.
  const char *PS30Shadow = R"EOSHADER(
sampler2D tex : register(s0);

f2a main(v2f IN)
{
  f2a OUT;
  OUT.c0 = tex2Dproj(tex, float4(IN.uv, IN.uv.y, 1.0f));
  OUT.c1 = tex2Dbias(tex, float4(IN.uv1.xy, IN.col.x, 0.0f));
  return OUT;
}
)EOSHADER";

  // samplers with nothing bound
  const char *PS30Unbound = R"EOSHADER(
sampler2D tex : register(s0);
samplerCUBE cube : register(s1);

f2a main(v2f IN)
{
  f2a OUT;
  OUT.c0 = tex2D(tex, IN.uv);
  OUT.c1 = texCUBE(cube, IN.uv2);
  return OUT;
}
)EOSHADER";

  struct PixelVertex
  {
    float pos[4];
    float col[4];
    float uv[4];
  };

  struct PixelDraw
  {
    const char *marker;
    // the cell of the targets it fills: 4 across, 2 down, in the first pair of targets (0-7), the
    // second (8-15) or the third (16-23)
    int cell;
    // vs_3_0 and ps_3_0, or vs_2_0
    bool sm3;
  };

  enum
  {
    DrawPS20,
    DrawPS2X,
    DrawPS30,
    DrawPS30Texture,
    DrawPS11,
    DrawTranscendentals,
    DrawPS20Lookups,
    DrawPS2XGradients,
    DrawAnisotropic,
    DrawCube,
    DrawVolume,
    DrawBorder,
    DrawSRGB,
    DrawSetLOD,
    DrawShadow,
    DrawUnbound,
    DrawTranscendentalsAsm,
    NumPixelDraws,
  };
  static constexpr int NumTargetPairs = 3;
  const PixelDraw PixelDraws[NumPixelDraws] = {
      {"ps_2_0", 0, false},
      {"ps_2_x", 1, false},
      {"ps_3_0", 2, true},
      {"ps_3_0 texture", 3, true},
      {"ps_1_1", 4, false},
      {"ps_3_0 transcendentals", 5, true},
      {"ps_2_0 lookups", 6, false},
      {"ps_2_x gradients", 7, false},
      {"ps_3_0 anisotropic", 8, true},
      {"ps_3_0 cube", 9, true},
      {"ps_3_0 volume", 10, true},
      {"ps_3_0 border", 11, true},
      {"ps_3_0 sRGB", 12, true},
      {"ps_3_0 SetLOD", 13, true},
      {"ps_3_0 shadow", 14, true},
      {"ps_3_0 unbound", 15, true},
      {"ps_3_0 transcendentals asm", 16, true},
  };

  // two triangles filling most of a cell. The corners' w are 1, 2, 1.5 and 3. With clockwise the
  // second triangle is clockwise like the first, otherwise it's counter-clockwise.
  void PixelQuad(int cell, bool clockwise, PixelVertex out[6])
  {
    cell %= 8;
    const float x0 = -1.0f + 0.5f * float(cell % 4) + 0.05f, x1 = x0 + 0.4f;
    const float y0 = 0.95f - 1.0f * float(cell / 4), y1 = y0 - 0.9f;
    const PixelVertex corners[4] = {
        // top left, top right, bottom left, bottom right
        {{x0, y0, 0.5f, 1.0f}, {0.2f, 0.4f, 0.6f, 1.0f}, {0.0f, 0.0f, 0.25f, 0.5f}},
        {{x1, y0, 0.5f, 2.0f}, {0.9f, 0.1f, 0.3f, 0.5f}, {1.0f, 0.0f, 0.75f, -0.5f}},
        {{x0, y1, 0.5f, 1.5f}, {0.5f, 0.8f, 0.0f, 0.25f}, {0.0f, 1.0f, -0.5f, 2.0f}},
        {{x1, y1, 0.5f, 3.0f}, {0.1f, 0.6f, 1.0f, 0.75f}, {1.0f, 1.0f, 1.5f, 1.0f}},
    };
    const int order[2][6] = {{0, 1, 2, 1, 3, 2}, {0, 1, 2, 1, 2, 3}};
    for(int v = 0; v < 6; v++)
    {
      out[v] = corners[order[clockwise ? 0 : 1][v]];
      // x, y and z as the w given
      for(int c = 0; c < 3; c++)
        out[v].pos[c] *= out[v].pos[3];
    }
  }

  // the pixels each pixel draw's reference has, relative to its cell's top left: a whole quad, and
  // others on either side of the diagonal (the ps_3_0 draw's triangles have different windings)
  static constexpr int NumProbes = 7;
  const int Probes[NumProbes][2] = {
      {40, 60}, {41, 60}, {40, 61}, {41, 61}, {23, 37}, {70, 100}, {88, 130},
  };

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

  // each probe of each pixel draw that was drawn: its marker, x, y, then both of its targets'
  // values as float bits
  void WritePixelReference(IDirect3DSurface9Ptr targets[NumTargetPairs * 2],
                           const bool drawn[NumPixelDraws])
  {
    const std::string path = tmpPath + "/D3D9_Shader_Debug_Zoo_pixels.txt";
    FILE *f = NULL;
    fopen_s(&f, path.c_str(), "w");
    if(!f)
    {
      TEST_ERROR("Couldn't write %s", path.c_str());
      return;
    }

    IDirect3DSurface9Ptr readback[NumTargetPairs * 2];
    D3DLOCKED_RECT locked[NumTargetPairs * 2] = {};
    for(int t = 0; t < NumTargetPairs * 2; t++)
    {
      CHECK_HR(dev->CreateOffscreenPlainSurface(screenWidth, screenHeight, D3DFMT_A32B32G32R32F,
                                                D3DPOOL_SYSTEMMEM, &readback[t], NULL));
      CHECK_HR(dev->GetRenderTargetData(targets[t], readback[t]));
      CHECK_HR(readback[t]->LockRect(&locked[t], NULL, D3DLOCK_READONLY));
    }

    for(int d = 0; d < NumPixelDraws; d++)
    {
      const PixelDraw &draw = PixelDraws[d];
      if(!drawn[d])
        continue;
      const int cell = draw.cell % 8, set = draw.cell / 8;
      const int cx = (cell % 4) * screenWidth / 4, cy = (cell / 4) * screenHeight / 2;
      for(const auto &probe : Probes)
      {
        const int x = cx + probe[0], y = cy + probe[1];
        fprintf(f, "%s\t%d\t%d", draw.marker, x, y);
        for(int t = set * 2; t < set * 2 + 2; t++)
        {
          const DWORD *texel =
              (const DWORD *)((const byte *)locked[t].pBits + y * locked[t].Pitch) + x * 4;
          for(int c = 0; c < 4; c++)
            fprintf(f, "\t%08x", texel[c]);
        }
        fprintf(f, "\n");
      }
    }

    for(int t = 0; t < NumTargetPairs * 2; t++)
      readback[t]->UnlockRect();
    fclose(f);
  }

  // a texel of the sampled textures, different in each level, face and slice: a level's texels
  // vary across it, so filtering between them shows
  static DWORD Texel(int x, int y, int z, int level)
  {
    return D3DCOLOR_ARGB(255 - level * 30, (x * 37 + level * 50) & 255, (y * 59 + z * 23) & 255,
                         (((x ^ y) * 16) + level * 40 + z * 7) & 255);
  }

  // a managed A8R8G8B8 texture with every level of Texel
  IDirect3DTexture9Ptr MakeMipTexture(UINT size)
  {
    IDirect3DTexture9Ptr ret;
    if(FAILED(dev->CreateTexture(size, size, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &ret, NULL)))
    {
      TEST_ERROR("Couldn't create a %ux%u texture", size, size);
      return ret;
    }
    for(DWORD l = 0; l < ret->GetLevelCount(); l++)
    {
      D3DLOCKED_RECT rect = {};
      ret->LockRect(l, &rect, NULL, 0);
      const int s = int(size >> l);
      for(int y = 0; y < s; y++)
        for(int x = 0; x < s; x++)
          ((DWORD *)((byte *)rect.pBits + y * rect.Pitch))[x] = Texel(x, y, 0, l);
      ret->UnlockRect(l);
    }
    return ret;
  }

  void SetFilter(DWORD sampler, DWORD min, DWORD mag, DWORD mip, DWORD address)
  {
    dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, min);
    dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, mag);
    dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, mip);
    dev->SetSamplerState(sampler, D3DSAMP_ADDRESSU, address);
    dev->SetSamplerState(sampler, D3DSAMP_ADDRESSV, address);
    dev->SetSamplerState(sampler, D3DSAMP_ADDRESSW, address);
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

    // the pixel draws' targets, in pairs, and what they need
    IDirect3DSurface9Ptr pixelTargets[NumTargetPairs * 2];
    for(int t = 0; t < NumTargetPairs * 2; t++)
      CHECK_HR(dev->CreateRenderTarget(screenWidth, screenHeight, D3DFMT_A32B32G32R32F,
                                       D3DMULTISAMPLE_NONE, 0, FALSE, &pixelTargets[t], NULL));
    IDirect3DSurface9Ptr backBuffer, backDepth;
    dev->GetRenderTarget(0, &backBuffer);
    dev->GetDepthStencilSurface(&backDepth);

    // what the lookups sample: a 64x64 texture with all its levels, the same with SetLOD(2), a
    // 16x16 cube and a 16x16x16 volume texture with all theirs
    IDirect3DTexture9Ptr mipTex = MakeMipTexture(64);
    IDirect3DTexture9Ptr lodTex = MakeMipTexture(64);
    if(lodTex)
      lodTex->SetLOD(2);

    IDirect3DCubeTexture9Ptr cubeTex;
    if(SUCCEEDED(dev->CreateCubeTexture(16, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cubeTex, NULL)))
    {
      for(DWORD l = 0; l < cubeTex->GetLevelCount(); l++)
      {
        for(int face = 0; face < 6; face++)
        {
          D3DLOCKED_RECT faceRect = {};
          cubeTex->LockRect(D3DCUBEMAP_FACES(face), l, &faceRect, NULL, 0);
          const int s = 16 >> l;
          for(int y = 0; y < s; y++)
            for(int x = 0; x < s; x++)
              ((DWORD *)((byte *)faceRect.pBits + y * faceRect.Pitch))[x] = Texel(x, y, face * 3, l);
          cubeTex->UnlockRect(D3DCUBEMAP_FACES(face), l);
        }
      }
    }
    else
    {
      TEST_ERROR("Couldn't create the cube texture");
    }

    IDirect3DVolumeTexture9Ptr volTex;
    if(SUCCEEDED(dev->CreateVolumeTexture(16, 16, 16, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                          &volTex, NULL)))
    {
      for(DWORD l = 0; l < volTex->GetLevelCount(); l++)
      {
        D3DLOCKED_BOX box = {};
        volTex->LockBox(l, &box, NULL, 0);
        const int s = 16 >> l;
        for(int z = 0; z < s; z++)
          for(int y = 0; y < s; y++)
            for(int x = 0; x < s; x++)
              ((DWORD *)((byte *)box.pBits + z * box.SlicePitch + y * box.RowPitch))[x] =
                  Texel(x, y, z, l);
        volTex->UnlockBox(l);
      }
    }
    else
    {
      TEST_ERROR("Couldn't create the volume texture");
    }

    // an 8x8 D24S8 texture, drawn each frame, which the shadow draw compares against: big texels,
    // so many pixels get results filtered between them. Not every GPU can sample depth formats.
    IDirect3DTexture9Ptr shadowTex;
    IDirect3DSurface9Ptr shadowDepth, shadowColour;
    if(SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                        D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D24S8)) &&
       SUCCEEDED(dev->CreateTexture(8, 8, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT,
                                    &shadowTex, NULL)))
    {
      shadowTex->GetSurfaceLevel(0, &shadowDepth);
      CHECK_HR(dev->CreateRenderTarget(8, 8, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                       &shadowColour, NULL));
    }
    else
    {
      TEST_WARN("D24S8 textures can't be sampled, leaving out the shadow draw");
    }
    // the shadow map's depth: u, from 0.02 on the left to 0.98 on the right
    const FFVertex shadowQuad[4] = {
        {-0.5f, -0.5f, 0.02f, 1.0f, 0xFFFFFFFF},
        {7.5f, -0.5f, 0.98f, 1.0f, 0xFFFFFFFF},
        {-0.5f, 7.5f, 0.02f, 1.0f, 0xFFFFFFFF},
        {7.5f, 7.5f, 0.98f, 1.0f, 0xFFFFFFFF},
    };
    const bool srgbRead = SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                           D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_SRGBREAD,
                                                           D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8));
    if(!srgbRead)
      TEST_WARN("A8R8G8B8 can't be read as sRGB, leaving out the sRGB draw");

    const D3DVERTEXELEMENT9 pixelElems[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9Ptr pixelDecl;
    CHECK_HR(dev->CreateVertexDeclaration(pixelElems, &pixelDecl));

    IDirect3DVertexShader9Ptr pixelVS20 = CreateVS(Compile(PixelVS, "main", "vs_2_0"));
    IDirect3DVertexShader9Ptr pixelVS30 = CreateVS(Compile(PixelVS, "main", "vs_3_0"));
    auto lookupPS = [&](const char *source, const char *profile) {
      return CreatePS(Compile(std::string(SampleV2F) + source, "main", profile));
    };
    IDirect3DPixelShader9Ptr pixelShaders[NumPixelDraws] = {
        CreatePS(Compile(PS20, "main", "ps_2_0")),
        CreatePS(Compile(PS2X, "main", "ps_2_a")),
        CreatePS(Compile(PS30, "main", "ps_3_0")),
        CreatePS(Compile(PS30Texture, "main", "ps_3_0")),
        NULL,
        lookupPS(PS30Transcendentals, "ps_3_0"),
        lookupPS(PS20Lookups, "ps_2_0"),
        lookupPS(PS2XGradients, "ps_2_a"),
        lookupPS(PS30Lookup2D, "ps_3_0"),
        lookupPS(PS30Cube, "ps_3_0"),
        lookupPS(PS30Volume, "ps_3_0"),
        lookupPS(PS30Border, "ps_3_0"),
        lookupPS(PS30Lookup2D, "ps_3_0"),
        lookupPS(PS30Lookup2D, "ps_3_0"),
        lookupPS(PS30Shadow, "ps_3_0"),
        lookupPS(PS30Unbound, "ps_3_0"),
    };
    auto assemblePS = [&](const std::string &src, int d) {
      if(!assemble)
        return;
      ID3DBlobPtr code, errors;
      if(FAILED(assemble(src.c_str(), src.size(), NULL, NULL, NULL, 0, &code, &errors)))
        TEST_ERROR("Couldn't assemble the %s shader: %s", PixelDraws[d].marker,
                   errors ? (const char *)errors->GetBufferPointer() : "");
      else
        dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &pixelShaders[d]);
    };
    assemblePS("ps_1_1\nmov r0, v0\n", DrawPS11);
    assemblePS(PS30TranscendentalsAsm, DrawTranscendentalsAsm);
    bool drawn[NumPixelDraws] = {};
    for(int d = 0; d < NumPixelDraws; d++)
    {
      if(!pixelShaders[d])
        TEST_ERROR("Couldn't create the %s pixel shader", PixelDraws[d].marker);
      drawn[d] = pixelShaders[d] != NULL;
    }
    drawn[DrawShadow] &= shadowTex != NULL;
    drawn[DrawSRGB] &= srgbRead;

    // each pixel draw's sampler 0 (and 1), and the uv mapping in the vertex shader's c0 and c1
    auto setupLookups = [&](int d) {
      float uv[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
      auto scale = [&](float s) {
        uv[0] = s;
        uv[5] = s;
      };
      SetFilter(0, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_WRAP);
      dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 1);
      dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
      dev->SetSamplerState(0, D3DSAMP_BORDERCOLOR, 0);
      dev->SetTexture(0, NULL);
      dev->SetTexture(1, NULL);
      switch(d)
      {
        case DrawPS30Texture:
          SetFilter(0, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_WRAP);
          dev->SetTexture(0, heights);
          break;
        case DrawPS20Lookups:
          scale(4.0f);
          dev->SetTexture(0, mipTex);
          break;
        case DrawPS2XGradients:
          // anisotropic, with texldd's derivatives
          SetFilter(0, D3DTEXF_ANISOTROPIC, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_WRAP);
          dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 8);
          scale(3.0f);
          dev->SetTexture(0, mipTex);
          break;
        case DrawAnisotropic:
        {
          // u stretched 8 times and the mapping turned by 30 degrees
          SetFilter(0, D3DTEXF_ANISOTROPIC, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_WRAP);
          dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 16);
          const float c = 0.8660254f, s = 0.5f;
          const float rows[8] = {8.0f * c, -8.0f * s, 0.0f, 0.0f, s, c, 0.0f, 0.0f};
          memcpy(uv, rows, sizeof(uv));
          dev->SetTexture(0, mipTex);
          break;
        }
        case DrawCube:
          SetFilter(0, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTADDRESS_CLAMP);
          dev->SetTexture(0, cubeTex);
          break;
        case DrawVolume: dev->SetTexture(0, volTex); break;
        case DrawBorder:
        {
          SetFilter(0, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTADDRESS_BORDER);
          dev->SetSamplerState(0, D3DSAMP_BORDERCOLOR, D3DCOLOR_ARGB(0x80, 0xFF, 0x40, 0x20));
          const float rows[8] = {1.6f, 0.0f, -0.3f, 0.0f, 0.0f, 1.6f, -0.3f, 0.0f};
          memcpy(uv, rows, sizeof(uv));
          dev->SetTexture(0, mipTex);
          break;
        }
        case DrawSRGB:
          dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE);
          scale(2.0f);
          dev->SetTexture(0, mipTex);
          break;
        case DrawSetLOD:
          scale(4.0f);
          dev->SetTexture(0, lodTex);
          break;
        case DrawShadow:
          // the comparisons filtered
          SetFilter(0, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTADDRESS_CLAMP);
          dev->SetTexture(0, shadowTex);
          break;
        default: break;
      }
      dev->SetVertexShaderConstantF(0, uv, 2);
    };

    // natively, what the GPU wrote at the probes after the first frame
    bool wroteReference = rdoc != NULL;

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

      // the shadow map the shadow draw samples, drawn in the frame
      if(shadowTex)
      {
        pushMarker("Shadow map");
        dev->SetRenderTarget(0, shadowColour);
        dev->SetDepthStencilSurface(shadowDepth);
        dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
        dev->SetVertexShader(NULL);
        dev->SetPixelShader(NULL);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, shadowQuad, sizeof(FFVertex));
        dev->SetDepthStencilSurface(backDepth);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        popMarker();
      }

      // the pixel draws, to the float targets: cells 0-7 to the first pair, 8-15 to the second,
      // 16-23 to the third
      for(int set = NumTargetPairs - 1; set >= 0; set--)
      {
        dev->SetRenderTarget(0, pixelTargets[set * 2]);
        dev->SetRenderTarget(1, pixelTargets[set * 2 + 1]);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
      }
      dev->SetVertexDeclaration(pixelDecl);
      int boundSet = 0;
      for(int d = 0; d < NumPixelDraws; d++)
      {
        const PixelDraw &draw = PixelDraws[d];
        if(!drawn[d])
          continue;
        if(draw.cell / 8 != boundSet)
        {
          boundSet = draw.cell / 8;
          dev->SetRenderTarget(0, pixelTargets[boundSet * 2]);
          dev->SetRenderTarget(1, pixelTargets[boundSet * 2 + 1]);
        }
        pushMarker(draw.marker);
        dev->SetVertexShader(draw.sm3 ? pixelVS30 : pixelVS20);
        dev->SetPixelShader(pixelShaders[d]);
        const float k[8] = {0.5f, -0.75f, 1.25f, 0.375f, 0.25f, 0.5f, -0.125f, 1.5f};
        dev->SetPixelShaderConstantF(0, k, 2);
        // the ps_3_0 draw writes oDepth
        dev->SetRenderState(D3DRS_ZENABLE, d == DrawPS30 ? D3DZB_TRUE : D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
        setupLookups(d);
        PixelVertex quad[6];
        PixelQuad(draw.cell, d != DrawPS30, quad);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(PixelVertex));
        popMarker();
      }
      dev->SetTexture(0, NULL);
      dev->SetTexture(1, NULL);
      dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
      SetFilter(0, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_WRAP);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
      dev->SetRenderTarget(1, NULL);
      dev->SetRenderTarget(0, backBuffer);

      pushMarker("Fixed function");
      dev->SetVertexShader(NULL);
      dev->SetPixelShader(NULL);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
      dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
      dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, ffTri, sizeof(FFVertex));
      popMarker();

      dev->EndScene();

      if(!wroteReference)
      {
        WritePixelReference(pixelTargets, drawn);
        wroteReference = true;
      }

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
