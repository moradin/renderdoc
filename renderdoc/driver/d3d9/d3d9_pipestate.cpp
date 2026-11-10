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

#include <algorithm>
#include "d3d9_device.h"
#include "d3d9_replay.h"
#include "d3d9_resources.h"
#include "d3d9_state.h"
#include "d3d9_vs_emulate.h"

// The D3D9 pipeline state (D3D9Pipe::State) and the descriptors the replay controller and the UI
// see, built from the replay device's current state:
//  - object bindings (textures, streams, index buffer, shaders, declaration, render targets) come
//    from the wrapper's tracked bindings, so they're wrapped objects with their captured IDs.
//  - everything else (render, sampler and texture stage states, transforms, lights, constants...)
//    is read back from the real device with D3D9FetchDeviceState. The replay device is never a
//    pure device, so Get* calls work.
//
// D3D9 binds directly to the device, so like D3D11 there's one virtual descriptor store whose
// descriptors are numbered (one byte each):
//  - 0 to 20: the texture and sampler state of each sampler, numbered as D3D9SamplerToSlot does:
//    pixel samplers 0-15, the displacement map sampler, then vertex samplers 0-3.
//  - 21 to 26: each stage's constant registers, as pseudo buffers: VS float, int, bool, then PS
//    float, int, bool. Each buffer holds the whole register file from register 0, which is what
//    FillCBufferVariables expects for a constant block using that register set. Their contents come
//    from the real device when they're read (GetBufferData).

