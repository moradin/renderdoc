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
#include "d3d9_contents.h"
#include "d3d9_device.h"
#include "d3d9_resources.h"

// Reading depth stencils that can't be locked
// -------------------------------------------
// D3D9 can't copy a depth stencil anywhere readable, so the values are found with the depth and
// stencil tests, without changing the depth stencil:
//
// * Depth, a bisection per bit: lo[] holds each pixel's lower bound as an integer, in an R32F
//   texture. For each bit, from the top, a full-screen draw outputs oDepth = (lo + bit) / max with
//   ZFUNC LESSEQUAL and writes 1 where it passes, then a second draw adds the bit to lo there.
// * Stencil, a draw per bit: STENCILFUNC EQUAL with the reference and mask both the bit, writing 1
//   where it passes, then the bit is added the same way.
// * Multisampled depth stencils are read a sample at a time: D3DRS_MULTISAMPLEMASK restricts the
//   test draw to one sample of a multisampled target, which is then resolved. Only that sample can
//   be non-zero, so anything over half of 1/samples means it passed, whatever filter the resolve
//   uses.
//
// The results are stored in the depth stencil's own layout (depth in the high bits, stencil in the
// low ones), with one slice per sample. Everything is drawn on the real device and its state is
// set back afterwards.

namespace
{
struct DepthFormatInfo
{
  uint32_t depthBits = 0;
  uint32_t stencilBits = 0;
  // bytes per pixel in the stored layout
  uint32_t bytes = 0;
};

bool GetDepthFormatInfo(D3DFORMAT format, DepthFormatInfo &info)
{
  switch((DWORD)format)
  {
    case D3DFMT_D16:
    case MAKEFOURCC('D', 'F', '1', '6'): info = {16, 0, 2}; return true;
    case D3DFMT_D15S1: info = {15, 1, 2}; return true;
    case D3DFMT_D24S8:
    case MAKEFOURCC('I', 'N', 'T', 'Z'): info = {24, 8, 4}; return true;
    case D3DFMT_D24X8:
    case MAKEFOURCC('D', 'F', '2', '4'): info = {24, 0, 4}; return true;
    case D3DFMT_D24X4S4: info = {24, 4, 4}; return true;
    // fp32 shaders can only bisect 24 bits exactly, so D32 is read to 24 bits
    case D3DFMT_D32: info = {24, 0, 4}; return true;
    // D24FS8's depth is a 20e4 float, which isn't handled
    default: return false;
  }
}

// the stored value for a pixel
uint32_t PackDepthStencil(D3DFORMAT format, uint32_t depth, uint32_t stencil)
{
  switch((DWORD)format)
  {
    case D3DFMT_D15S1: return (depth << 1) | (stencil & 0x1);
    case D3DFMT_D24S8:
    case MAKEFOURCC('I', 'N', 'T', 'Z'): return (depth << 8) | (stencil & 0xFF);
    case D3DFMT_D24X8:
    case MAKEFOURCC('D', 'F', '2', '4'): return depth << 8;
    case D3DFMT_D24X4S4: return (depth << 8) | (stencil & 0xF);
    // scale the 24-bit value to 32 bits
    case D3DFMT_D32: return (depth << 8) | (depth >> 16);
    default: return depth;
  }
}

const char *DepthVS = R"(
float4 main(float4 pos : POSITION) : POSITION
{
  return pos;
}
)";

// c0: x = the bit being tested, y = 1 / the largest depth value, zw = 1 / size
const char *DepthTestPS = R"(
sampler2D lo : register(s0);
float4 params : register(c0);

float4 main(float2 vpos : VPOS, out float depth : DEPTH) : COLOR0
{
  float l = tex2Dlod(lo, float4((vpos + 0.5f) * params.zw, 0, 0)).r;
  depth = (l + params.x) * params.y;
  return 1.0f;
}
)";

const char *StencilTestPS = R"(
float4 main() : COLOR0
{
  return 1.0f;
}
)";

// c0: x = the bit to add where the test passed, y = the sample count, zw = 1 / size
const char *CombinePS = R"(
sampler2D acc : register(s0);
sampler2D passed : register(s1);
float4 params : register(c0);

float4 main(float2 vpos : VPOS) : COLOR0
{
  float2 uv = (vpos + 0.5f) * params.zw;
  float a = tex2Dlod(acc, float4(uv, 0, 0)).r;
  float p = tex2Dlod(passed, float4(uv, 0, 0)).r * params.y;
  return a + (p > 0.5f ? params.x : 0.0f);
}
)";

