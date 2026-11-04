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

#include "driver/dx/official/d3dcompiler.h"
#include "driver/shaders/dxbc/dxbc_compile.h"
#include "maths/formatpacking.h"
#include "d3d9_device.h"
#include "d3d9_replay.h"
#include "d3d9_resources.h"

// The texture viewer
// ------------------
// Everything is drawn on the real device, into the bound output window (an additional swapchain on
// the real device, see d3d9_replay.cpp). The device's state is saved first and set back afterwards,
// with the bindings the wrappers track, since the replay continues from it.
//
// A subresource is displayed in one of three ways (GetDisplaySource):
// * Textures in D3DPOOL_DEFAULT with a format the sampler returns as it's described (most colour
//   formats, see NativeSwizzle) are sampled directly. The sampled value goes through a matrix that
//   moves its channels where the description has them: L8 samples as (L, L, L, 1) but is described
//   as R8, so it's shown as (L, 0, 0, 1).
// * Render target surfaces in those formats (back buffers, standalone render targets) are copied
//   into a texture with StretchRect, resolving multisampled ones, and sampled from there.
// * Everything else (depth stencils, INTZ, formats like P8, A4L4, L6V5U5 or UYVY, ATI1/ATI2,
//   system memory textures) is read with ReadSubresource, decoded on the CPU (d3d9_pixel.cpp) and
//   uploaded to an A32B32G32R32F texture. That's cached per subresource until the next replay.
//
// The display shader is the D3D11 one in SM3 form: range, channel masks, flip, RGBM, YUV
// decoding, NaN/inf/negative and clipping overlays, and the gamma curve. Output windows are
// X8R8G8B8 written with D3DRS_SRGBWRITEENABLE where the driver supports it (D3D11's output windows
// are sRGB too), otherwise the shader encodes sRGB itself.

// the mesh viewer's shaders, in d3d9_rendermesh.cpp. They're compiled with the others.
extern const char *D3D9MeshVS;
extern const char *D3D9MeshPS;

namespace
{
// c0: xy = the quad's top left corner, zw = its size, both in clip space
// c1: xy = half a pixel in clip space, D3D9's pixel centres are at integer coordinates
const char *TexDisplayVS = R"(
float4 posScale : register(c0);
float4 halfPixel : register(c1);

struct v2f
{
  float4 pos : POSITION;
  float2 uv : TEXCOORD0;
};

v2f main(float4 pos : POSITION)
{
  v2f OUT;
  OUT.pos = float4(posScale.xy + pos.xy * posScale.zw - float2(1.0f, -1.0f) + halfPixel.xy, 0, 1);
  OUT.uv = float2(pos.x, -pos.y);
  return OUT;
}
)";

// c0: x = type (0 2D, 1 cube, 2 volume), y = lod, z = cube face or volume w, w = flip Y
// c1: x = range minimum, y = inverse range size, z = RGBM multiplier, w = decode YUV
// c2: channel mask
// c3: x = raw output, y = NaN/inf/negative overlay, z = clipping overlay, w = gamma curve
// c4-c7: rows of the channel matrix, c8: added after it
// c9: x = encode the output as sRGB, when the target can't
const char *TexDisplayPS = R"(
sampler2D tex2d : register(s0);
samplerCUBE texcube : register(s1);
sampler3D tex3d : register(s2);

float4 texInfo : register(c0);
float4 range : register(c1);
float4 channels : register(c2);
float4 flags : register(c3);
float4 swz0 : register(c4);
float4 swz1 : register(c5);
float4 swz2 : register(c6);
float4 swz3 : register(c7);
float4 swzAdd : register(c8);
float4 outParams : register(c9);

float3 CubeDir(float face, float2 uv)
{
  float2 st = uv * 2.0f - 1.0f;
  if(face < 0.5f)
    return float3(1.0f, -st.y, -st.x);
  if(face < 1.5f)
    return float3(-1.0f, -st.y, st.x);
  if(face < 2.5f)
    return float3(st.x, 1.0f, st.y);
  if(face < 3.5f)
    return float3(st.x, -1.0f, -st.y);
  if(face < 4.5f)
    return float3(st.x, -st.y, 1.0f);
  return float3(-st.x, -st.y, -1.0f);
}

bool IsNaN(float f)
{
  return !(f < 0.0f) && !(f > 0.0f) && !(f == 0.0f);
}

bool IsInf(float f)
{
  return abs(f) > 3.402823466e+38f;
}

float SRGBToLinear(float s)
{
  if(s <= 0.04045f)
    return s / 12.92f;
  return pow(abs((s + 0.055f) / 1.055f), 2.4f);
}