namespace
{
enum D3D9ConstantSet
{
  D3D9Constants_Float = 0,
  D3D9Constants_Int,
  D3D9Constants_Bool,
  D3D9Constants_Count,
};

enum : uint32_t
{
  D3D9Descriptor_Constants = D3D9_NUM_TEXTURE_SLOTS,
  D3D9Descriptor_Count = D3D9Descriptor_Constants + 2 * D3D9Constants_Count,
};

const uint32_t D3D9_NUM_TEXTURE_STAGES = 8;
const uint32_t D3D9_NUM_CLIP_PLANES = 6;

uint32_t ConstantsIndex(ShaderStage stage, D3D9ConstantSet set)
{
  return (stage == ShaderStage::Pixel ? D3D9Constants_Count : 0) + set;
}

uint32_t ConstantsDescriptor(ShaderStage stage, D3D9ConstantSet set)
{
  return D3D9Descriptor_Constants + ConstantsIndex(stage, set);
}

uint32_t ConstantRegisterSize(D3D9ConstantSet set)
{
  return set == D3D9Constants_Bool ? 4 : 16;
}

// the descriptor of a sampler register as a shader numbers it, or ~0U if there's no such sampler.
// SM3 vertex shaders number the vertex texture samplers 0-3.
uint32_t SamplerRegisterDescriptor(ShaderStage stage, uint32_t reg)
{
  if(stage == ShaderStage::Vertex)
    return reg < D3D9_NUM_VERTEX_SAMPLERS ? D3D9SamplerToSlot(D3DVERTEXTEXTURESAMPLER0 + reg) : ~0U;
  return reg < D3D9_NUM_PIXEL_SAMPLERS ? reg : ~0U;
}

ResourceId WrappedID(IUnknown *obj)
{
  WrappedD3D9Object *wrapped = obj ? GetWrappedObject(obj) : NULL;
  return wrapped ? wrapped->GetResourceID() : ResourceId();
}

float DWORDToFloat(DWORD d)
{
  float f;
  memcpy(&f, &d, sizeof(f));
  return f;
}

FloatVector ColorToVector(DWORD argb)
{
  return FloatVector(float((argb >> 16) & 0xff) / 255.0f, float((argb >> 8) & 0xff) / 255.0f,
                     float(argb & 0xff) / 255.0f, float((argb >> 24) & 0xff) / 255.0f);
}

FloatVector ColorToVector(const D3DCOLORVALUE &c)
{
  return FloatVector(c.r, c.g, c.b, c.a);
}

rdcstr StripPrefix(const rdcstr &str, const char *prefix)
{
  size_t len = strlen(prefix);
  if(str.beginsWith(prefix))
    return str.substr(len);
  return str;
}

CompareFunction MakeCompareFunc(DWORD func)
{
  switch(func)
  {
    case D3DCMP_NEVER: return CompareFunction::Never;
    case D3DCMP_LESS: return CompareFunction::Less;
    case D3DCMP_EQUAL: return CompareFunction::Equal;
    case D3DCMP_LESSEQUAL: return CompareFunction::LessEqual;
    case D3DCMP_GREATER: return CompareFunction::Greater;
    case D3DCMP_NOTEQUAL: return CompareFunction::NotEqual;
    case D3DCMP_GREATEREQUAL: return CompareFunction::GreaterEqual;
    default: break;
  }
  return CompareFunction::AlwaysTrue;
}

StencilOperation MakeStencilOp(DWORD op)
{
  switch(op)
  {
    case D3DSTENCILOP_ZERO: return StencilOperation::Zero;
    case D3DSTENCILOP_REPLACE: return StencilOperation::Replace;
    case D3DSTENCILOP_INCRSAT: return StencilOperation::IncSat;
    case D3DSTENCILOP_DECRSAT: return StencilOperation::DecSat;
    case D3DSTENCILOP_INVERT: return StencilOperation::Invert;
    case D3DSTENCILOP_INCR: return StencilOperation::IncWrap;
    case D3DSTENCILOP_DECR: return StencilOperation::DecWrap;
    default: break;
  }
  return StencilOperation::Keep;
}

BlendMultiplier MakeBlendMultiplier(DWORD blend, bool alpha)
{
  switch(blend)
  {
    case D3DBLEND_ZERO: return BlendMultiplier::Zero;
    case D3DBLEND_ONE: return BlendMultiplier::One;
    case D3DBLEND_SRCCOLOR: return BlendMultiplier::SrcCol;
    case D3DBLEND_INVSRCCOLOR: return BlendMultiplier::InvSrcCol;
    case D3DBLEND_SRCALPHA: return BlendMultiplier::SrcAlpha;
    case D3DBLEND_INVSRCALPHA: return BlendMultiplier::InvSrcAlpha;
    case D3DBLEND_DESTALPHA: return BlendMultiplier::DstAlpha;
    case D3DBLEND_INVDESTALPHA: return BlendMultiplier::InvDstAlpha;
    case D3DBLEND_DESTCOLOR: return BlendMultiplier::DstCol;
    case D3DBLEND_INVDESTCOLOR: return BlendMultiplier::InvDstCol;
    case D3DBLEND_SRCALPHASAT: return BlendMultiplier::SrcAlphaSat;
    case D3DBLEND_BOTHSRCALPHA: return BlendMultiplier::SrcAlpha;
    case D3DBLEND_BOTHINVSRCALPHA: return BlendMultiplier::InvSrcAlpha;
    case D3DBLEND_BLENDFACTOR:
      return alpha ? BlendMultiplier::FactorAlpha : BlendMultiplier::FactorRGB;
    case D3DBLEND_INVBLENDFACTOR:
      return alpha ? BlendMultiplier::InvFactorAlpha : BlendMultiplier::InvFactorRGB;
    case D3DBLEND_SRCCOLOR2: return BlendMultiplier::Src1Col;
    case D3DBLEND_INVSRCCOLOR2: return BlendMultiplier::InvSrc1Col;
    default: break;
  }
  return BlendMultiplier::One;
}

BlendEquation MakeBlendEquation(DWORD src, DWORD dst, DWORD op, bool alpha)
{
  BlendEquation ret;

  // BOTHSRCALPHA and BOTHINVSRCALPHA set the destination too, and override it
  if(src == D3DBLEND_BOTHSRCALPHA)
    dst = D3DBLEND_INVSRCALPHA;
  else if(src == D3DBLEND_BOTHINVSRCALPHA)
    dst = D3DBLEND_SRCALPHA;

  ret.source = MakeBlendMultiplier(src, alpha);
  ret.destination = MakeBlendMultiplier(dst, alpha);

  switch(op)
  {
    case D3DBLENDOP_SUBTRACT: ret.operation = BlendOperation::Subtract; break;
    case D3DBLENDOP_REVSUBTRACT: ret.operation = BlendOperation::ReversedSubtract; break;
    case D3DBLENDOP_MIN: ret.operation = BlendOperation::Minimum; break;
    case D3DBLENDOP_MAX: ret.operation = BlendOperation::Maximum; break;
    default: ret.operation = BlendOperation::Add; break;
  }

  return ret;
}

AddressMode MakeAddressMode(DWORD mode)
{
  switch(mode)
  {
    case D3DTADDRESS_MIRROR: return AddressMode::Mirror;
    case D3DTADDRESS_CLAMP: return AddressMode::ClampEdge;
    case D3DTADDRESS_BORDER: return AddressMode::ClampBorder;
    case D3DTADDRESS_MIRRORONCE: return AddressMode::MirrorOnce;
    default: break;
  }
  return AddressMode::Wrap;
}

FilterMode MakeFilterMode(DWORD filter)
{
  switch(filter)
  {
    case D3DTEXF_NONE: return FilterMode::NoFilter;
    case D3DTEXF_POINT: return FilterMode::Point;
    case D3DTEXF_LINEAR:
    case D3DTEXF_PYRAMIDALQUAD:
    case D3DTEXF_GAUSSIANQUAD:
    case D3DTEXF_CONVOLUTIONMONO: return FilterMode::Linear;
    case D3DTEXF_ANISOTROPIC: return FilterMode::Anisotropic;
    default: break;
  }
  return FilterMode::Point;
}

Topology MakeTopology(DWORD prim)
{
  switch(prim)
  {
    case D3DPT_POINTLIST: return Topology::PointList;
    case D3DPT_LINELIST: return Topology::LineList;
    case D3DPT_LINESTRIP: return Topology::LineStrip;
    case D3DPT_TRIANGLELIST: return Topology::TriangleList;
    case D3DPT_TRIANGLESTRIP: return Topology::TriangleStrip;
    case D3DPT_TRIANGLEFAN: return Topology::TriangleFan;
    default: break;
  }
  return Topology::Unknown;
}

rdcstr TextureOpName(DWORD op)
{
  switch(op)
  {
    case D3DTOP_DISABLE: return "DISABLE";
    case D3DTOP_SELECTARG1: return "SELECTARG1";
    case D3DTOP_SELECTARG2: return "SELECTARG2";
    case D3DTOP_MODULATE: return "MODULATE";
    case D3DTOP_MODULATE2X: return "MODULATE2X";
    case D3DTOP_MODULATE4X: return "MODULATE4X";
    case D3DTOP_ADD: return "ADD";
    case D3DTOP_ADDSIGNED: return "ADDSIGNED";
    case D3DTOP_ADDSIGNED2X: return "ADDSIGNED2X";
    case D3DTOP_SUBTRACT: return "SUBTRACT";
    case D3DTOP_ADDSMOOTH: return "ADDSMOOTH";
    case D3DTOP_BLENDDIFFUSEALPHA: return "BLENDDIFFUSEALPHA";
    case D3DTOP_BLENDTEXTUREALPHA: return "BLENDTEXTUREALPHA";
    case D3DTOP_BLENDFACTORALPHA: return "BLENDFACTORALPHA";
    case D3DTOP_BLENDTEXTUREALPHAPM: return "BLENDTEXTUREALPHAPM";
    case D3DTOP_BLENDCURRENTALPHA: return "BLENDCURRENTALPHA";
    case D3DTOP_PREMODULATE: return "PREMODULATE";
    case D3DTOP_MODULATEALPHA_ADDCOLOR: return "MODULATEALPHA_ADDCOLOR";
    case D3DTOP_MODULATECOLOR_ADDALPHA: return "MODULATECOLOR_ADDALPHA";
    case D3DTOP_MODULATEINVALPHA_ADDCOLOR: return "MODULATEINVALPHA_ADDCOLOR";
    case D3DTOP_MODULATEINVCOLOR_ADDALPHA: return "MODULATEINVCOLOR_ADDALPHA";
    case D3DTOP_BUMPENVMAP: return "BUMPENVMAP";
    case D3DTOP_BUMPENVMAPLUMINANCE: return "BUMPENVMAPLUMINANCE";
    case D3DTOP_DOTPRODUCT3: return "DOTPRODUCT3";
    case D3DTOP_MULTIPLYADD: return "MULTIPLYADD";
    case D3DTOP_LERP: return "LERP";
    default: break;
  }
  return StringFormat::Fmt("0x%x", op);
}

rdcstr TextureArgName(DWORD arg)
{
  rdcstr ret;
  switch(arg & D3DTA_SELECTMASK)
  {
    case D3DTA_DIFFUSE: ret = "DIFFUSE"; break;
    case D3DTA_CURRENT: ret = "CURRENT"; break;
    case D3DTA_TEXTURE: ret = "TEXTURE"; break;
    case D3DTA_TFACTOR: ret = "TFACTOR"; break;
    case D3DTA_SPECULAR: ret = "SPECULAR"; break;
    case D3DTA_TEMP: ret = "TEMP"; break;
    case D3DTA_CONSTANT: ret = "CONSTANT"; break;
    default: ret = StringFormat::Fmt("0x%x", arg & D3DTA_SELECTMASK); break;
  }
  if(arg & D3DTA_COMPLEMENT)
    ret += " | COMPLEMENT";
  if(arg & D3DTA_ALPHAREPLICATE)
    ret += " | ALPHAREPLICATE";
  return ret;
}

rdcstr TexCoordGenerationName(DWORD index)
{
  switch(index & 0xffff0000)
  {
    case D3DTSS_TCI_PASSTHRU: return "PASSTHRU";
    case D3DTSS_TCI_CAMERASPACENORMAL: return "CAMERASPACENORMAL";
    case D3DTSS_TCI_CAMERASPACEPOSITION: return "CAMERASPACEPOSITION";
    case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR: return "CAMERASPACEREFLECTIONVECTOR";
    case D3DTSS_TCI_SPHEREMAP: return "SPHEREMAP";
    default: break;
  }
  return StringFormat::Fmt("0x%x", index & 0xffff0000);
}

rdcstr TextureTransformFlagsName(DWORD flags)
{
  rdcstr ret;
  switch(flags & ~D3DTTFF_PROJECTED)
  {
    case D3DTTFF_DISABLE: ret = "DISABLE"; break;
    case D3DTTFF_COUNT1: ret = "COUNT1"; break;
    case D3DTTFF_COUNT2: ret = "COUNT2"; break;
    case D3DTTFF_COUNT3: ret = "COUNT3"; break;
    case D3DTTFF_COUNT4: ret = "COUNT4"; break;
    default: ret = StringFormat::Fmt("0x%x", flags & ~D3DTTFF_PROJECTED); break;
  }
  if(flags & D3DTTFF_PROJECTED)
    ret += " | PROJECTED";
  return ret;
}

rdcstr FogModeName(DWORD mode)
{
  switch(mode)
  {
    case D3DFOG_NONE: return "NONE";
    case D3DFOG_EXP: return "EXP";
    case D3DFOG_EXP2: return "EXP2";
    case D3DFOG_LINEAR: return "LINEAR";
    default: break;
  }
  return StringFormat::Fmt("0x%x", mode);
}

rdcstr MaterialSourceName(DWORD source)
{
  switch(source)
  {
    case D3DMCS_MATERIAL: return "MATERIAL";
    case D3DMCS_COLOR1: return "COLOR1";
    case D3DMCS_COLOR2: return "COLOR2";
    default: break;
  }
  return StringFormat::Fmt("0x%x", source);
}

rdcstr VertexBlendName(DWORD blend)
{
  switch(blend)
  {
    case D3DVBF_DISABLE: return "DISABLE";
    case D3DVBF_1WEIGHTS: return "1WEIGHTS";
    case D3DVBF_2WEIGHTS: return "2WEIGHTS";
    case D3DVBF_3WEIGHTS: return "3WEIGHTS";
    case D3DVBF_TWEENING: return "TWEENING";
    case D3DVBF_0WEIGHTS: return "0WEIGHTS";
    default: break;
  }
  return StringFormat::Fmt("0x%x", blend);
}

rdcstr ShadeModeName(DWORD mode)
{
  switch(mode)
  {
    case D3DSHADE_FLAT: return "FLAT";
    case D3DSHADE_GOURAUD: return "GOURAUD";
    case D3DSHADE_PHONG: return "PHONG";
    default: break;
  }
  return StringFormat::Fmt("0x%x", mode);
}

rdcstr DeclUsageName(BYTE usage)
{
  switch(usage)
  {
    case D3DDECLUSAGE_POSITION: return "POSITION";
    case D3DDECLUSAGE_BLENDWEIGHT: return "BLENDWEIGHT";
    case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
    case D3DDECLUSAGE_NORMAL: return "NORMAL";
    case D3DDECLUSAGE_PSIZE: return "PSIZE";
    case D3DDECLUSAGE_TEXCOORD: return "TEXCOORD";
    case D3DDECLUSAGE_TANGENT: return "TANGENT";
    case D3DDECLUSAGE_BINORMAL: return "BINORMAL";
    case D3DDECLUSAGE_TESSFACTOR: return "TESSFACTOR";
    case D3DDECLUSAGE_POSITIONT: return "POSITIONT";
    case D3DDECLUSAGE_COLOR: return "COLOR";
    case D3DDECLUSAGE_FOG: return "FOG";
    case D3DDECLUSAGE_DEPTH: return "DEPTH";
    case D3DDECLUSAGE_SAMPLE: return "SAMPLE";
    default: break;
  }
  return StringFormat::Fmt("USAGE%u", usage);
}

rdcstr DeclMethodName(BYTE method)
{
  switch(method)
  {
    case D3DDECLMETHOD_DEFAULT: return "DEFAULT";
    case D3DDECLMETHOD_PARTIALU: return "PARTIALU";
    case D3DDECLMETHOD_PARTIALV: return "PARTIALV";
    case D3DDECLMETHOD_CROSSUV: return "CROSSUV";
    case D3DDECLMETHOD_UV: return "UV";
    case D3DDECLMETHOD_LOOKUP: return "LOOKUP";
    case D3DDECLMETHOD_LOOKUPPRESAMPLED: return "LOOKUPPRESAMPLED";
    default: break;
  }
  return StringFormat::Fmt("0x%x", method);
}

// a vertex declaration type's name and how its data is stored
rdcstr DeclTypeName(BYTE type, ResourceFormat &fmt)
{
  fmt = ResourceFormat();
  fmt.type = ResourceFormatType::Regular;

  auto set = [&fmt](CompType comp, uint8_t count, uint8_t width) {
    fmt.compType = comp;
    fmt.compCount = count;
    fmt.compByteWidth = width;
  };

  switch(type)
  {
    case D3DDECLTYPE_FLOAT1: set(CompType::Float, 1, 4); return "FLOAT1";
    case D3DDECLTYPE_FLOAT2: set(CompType::Float, 2, 4); return "FLOAT2";
    case D3DDECLTYPE_FLOAT3: set(CompType::Float, 3, 4); return "FLOAT3";
    case D3DDECLTYPE_FLOAT4: set(CompType::Float, 4, 4); return "FLOAT4";
    case D3DDECLTYPE_D3DCOLOR:
      set(CompType::UNorm, 4, 1);
      fmt.SetBGRAOrder(true);
      return "D3DCOLOR";
    case D3DDECLTYPE_UBYTE4: set(CompType::UInt, 4, 1); return "UBYTE4";
    case D3DDECLTYPE_SHORT2: set(CompType::SInt, 2, 2); return "SHORT2";
    case D3DDECLTYPE_SHORT4: set(CompType::SInt, 4, 2); return "SHORT4";
    case D3DDECLTYPE_UBYTE4N: set(CompType::UNorm, 4, 1); return "UBYTE4N";
    case D3DDECLTYPE_SHORT2N: set(CompType::SNorm, 2, 2); return "SHORT2N";
    case D3DDECLTYPE_SHORT4N: set(CompType::SNorm, 4, 2); return "SHORT4N";
    case D3DDECLTYPE_USHORT2N: set(CompType::UNorm, 2, 2); return "USHORT2N";
    case D3DDECLTYPE_USHORT4N: set(CompType::UNorm, 4, 2); return "USHORT4N";
    case D3DDECLTYPE_UDEC3:
      fmt.type = ResourceFormatType::R10G10B10A2;
      set(CompType::UInt, 4, 1);
      return "UDEC3";
    case D3DDECLTYPE_DEC3N:
      fmt.type = ResourceFormatType::R10G10B10A2;
      set(CompType::SNorm, 4, 1);
      return "DEC3N";
    case D3DDECLTYPE_FLOAT16_2: set(CompType::Float, 2, 2); return "FLOAT16_2";
    case D3DDECLTYPE_FLOAT16_4: set(CompType::Float, 4, 2); return "FLOAT16_4";
    case D3DDECLTYPE_UNUSED: set(CompType::Float, 0, 4); return "UNUSED";
    default: break;
  }

  fmt.type = ResourceFormatType::Undefined;
  return StringFormat::Fmt("0x%x", type);
}

rdcstr ShaderModelName(IDirect3DVertexShader9 *vs, IDirect3DPixelShader9 *ps)
{
  UINT size = 0;
  if(vs)
    vs->GetFunction(NULL, &size);
  else if(ps)
    ps->GetFunction(NULL, &size);

  if(size < sizeof(DWORD))
    return rdcstr();

  bytebuf data;
  data.resize(size);
  HRESULT hr = vs ? vs->GetFunction(data.data(), &size) : ps->GetFunction(data.data(), &size);
  if(FAILED(hr))
    return rdcstr();

  DWORD version;
  memcpy(&version, data.data(), sizeof(version));

  uint32_t major = (version >> 8) & 0xff;
  uint32_t minor = version & 0xff;

  if(minor == 0xff)
    return StringFormat::Fmt("%s_%u_sw", vs ? "vs" : "ps", major);
  return StringFormat::Fmt("%s_%u_%u", vs ? "vs" : "ps", major, minor);
}

uint32_t ShaderMajorVersion(IDirect3DVertexShader9 *vs)
{
  UINT size = 0;
  if(vs == NULL || FAILED(vs->GetFunction(NULL, &size)) || size < sizeof(DWORD))
    return 0;

  bytebuf data;
  data.resize(size);
  if(FAILED(vs->GetFunction(data.data(), &size)))
    return 0;

  DWORD version;
  memcpy(&version, data.data(), sizeof(version));
  return (version >> 8) & 0xff;
}

// sampler states indexed by D3DSAMPLERSTATETYPE
struct D3D9SamplerStateValues
{
  DWORD values[D3DSAMP_DMAPOFFSET + 1] = {};
};

void FillSampler(D3D9Pipe::Sampler &dst, DWORD sampler, const D3D9SamplerStateValues &src,
                 IDirect3DBaseTexture9 *tex)
{
  const DWORD *v = src.values;

  dst.samplerIndex = sampler;
  dst.resourceId = WrappedID(tex);
  dst.addressU = MakeAddressMode(v[D3DSAMP_ADDRESSU]);
  dst.addressV = MakeAddressMode(v[D3DSAMP_ADDRESSV]);
  dst.addressW = MakeAddressMode(v[D3DSAMP_ADDRESSW]);
  dst.borderColor = ColorToVector(v[D3DSAMP_BORDERCOLOR]);
  dst.magFilter = MakeFilterMode(v[D3DSAMP_MAGFILTER]);
  dst.minFilter = MakeFilterMode(v[D3DSAMP_MINFILTER]);
  dst.mipFilter = MakeFilterMode(v[D3DSAMP_MIPFILTER]);
  dst.mipLODBias = DWORDToFloat(v[D3DSAMP_MIPMAPLODBIAS]);
  dst.maxMipLevel = v[D3DSAMP_MAXMIPLEVEL];
  dst.maxAnisotropy = v[D3DSAMP_MAXANISOTROPY];
  dst.srgbTexture = v[D3DSAMP_SRGBTEXTURE] != 0;
  dst.elementIndex = v[D3DSAMP_ELEMENTINDEX];
  dst.dmapOffset = v[D3DSAMP_DMAPOFFSET];
}

SamplerDescriptor MakeSamplerDescriptor(const D3D9Pipe::Sampler &samp)
{
  SamplerDescriptor ret;
  ret.type = DescriptorType::Sampler;
  ret.addressU = samp.addressU;
  ret.addressV = samp.addressV;
  ret.addressW = samp.addressW;
  ret.borderColorValue.floatValue = {samp.borderColor.x, samp.borderColor.y, samp.borderColor.z,
                                     samp.borderColor.w};
  ret.borderColorType = CompType::Float;
  ret.compareFunction = CompareFunction::AlwaysTrue;
  ret.filter.minify = samp.minFilter;
  ret.filter.magnify = samp.magFilter;
  ret.filter.mip = samp.mipFilter;
  ret.filter.filter = FilterFunction::Normal;
  if(samp.minFilter == FilterMode::Anisotropic || samp.magFilter == FilterMode::Anisotropic)
    ret.maxAnisotropy = (float)samp.maxAnisotropy;
  ret.mipBias = samp.mipLODBias;
  // MAXMIPLEVEL is the most detailed mip that can be used
  ret.minLOD = (float)samp.maxMipLevel;
  ret.maxLOD = FLT_MAX;
  ret.seamlessCubemaps = true;
  return ret;
}

void ReadSamplerStates(IDirect3DDevice9 *dev, DWORD sampler, D3D9SamplerStateValues &dst)
{
  for(DWORD t = D3DSAMP_ADDRESSU; t <= D3DSAMP_DMAPOFFSET; t++)
    dev->GetSamplerState(sampler, (D3DSAMPLERSTATETYPE)t, &dst.values[t]);
}
};    // namespace

