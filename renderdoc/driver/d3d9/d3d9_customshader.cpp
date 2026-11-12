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

#include "driver/shaders/dxbc/dxbc_container.h"
#include "d3d9_device.h"
#include "d3d9_replay.h"
#include "d3d9_resources.h"

// Custom display shaders
// ----------------------
// The texture viewer's custom shaders, as on D3D11: a pixel shader the user writes is drawn over
// the viewed subresource into a texture of our own (m_CustomOutput), which the texture viewer then
// displays, picks and saves in its place.
//
// D3D9 has no texture objects, constant buffers or integer loads, so the shaders are ps_3_0 with
// D3D9's bindings:
// * The texture is in a sampler: the same texture the texture viewer samples (GetDisplaySource),
//   which is the application's texture where it can be sampled directly, a copy of a render target
//   surface, or the subresource decoded on the CPU into an A32B32G32R32F texture of its own. It's
//   bound to every sampler the shader declares with the matching type (sampler2D for 2D data,
//   samplerCUBE for a cube texture, sampler3D for a volume), with point filtering and clamping.
//   The prefix names s0, s1 and s2 for them, like the display shader, but HLSL's Texture2D and
//   SamplerState pairs (which fxc gives a sampler of their own) work too.
// * The values the prefix's RD_ functions return are float4 constants that the compiler places,
//   found by name in the shader's constant table. Every other constant is 0, as D3D11 leaves the
//   $Globals it doesn't recognise.
// * The vertex shader is the texture viewer's, with the UV in TEXCOORD0 and zw 0. SV_Position is
//   VPOS on ps_3_0, which has D3D9's pixel centres at integer coordinates (D3D11's are at .5).
//
// RD_TextureType says which sampler type holds the data, not the resource's type: a cube face or
// volume slice that had to be decoded is 2D. Decoded depth stencils are 2D with depth in red and
// stencil in green, and their type says so.

namespace
{
const char CustomPrefix[] = R"EOPREFIX(
// D3D9 custom display shaders are ps_3_0. The texture is bound to every sampler of the matching
// type the shader declares: sampler2D for RD_TextureType_2D, _Depth and _DepthStencil (depth in
// red, stencil in green), samplerCUBE for RD_TextureType_Cube and sampler3D for RD_TextureType_3D.
// These registers are a suggestion, others work as well. Samplers filter with POINT.
#define RD_FLOAT_2D_BINDING s0
#define RD_FLOAT_CUBE_BINDING s1
#define RD_FLOAT_3D_BINDING s2

// filled by name, wherever the compiler puts them. ps_3_0 only converts floats it knows are
// positive to uint, hence the max() below.
float4 RD_D3D9_TexDim;
// x = mip, y = slice or face, z = sample, w = texture type
float4 RD_D3D9_Selected;
float4 RD_D3D9_Range;
float4 RD_D3D9_YUVDownsampleRate;
float4 RD_D3D9_YUVAChannels;

#define RD_TextureType_2D 2
#define RD_TextureType_3D 3
#define RD_TextureType_Depth 4
#define RD_TextureType_DepthStencil 5
#define RD_TextureType_Cube 8

// for compatibility
#define RD_TextureType_1D 999
#define RD_TextureType_DepthMS 999
#define RD_TextureType_DepthStencilMS 999
#define RD_TextureType_2DMS 999
#define RD_TextureType_Cube_Array 999
#define RD_TextureType_1D_Array 999
#define RD_TextureType_2D_Array 999
#define RD_TextureType_Rect 999
#define RD_TextureType_Buffer 999
#define RD_TextureType_2DMS_Array 999

// possible values on D3D9 (the sampler type that holds the data, not a binding):
// RD_TextureType_2D
// RD_TextureType_3D
// RD_TextureType_Cube
// RD_TextureType_Depth
// RD_TextureType_DepthStencil
uint RD_TextureType()
{
  return (uint)max(RD_D3D9_Selected.w, 0.0f);
}

// the sample the data is from. Multisampled render targets are resolved, which gives
// -numSamples. Multisampled depth stencils have the selected sample.
int RD_SelectedSample()
{
  return (int)RD_D3D9_Selected.z;
}

uint RD_SelectedSliceFace()
{
  return (uint)max(RD_D3D9_Selected.y, 0.0f);
}

uint RD_SelectedMip()
{
  return (uint)max(RD_D3D9_Selected.x, 0.0f);
}

// xyz = width, height, depth (6 for a cube). w = # mips
uint4 RD_TexDim()
{
  return (uint4)max(RD_D3D9_TexDim, 0.0f);
}

// YUY2 and UYVY are decoded to (V, Y, U) at full rate. Zero for other formats.
uint4 RD_YUVDownsampleRate()
{
  return (uint4)max(RD_D3D9_YUVDownsampleRate, 0.0f);
}

uint4 RD_YUVAChannels()
{
  return (uint4)max(RD_D3D9_YUVAChannels, 0.0f);
}

// a pair with minimum and maximum selected range values
float2 RD_SelectedRange()
{
  return RD_D3D9_Range.xy;
}

// errors give the line in the user's file
#line 1
)EOPREFIX";