float LinearToSRGB(float l)
{
  if(l <= 0.0031308f)
    return l * 12.92f;
  return 1.055f * pow(abs(l), 1.0f / 2.4f) - 0.055f;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
  if(texInfo.w > 0.5f)
    uv.y = 1.0f - uv.y;

  float4 s;
  if(texInfo.x < 0.5f)
    s = tex2Dlod(tex2d, float4(uv, 0, texInfo.y));
  else if(texInfo.x < 1.5f)
    s = texCUBElod(texcube, float4(CubeDir(texInfo.z, uv), texInfo.y));
  else
    s = tex3Dlod(tex3d, float4(uv, texInfo.z, texInfo.y));

  float4 col = float4(dot(swz0, s), dot(swz1, s), dot(swz2, s), dot(swz3, s)) + swzAdd;

  if(flags.x > 0.5f)
    return col;

  if(range.w > 0.5f)
  {
    // YUV is decoded with luma in green, Cb in blue and Cr in red
    const float Kr = 0.2126f;
    const float Kb = 0.0722f;

    float L = col.g;
    float Pb = col.b - 0.5f;
    float Pr = col.r - 0.5f;

    col.b = L + (Pb / 0.5f) * (1 - Kb);
    col.r = L + (Pr / 0.5f) * (1 - Kr);
    col.g = (L - Kr * col.r - Kb * col.b) / (1.0f - Kr - Kb);
  }

  // RGBM encoding
  if(range.z > 0.0f)
    col = float4(col.rgb * col.a * range.z, 1.0f);

  float4 pre = col;

  col = (col - range.x) * range.y;

  if(channels.x < 0.5f)
    col.x = pre.x = 0.0f;
  if(channels.y < 0.5f)
    col.y = pre.y = 0.0f;
  if(channels.z < 0.5f)
    col.z = pre.z = 0.0f;
  if(channels.w < 0.5f)
    col.w = pre.w = 1.0f;

  if(flags.y > 0.5f)
  {
    if(IsNaN(pre.r) || IsNaN(pre.g) || IsNaN(pre.b) || IsNaN(pre.a))
      return float4(1, 0, 0, 1);

    if(IsInf(pre.r) || IsInf(pre.g) || IsInf(pre.b) || IsInf(pre.a))
      return float4(0, 1, 0, 1);

    if(pre.r < 0 || pre.g < 0 || pre.b < 0 || pre.a < 0)
      return float4(0, 0, 1, 1);

    col = float4(dot(col.xyz, float3(0.2126f, 0.7152f, 0.0722f)).xxx, 1);
  }
  else if(flags.z > 0.5f)
  {
    if(col.r < 0 || col.g < 0 || col.b < 0 || col.a < 0)
      return float4(1, 0, 0, 1);

    if(col.r > 1.00001f || col.g > 1.00001f || col.b > 1.00001f || col.a > 1.00001f)
      return float4(0, 1, 0, 1);

    col = float4(dot(col.xyz, float3(0.2126f, 0.7152f, 0.0722f)).xxx, 1);
  }
  else
  {
    // if only one channel is selected, show it in grey (alpha as is, the others summed)
    if(dot(channels, 1.0f) == 1.0f)
    {
      if(channels.a == 1.0f)
        col = float4(col.aaa, 1);
      else
        col = float4(dot(col.rgb, 1.0f).xxx, 1);
    }
  }

  if(flags.w > 0.5f)
    col.rgb = float3(SRGBToLinear(col.r), SRGBToLinear(col.g), SRGBToLinear(col.b));

  if(outParams.x > 0.5f)
    col.rgb = float3(LinearToSRGB(saturate(col.r)), LinearToSRGB(saturate(col.g)),
                     LinearToSRGB(saturate(col.b)));

  return col;
}
)";

// c0: the first colour, c1: the second, c2.x: the size of a square in pixels
const char *CheckerboardPS = R"(
float4 primary : register(c0);
float4 secondary : register(c1);
float4 params : register(c2);

float4 main(float2 vpos : VPOS) : COLOR0
{
  float2 ab = fmod(vpos, (params.x * 2.0f).xx);

  bool first = ((ab.x < params.x && ab.y < params.x) || (ab.x >= params.x && ab.y >= params.x));

  return first ? primary : secondary;
}
)";

const Vec4f TexQuad[4] = {
    Vec4f(0.0f, 0.0f, 0.0f, 1.0f),
    Vec4f(0.0f, -1.0f, 0.0f, 1.0f),
    Vec4f(1.0f, 0.0f, 0.0f, 1.0f),
    Vec4f(1.0f, -1.0f, 0.0f, 1.0f),
};

rdcstr CompileShader(const char *source, const char *profile, DWORD flags, rdcarray<DWORD> &bytecode)
{
  HMODULE d3dcompiler = GetD3DCompiler();
  if(d3dcompiler == NULL)
    return "Couldn't load d3dcompiler_47.dll";

  pD3DCompile compile = (pD3DCompile)GetProcAddress(d3dcompiler, "D3DCompile");
  if(compile == NULL)
    return "Couldn't find D3DCompile";

  ID3DBlob *code = NULL, *errors = NULL;
  HRESULT hr = compile(source, strlen(source), "renderdoc_d3d9_debug", NULL, NULL, "main", profile,
                       flags, 0, &code, &errors);

  rdcstr ret;
  if(FAILED(hr) || code == NULL)
  {
    ret = StringFormat::Fmt("Couldn't compile the %s debug shader: %s", profile,
                            errors ? (const char *)errors->GetBufferPointer() : ToStr(hr).c_str());
  }
  else
  {
    bytecode.resize(code->GetBufferSize() / sizeof(DWORD));
    memcpy(bytecode.data(), code->GetBufferPointer(), bytecode.byteSize());
  }

  SAFE_RELEASE(code);
  SAFE_RELEASE(errors);
  return ret;
}

WrappedD3D9Object *LookupObject(WrappedD3DDevice9 *device, ResourceId id)
{
  D3D9ResourceManager *rm = device->GetResourceManager();
  if(id == ResourceId() || !rm->HasResource(id))
    return NULL;
  return GetWrappedObject(rm->GetResource(id));
}

DWORD FloatBits(float f)
{
  DWORD ret;
  memcpy(&ret, &f, sizeof(ret));
  return ret;
}