void D3D9Replay::CreatePipelineStateResources()
{
  m_DescriptorStore = ResourceIDGen::GetNewUniqueID();

  const char *stageNames[] = {"VS", "PS"};
  const char *setNames[] = {"float constants (c#)", "integer constants (i#)",
                            "boolean constants (b#)"};
  // the same formats the pipeline state viewer opens them with
  const char *setFormats[] = {"float4 c;", "int4 i;", "uint b;"};

  D3D9DeviceState state;
  m_pDevice->FetchDeviceState(state);

  const D3D9ShaderConstants *consts[] = {&state.VertexShaderConstants, &state.PixelShaderConstants};

  for(uint32_t s = 0; s < 2; s++)
  {
    m_ConstantCounts[s * D3D9Constants_Count + D3D9Constants_Float] = consts[s]->Float.count();
    m_ConstantCounts[s * D3D9Constants_Count + D3D9Constants_Int] = consts[s]->Int.count();
    m_ConstantCounts[s * D3D9Constants_Count + D3D9Constants_Bool] = consts[s]->Bool.count();

    for(uint32_t c = 0; c < D3D9Constants_Count; c++)
    {
      ResourceId id = ResourceIDGen::GetNewUniqueID();
      m_ConstantsBuffers[s * D3D9Constants_Count + c] = id;

      ResourceDescription &descr = GetResourceDesc(id);
      descr.name = StringFormat::Fmt("%s %s", stageNames[s], setNames[c]);
      descr.autogeneratedName = false;
      descr.type = ResourceType::Buffer;

      SDObject *annotation = makeSDStruct("annotations"_lit, ""_lit);
      annotation->AddAndOwnChild(makeSDString("__rd_format"_lit, setFormats[c]));
      m_ConstantsAnnotations[s * D3D9Constants_Count + c] = annotation;
      descr.annotations = annotation;
    }
  }

  // DrawPrimitiveUP's data, and post-transform vertices (d3d9_postvs.cpp)
  CreatePostVSResources();
}