// D3D9 has 224 ps_3_0 float constants, and 16 integer and boolean ones
const UINT NumFloatConstants = 224;
const UINT NumIntConstants = 16;
const UINT NumBoolConstants = 16;

const Vec4f CustomQuad[4] = {
    Vec4f(0.0f, 0.0f, 0.0f, 1.0f),
    Vec4f(0.0f, -1.0f, 0.0f, 1.0f),
    Vec4f(1.0f, 0.0f, 0.0f, 1.0f),
    Vec4f(1.0f, -1.0f, 0.0f, 1.0f),
};

D3DFORMAT GetResourceFormat(WrappedD3D9Object *obj)
{
  switch(obj->GetResourceType())
  {
    case Resource_Texture:
    {
      D3DSURFACE_DESC desc = {};
      ((WrappedD3DTexture9 *)obj)->GetReal()->GetLevelDesc(0, &desc);
      return desc.Format;
    }
    case Resource_CubeTexture:
    {
      D3DSURFACE_DESC desc = {};
      ((WrappedD3DCubeTexture9 *)obj)->GetReal()->GetLevelDesc(0, &desc);
      return desc.Format;
    }
    case Resource_VolumeTexture:
    {
      D3DVOLUME_DESC desc = {};
      ((WrappedD3DVolumeTexture9 *)obj)->GetReal()->GetLevelDesc(0, &desc);
      return desc.Format;
    }
    case Resource_Surface:
    {
      D3DSURFACE_DESC desc = {};
      ((WrappedD3DSurface9 *)obj)->GetReal()->GetDesc(&desc);
      return desc.Format;
    }
    default: return D3DFMT_UNKNOWN;
  }
}

// 0 for colour formats, 1 for depth only, 2 for depth and stencil
int DepthStencilKind(D3DFORMAT format)
{
  switch((DWORD)format)
  {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D24X8:
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D32_LOCKABLE:
    case MAKEFOURCC('D', 'F', '2', '4'):
    case MAKEFOURCC('D', 'F', '1', '6'): return 1;
    case D3DFMT_D15S1:
    case D3DFMT_D24S8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case MAKEFOURCC('I', 'N', 'T', 'Z'): return 2;
    default: return 0;
  }
}

// The sampler type that holds the display source's data, see the top of the file
D3D9BC::SamplerTextureType SourceSamplerType(uint32_t srcType)
{
  switch(srcType)
  {
    case 1: return D3D9BC::SamplerTextureType::Cube;
    case 2: return D3D9BC::SamplerTextureType::Volume;
    default: return D3D9BC::SamplerTextureType::Tex2D;
  }
}

};    // namespace

// d3d9_rendertexture.cpp
void D3D9SetDebugDrawState(IDirect3DDevice9 *dev);

rdcarray<ShaderSourcePrefix> D3D9Replay::GetCustomShaderSourcePrefixes()
{
  return {
      {ShaderEncoding::HLSL, CustomPrefix},
  };
}

void D3D9Replay::SetCustomShaderIncludes(const rdcarray<rdcstr> &directories)
{
  m_CustomShaderIncludes = directories;
}

void D3D9Replay::BuildCustomShader(ShaderEncoding sourceEncoding, const bytebuf &source,
                                   const rdcstr &entry, const ShaderCompileFlags &compileFlags,
                                   ShaderStage type, ResourceId &id, rdcstr &errors)
{
  id = ResourceId();

  if(type != ShaderStage::Pixel)
  {
    errors = "Custom display shaders are pixel shaders";
    return;
  }

  // D3D9 only links ps_3_0 with the texture viewer's vs_3_0
  rdcstr profile = DXBC::GetProfile(compileFlags);
  if(sourceEncoding == ShaderEncoding::HLSL && !profile.empty() && profile != "ps_3_0")
  {
    errors = StringFormat::Fmt(
        "D3D9 custom display shaders are compiled as ps_3_0, the profile %s can't be used",
        profile.c_str());
    return;
  }

  if(sourceEncoding != ShaderEncoding::HLSL && sourceEncoding != ShaderEncoding::D3D9Bytecode)
  {
    errors =
        StringFormat::Fmt("D3D9 custom display shaders can't be %s", ToStr(sourceEncoding).c_str());
    return;
  }

  rdcarray<DWORD> bytecode;
  if(!BuildShaderBytecode(sourceEncoding, source, entry, compileFlags, "ps_3_0",
                          m_CustomShaderIncludes, ShaderStage::Pixel, bytecode, errors))
    return;

  CustomShader custom;
  custom.info.Init(ResourceId(), (const uint32_t *)bytecode.data(), bytecode.size());

  const D3D9BC::Program &prog = custom.info.GetProgram();
  if(!prog.validVersion || prog.version.type != D3D9BC::ShaderType::Pixel || prog.version.major != 3)
  {
    errors = "D3D9 custom display shaders must be ps_3_0, to link with the texture viewer's vs_3_0";
    return;
  }

  // through the wrapped device, so it has a ResourceId
  HRESULT hr = m_pDevice->CreatePixelShader(bytecode.data(), &custom.shader);
  if(FAILED(hr) || custom.shader == NULL)
  {
    errors = StringFormat::Fmt("Couldn't create the shader: %s", ToStr(hr).c_str());
    return;
  }

  id = GetResID(custom.shader);
  m_CustomShaders[id] = std::move(custom);
}