void SetIdentitySwizzle(Vec4f swizzle[4], Vec4f &add)
{
  swizzle[0] = Vec4f(1.0f, 0.0f, 0.0f, 0.0f);
  swizzle[1] = Vec4f(0.0f, 1.0f, 0.0f, 0.0f);
  swizzle[2] = Vec4f(0.0f, 0.0f, 1.0f, 0.0f);
  swizzle[3] = Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
  add = Vec4f();
}

// Whether a format can be sampled directly, and how to move the sampled channels to where its
// description (D3D9MakeResourceFormat) has them. D3D9 fills channels a format doesn't have with
// 1, luminance is replicated to RGB, and the X channel of the X formats reads as 1.
bool NativeSwizzle(D3DFORMAT format, Vec4f swizzle[4], Vec4f &add)
{
  SetIdentitySwizzle(swizzle, add);

  const Vec4f zero;

  switch((DWORD)format)
  {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_A32B32G32R32F:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_Q16W16V16U16:
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5: return true;
    // no alpha
    case D3DFMT_X8R8G8B8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_R5G6B5:
      swizzle[3] = zero;
      add = Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
      return true;
    // one channel, described as red
    case D3DFMT_L8:
    case D3DFMT_L16:
    case D3DFMT_R16F:
    case D3DFMT_R32F:
      swizzle[1] = swizzle[2] = swizzle[3] = zero;
      add = Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
      return true;
    // two channels, red and green
    case D3DFMT_G16R16:
    case D3DFMT_G16R16F:
    case D3DFMT_G32R32F:
    case D3DFMT_V8U8:
    case D3DFMT_V16U16:
      swizzle[2] = swizzle[3] = zero;
      add = Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
      return true;
    // luminance as red, alpha as green
    case D3DFMT_A8L8:
      swizzle[1] = Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
      swizzle[2] = swizzle[3] = zero;
      add = Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
      return true;
    case D3DFMT_A8: swizzle[0] = swizzle[1] = swizzle[2] = zero; return true;
    default: return false;
  }
}

bool IsDepthUsage(D3DFORMAT format, DWORD usage)
{
  if(usage & D3DUSAGE_DEPTHSTENCIL)
    return true;

  switch((DWORD)format)
  {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D15S1:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D32_LOCKABLE:
    case MAKEFOURCC('I', 'N', 'T', 'Z'):
    case MAKEFOURCC('D', 'F', '2', '4'):
    case MAKEFOURCC('D', 'F', '1', '6'): return true;
    default: return false;
  }
}

};

// compiles one of the debug shaders, for the overlays (d3d9_overlay.cpp). Returns an error, or an
// empty string.
rdcstr D3D9CompileDebugShader(const char *source, const char *profile, DWORD flags,
                              rdcarray<DWORD> &bytecode)
{
  return CompileShader(source, profile, flags, bytecode);
}

// state that affects our draws, beyond what each draw sets itself. The mesh viewer uses it too.
void D3D9SetDebugDrawState(IDirect3DDevice9 *dev)
{
  for(DWORD i = 1; i < D3D9_NUM_RENDER_TARGETS; i++)
    dev->SetRenderTarget(i, NULL);

  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
  dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
  dev->SetRenderState(D3DRS_CLIPPING, TRUE);
  dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
  dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_DITHERENABLE, FALSE);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
  dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
  dev->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
  dev->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);
  dev->SetRenderState(D3DRS_POINTSIZE, FloatBits(1.0f));
  dev->SetRenderState(D3DRS_POINTSIZE_MIN, FloatBits(1.0f));
  dev->SetRenderState(D3DRS_WRAP0, 0);
  dev->SetRenderState(D3DRS_WRAP1, 0);
  dev->SetRenderState(D3DRS_WRAP2, 0);
  // NVIDIA's alpha to coverage switch
  dev->SetRenderState(D3DRS_ADAPTIVETESS_Y, D3DFMT_UNKNOWN);

  dev->SetStreamSourceFreq(0, 1);

  // on the mixed vertex processing replay device, our vertex buffers (made without
  // D3DUSAGE_SOFTWAREPROCESSING) can only be drawn with hardware vertex processing
  if(dev->GetSoftwareVertexProcessing())
    dev->SetSoftwareVertexProcessing(FALSE);

  for(DWORD s = 0; s < 3; s++)
  {
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
    dev->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, 1);
    dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetSamplerState(s, D3DSAMP_ELEMENTINDEX, 0);
    dev->SetTexture(s, NULL);
  }
}

///////////////////////////////////////////////////////////////////////////////
// saving and restoring the replayed state

bool WrappedD3DDevice9::SaveReplayDrawState(D3D9DeviceState &state)
{
  if(m_Recording)
  {
    // this is called on every repaint, so only say so once
    static bool warned = false;
    if(!warned)
      RDCWARN("Nothing is drawn in the texture and mesh viewers while a state block is recorded");
    warned = true;
    return false;
  }

  D3D9FetchDeviceState(m_device, m_InitParams.BehaviorFlags, m_LightIndices, m_PaletteNumbers, state);
  return true;
}