rdcstr CompileShader(const char *source, const char *profile, rdcarray<DWORD> &bytecode)
{
  HMODULE d3dcompiler = GetD3DCompiler();
  if(d3dcompiler == NULL)
    return "Couldn't load d3dcompiler_47.dll";

  pD3DCompile compile = (pD3DCompile)GetProcAddress(d3dcompiler, "D3DCompile");
  if(compile == NULL)
    return "Couldn't find D3DCompile";

  ID3DBlob *code = NULL, *errors = NULL;
  HRESULT hr = compile(source, strlen(source), "renderdoc_d3d9_depth", NULL, NULL, "main", profile,
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

  rdcstr ret;
  if(FAILED(hr) || code == NULL)
  {
    ret = StringFormat::Fmt("Couldn't compile the %s depth read back shader: %s", profile,
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

struct FullscreenVertex
{
  float x, y, z, w;
};

const FullscreenVertex FullscreenQuad[4] = {
    {-1.0f, -1.0f, 0.5f, 1.0f},
    {-1.0f, 1.0f, 0.5f, 1.0f},
    {1.0f, -1.0f, 0.5f, 1.0f},
    {1.0f, 1.0f, 0.5f, 1.0f},
};
};

bool WrappedD3DDevice9::InitDepthReadback(rdcstr &err)
{
  if(m_DepthReadback.tried)
  {
    err = m_DepthReadback.error;
    return err.empty();
  }

  m_DepthReadback.tried = true;

  D3DCAPS9 caps = {};
  m_device->GetDeviceCaps(&caps);
  if(caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0))
  {
    m_DepthReadback.error = "Reading depth stencils back needs shader model 3 hardware";
    err = m_DepthReadback.error;
    return false;
  }

  rdcarray<DWORD> vs, depthPS, stencilPS, combinePS;
  err = CompileShader(DepthVS, "vs_3_0", vs);
  if(err.empty())
    err = CompileShader(DepthTestPS, "ps_3_0", depthPS);
  if(err.empty())
    err = CompileShader(StencilTestPS, "ps_3_0", stencilPS);
  if(err.empty())
    err = CompileShader(CombinePS, "ps_3_0", combinePS);

  HRESULT hr = S_OK;
  if(err.empty())
  {
    hr = m_device->CreateVertexShader(vs.data(), &m_DepthReadback.vs);
    if(SUCCEEDED(hr))
      hr = m_device->CreatePixelShader(depthPS.data(), &m_DepthReadback.depthTest);
    if(SUCCEEDED(hr))
      hr = m_device->CreatePixelShader(stencilPS.data(), &m_DepthReadback.stencilTest);
    if(SUCCEEDED(hr))
      hr = m_device->CreatePixelShader(combinePS.data(), &m_DepthReadback.combine);

    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    if(SUCCEEDED(hr))
      hr = m_device->CreateVertexDeclaration(elements, &m_DepthReadback.decl);

    if(FAILED(hr))
      err = StringFormat::Fmt("Couldn't create the depth read back shaders: %s", ToStr(hr).c_str());
  }

  if(!err.empty())
  {
    RDCWARN("%s", err.c_str());
    m_DepthReadback.error = err;
    m_DepthReadback.Release();
    return false;
  }

  return true;
}

void WrappedD3DDevice9::DepthReadback::Release()
{
  SAFE_RELEASE(vs);
  SAFE_RELEASE(depthTest);
  SAFE_RELEASE(stencilTest);
  SAFE_RELEASE(combine);
  SAFE_RELEASE(decl);
}

rdcstr WrappedD3DDevice9::ReadDepthStencil(IDirect3DSurface9 *ds, D3D9SubresourceData &sub,
                                           rdcstr &note)
{
  D3DSURFACE_DESC desc = {};
  ds->GetDesc(&desc);

  DepthFormatInfo info;
  if(!GetDepthFormatInfo(desc.Format, info))
    return StringFormat::Fmt("Depth-stencil contents in %s can't be read back",
                             ToStr(desc.Format).c_str());

  UINT samples = 1;
  if(desc.MultiSampleType == D3DMULTISAMPLE_NONMASKABLE)
    return "Depth-stencil contents with D3DMULTISAMPLE_NONMASKABLE can't be read back: its samples "
           "can't be drawn to one at a time";
  if(desc.MultiSampleType != D3DMULTISAMPLE_NONE)
    samples = (UINT)desc.MultiSampleType;

  // the draws' state calls would go into the state block
  if(m_Recording)
    return "Couldn't read the depth stencil back, a state block was being recorded";

  rdcstr err;
  if(!InitDepthReadback(err))
    return err;

  const UINT width = desc.Width, height = desc.Height;

  // lo[] ping-pongs between two textures, passed holds where the test passed (resolved, if
  // multisampled)
  IDirect3DTexture9 *lo[2] = {}, *passed = NULL;
  IDirect3DSurface9 *loSurf[2] = {}, *passedSurf = NULL, *msTarget = NULL, *sysmem = NULL;

  HRESULT hr = S_OK;
  for(int i = 0; i < 2 && SUCCEEDED(hr); i++)
  {
    hr = m_device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                 D3DPOOL_DEFAULT, &lo[i], NULL);
    if(SUCCEEDED(hr))
      hr = lo[i]->GetSurfaceLevel(0, &loSurf[i]);
  }
  if(SUCCEEDED(hr))
    hr = m_device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
                                 D3DPOOL_DEFAULT, &passed, NULL);
  if(SUCCEEDED(hr))
    hr = passed->GetSurfaceLevel(0, &passedSurf);
  if(SUCCEEDED(hr) && samples > 1)
    hr = m_device->CreateRenderTarget(width, height, D3DFMT_R32F, desc.MultiSampleType,
                                      desc.MultiSampleQuality, FALSE, &msTarget, NULL);
  if(SUCCEEDED(hr))
    hr = m_device->CreateOffscreenPlainSurface(width, height, D3DFMT_R32F, D3DPOOL_SYSTEMMEM,
                                              &sysmem, NULL);

  if(FAILED(hr))
    err = StringFormat::Fmt(samples > 1 ? "Couldn't create multisampled R32F targets to read the "
                                          "depth stencil back: %s"
                                        : "Couldn't create R32F targets to read the depth stencil "
                                          "back: %s",
                            ToStr(hr).c_str());

  // per sample, the depth and stencil values of each pixel
  rdcarray<uint32_t> depthValues, stencilValues;

  if(err.empty())
  {
    depthValues.resize(size_t(width) * height * samples);
    stencilValues.resize(size_t(width) * height * samples);

    D3D9DeviceState before;
    D3D9FetchDeviceState(m_device, m_InitParams.BehaviorFlags, m_LightIndices, m_PaletteNumbers,
                         before);

    // if the application is in a scene, draw in it
    const bool ownScene = SUCCEEDED(m_device->BeginScene());

    for(DWORD i = 1; i < D3D9_NUM_RENDER_TARGETS; i++)
      if(m_RenderTargets[i])
        m_device->SetRenderTarget(i, NULL);

    // the depth stencil may be a texture bound for sampling
    for(uint32_t slot = 0; slot < D3D9_NUM_TEXTURE_SLOTS; slot++)
      if(m_Bindings.textures[slot])
        m_device->SetTexture(D3D9SlotToSampler(slot), NULL);

    m_device->SetVertexShader(m_DepthReadback.vs);
    m_device->SetVertexDeclaration(m_DepthReadback.decl);
    m_device->SetStreamSourceFreq(0, 1);

    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    m_device->SetRenderState(D3DRS_DEPTHBIAS, 0);
    m_device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    m_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    m_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    m_device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    m_device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    m_device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    m_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    m_device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    m_device->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
    m_device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    // NVIDIA's alpha to coverage switch
    m_device->SetRenderState(D3DRS_ADAPTIVETESS_Y, D3DFMT_UNKNOWN);

    for(DWORD s = 0; s < 2; s++)
    {
      m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      m_device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      m_device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
      m_device->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
      m_device->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
    }

    const D3DVIEWPORT9 viewport = {0, 0, width, height, 0.0f, 1.0f};
    IDirect3DSurface9 *testTarget = samples > 1 ? msTarget : passedSurf;

    // Runs one bisection (depth) or set of stencil tests for a sample, leaving the integers in
    // lo[], and returns which one.
    auto run = [&](bool depth, uint32_t bits, UINT sample) -> int {
      int cur = 0;

      m_device->SetRenderTarget(0, loSurf[cur]);
      m_device->SetDepthStencilSurface(NULL);
      m_device->SetViewport(&viewport);
      m_device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);

      const float maxValue = float((1U << bits) - 1);

      for(uint32_t k = 0; k < bits; k++)
      {
        // depth from the top bit down, stencil bits in any order
        const uint32_t bit = depth ? 1U << (bits - 1 - k) : 1U << k;

        // where the test passes
        m_device->SetRenderTarget(0, testTarget);
        m_device->SetDepthStencilSurface(ds);
        m_device->SetViewport(&viewport);
        m_device->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);

        m_device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        m_device->SetRenderState(D3DRS_ZFUNC, depth ? D3DCMP_LESSEQUAL : D3DCMP_ALWAYS);
        m_device->SetRenderState(D3DRS_STENCILENABLE, depth ? FALSE : TRUE);
        m_device->SetRenderState(D3DRS_STENCILREF, bit);
        m_device->SetRenderState(D3DRS_STENCILMASK, bit);
        m_device->SetRenderState(D3DRS_MULTISAMPLEMASK, samples > 1 ? (1U << sample) : 0xFFFFFFFF);

        const float testParams[4] = {float(bit), 1.0f / maxValue, 1.0f / float(width),
                                     1.0f / float(height)};
        m_device->SetPixelShader(depth ? m_DepthReadback.depthTest : m_DepthReadback.stencilTest);
        m_device->SetPixelShaderConstantF(0, testParams, 1);
        m_device->SetTexture(0, depth ? lo[cur] : NULL);
        m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, FullscreenQuad, sizeof(FullscreenVertex));

        if(samples > 1)
          m_device->StretchRect(msTarget, NULL, passedSurf, NULL, D3DTEXF_NONE);

        // add the bit where it passed
        m_device->SetTexture(0, NULL);
        m_device->SetRenderTarget(0, loSurf[1 - cur]);
        m_device->SetDepthStencilSurface(NULL);
        m_device->SetViewport(&viewport);
        m_device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        m_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        m_device->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);

        const float combineParams[4] = {float(bit), float(samples), 1.0f / float(width),
                                        1.0f / float(height)};
        m_device->SetPixelShader(m_DepthReadback.combine);
        m_device->SetPixelShaderConstantF(0, combineParams, 1);
        m_device->SetTexture(0, lo[cur]);
        m_device->SetTexture(1, passed);
        m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, FullscreenQuad, sizeof(FullscreenVertex));
        m_device->SetTexture(0, NULL);
        m_device->SetTexture(1, NULL);

        cur = 1 - cur;
      }

      return cur;
    };

    // copies the integers out of lo[]
    auto readInts = [&](int cur, uint32_t *out) -> HRESULT {
      HRESULT res = m_device->GetRenderTargetData(loSurf[cur], sysmem);
      if(FAILED(res))
        return res;

      D3DLOCKED_RECT locked = {};
      res = sysmem->LockRect(&locked, NULL, D3DLOCK_READONLY);
      if(FAILED(res))
        return res;

      for(UINT y = 0; y < height; y++)
      {
        const float *row = (const float *)((const byte *)locked.pBits + y * locked.Pitch);
        for(UINT x = 0; x < width; x++)
          out[y * width + x] = uint32_t(RDCMAX(row[x], 0.0f) + 0.5f);
      }

      sysmem->UnlockRect();
      return S_OK;
    };

    for(UINT sample = 0; sample < samples && SUCCEEDED(hr); sample++)
    {
      const size_t offs = size_t(width) * height * sample;

      if(info.depthBits > 0)
        hr = readInts(run(true, info.depthBits, sample), depthValues.data() + offs);
      if(info.stencilBits > 0 && SUCCEEDED(hr))
        hr = readInts(run(false, info.stencilBits, sample), stencilValues.data() + offs);
    }

    if(FAILED(hr))
      err = StringFormat::Fmt("Couldn't read the depth stencil back: %s", ToStr(hr).c_str());

    if(ownScene)
      m_device->EndScene();

    // put everything back: the targets first, since setting render target 0 resets the viewport
    // and scissor rect
    for(DWORD i = 0; i < D3D9_NUM_RENDER_TARGETS; i++)
      if(m_RenderTargets[i])
        m_device->SetRenderTarget(i, Unwrap(m_RenderTargets[i]));
    m_device->SetDepthStencilSurface(Unwrap(m_DepthStencil));

    D3D9DeviceState after;
    D3D9FetchDeviceState(m_device, m_InitParams.BehaviorFlags, m_LightIndices, m_PaletteNumbers,
                         after);
    D3D9RestoreDeviceState(m_device, before, after);
    RestoreRealBindings(before.FVF);
  }

  for(int i = 0; i < 2; i++)
  {
    SAFE_RELEASE(loSurf[i]);
    SAFE_RELEASE(lo[i]);
  }
  SAFE_RELEASE(passedSurf);
  SAFE_RELEASE(passed);
  SAFE_RELEASE(msTarget);
  SAFE_RELEASE(sysmem);

  if(!err.empty())
    return err;

  sub.Width = width;
  sub.Height = height;
  sub.Depth = samples;
  sub.RowPitch = width * info.bytes;
  sub.RowCount = height;
  sub.Data.resize(size_t(sub.RowPitch) * height * samples);

  for(size_t i = 0; i < depthValues.size(); i++)
  {
    const uint32_t v = PackDepthStencil(desc.Format, depthValues[i], stencilValues[i]);
    if(info.bytes == 2)
      ((uint16_t *)sub.Data.data())[i] = (uint16_t)v;
    else
      ((uint32_t *)sub.Data.data())[i] = v;
  }

  note = "Read back with depth and stencil tests, see d3d9_depth.cpp";
  if(samples > 1)
    note += StringFormat::Fmt(". Multisampled: each of the %u slices is one sample", samples);

  return rdcstr();
}