int D3D9Replay::ConstantsBufferIndex(ResourceId id) const
{
  if(id == ResourceId())
    return -1;

  for(int i = 0; i < (int)ARRAY_COUNT(m_ConstantsBuffers); i++)
    if(m_ConstantsBuffers[i] == id)
      return i;

  return -1;
}

ResourceId D3D9Replay::GetConstantsBufferID(ShaderStage stage, uint32_t set) const
{
  if(set >= D3D9Constants_Count)
    return ResourceId();

  if(stage == ShaderStage::Vertex)
    return m_ConstantsBuffers[set];
  if(stage == ShaderStage::Pixel)
    return m_ConstantsBuffers[D3D9Constants_Count + set];

  return ResourceId();
}

BufferDescription D3D9Replay::GetConstantsBuffer(ResourceId id)
{
  BufferDescription ret;

  int idx = ConstantsBufferIndex(id);
  if(idx < 0)
    return ret;

  ret.resourceId = id;
  ret.creationFlags = BufferCategory::Constants;
  ret.length = uint64_t(m_ConstantCounts[idx]) *
               ConstantRegisterSize(D3D9ConstantSet(idx % D3D9Constants_Count));
  return ret;
}

void D3D9Replay::GetConstantsData(ResourceId id, uint64_t offset, uint64_t len, bytebuf &retData)
{
  retData.clear();

  int idx = ConstantsBufferIndex(id);
  if(idx < 0)
    return;

  D3D9DeviceState state;
  m_pDevice->FetchDeviceState(state);

  const D3D9ShaderConstants &consts =
      idx < D3D9Constants_Count ? state.VertexShaderConstants : state.PixelShaderConstants;

  bytebuf data;
  switch(D3D9ConstantSet(idx % D3D9Constants_Count))
  {
    case D3D9Constants_Float:
      data.assign((const byte *)consts.Float.data(), consts.Float.byteSize());
      break;
    case D3D9Constants_Int:
      data.assign((const byte *)consts.Int.data(), consts.Int.byteSize());
      break;
    case D3D9Constants_Bool:
    {
      // one uint32 per register, 0 or 1
      data.resize(consts.Bool.size() * sizeof(uint32_t));
      for(size_t i = 0; i < consts.Bool.size(); i++)
      {
        uint32_t b = consts.Bool[i] ? 1 : 0;
        memcpy(data.data() + i * sizeof(uint32_t), &b, sizeof(b));
      }
      break;
    }
    default: break;
  }

  if(offset >= data.size())
    return;

  uint64_t size = data.size() - offset;
  if(len > 0)
    size = RDCMIN(size, len);

  retData.assign(data.data() + offset, (size_t)size);
}

APIProperties D3D9Replay::GetAPIProperties()
{
  APIProperties ret;

  ret.pipelineType = GraphicsAPI::D3D9;
  ret.localRenderer = GraphicsAPI::D3D9;
  ret.vendor = m_DriverInfo.vendor;
  ret.degraded = false;
  ret.shaderDebugging = false;
  ret.pixelHistory = false;

  return ret;
}

rdcarray<EventUsage> D3D9Replay::GetUsage(ResourceId id)
{
  auto it = m_ResourceUses.find(id);
  if(it == m_ResourceUses.end())
    return {};
  return it->second;
}

void D3D9Replay::SetPipelineStates(D3D11Pipe::State *d3d11, D3D12Pipe::State *d3d12,
                                   GLPipe::State *gl, VKPipe::State *vk, D3D9Pipe::State *d3d9)
{
  m_D3D9PipelineState = d3d9;
}

Descriptor D3D9Replay::MakeSurfaceDescriptor(IDirect3DSurface9 *surf)
{
  Descriptor ret;
  ret.type = DescriptorType::ReadWriteImage;

  WrappedD3D9Object *obj = surf ? GetWrappedObject(surf) : NULL;
  if(obj == NULL)
    return ret;

  ret.resource = m_pDevice->SurfaceTextureID(surf);

  WrappedD3D9Object *container = obj->GetContainer();
  if(container && container->GetResourceType() != Resource_SwapChain)
  {
    // a level or face of a texture: the surface is like a view of it
    ret.view = obj->GetResourceID();
    ret.firstMip = (uint8_t)obj->GetChildLevel();
    if(container->GetResourceType() == Resource_CubeTexture)
      ret.firstSlice = (uint16_t)obj->GetChildFace();
  }

  TextureDescription tex = GetTexture(ret.resource);
  ret.format = tex.format;
  ret.textureType = tex.type;
  if(ret.textureType == TextureType::TextureCube)
    ret.textureType = TextureType::Texture2D;

  return ret;
}

Topology D3D9Replay::GetEventTopology(uint32_t eventId)
{
  const ActionDescription *action = m_pDevice->GetAction(eventId);
  if(action == NULL || !(action->flags & ActionFlags::Drawcall) || action->events.empty())
    return Topology::Unknown;

  SDFile *file = m_pDevice->GetStructuredFile();
  uint32_t chunkIdx = action->events.back().chunkIndex;
  if(file == NULL || chunkIdx >= file->chunks.size() || file->chunks[chunkIdx] == NULL)
    return Topology::Unknown;

  const SDObject *prim = file->chunks[chunkIdx]->FindChild("PrimitiveType");
  if(prim == NULL)
    return Topology::Unknown;

  return MakeTopology(prim->AsUInt32());
}