void WrappedD3DDevice9::RestoreReplayDrawState(const D3D9DeviceState &state)
{
  // the targets first, since setting render target 0 resets the viewport
  for(DWORD i = 0; i < D3D9_NUM_RENDER_TARGETS; i++)
    if(m_RenderTargets[i] || i == 0)
      m_device->SetRenderTarget(i, Unwrap(m_RenderTargets[i]));
  m_device->SetDepthStencilSurface(Unwrap(m_DepthStencil));

  D3D9DeviceState current;
  D3D9FetchDeviceState(m_device, m_InitParams.BehaviorFlags, m_LightIndices, m_PaletteNumbers,
                       current);
  D3D9RestoreDeviceState(m_device, state, current);
  RestoreRealBindings(state.FVF);
}

///////////////////////////////////////////////////////////////////////////////
// setup

void D3D9Replay::DebugRender::Release()
{
  SAFE_RELEASE(texDisplayVS);
  SAFE_RELEASE(texDisplayPS);
  SAFE_RELEASE(checkerPS);
  SAFE_RELEASE(quadDecl);
  SAFE_RELEASE(meshVS);
  SAFE_RELEASE(meshPS);
  SAFE_RELEASE(meshDecl);
  ok = false;
}

bool D3D9Replay::InitDebugRender()
{
  if(m_Debug.tried)
    return m_Debug.ok;

  m_Debug.tried = true;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  D3DCAPS9 caps = {};
  dev->GetDeviceCaps(&caps);
  if(caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0))
  {
    RDCERR("The D3D9 texture and mesh viewers need shader model 3 hardware");
    return false;
  }

  rdcarray<DWORD> texVS, texPS, checkPS, meshVS, meshPS;
  rdcstr err = CompileShader(TexDisplayVS, "vs_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, texVS);
  // strict IEEE, so the NaN checks aren't optimised away
  if(err.empty())
    err = CompileShader(TexDisplayPS, "ps_3_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS, texPS);
  if(err.empty())
    err = CompileShader(CheckerboardPS, "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, checkPS);

  if(!err.empty())
  {
    RDCERR("%s", err.c_str());
    return false;
  }

  HRESULT hr = dev->CreateVertexShader(texVS.data(), &m_Debug.texDisplayVS);
  if(SUCCEEDED(hr))
    hr = dev->CreatePixelShader(texPS.data(), &m_Debug.texDisplayPS);
  if(SUCCEEDED(hr))
    hr = dev->CreatePixelShader(checkPS.data(), &m_Debug.checkerPS);

  // the mesh viewer's shaders are optional, the texture viewer works without them
  rdcstr meshErr = CompileShader(D3D9MeshVS, "vs_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, meshVS);
  if(meshErr.empty())
    meshErr = CompileShader(D3D9MeshPS, "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, meshPS);
  if(meshErr.empty() && SUCCEEDED(dev->CreateVertexShader(meshVS.data(), &m_Debug.meshVS)))
    dev->CreatePixelShader(meshPS.data(), &m_Debug.meshPS);
  if(m_Debug.meshVS == NULL || m_Debug.meshPS == NULL)
  {
    RDCERR("The mesh viewer can't draw: %s",
           meshErr.empty() ? "couldn't create its shaders" : meshErr.c_str());
    SAFE_RELEASE(m_Debug.meshVS);
    SAFE_RELEASE(m_Debug.meshPS);
  }

  const D3DVERTEXELEMENT9 quadElements[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };
  if(SUCCEEDED(hr))
    hr = dev->CreateVertexDeclaration(quadElements, &m_Debug.quadDecl);

  // position, secondary and the vertex index, see MeshVertex in d3d9_rendermesh.cpp
  const D3DVERTEXELEMENT9 meshElements[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      {0, 32, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
      D3DDECL_END(),
  };
  if(SUCCEEDED(hr))
    hr = dev->CreateVertexDeclaration(meshElements, &m_Debug.meshDecl);

  if(FAILED(hr))
  {
    RDCERR("Couldn't create the D3D9 debug shaders: %s", ToStr(hr).c_str());
    m_Debug.Release();
    return false;
  }

  m_Debug.srgbWrite = SUCCEEDED(m_D3D->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                         D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_SRGBWRITE,
                                                         D3DRTYPE_SURFACE, D3DFMT_X8R8G8B8));
  if(!m_Debug.srgbWrite)
    RDCLOG("sRGB writes to X8R8G8B8 aren't supported, the display shader encodes sRGB itself");

  m_Debug.ok = true;
  return true;
}

void D3D9Replay::ClearReplayCaches()
{
  for(auto it = m_Decoded.begin(); it != m_Decoded.end(); ++it)
    SAFE_RELEASE(it->second.upload);
  m_Decoded.clear();

  for(auto it = m_MeshCache.begin(); it != m_MeshCache.end(); ++it)
    SAFE_RELEASE(it->second.vb);
  m_MeshCache.clear();

  // the highlight cache is keyed by event, but a replay could change the same event's data
  m_HighlightCache.cacheKey = 0;
}

void D3D9Replay::ReleaseDebugRender()
{
  ClearReplayCaches();

  for(auto it = m_SurfaceCopies.begin(); it != m_SurfaceCopies.end(); ++it)
    SAFE_RELEASE(it->second);
  m_SurfaceCopies.clear();

  ReleaseOverlay();

  m_Debug.Release();
}

bool D3D9Replay::GetOutputTarget(OutputTarget &target)
{
  auto it = m_OutputWindows.find(m_CurrentOutputWindow);
  if(it == m_OutputWindows.end() || it->second.backBuffer == NULL)
    return false;

  target.rt = it->second.backBuffer;
  target.ds = it->second.depthStencil;
  target.width = it->second.width;
  target.height = it->second.height;
  return target.width > 0 && target.height > 0;
}

///////////////////////////////////////////////////////////////////////////////
// what to sample

bool D3D9Replay::GetDisplaySource(ResourceId id, const Subresource &sub, bool allowDecoded,
                                  DisplaySource &src)
{
  WrappedD3D9Object *obj = LookupObject(m_pDevice, id);
  if(obj == NULL)
    return false;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  src = DisplaySource();
  SetIdentitySwizzle(src.swizzle, src.add);

  // whether textures in a format can be linearly filtered (float formats often can't be)
  auto isFilterable = [this](D3DFORMAT fmt) {
    auto it = m_Filterable.find((DWORD)fmt);
    if(it == m_Filterable.end())
    {
      const bool filterable =
          SUCCEEDED(m_D3D->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                             D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, fmt));
      it = m_Filterable.insert(std::make_pair((DWORD)fmt, filterable)).first;
    }
    return it->second;
  };

  D3DFORMAT format = D3DFMT_UNKNOWN;
  D3DPOOL pool = D3DPOOL_DEFAULT;
  DWORD usage = 0;
  bool native = false;

  switch(obj->GetResourceType())
  {
    case Resource_Texture:
    {
      IDirect3DTexture9 *tex = ((WrappedD3DTexture9 *)obj)->GetReal();
      D3DSURFACE_DESC desc = {};
      tex->GetLevelDesc(0, &desc);
      format = desc.Format;
      pool = desc.Pool;
      usage = desc.Usage;
      src.tex = tex;
      src.type = 0;
      src.lod = float(RDCMIN(sub.mip, (uint32_t)tex->GetLevelCount() - 1));
      native = true;
      break;
    }
    case Resource_CubeTexture:
    {
      IDirect3DCubeTexture9 *tex = ((WrappedD3DCubeTexture9 *)obj)->GetReal();
      D3DSURFACE_DESC desc = {};
      tex->GetLevelDesc(0, &desc);
      format = desc.Format;
      pool = desc.Pool;
      usage = desc.Usage;
      src.tex = tex;
      src.type = 1;
      src.lod = float(RDCMIN(sub.mip, (uint32_t)tex->GetLevelCount() - 1));
      src.slice = float(RDCMIN(sub.slice, 5U));
      native = true;
      break;
    }
    case Resource_VolumeTexture:
    {
      IDirect3DVolumeTexture9 *tex = ((WrappedD3DVolumeTexture9 *)obj)->GetReal();
      const UINT mip = RDCMIN(sub.mip, (uint32_t)tex->GetLevelCount() - 1);
      D3DVOLUME_DESC desc = {};
      tex->GetLevelDesc(mip, &desc);
      format = desc.Format;
      pool = desc.Pool;
      usage = desc.Usage;
      src.tex = tex;
      src.type = 2;
      src.lod = float(mip);
      // the middle of the slice, so linear filtering doesn't blend in the next
      src.slice = (float(RDCMIN(sub.slice, desc.Depth - 1)) + 0.5f) / float(desc.Depth);
      native = true;
      break;
    }
    case Resource_Surface:
    {
      // render targets are copied into a texture of the same format to sample them
      IDirect3DSurface9 *surf = ((WrappedD3DSurface9 *)obj)->GetReal();
      D3DSURFACE_DESC desc = {};
      surf->GetDesc(&desc);

      Vec4f swizzle[4], add;
      if(!(desc.Usage & D3DUSAGE_RENDERTARGET) || IsDepthUsage(desc.Format, desc.Usage) ||
         !NativeSwizzle(desc.Format, swizzle, add))
        break;

      IDirect3DTexture9 *&copy = m_SurfaceCopies[id];
      if(copy)
      {
        D3DSURFACE_DESC copyDesc = {};
        copy->GetLevelDesc(0, &copyDesc);
        if(copyDesc.Width != desc.Width || copyDesc.Height != desc.Height ||
           copyDesc.Format != desc.Format)
          SAFE_RELEASE(copy);
      }

      HRESULT hr = S_OK;
      if(copy == NULL)
        hr = dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                D3DPOOL_DEFAULT, &copy, NULL);

      IDirect3DSurface9 *dst = NULL;
      if(SUCCEEDED(hr))
        hr = copy->GetSurfaceLevel(0, &dst);
      // StretchRect doesn't touch the device's state. It resolves multisampled surfaces.
      if(SUCCEEDED(hr))
        hr = dev->StretchRect(surf, NULL, dst, NULL, D3DTEXF_NONE);
      SAFE_RELEASE(dst);

      if(FAILED(hr))
      {
        RDCWARN("Couldn't copy %s to display it: %s, decoding it instead", ToStr(id).c_str(),
                ToStr(hr).c_str());
        SAFE_RELEASE(copy);
        m_SurfaceCopies.erase(id);
        break;
      }

      src.tex = copy;
      src.type = 0;
      memcpy(src.swizzle, swizzle, sizeof(swizzle));
      src.add = add;
      src.filterable = isFilterable(desc.Format);
      return true;
    }
    default: return false;
  }

  if(native && pool == D3DPOOL_DEFAULT && !IsDepthUsage(format, usage) &&
     NativeSwizzle(format, src.swizzle, src.add))
  {
    src.filterable = isFilterable(format);
    return true;
  }

  if(!allowDecoded)
    return false;

  // decoded on the CPU, then uploaded
  src = DisplaySource();
  SetIdentitySwizzle(src.swizzle, src.add);

  DecodedSubresource &decoded = GetDecoded(id, sub);
  if(decoded.texels.empty())
    return false;

  if(decoded.upload == NULL)
  {
    IDirect3DTexture9 *sysmem = NULL;
    HRESULT hr = dev->CreateTexture(decoded.width, decoded.height, 1, 0, D3DFMT_A32B32G32R32F,
                                    D3DPOOL_SYSTEMMEM, &sysmem, NULL);
    if(SUCCEEDED(hr))
    {
      D3DLOCKED_RECT locked = {};
      hr = sysmem->LockRect(0, &locked, NULL, 0);
      if(SUCCEEDED(hr))
      {
        for(uint32_t y = 0; y < decoded.height; y++)
          memcpy((byte *)locked.pBits + y * locked.Pitch, &decoded.texels[y * decoded.width],
                 decoded.width * sizeof(FloatVector));
        sysmem->UnlockRect(0);
      }
    }
    if(SUCCEEDED(hr))
      hr = dev->CreateTexture(decoded.width, decoded.height, 1, 0, D3DFMT_A32B32G32R32F,
                              D3DPOOL_DEFAULT, &decoded.upload, NULL);
    if(SUCCEEDED(hr))
      hr = dev->UpdateTexture(sysmem, decoded.upload);
    SAFE_RELEASE(sysmem);

    if(FAILED(hr))
    {
      RDCWARN("Couldn't upload the decoded contents of %s: %s", ToStr(id).c_str(), ToStr(hr).c_str());
      SAFE_RELEASE(decoded.upload);
      return false;
    }
  }

  src.tex = decoded.upload;
  src.type = 0;
  src.filterable = false;
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// drawing

void D3D9Replay::DrawTextureQuad(const TextureDisplay &cfg, const DisplaySource &src,
                                 const Vec4f &posScale, int32_t targetWidth, int32_t targetHeight,
                                 bool blend, bool srgbWrite)
{
  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  dev->SetVertexShader(m_Debug.texDisplayVS);
  dev->SetPixelShader(m_Debug.texDisplayPS);
  dev->SetVertexDeclaration(m_Debug.quadDecl);

  const float vsConsts[8] = {
      posScale.x,
      posScale.y,
      posScale.z,
      posScale.w,
      -1.0f / float(targetWidth),
      1.0f / float(targetHeight),
      0.0f,
      0.0f,
  };
  dev->SetVertexShaderConstantF(0, vsConsts, 2);

  // the sampler for the texture's type, see TexDisplayPS
  const DWORD sampler = src.type;
  dev->SetTexture(sampler, src.tex);

  // linear when zoomed out on the top mip, like the other APIs
  const bool linear = src.filterable && cfg.scale < 1.0f && cfg.subresource.mip == 0;
  dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
  dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);

  float rangeMax = cfg.rangeMax;
  if(rangeMax <= cfg.rangeMin)
    rangeMax += 0.00001f;
  float invRange = 1.0f / (rangeMax - cfg.rangeMin);
  if(!RDCISFINITE(invRange))
    invRange = FLT_MAX;

  bool gammaCurve = cfg.linearDisplayAsGamma;
  bool encodeSRGB = false;
  // without sRGB writes, displaying as gamma means writing the value as it is
  if(!srgbWrite && !cfg.rawOutput)
  {
    encodeSRGB = !gammaCurve;
    gammaCurve = false;
  }

  float psConsts[10][4] = {
      {float(src.type), src.lod, src.slice, cfg.flipY ? 1.0f : 0.0f},
      {cfg.rangeMin, invRange, cfg.hdrMultiplier, cfg.decodeYUV ? 1.0f : 0.0f},
      {cfg.red ? 1.0f : 0.0f, cfg.green ? 1.0f : 0.0f, cfg.blue ? 1.0f : 0.0f,
       cfg.alpha ? 1.0f : 0.0f},
      {cfg.rawOutput ? 1.0f : 0.0f, cfg.overlay == DebugOverlay::NaN ? 1.0f : 0.0f,
       cfg.overlay == DebugOverlay::Clipping ? 1.0f : 0.0f, gammaCurve ? 1.0f : 0.0f},
      {},
      {},
      {},
      {},
      {src.add.x, src.add.y, src.add.z, src.add.w},
      {encodeSRGB ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f},
  };
  for(int i = 0; i < 4; i++)
    memcpy(psConsts[4 + i], &src.swizzle[i], sizeof(Vec4f));
  dev->SetPixelShaderConstantF(0, &psConsts[0][0], 10);

  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite ? TRUE : FALSE);

  if(blend && !cfg.rawOutput)
  {
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  }
  else
  {
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  }

  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, TexQuad, sizeof(Vec4f));

  dev->SetTexture(sampler, NULL);
}