void D3D9Replay::FreeCustomShader(ResourceId id)
{
  auto it = m_CustomShaders.find(id);
  if(it == m_CustomShaders.end())
    return;

  SAFE_RELEASE(it->second.shader);
  m_CustomShaders.erase(it);
}

void D3D9Replay::ReleaseCustomShaders()
{
  for(auto it = m_CustomShaders.begin(); it != m_CustomShaders.end(); ++it)
    SAFE_RELEASE(it->second.shader);
  m_CustomShaders.clear();

  SAFE_RELEASE(m_CustomOutput.tex);
  m_CustomOutput = CustomOutput();
}

ResourceId D3D9Replay::ApplyCustomShader(TextureDisplay &display)
{
  auto shaderIt = m_CustomShaders.find(display.customShaderId);
  if(shaderIt == m_CustomShaders.end() || !InitDebugRender())
    return ResourceId();

  const CustomShader &custom = shaderIt->second;

  const TextureDescription tex = GetTexture(display.resourceId);
  if(tex.resourceId == ResourceId())
    return ResourceId();

  WrappedD3D9Object *obj =
      GetWrappedObject(m_pDevice->GetResourceManager()->GetResource(tex.resourceId));
  const D3DFORMAT format = obj ? GetResourceFormat(obj) : D3DFMT_UNKNOWN;

  DisplaySource src;
  if(!GetDisplaySource(display.resourceId, display.subresource, true, src))
    return ResourceId();
  ScopedBaseLOD baseLOD(src.tex);

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  // the output has the texture's size and mips, and is drawn at the selected mip
  {
    const uint32_t width = RDCMAX(1U, tex.width);
    const uint32_t height = RDCMAX(1U, tex.height);
    const uint32_t mips = RDCMAX(1U, tex.mips);

    if(m_CustomOutput.tex && (m_CustomOutput.width != width || m_CustomOutput.height != height ||
                              m_CustomOutput.mips != mips))
    {
      SAFE_RELEASE(m_CustomOutput.tex);
      m_CustomOutput = CustomOutput();
    }

    if(m_CustomOutput.tex == NULL)
    {
      HRESULT hr =
          m_pDevice->CreateTexture(width, height, mips, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F,
                                   D3DPOOL_DEFAULT, &m_CustomOutput.tex, NULL);
      if(FAILED(hr) || m_CustomOutput.tex == NULL)
      {
        RDCERR("Couldn't create the %ux%u custom shader texture: %s", width, height,
               ToStr(hr).c_str());
        m_CustomOutput = CustomOutput();
        return ResourceId();
      }

      m_CustomOutput.id = GetResID(m_CustomOutput.tex);
      m_CustomOutput.width = width;
      m_CustomOutput.height = height;
      m_CustomOutput.mips = mips;
    }
  }

  // pick pixel's decoded copies of what was drawn before are out of date now
  for(auto it = m_Decoded.begin(); it != m_Decoded.end();)
  {
    if(it->first.id == m_CustomOutput.id)
    {
      SAFE_RELEASE(it->second.upload);
      it = m_Decoded.erase(it);
    }
    else
    {
      ++it;
    }
  }

  const uint32_t mip = RDCMIN(display.subresource.mip, m_CustomOutput.mips - 1);
  const uint32_t outW = RDCMAX(1U, m_CustomOutput.width >> mip);
  const uint32_t outH = RDCMAX(1U, m_CustomOutput.height >> mip);

  IDirect3DSurface9 *target = NULL;
  Unwrap(m_CustomOutput.tex)->GetSurfaceLevel(mip, &target);
  if(target == NULL)
  {
    RDCERR("Couldn't get the custom shader texture's mip %u", mip);
    return ResourceId();
  }

  // the values the prefix's functions return
  const D3D9BC::SamplerTextureType srcSampler = SourceSamplerType(src.type);

  float textureType = 2.0f;
  if(srcSampler == D3D9BC::SamplerTextureType::Cube)
    textureType = 8.0f;
  else if(srcSampler == D3D9BC::SamplerTextureType::Volume)
    textureType = 3.0f;
  else if(DepthStencilKind(format) == 1)
    textureType = 4.0f;
  else if(DepthStencilKind(format) == 2)
    textureType = 5.0f;

  // render targets are resolved, depth stencils read a sample (the first for ~0U)
  float sample = 0.0f;
  if(tex.msSamp > 1)
  {
    if(DepthStencilKind(format) != 0)
      sample = display.subresource.sample == ~0U
                   ? 0.0f
                   : float(RDCMIN(display.subresource.sample, tex.msSamp - 1));
    else
      sample = -float(tex.msSamp);
  }

  Vec4f yuvRate, yuvChannels;
  if(format == D3DFMT_YUY2 || format == D3DFMT_UYVY)
  {
    // decoded to (V, Y, U) at full rate (d3d9_pixel.cpp)
    yuvRate = Vec4f(1.0f, 1.0f, 1.0f, 8.0f);
    yuvChannels = Vec4f(1.0f, 2.0f, 0.0f, 255.0f);
  }

  struct Named
  {
    const char *name;
    Vec4f value;
  } named[] = {
      {"RD_D3D9_TexDim", Vec4f(float(tex.width), float(tex.height),
                               float(tex.type == TextureType::Texture3D ? tex.depth : tex.arraysize),
                               float(tex.mips))},
      {"RD_D3D9_Selected",
       Vec4f(float(display.subresource.mip), float(display.subresource.slice), sample, textureType)},
      {"RD_D3D9_Range", Vec4f(display.rangeMin, display.rangeMax, 0.0f, 0.0f)},
      {"RD_D3D9_YUVDownsampleRate", yuvRate},
      {"RD_D3D9_YUVAChannels", yuvChannels},
  };

  Vec4f constants[NumFloatConstants] = {};
  for(const D3D9BC::CTABConstant &c : custom.info.GetConstantTable().constants)
  {
    if(c.set != D3D9BC::RegisterSet::Float4 || c.registerCount == 0 ||
       c.registerIndex >= NumFloatConstants)
      continue;

    for(const Named &n : named)
      if(c.name == n.name)
        constants[c.registerIndex] = n.value;
  }

  D3D9DeviceState state;
  if(!m_pDevice->SaveReplayDrawState(state))
  {
    SAFE_RELEASE(target);
    return ResourceId();
  }

  const bool ownScene = SUCCEEDED(dev->BeginScene());

  D3D9SetDebugDrawState(dev);
  dev->SetRenderTarget(0, target);
  dev->SetDepthStencilSurface(NULL);
  const D3DVIEWPORT9 viewport = {0, 0, outW, outH, 0.0f, 1.0f};
  dev->SetViewport(&viewport);

  // whatever the shader doesn't write (with clip or discard) stays 0, as on D3D11
  dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);

  // the texture in every sampler of its type that the shader declares, nothing in the others
  for(DWORD s = 0; s < 16; s++)
    dev->SetTexture(s, NULL);

  for(const D3D9BC::Instruction &inst : custom.info.GetProgram().instructions)
  {
    if(inst.op != D3D9BC::Opcode::Dcl || !inst.hasDst ||
       inst.dst.type != D3D9BC::RegType::Sampler || inst.dst.index >= 16)
      continue;

    const DWORD s = inst.dst.index;
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

    if(inst.textureType == srcSampler)
      dev->SetTexture(s, src.tex);
  }

  const int zeroInts[NumIntConstants * 4] = {};
  const BOOL zeroBools[NumBoolConstants] = {};
  dev->SetPixelShaderConstantF(0, &constants[0].x, NumFloatConstants);
  dev->SetPixelShaderConstantI(0, zeroInts, NumIntConstants);
  dev->SetPixelShaderConstantB(0, zeroBools, NumBoolConstants);

  // the whole target, with D3D9's half pixel offset
  const float vsConsts[8] = {
      0.0f, 0.0f, 2.0f, 2.0f, -1.0f / float(outW), 1.0f / float(outH), 0.0f, 0.0f,
  };
  dev->SetVertexShaderConstantF(0, vsConsts, 2);

  dev->SetVertexShader(m_Debug.texDisplayVS);
  dev->SetPixelShader(Unwrap(custom.shader));
  dev->SetVertexDeclaration(m_Debug.quadDecl);
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, CustomQuad, sizeof(Vec4f));

  if(ownScene)
    dev->EndScene();

  m_pDevice->RestoreReplayDrawState(state);

  SAFE_RELEASE(target);

  return m_CustomOutput.id;
}