void D3D9Replay::SavePipelineState(uint32_t eventId)
{
  if(m_D3D9PipelineState == NULL)
    return;

  D3D9Pipe::State &ret = *m_D3D9PipelineState;
  ret = D3D9Pipe::State();

  // the DrawPrimitiveUP pseudo buffers read this event's data
  m_PipelineEvent = eventId;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();
  const D3D9ObjectBindings &bind = m_pDevice->GetBindings();

  D3D9DeviceState state;
  m_pDevice->FetchDeviceState(state);

  // render states indexed by D3DRENDERSTATETYPE. D3DRS_BLENDOPALPHA is the highest.
  DWORD rs[D3DRS_BLENDOPALPHA + 1] = {};
  for(const D3D9RenderStateValue &v : state.RenderStates)
  {
    if((uint32_t)v.State < ARRAY_COUNT(rs))
      rs[v.State] = v.Value;

    D3D9Pipe::RenderState r;
    r.name = ToStr(v.State);
    r.value = v.Value;
    ret.renderStates.push_back(r);
  }

  ret.descriptorStore = m_DescriptorStore;
  ret.descriptorCount = D3D9Descriptor_Count;
  ret.descriptorByteSize = 1;

  //////////////////////////////////////////////////////////////////////////
  // input assembly

  {
    D3D9Pipe::InputAssembly &ia = ret.inputAssembly;

    IDirect3DVertexDeclaration9 *decl = bind.decl;
    bool releaseDecl = false;
    if(decl)
    {
      ia.declarationId = WrappedID(decl);
    }
    else if(bind.declFromFVF)
    {
      // the runtime owns declarations made from FVFs, ask it for this one
      ia.fvf = state.FVF;
      if(SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl)
        releaseDecl = true;
      else
        decl = NULL;
    }

    if(decl)
    {
      UINT num = 0;
      decl->GetDeclaration(NULL, &num);
      rdcarray<D3DVERTEXELEMENT9> elems;
      elems.resize(num);
      if(num > 0 && SUCCEEDED(decl->GetDeclaration(elems.data(), &num)))
      {
        for(const D3DVERTEXELEMENT9 &e : elems)
        {
          if(e.Stream == 0xFF)
            break;

          D3D9Pipe::VertexElement el;
          el.stream = e.Stream;
          el.byteOffset = e.Offset;
          el.declType = DeclTypeName(e.Type, el.format);
          el.method = DeclMethodName(e.Method);
          el.semanticName = DeclUsageName(e.Usage);
          el.semanticIndex = e.UsageIndex;
          ia.vertexElements.push_back(el);
        }
      }

      if(releaseDecl)
        decl->Release();
    }

    ia.streams.resize(D3D9_NUM_STREAMS);
    for(uint32_t i = 0; i < D3D9_NUM_STREAMS; i++)
    {
      D3D9Pipe::StreamSource &s = ia.streams[i];
      s.resourceId = WrappedID(bind.streams[i].buffer);
      s.byteOffset = bind.streams[i].offset;
      s.byteStride = bind.streams[i].stride;

      UINT freq = i < state.StreamFrequencies.size() ? state.StreamFrequencies[i] : 1;
      s.indexedData = (freq & D3DSTREAMSOURCE_INDEXEDDATA) != 0;
      s.instanceData = (freq & D3DSTREAMSOURCE_INSTANCEDATA) != 0;
      s.frequency = freq & ~(D3DSTREAMSOURCE_INDEXEDDATA | D3DSTREAMSOURCE_INSTANCEDATA);
    }

    if(bind.indices)
    {
      ia.indexBuffer.resourceId = WrappedID(bind.indices);

      D3DINDEXBUFFER_DESC desc = {};
      if(SUCCEEDED(bind.indices->GetDesc(&desc)))
        ia.indexBuffer.byteStride = desc.Format == D3DFMT_INDEX32 ? 4 : 2;
    }

    // DrawPrimitiveUP and DrawIndexedPrimitiveUP data is only in their chunks, and the runtime
    // unbinds stream 0 and the indices after them. Their copy is presented as pseudo buffers.
    if(const D3D9UserPrimitiveData *up = m_pDevice->GetUserPrimitives(eventId))
    {
      ia.streams[0].resourceId = m_UPVertexBuffer;
      ia.streams[0].byteOffset = 0;
      ia.streams[0].byteStride = up->stride;
      ia.streams[0].indexedData = ia.streams[0].instanceData = false;
      ia.streams[0].frequency = 1;
      if(up->indexed)
      {
        ia.indexBuffer.resourceId = m_UPIndexBuffer;
        ia.indexBuffer.byteStride = up->index32 ? 4 : 2;
      }
    }

    ia.topology = GetEventTopology(eventId);
  }

  //////////////////////////////////////////////////////////////////////////
  // shaders

  {
    D3D9Pipe::Shader *dst[] = {&ret.vertexShader, &ret.pixelShader};
    const ShaderStage stages[] = {ShaderStage::Vertex, ShaderStage::Pixel};

    for(uint32_t s = 0; s < 2; s++)
    {
      D3D9Pipe::Shader &sh = *dst[s];
      sh.stage = stages[s];
      // an edited shader is bound in place of the original. The original's ID is shown, as on
      // D3D11, with what's bound's reflection.
      const ResourceId bound = s == 0 ? WrappedID(bind.vs) : WrappedID(bind.ps);
      sh.resourceId = m_pDevice->GetResourceManager()->GetUnreplacedID(bound);
      sh.shaderModel = s == 0 ? ShaderModelName(bind.vs, NULL) : ShaderModelName(NULL, bind.ps);

      if(bound != ResourceId())
        sh.reflection = GetShader(ResourceId(), bound, ShaderEntryPoint("main", sh.stage));
      // fixed function vertex processing gets a reflection with its outputs, for the mesh viewer
      else if(s == 0)
        sh.reflection = GetFFPReflection(eventId, state);

      uint32_t base = s * D3D9Constants_Count;
      sh.floatConstantsBuffer = m_ConstantsBuffers[base + D3D9Constants_Float];
      sh.floatConstantCount = m_ConstantCounts[base + D3D9Constants_Float];
      sh.intConstantsBuffer = m_ConstantsBuffers[base + D3D9Constants_Int];
      sh.intConstantCount = m_ConstantCounts[base + D3D9Constants_Int];
      sh.boolConstantsBuffer = m_ConstantsBuffers[base + D3D9Constants_Bool];
      sh.boolConstantCount = m_ConstantCounts[base + D3D9Constants_Bool];
    }
  }

  //////////////////////////////////////////////////////////////////////////
  // samplers

  {
    D3D9SamplerStateValues samplerStates[D3D9_NUM_TEXTURE_SLOTS];
    for(const D3D9SamplerStates &ss : state.SamplerStates)
    {
      uint32_t slot = D3D9SamplerToSlot(ss.Sampler);
      if(slot >= D3D9_NUM_TEXTURE_SLOTS)
        continue;
      for(const D3D9SamplerStateValue &v : ss.States)
        if((uint32_t)v.State < ARRAY_COUNT(samplerStates[slot].values))
          samplerStates[slot].values[v.State] = v.Value;
    }

    ret.pixelSamplers.resize(D3D9_NUM_PIXEL_SAMPLERS);
    for(uint32_t i = 0; i < D3D9_NUM_PIXEL_SAMPLERS; i++)
      FillSampler(ret.pixelSamplers[i], i, samplerStates[i], bind.textures[i]);

    ret.vertexSamplers.resize(D3D9_NUM_VERTEX_SAMPLERS);
    for(uint32_t i = 0; i < D3D9_NUM_VERTEX_SAMPLERS; i++)
    {
      DWORD sampler = D3DVERTEXTEXTURESAMPLER0 + i;
      uint32_t slot = D3D9SamplerToSlot(sampler);
      FillSampler(ret.vertexSamplers[i], sampler, samplerStates[slot], bind.textures[slot]);
    }

    uint32_t dmapSlot = D3D9SamplerToSlot(D3DDMAPSAMPLER);
    FillSampler(ret.displacementMapSampler, D3DDMAPSAMPLER, samplerStates[dmapSlot],
                bind.textures[dmapSlot]);
  }

  //////////////////////////////////////////////////////////////////////////
  // fixed function

  {
    D3D9Pipe::FixedFunction &ff = ret.fixedFunction;

    DWORD tss[D3D9_NUM_TEXTURE_STAGES][D3DTSS_CONSTANT + 1] = {};
    for(const D3D9TextureStageStates &ts : state.TextureStageStates)
    {
      if(ts.Stage >= D3D9_NUM_TEXTURE_STAGES)
        continue;
      for(const D3D9TextureStageStateValue &v : ts.States)
        if((uint32_t)v.State <= D3DTSS_CONSTANT)
          tss[ts.Stage][v.State] = v.Value;
    }

    ff.textureStages.resize(D3D9_NUM_TEXTURE_STAGES);
    for(uint32_t i = 0; i < D3D9_NUM_TEXTURE_STAGES; i++)
    {
      D3D9Pipe::TextureStage &dst = ff.textureStages[i];
      const DWORD *v = tss[i];

      dst.stage = i;
      dst.colorOp = TextureOpName(v[D3DTSS_COLOROP]);
      dst.colorArg0 = TextureArgName(v[D3DTSS_COLORARG0]);
      dst.colorArg1 = TextureArgName(v[D3DTSS_COLORARG1]);
      dst.colorArg2 = TextureArgName(v[D3DTSS_COLORARG2]);
      dst.alphaOp = TextureOpName(v[D3DTSS_ALPHAOP]);
      dst.alphaArg0 = TextureArgName(v[D3DTSS_ALPHAARG0]);
      dst.alphaArg1 = TextureArgName(v[D3DTSS_ALPHAARG1]);
      dst.alphaArg2 = TextureArgName(v[D3DTSS_ALPHAARG2]);
      dst.resultArg = TextureArgName(v[D3DTSS_RESULTARG]);
      dst.texCoordIndex = v[D3DTSS_TEXCOORDINDEX] & 0xffff;
      dst.texCoordGeneration = TexCoordGenerationName(v[D3DTSS_TEXCOORDINDEX]);
      dst.textureTransformFlags = TextureTransformFlagsName(v[D3DTSS_TEXTURETRANSFORMFLAGS]);
      dst.bumpEnvMatrix = {
          DWORDToFloat(v[D3DTSS_BUMPENVMAT00]),
          DWORDToFloat(v[D3DTSS_BUMPENVMAT01]),
          DWORDToFloat(v[D3DTSS_BUMPENVMAT10]),
          DWORDToFloat(v[D3DTSS_BUMPENVMAT11]),
      };
      dst.bumpEnvLScale = DWORDToFloat(v[D3DTSS_BUMPENVLSCALE]);
      dst.bumpEnvLOffset = DWORDToFloat(v[D3DTSS_BUMPENVLOFFSET]);
      dst.constant = ColorToVector(v[D3DTSS_CONSTANT]);
    }

    for(const D3D9TransformValue &t : state.Transforms)
    {
      D3D9Pipe::Transform tr;
      tr.name = StripPrefix(ToStr(t.State), "D3DTS_");
      for(int r = 0; r < 4; r++)
        for(int c = 0; c < 4; c++)
          tr.matrix[r * 4 + c] = t.Matrix.m[r][c];
      ff.transforms.push_back(tr);
    }

    for(const D3D9LightState &l : state.Lights)
    {
      D3D9Pipe::Light light;
      light.index = l.Index;
      light.enabled = l.Enabled != FALSE;
      light.type = StripPrefix(ToStr(l.Light.Type), "D3DLIGHT_");
      light.diffuse = ColorToVector(l.Light.Diffuse);
      light.specular = ColorToVector(l.Light.Specular);
      light.ambient = ColorToVector(l.Light.Ambient);
      light.position = FloatVector(l.Light.Position.x, l.Light.Position.y, l.Light.Position.z, 1.0f);
      light.direction =
          FloatVector(l.Light.Direction.x, l.Light.Direction.y, l.Light.Direction.z, 0.0f);
      light.range = l.Light.Range;
      light.falloff = l.Light.Falloff;
      light.attenuation0 = l.Light.Attenuation0;
      light.attenuation1 = l.Light.Attenuation1;
      light.attenuation2 = l.Light.Attenuation2;
      light.theta = l.Light.Theta;
      light.phi = l.Light.Phi;
      ff.lights.push_back(light);
    }

    ff.material.diffuse = ColorToVector(state.Material.Diffuse);
    ff.material.ambient = ColorToVector(state.Material.Ambient);
    ff.material.specular = ColorToVector(state.Material.Specular);
    ff.material.emissive = ColorToVector(state.Material.Emissive);
    ff.material.power = state.Material.Power;

    ff.lighting = rs[D3DRS_LIGHTING] != 0;
    ff.ambient = ColorToVector(rs[D3DRS_AMBIENT]);
    ff.specularEnable = rs[D3DRS_SPECULARENABLE] != 0;
    ff.normalizeNormals = rs[D3DRS_NORMALIZENORMALS] != 0;
    ff.localViewer = rs[D3DRS_LOCALVIEWER] != 0;
    ff.colorVertex = rs[D3DRS_COLORVERTEX] != 0;
    ff.diffuseMaterialSource = MaterialSourceName(rs[D3DRS_DIFFUSEMATERIALSOURCE]);
    ff.ambientMaterialSource = MaterialSourceName(rs[D3DRS_AMBIENTMATERIALSOURCE]);
    ff.specularMaterialSource = MaterialSourceName(rs[D3DRS_SPECULARMATERIALSOURCE]);
    ff.emissiveMaterialSource = MaterialSourceName(rs[D3DRS_EMISSIVEMATERIALSOURCE]);
    ff.vertexBlend = VertexBlendName(rs[D3DRS_VERTEXBLEND]);
    ff.indexedVertexBlend = rs[D3DRS_INDEXEDVERTEXBLENDENABLE] != 0;
    ff.shadeMode = ShadeModeName(rs[D3DRS_SHADEMODE]);
    ff.textureFactor = ColorToVector(rs[D3DRS_TEXTUREFACTOR]);

    ff.fog.enabled = rs[D3DRS_FOGENABLE] != 0;
    ff.fog.color = ColorToVector(rs[D3DRS_FOGCOLOR]);
    ff.fog.tableMode = FogModeName(rs[D3DRS_FOGTABLEMODE]);
    ff.fog.vertexMode = FogModeName(rs[D3DRS_FOGVERTEXMODE]);
    ff.fog.start = DWORDToFloat(rs[D3DRS_FOGSTART]);
    ff.fog.end = DWORDToFloat(rs[D3DRS_FOGEND]);
    ff.fog.density = DWORDToFloat(rs[D3DRS_FOGDENSITY]);
    ff.fog.rangeFog = rs[D3DRS_RANGEFOGENABLE] != 0;

    ff.clipPlanes.resize(D3D9_NUM_CLIP_PLANES);
    for(uint32_t i = 0; i < D3D9_NUM_CLIP_PLANES; i++)
    {
      ff.clipPlanes[i].enabled = (rs[D3DRS_CLIPPLANEENABLE] & (1U << i)) != 0;
      if(i < state.ClipPlanes.size())
      {
        const D3D9Vec4f &p = state.ClipPlanes[i];
        ff.clipPlanes[i].plane = FloatVector(p.x, p.y, p.z, p.w);
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////
  // rasterizer

  {
    D3D9Pipe::Rasterizer &rast = ret.rasterizer;

    switch(rs[D3DRS_FILLMODE])
    {
      case D3DFILL_POINT: rast.fillMode = FillMode::Point; break;
      case D3DFILL_WIREFRAME: rast.fillMode = FillMode::Wireframe; break;
      default: rast.fillMode = FillMode::Solid; break;
    }

    // front faces are clockwise
    switch(rs[D3DRS_CULLMODE])
    {
      case D3DCULL_NONE: rast.cullMode = CullMode::NoCull; break;
      case D3DCULL_CW: rast.cullMode = CullMode::Front; break;
      default: rast.cullMode = CullMode::Back; break;
    }

    rast.viewport.enabled = true;
    rast.viewport.x = (float)state.Viewport.X;
    rast.viewport.y = (float)state.Viewport.Y;
    rast.viewport.width = (float)state.Viewport.Width;
    rast.viewport.height = (float)state.Viewport.Height;
    rast.viewport.minDepth = state.Viewport.MinZ;
    rast.viewport.maxDepth = state.Viewport.MaxZ;

    rast.scissor.enabled = rs[D3DRS_SCISSORTESTENABLE] != 0;
    rast.scissor.x = state.ScissorRect.left;
    rast.scissor.y = state.ScissorRect.top;
    rast.scissor.width = state.ScissorRect.right - state.ScissorRect.left;
    rast.scissor.height = state.ScissorRect.bottom - state.ScissorRect.top;

    rast.depthBias = DWORDToFloat(rs[D3DRS_DEPTHBIAS]);
    rast.slopeScaledDepthBias = DWORDToFloat(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
    rast.multisampleEnable = rs[D3DRS_MULTISAMPLEANTIALIAS] != 0;
    rast.multisampleMask = rs[D3DRS_MULTISAMPLEMASK];
    rast.antialiasedLines = rs[D3DRS_ANTIALIASEDLINEENABLE] != 0;
    rast.clipping = rs[D3DRS_CLIPPING] != 0;
  }

  //////////////////////////////////////////////////////////////////////////
  // output merger

  {
    D3D9Pipe::OutputMerger &om = ret.outputMerger;

    D3D9Pipe::DepthStencilState &ds = om.depthStencilState;
    ds.depthEnable = rs[D3DRS_ZENABLE] != D3DZB_FALSE;
    ds.wBuffer = rs[D3DRS_ZENABLE] == D3DZB_USEW;
    ds.depthWrites = rs[D3DRS_ZWRITEENABLE] != 0;
    ds.depthFunction = MakeCompareFunc(rs[D3DRS_ZFUNC]);
    ds.stencilEnable = rs[D3DRS_STENCILENABLE] != 0;
    ds.twoSidedStencil = rs[D3DRS_TWOSIDEDSTENCILMODE] != 0;

    ds.frontFace.failOperation = MakeStencilOp(rs[D3DRS_STENCILFAIL]);
    ds.frontFace.depthFailOperation = MakeStencilOp(rs[D3DRS_STENCILZFAIL]);
    ds.frontFace.passOperation = MakeStencilOp(rs[D3DRS_STENCILPASS]);
    ds.frontFace.function = MakeCompareFunc(rs[D3DRS_STENCILFUNC]);
    ds.frontFace.reference = rs[D3DRS_STENCILREF];
    ds.frontFace.compareMask = rs[D3DRS_STENCILMASK];
    ds.frontFace.writeMask = rs[D3DRS_STENCILWRITEMASK];

    ds.backFace.failOperation = MakeStencilOp(rs[D3DRS_CCW_STENCILFAIL]);
    ds.backFace.depthFailOperation = MakeStencilOp(rs[D3DRS_CCW_STENCILZFAIL]);
    ds.backFace.passOperation = MakeStencilOp(rs[D3DRS_CCW_STENCILPASS]);
    ds.backFace.function = MakeCompareFunc(rs[D3DRS_CCW_STENCILFUNC]);
    ds.backFace.reference = rs[D3DRS_STENCILREF];
    ds.backFace.compareMask = rs[D3DRS_STENCILMASK];
    ds.backFace.writeMask = rs[D3DRS_STENCILWRITEMASK];

    om.alphaTest.enabled = rs[D3DRS_ALPHATESTENABLE] != 0;
    om.alphaTest.function = MakeCompareFunc(rs[D3DRS_ALPHAFUNC]);
    om.alphaTest.reference = rs[D3DRS_ALPHAREF] & 0xff;

    D3D9Pipe::BlendState &blend = om.blendState;
    blend.separateAlphaBlend = rs[D3DRS_SEPARATEALPHABLENDENABLE] != 0;

    FloatVector factor = ColorToVector(rs[D3DRS_BLENDFACTOR]);
    blend.blendFactor = {factor.x, factor.y, factor.z, factor.w};

    ColorBlend cb;
    cb.enabled = rs[D3DRS_ALPHABLENDENABLE] != 0;
    cb.logicOperationEnabled = false;
    cb.colorBlend =
        MakeBlendEquation(rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_BLENDOP], false);
    if(blend.separateAlphaBlend)
      cb.alphaBlend = MakeBlendEquation(rs[D3DRS_SRCBLENDALPHA], rs[D3DRS_DESTBLENDALPHA],
                                        rs[D3DRS_BLENDOPALPHA], true);
    else
      cb.alphaBlend =
          MakeBlendEquation(rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_BLENDOP], true);

    const D3DRENDERSTATETYPE writeMasks[D3D9_NUM_RENDER_TARGETS] = {
        D3DRS_COLORWRITEENABLE,
        D3DRS_COLORWRITEENABLE1,
        D3DRS_COLORWRITEENABLE2,
        D3DRS_COLORWRITEENABLE3,
    };

    blend.blends.resize(D3D9_NUM_RENDER_TARGETS);
    for(uint32_t i = 0; i < D3D9_NUM_RENDER_TARGETS; i++)
    {
      blend.blends[i] = cb;
      // D3DCOLORWRITEENABLE_RED etc. are the same bits as RenderDoc's write mask
      blend.blends[i].writeMask = (byte)(rs[writeMasks[i]] & 0xf);
    }

    om.srgbWrite = rs[D3DRS_SRGBWRITEENABLE] != 0;
    om.dither = rs[D3DRS_DITHERENABLE] != 0;

    om.renderTargets.resize(D3D9_NUM_RENDER_TARGETS);
    for(uint32_t i = 0; i < D3D9_NUM_RENDER_TARGETS; i++)
      om.renderTargets[i] = MakeSurfaceDescriptor(m_pDevice->GetBoundRenderTarget(i));

    om.depthTarget = MakeSurfaceDescriptor(m_pDevice->GetBoundDepthStencil());
  }
}

rdcarray<Descriptor> D3D9Replay::GetDescriptors(ResourceId descriptorStore,
                                                const rdcarray<DescriptorRange> &ranges)
{
  rdcarray<Descriptor> ret;

  if(descriptorStore != m_DescriptorStore)
  {
    RDCERR("Descriptors query for invalid descriptor store on fixed bindings API (D3D9)");
    return ret;
  }

  const D3D9ObjectBindings &bind = m_pDevice->GetBindings();

  size_t count = 0;
  for(const DescriptorRange &r : ranges)
    count += r.count;
  ret.resize(count);

  size_t dst = 0;
  for(const DescriptorRange &r : ranges)
  {
    uint32_t descriptorId = r.offset;

    for(uint32_t i = 0; i < r.count; i++, dst++, descriptorId++)
    {
      Descriptor &d = ret[dst];

      if(descriptorId < D3D9_NUM_TEXTURE_SLOTS)
      {
        d.type = DescriptorType::ImageSampler;
        d.resource = WrappedID(bind.textures[descriptorId]);

        if(d.resource != ResourceId())
        {
          TextureDescription tex = GetTexture(d.resource);
          d.format = tex.format;
          d.textureType = tex.type;
          d.firstMip = 0;
          d.numMips = (uint8_t)RDCMIN(tex.mips, 255U);
          d.firstSlice = 0;
          d.numSlices = (uint16_t)tex.arraysize;
        }
      }
      else if(descriptorId < D3D9Descriptor_Count)
      {
        uint32_t idx = descriptorId - D3D9Descriptor_Constants;
        d.type = DescriptorType::ConstantBuffer;
        d.resource = m_ConstantsBuffers[idx];
        d.byteOffset = 0;
        d.byteSize = uint64_t(m_ConstantCounts[idx]) *
                     ConstantRegisterSize(D3D9ConstantSet(idx % D3D9Constants_Count));
      }
    }
  }

  return ret;
}

rdcarray<SamplerDescriptor> D3D9Replay::GetSamplerDescriptors(ResourceId descriptorStore,
                                                              const rdcarray<DescriptorRange> &ranges)
{
  rdcarray<SamplerDescriptor> ret;

  if(descriptorStore != m_DescriptorStore)
  {
    RDCERR("Descriptors query for invalid descriptor store on fixed bindings API (D3D9)");
    return ret;
  }

  IDirect3DDevice9 *dev = m_pDevice->GetReal();
  const D3D9ObjectBindings &bind = m_pDevice->GetBindings();

  size_t count = 0;
  for(const DescriptorRange &r : ranges)
    count += r.count;
  ret.resize(count);

  size_t dst = 0;
  for(const DescriptorRange &r : ranges)
  {
    uint32_t descriptorId = r.offset;

    for(uint32_t i = 0; i < r.count; i++, dst++, descriptorId++)
    {
      if(descriptorId >= D3D9_NUM_TEXTURE_SLOTS)
        continue;

      DWORD sampler = D3D9SlotToSampler(descriptorId);

      D3D9SamplerStateValues states;
      ReadSamplerStates(dev, sampler, states);

      D3D9Pipe::Sampler samp;
      FillSampler(samp, sampler, states, bind.textures[descriptorId]);
      ret[dst] = MakeSamplerDescriptor(samp);
    }
  }

  return ret;
}

rdcarray<DescriptorAccess> D3D9Replay::GetDescriptorAccess(uint32_t eventId)
{
  rdcarray<DescriptorAccess> ret;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();
  const D3D9ObjectBindings &bind = m_pDevice->GetBindings();

  DescriptorAccess access;
  access.descriptorStore = m_DescriptorStore;
  access.byteSize = 1;

  // accesses from a shader's reflection: constant blocks by their register set, samplers and
  // textures by their sampler register
  auto addReflection = [&](const ShaderReflection &refl, ShaderStage stage) {
    access.stage = stage;

    // the block's fixedBindNumber is its register set: 0 float (c#), 1 integer (i#), 2 boolean (b#)
    for(uint16_t i = 0; i < refl.constantBlocks.size() && i < 0xffff; i++)
    {
      uint32_t set = refl.constantBlocks[i].fixedBindNumber;
      if(set >= D3D9Constants_Count)
        continue;
      access.type = DescriptorType::ConstantBuffer;
      access.index = i;
      access.byteOffset = ConstantsDescriptor(stage, D3D9ConstantSet(set));
      ret.push_back(access);
    }

    for(uint16_t i = 0; i < refl.samplers.size() && i < 0xffff; i++)
    {
      uint32_t desc = SamplerRegisterDescriptor(stage, refl.samplers[i].fixedBindNumber);
      if(desc == ~0U)
        continue;
      access.type = DescriptorType::Sampler;
      access.index = i;
      access.byteOffset = desc;
      ret.push_back(access);
    }

    for(uint16_t i = 0; i < refl.readOnlyResources.size() && i < 0xffff; i++)
    {
      uint32_t desc = SamplerRegisterDescriptor(stage, refl.readOnlyResources[i].fixedBindNumber);
      if(desc == ~0U)
        continue;
      access.type = refl.readOnlyResources[i].descriptorType;
      if(access.type == DescriptorType::Unknown)
        access.type = DescriptorType::ImageSampler;
      access.index = i;
      access.byteOffset = desc;
      ret.push_back(access);
    }
  };

  // without reflection, every sampler with a texture bound that the stage could use. The index is
  // the sampler register, since there's no reflection list to index.
  auto addBoundTextures = [&](ShaderStage stage, uint32_t numRegisters) {
    access.stage = stage;
    access.type = DescriptorType::ImageSampler;
    for(uint32_t reg = 0; reg < numRegisters; reg++)
    {
      uint32_t desc = SamplerRegisterDescriptor(stage, reg);
      if(desc == ~0U || bind.textures[desc] == NULL)
        continue;
      access.index = (uint16_t)reg;
      access.byteOffset = desc;
      ret.push_back(access);
    }
  };

  // vertex shader
  ResourceId vs = WrappedID(bind.vs);
  if(vs != ResourceId())
  {
    const ShaderReflection *refl =
        GetShader(ResourceId(), vs, ShaderEntryPoint("main", ShaderStage::Vertex));
    if(refl)
      addReflection(*refl, ShaderStage::Vertex);
    else if(ShaderMajorVersion(bind.vs) >= 3)
      addBoundTextures(ShaderStage::Vertex, D3D9_NUM_VERTEX_SAMPLERS);
  }

  // the displacement map is read by the tessellator, before the vertex shader
  uint32_t dmapSlot = D3D9SamplerToSlot(D3DDMAPSAMPLER);
  if(bind.textures[dmapSlot] != NULL)
  {
    access.stage = ShaderStage::Vertex;
    access.type = DescriptorType::ImageSampler;
    access.index = DescriptorAccess::NoShaderBinding;
    access.byteOffset = dmapSlot;
    ret.push_back(access);
  }

  // pixel shader, or the fixed function texture stages
  ResourceId ps = WrappedID(bind.ps);
  if(ps != ResourceId())
  {
    const ShaderReflection *refl =
        GetShader(ResourceId(), ps, ShaderEntryPoint("main", ShaderStage::Pixel));
    if(refl)
      addReflection(*refl, ShaderStage::Pixel);
    else
      addBoundTextures(ShaderStage::Pixel, D3D9_NUM_PIXEL_SAMPLERS);
  }
  else
  {
    // each texture stage samples its own sampler, up to the first disabled stage
    access.stage = ShaderStage::Pixel;
    access.type = DescriptorType::ImageSampler;
    for(uint32_t stage = 0; stage < D3D9_NUM_TEXTURE_STAGES; stage++)
    {
      DWORD op = D3DTOP_DISABLE;
      if(FAILED(dev->GetTextureStageState(stage, D3DTSS_COLOROP, &op)) || op == D3DTOP_DISABLE)
        break;
      access.index = (uint16_t)stage;
      access.byteOffset = stage;
      ret.push_back(access);
    }
  }

  return ret;
}

// Resource usage
// --------------
// Recorded as the capture loads, the way D3D11 does, so the device's state is what each event saw:
// - draws (AddDrawUsage): the streams the vertex declaration reads, the index buffer for indexed
//   draws, the textures each stage samples and the constants it reads (as GetDescriptorAccess
//   finds them, so with reflection only the samplers and register sets the shader uses), the
//   bound render targets, and the depth stencil when the depth or stencil test is on.
//   DrawPrimitiveUP and DrawIndexedPrimitiveUP read the application's memory rather than the
//   bound buffers, and ProcessVertices reads the streams through the vertex shader and writes its
//   destination buffer (StreamOut) without rasterising.
// - clears, ColorFill, copies, resolves and GenerateMipSubLevels, by the device where they're
//   loaded (d3d9_device_serialise.cpp, AddCopyAction).
// - CPU writes: Unlock inside the frame, and GDI drawing and GetFrontBufferData, whose contents
//   are in the capture.
// Render targets count as written whatever the colour write mask, as on D3D11.

void D3D9Replay::AddUsage(ResourceId id, uint32_t eventId, ResourceUsage usage)
{
  if(id != ResourceId())
    m_ResourceUses[id].push_back(EventUsage(eventId, usage));
}

void D3D9Replay::AddDrawUsage(uint32_t eventId, ActionFlags flags, D3D9DrawSource source,
                              const ResourceId (&targets)[D3D9_NUM_RENDER_TARGETS], ResourceId depth)
{
  IDirect3DDevice9 *dev = m_pDevice->GetReal();
  const D3D9ObjectBindings &bind = m_pDevice->GetBindings();

  const bool rasterised = source != D3D9DrawSource::ProcessVertices;

  if(source != D3D9DrawSource::UserPointer)
  {
    rdcarray<D3D9Emu::InputElement> elements;
    ReadVertexElements(elements);
    bool streamUsed[D3D9_NUM_STREAMS] = {};
    for(const D3D9Emu::InputElement &el : elements)
      if(el.stream < D3D9_NUM_STREAMS)
        streamUsed[el.stream] = true;
    for(uint32_t s = 0; s < D3D9_NUM_STREAMS; s++)
      if(streamUsed[s])
        AddUsage(WrappedID(bind.streams[s].buffer), eventId, ResourceUsage::VertexBuffer);

    if(source == D3D9DrawSource::Buffers && (flags & ActionFlags::Indexed))
      AddUsage(WrappedID(bind.indices), eventId, ResourceUsage::IndexBuffer);
  }

  for(const DescriptorAccess &access : GetDescriptorAccess(eventId))
  {
    if(!rasterised && access.stage != ShaderStage::Vertex)
      continue;

    if(access.byteOffset >= D3D9Descriptor_Constants)
    {
      uint32_t idx = access.byteOffset - D3D9Descriptor_Constants;
      if(idx < ARRAY_COUNT(m_ConstantUses))
        m_ConstantUses[idx].push_back(eventId);
    }
    else if(access.byteOffset < D3D9_NUM_TEXTURE_SLOTS)
    {
      AddUsage(WrappedID(bind.textures[access.byteOffset]), eventId,
               access.stage == ShaderStage::Vertex ? ResourceUsage::VS_Resource
                                                   : ResourceUsage::PS_Resource);
    }
  }

  if(!rasterised)
    return;

  for(uint32_t i = 0; i < D3D9_NUM_RENDER_TARGETS; i++)
    AddUsage(targets[i], eventId, ResourceUsage::ColorTarget);

  DWORD zenable = D3DZB_FALSE, stencil = FALSE;
  dev->GetRenderState(D3DRS_ZENABLE, &zenable);
  dev->GetRenderState(D3DRS_STENCILENABLE, &stencil);
  if(zenable != D3DZB_FALSE || stencil)
    AddUsage(depth, eventId, ResourceUsage::DepthStencilTarget);
}

void D3D9Replay::FinishUsage()
{
  for(uint32_t i = 0; i < ARRAY_COUNT(m_ConstantUses); i++)
  {
    const ResourceUsage usage =
        i < D3D9Constants_Count ? ResourceUsage::VS_Constants : ResourceUsage::PS_Constants;
    for(uint32_t eventId : m_ConstantUses[i])
      AddUsage(m_ConstantsBuffers[i], eventId, usage);
    m_ConstantUses[i].clear();
  }

  // a resource bound in several places, or read by several samplers, is listed once per event and
  // usage
  for(auto it = m_ResourceUses.begin(); it != m_ResourceUses.end(); ++it)
  {
    rdcarray<EventUsage> &v = it->second;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()) - v.begin(), ~0U);
  }
}