bool D3D9Replay::RenderTexture(TextureDisplay cfg)
{
  OutputTarget target;
  if(!GetOutputTarget(target) || !InitDebugRender())
    return false;

  if(cfg.customShaderId != ResourceId())
    RDCWARN("Custom display shaders aren't supported for D3D9 captures yet");

  TextureDescription tex = GetTexture(cfg.resourceId);
  if(tex.resourceId == ResourceId())
    return false;

  // reading back and copying happen before the state is saved, they don't change it (depth
  // stencils are read with their own save and restore)
  DisplaySource src;
  if(!GetDisplaySource(cfg.resourceId, cfg.subresource, true, src))
    return false;

  // thumbnails of A8 textures show their alpha
  if(tex.format.type == ResourceFormatType::A8 && cfg.scale <= 0.0f)
  {
    cfg.red = cfg.green = cfg.blue = false;
    cfg.alpha = true;
  }

  const float outW = float(target.width), outH = float(target.height);
  const float texW = float(tex.width), texH = float(tex.height);

  Vec4f posScale(cfg.xOffset * (2.0f / outW), -cfg.yOffset * (2.0f / outH), 0.0f, 0.0f);

  float scale = cfg.scale;
  if(scale <= 0.0f)
  {
    const float xscale = outW / texW;
    const float yscale = outH / texH;

    scale = RDCMIN(xscale, yscale);

    if(yscale > xscale)
    {
      posScale.x = 0;
      posScale.y = texH * scale / outH - 1.0f;
    }
    else
    {
      posScale.y = 0;
      posScale.x = 1.0f - texW * scale / outW;
    }
  }
  cfg.scale = scale;

  posScale.z = (texW / outW) * scale * 2.0f;
  posScale.w = (texH / outH) * scale * 2.0f;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  D3D9DeviceState state;
  if(!m_pDevice->SaveReplayDrawState(state))
    return false;

  const bool ownScene = SUCCEEDED(dev->BeginScene());

  D3D9SetDebugDrawState(dev);
  dev->SetRenderTarget(0, target.rt);
  dev->SetDepthStencilSurface(NULL);
  const D3DVIEWPORT9 viewport = {0, 0, (DWORD)target.width, (DWORD)target.height, 0.0f, 1.0f};
  dev->SetViewport(&viewport);

  DrawTextureQuad(cfg, src, posScale, target.width, target.height, true, m_Debug.srgbWrite);

  if(ownScene)
    dev->EndScene();

  m_pDevice->RestoreReplayDrawState(state);

  return true;
}

