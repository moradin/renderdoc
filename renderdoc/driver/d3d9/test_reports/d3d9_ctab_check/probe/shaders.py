# HLSL used to check the constant table reader against D3DX. Every uniform has an initialiser,
# and every one is read so the compiler allocates all its registers.

FLOATS_VS = r'''
struct S { float a; float3 b; float2x2 c; };
struct Inner { float2 p; int q; };
struct Outer { Inner i[2]; float4 r; };

float f1 = 1.5;
float2 f2 = {2, 3};
float3 f3 = {4, 5, 6};
float4 f4 = {7, 8, 9, 10};
float4x4 m44 = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
row_major float4x4 rm44 = {21,22,23,24, 25,26,27,28, 29,30,31,32, 33,34,35,36};
float3x2 m32 = {41,42, 43,44, 45,46};
row_major float3x2 rm32 = {51,52, 53,54, 55,56};
float2x3 m23 = {61,62,63, 64,65,66};
float arr1[3] = {71, 72, 73};
float3 arr3[2] = {81,82,83, 84,85,86};
float2x2 marr[2] = {91,92,93,94, 95,96,97,98};
S s = {101, 102,103,104, 105,106,107,108};
S sarr[2] = {111, 112,113,114, 115,116,117,118, 121, 122,123,124, 125,126,127,128};
Outer o = {131,132,133, 134,135,136, 137,138,139,140};
int fi = 7;
int3 fi3 = {-1, 2, -3};
bool fb = true;
bool2 fb2 = {false, true};

float noinit;

float4 main(float4 p : POSITION, float4 idx : TEXCOORD0) : POSITION
{
  int k = (int)idx.x;
  float4 r = p * f1 + float4(f2, f3.x, f4.w) + f3.yzxx + f4;
  r += mul(p, m44) + mul(p, rm44);
  r.xy += mul(p.xyz, m32) + mul(p.xyz, rm32);
  r.xyz += mul(p.xy, m23);
  r.x += arr1[k];
  r.xyz += arr3[k];
  r.xy += mul(p.xy, marr[k]);
  r += s.a + float4(s.b, 1) + float4(s.c[0], s.c[1]);
  r += sarr[k].a + float4(sarr[k].b, 1) + float4(sarr[k].c[0], sarr[k].c[1]);
  r += float4(o.i[k].p, o.i[k].q, 0) + o.r;
  r += fi + float4(fi3, 0) + fb + float4(fb2, 0, 0) + noinit;
  return r;
}
'''

# integer and boolean registers: loop counts and branch conditions
INTBOOL_PS = r'''
int loops = 5;
int4 loops4 = {3, 0, 1, 0};
bool useA = true;
bool useB = false;
bool flags[3] = {true, false, true};
float4 colA = {0.25, 0.5, 0.75, 1};
sampler2D tex;
samplerCUBE cube;
sampler3D vol;
sampler2D texarr[2];

float4 main(float4 uv : TEXCOORD0) : COLOR0
{
  float4 r = 0;
  [loop] for(int i = 0; i < loops; i++)
    r += tex2Dlod(tex, float4(uv.xy + i * 0.01, 0, 0));
  [loop] for(int j = 0; j < loops4.x; j++)
    r += 0.1;
  [branch] if(useA)
    r += colA;
  [branch] if(useB)
    r += texCUBE(cube, uv.xyz);
  [branch] if(flags[0]) r += 1;
  [branch] if(flags[1]) r += 2;
  [branch] if(flags[2]) r += tex3D(vol, uv.xyz);
  r += tex2D(texarr[0], uv.xy) + tex2D(texarr[1], uv.zw);
  return r;
}
'''

INTBOOL_VS = r'''
int count = 4;
bool skin = true;
bool2 bb = {true, false};
float4x3 bones[4] = {
  1,0,0, 0,1,0, 0,0,1, 0,0,0,
  2,0,0, 0,2,0, 0,0,2, 0,0,0,
  3,0,0, 0,3,0, 0,0,3, 0,0,0,
  4,0,0, 0,4,0, 0,0,4, 0,0,0 };
row_major float4x3 rbones[2] = {
  1,2,3, 4,5,6, 7,8,9, 10,11,12,
  13,14,15, 16,17,18, 19,20,21, 22,23,24 };
float4 main(float4 p : POSITION) : POSITION
{
  float4 r = p;
  [loop] for(int i = 0; i < count; i++)
    r.xyz += mul(p, bones[i]);
  [branch] if(skin) r.xyz += mul(p, rbones[0]) + mul(p, rbones[1]);
  [branch] if(bb.x) r += 1;
  [branch] if(bb.y) r += 2;
  return r;
}
'''

SM2_PS = r'''
float4 tint = {0.5, 0.25, 1, 1};
float3x3 cm = {1,0,0, 0,1,0, 0,0,1};
float k[2] = {0.125, 0.0625};
sampler2D t0;
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
  float4 c = tex2D(t0, uv);
  return float4(mul(c.rgb, cm), c.a) * tint + k[0] + k[1];
}
'''

LEGACY_PS = r'''
float4 tint = {0.5, 0.25, 1, 1};
float4 add2[2] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
sampler2D t0;
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
  return tex2D(t0, uv) * tint + add2[0] + add2[1];
}
'''

LEGACY_VS = r'''
float4x4 wvp = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
float3 light = {0.5, 0.5, 0.7};
float4 main(float4 p : POSITION, float3 n : NORMAL, out float4 c : COLOR0) : POSITION
{
  c = saturate(dot(n, light));
  return mul(p, wvp);
}
'''

# (name, source, profile, legacy compiler)
SHADERS = [
    ('floats_vs3', FLOATS_VS, 'vs_3_0', False),
    ('floats_vs2', FLOATS_VS, 'vs_2_0', False),
    ('intbool_ps3', INTBOOL_PS, 'ps_3_0', False),
    ('intbool_vs3', INTBOOL_VS, 'vs_3_0', False),
    ('intbool_vs2a', INTBOOL_VS, 'vs_2_a', False),
    ('sm2_ps20', SM2_PS, 'ps_2_0', False),
    ('legacy_ps11', LEGACY_PS, 'ps_1_1', True),
    ('legacy_ps14', LEGACY_PS, 'ps_1_4', True),
    ('legacy_vs11', LEGACY_VS, 'vs_1_1', True),
]

# arrays and structs only partly read, so the compiler allocates fewer registers than the type has
PARTIAL_VS = r'''
float4 part[4] = {1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4};
struct T { float4 a; float4 b; float4 c; };
T tpart = {5,5,5,5, 6,6,6,6, 7,7,7,7};
float4x4 mpart = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
bool bpart[4] = {true, true, false, true};
float4 main(float4 p : POSITION) : POSITION
{
  float4 r = p * part[1] + tpart.a + mpart._11_21_31_41;
  [branch] if(bpart[1]) r += 1;
  return r;
}
'''
SHADERS.append(('partial_vs3', PARTIAL_VS, 'vs_3_0', False))