rdcarray<DescriptorLogicalLocation> D3D9Replay::GetDescriptorLocations(
    ResourceId descriptorStore, const rdcarray<DescriptorRange> &ranges)
{
  rdcarray<DescriptorLogicalLocation> ret;

  if(descriptorStore != m_DescriptorStore)
  {
    RDCERR("Descriptors query for invalid descriptor store on fixed bindings API (D3D9)");
    return ret;
  }

  size_t count = 0;
  for(const DescriptorRange &r : ranges)
    count += r.count;
  ret.resize(count);

  const char *setPrefix[] = {"c", "i", "b"};

  size_t dst = 0;
  for(const DescriptorRange &r : ranges)
  {
    uint32_t descriptorId = r.offset;

    for(uint32_t i = 0; i < r.count; i++, dst++, descriptorId++)
    {
      DescriptorLogicalLocation &loc = ret[dst];

      if(descriptorId < D3D9_NUM_PIXEL_SAMPLERS)
      {
        loc.stageMask = ShaderStageMask::Pixel;
        loc.category = DescriptorCategory::ReadOnlyResource;
        loc.fixedBindNumber = descriptorId;
        loc.logicalBindName = StringFormat::Fmt("s%u", descriptorId);
      }
      else if(descriptorId == D3D9SamplerToSlot(D3DDMAPSAMPLER))
      {
        loc.stageMask = ShaderStageMask::Vertex;
        loc.category = DescriptorCategory::ReadOnlyResource;
        loc.fixedBindNumber = 0;
        loc.logicalBindName = "DMAP";
      }
      else if(descriptorId < D3D9_NUM_TEXTURE_SLOTS)
      {
        uint32_t reg = D3D9SlotToSampler(descriptorId) - D3DVERTEXTEXTURESAMPLER0;
        loc.stageMask = ShaderStageMask::Vertex;
        loc.category = DescriptorCategory::ReadOnlyResource;
        loc.fixedBindNumber = reg;
        loc.logicalBindName = StringFormat::Fmt("s%u", reg);
      }
      else if(descriptorId < D3D9Descriptor_Count)
      {
        uint32_t idx = descriptorId - D3D9Descriptor_Constants;
        loc.stageMask = idx < D3D9Constants_Count ? ShaderStageMask::Vertex : ShaderStageMask::Pixel;
        loc.category = DescriptorCategory::ConstantBlock;
        loc.fixedBindNumber = idx % D3D9Constants_Count;
        loc.logicalBindName = StringFormat::Fmt("%s#", setPrefix[idx % D3D9Constants_Count]);
      }
      else
      {
        loc.category = DescriptorCategory::Unknown;
      }
    }
  }

  return ret;
}