void D3D9Replay::RenderCheckerboard(FloatVector dark, FloatVector light)
{
  OutputTarget target;
  if(!GetOutputTarget(target) || !InitDebugRender())
    return;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  D3D9DeviceState state;
  if(!m_pDevice->SaveReplayDrawState(state))
    return;

  const bool ownScene = SUCCEEDED(dev->BeginScene());

  D3D9SetDebugDrawState(dev);
  dev->SetRenderTarget(0, target.rt);
  dev->SetDepthStencilSurface(NULL);
  const D3DVIEWPORT9 viewport = {0, 0, (DWORD)target.width, (DWORD)target.height, 0.0f, 1.0f};
  dev->SetViewport(&viewport);

  // the colours are in sRGB, converted to linear for the sRGB write to give them back
  if(m_Debug.srgbWrite)
  {
    dark = ConvertSRGBToLinear(dark);
    light = ConvertSRGBToLinear(light);
  }
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, m_Debug.srgbWrite ? TRUE : FALSE);

  const float psConsts[3][4] = {
      {dark.x, dark.y, dark.z, dark.w},
      {light.x, light.y, light.z, light.w},
      {64.0f, 0.0f, 0.0f, 0.0f},
  };
  dev->SetPixelShaderConstantF(0, &psConsts[0][0], 3);

  // the whole target
  const float vsConsts[8] = {
      0.0f, 0.0f, 2.0f, 2.0f, -1.0f / float(target.width), 1.0f / float(target.height), 0.0f, 0.0f,
  };
  dev->SetVertexShaderConstantF(0, vsConsts, 2);

  dev->SetVertexShader(m_Debug.texDisplayVS);
  dev->SetPixelShader(m_Debug.checkerPS);
  dev->SetVertexDeclaration(m_Debug.quadDecl);
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, TexQuad, sizeof(Vec4f));

  if(ownScene)
    dev->EndScene();

  m_pDevice->RestoreReplayDrawState(state);
}

void D3D9Replay::RenderHighlightBox(float w, float h, float scale)
{
  OutputTarget target;
  if(!GetOutputTarget(target))
    return;

  // ColorFill draws the rectangles without touching the device's state
  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  const LONG sz = LONG(scale);

  // top left, x and y
  const LONG tlx = LONG(w / 2.0f + 0.5f);
  const LONG tly = LONG(h / 2.0f + 0.5f);

  RECT rect[4] = {
      {tlx, tly, tlx + 1, tly + sz},
      {tlx + sz, tly, tlx + sz + 1, tly + sz + 1},
      {tlx, tly, tlx + sz, tly + 1},
      {tlx, tly + sz, tlx + sz, tly + sz + 1},
  };

  auto fill = [&](const RECT &r, D3DCOLOR col) {
    RECT clipped = {RDCMAX(r.left, 0L), RDCMAX(r.top, 0L), RDCMIN(r.right, (LONG)target.width),
                    RDCMIN(r.bottom, (LONG)target.height)};
    if(clipped.right > clipped.left && clipped.bottom > clipped.top)
      dev->ColorFill(target.rt, &clipped, col);
  };

  // inner, white
  for(size_t i = 0; i < ARRAY_COUNT(rect); i++)
    fill(rect[i], D3DCOLOR_ARGB(255, 255, 255, 255));

  // then the outer black border, the same rectangles moved out by a pixel and grown to meet
  rect[0].left--;
  rect[0].right--;
  rect[1].left++;
  rect[1].right++;
  rect[2].left--;
  rect[2].right--;
  rect[3].left--;
  rect[3].right--;

  rect[0].top--;
  rect[0].bottom--;
  rect[1].top--;
  rect[1].bottom--;
  rect[2].top--;
  rect[2].bottom--;
  rect[3].top++;
  rect[3].bottom++;

  rect[0].bottom += 2;
  rect[1].bottom += 2;
  rect[2].right += 2;
  rect[3].right += 2;

  for(size_t i = 0; i < ARRAY_COUNT(rect); i++)
    fill(rect[i], D3DCOLOR_ARGB(255, 0, 0, 0));
}

void D3D9Replay::GetOutputWindowData(uint64_t id, bytebuf &retData)
{
  retData.clear();

  auto it = m_OutputWindows.find(id);
  if(it == m_OutputWindows.end() || it->second.backBuffer == NULL)
    return;

  const OutputWindow &outw = it->second;
  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  IDirect3DSurface9 *readback = NULL;
  HRESULT hr = dev->CreateOffscreenPlainSurface(
      (UINT)outw.width, (UINT)outw.height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL);
  if(SUCCEEDED(hr))
    hr = dev->GetRenderTargetData(outw.backBuffer, readback);

  D3DLOCKED_RECT locked = {};
  if(SUCCEEDED(hr))
    hr = readback->LockRect(&locked, NULL, D3DLOCK_READONLY);

  if(FAILED(hr))
  {
    RDCERR("Couldn't read the output window back: %s", ToStr(hr).c_str());
    SAFE_RELEASE(readback);
    return;
  }

  // tightly packed RGB, from BGRX
  retData.resize(size_t(outw.width) * outw.height * 3);
  byte *dst = retData.data();
  for(int32_t y = 0; y < outw.height; y++)
  {
    const byte *src = (const byte *)locked.pBits + y * locked.Pitch;
    for(int32_t x = 0; x < outw.width; x++)
    {
      dst[x * 3 + 0] = src[x * 4 + 2];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 0];
    }
    dst += outw.width * 3;
  }

  readback->UnlockRect();
  SAFE_RELEASE(readback);
}

///////////////////////////////////////////////////////////////////////////////
// reading what can only be sampled

bool D3D9Replay::ReadDecodedFromGPU(ResourceId id, const Subresource &sub, DecodedSubresource &decoded)
{
  if(!InitDebugRender())
    return false;

  DisplaySource src;
  if(!GetDisplaySource(id, sub, false, src))
    return false;

  TextureDescription tex = GetTexture(id);
  const uint32_t width = RDCMAX(1U, tex.width >> sub.mip);
  const uint32_t height = RDCMAX(1U, tex.height >> sub.mip);

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  IDirect3DSurface9 *rt = NULL, *readback = NULL;
  HRESULT hr = dev->CreateRenderTarget(width, height, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0,
                                       FALSE, &rt, NULL);
  if(SUCCEEDED(hr))
    hr = dev->CreateOffscreenPlainSurface(width, height, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM,
                                          &readback, NULL);

  if(FAILED(hr))
  {
    decoded.error =
        StringFormat::Fmt("Couldn't create a float render target to read it: %s", ToStr(hr).c_str());
    SAFE_RELEASE(rt);
    SAFE_RELEASE(readback);
    return false;
  }

  TextureDisplay cfg;
  cfg.resourceId = id;
  cfg.subresource = sub;
  cfg.red = cfg.green = cfg.blue = cfg.alpha = true;
  cfg.rangeMin = 0.0f;
  cfg.rangeMax = 1.0f;
  cfg.scale = 1.0f;
  cfg.hdrMultiplier = -1.0f;
  cfg.flipY = false;
  cfg.decodeYUV = false;
  cfg.linearDisplayAsGamma = false;
  cfg.rawOutput = true;

  D3D9DeviceState state;
  if(!m_pDevice->SaveReplayDrawState(state))
  {
    decoded.error = "Couldn't draw it to read it, a state block was being recorded";
    SAFE_RELEASE(rt);
    SAFE_RELEASE(readback);
    return false;
  }

  const bool ownScene = SUCCEEDED(dev->BeginScene());

  D3D9SetDebugDrawState(dev);
  dev->SetRenderTarget(0, rt);
  dev->SetDepthStencilSurface(NULL);
  const D3DVIEWPORT9 viewport = {0, 0, width, height, 0.0f, 1.0f};
  dev->SetViewport(&viewport);

  DrawTextureQuad(cfg, src, Vec4f(0.0f, 0.0f, 2.0f, 2.0f), (int32_t)width, (int32_t)height, false,
                  false);

  if(ownScene)
    dev->EndScene();

  m_pDevice->RestoreReplayDrawState(state);

  hr = dev->GetRenderTargetData(rt, readback);

  D3DLOCKED_RECT locked = {};
  if(SUCCEEDED(hr))
    hr = readback->LockRect(&locked, NULL, D3DLOCK_READONLY);

  if(SUCCEEDED(hr))
  {
    decoded.width = width;
    decoded.height = height;
    decoded.texels.resize(size_t(width) * height);
    for(uint32_t y = 0; y < height; y++)
      memcpy(&decoded.texels[y * width], (const byte *)locked.pBits + y * locked.Pitch,
             width * sizeof(FloatVector));
    readback->UnlockRect();
  }
  else
  {
    decoded.error = StringFormat::Fmt("Couldn't read the float render target: %s", ToStr(hr).c_str());
  }

  SAFE_RELEASE(rt);
  SAFE_RELEASE(readback);

  return SUCCEEDED(hr);
}
