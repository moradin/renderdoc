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

#include "d3d9_replay.h"
#include "core/settings.h"
#include "driver/dxgi/dxgi_common.h"
#include "driver/shaders/dxbc/dxbc_compile.h"
#include "maths/formatpacking.h"
#include "replay/dummy_driver.h"
#include "d3d9_common.h"
#include "d3d9_device.h"
#include "d3d9_resources.h"

RDOC_CONFIG(bool, D3D9_ForceReplayOnExDevice, false,
            "Replay captures made with a plain D3D9 device on a D3D9Ex device, as D3D9Ex "
            "captures are. Managed textures and buffers then go in the default pool, so SetLOD "
            "is ignored, but the replay device isn't lost when another program takes the display "
            "in exclusive fullscreen, and can be created while one has it.");

namespace
{
// D3D9 formats as RenderDoc describes them, through their DXGI equivalents where there are any
ResourceFormat D3D9MakeResourceFormat(D3DFORMAT format)
{
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;

  switch((DWORD)format)
  {
    case D3DFMT_A8R8G8B8: dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case D3DFMT_X8R8G8B8: dxgi = DXGI_FORMAT_B8G8R8X8_UNORM; break;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8: dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case D3DFMT_R5G6B5: dxgi = DXGI_FORMAT_B5G6R5_UNORM; break;
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5: dxgi = DXGI_FORMAT_B5G5R5A1_UNORM; break;
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: dxgi = DXGI_FORMAT_B4G4R4A4_UNORM; break;
    case D3DFMT_A2B10G10R10: dxgi = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case D3DFMT_G16R16: dxgi = DXGI_FORMAT_R16G16_UNORM; break;
    case D3DFMT_A16B16G16R16: dxgi = DXGI_FORMAT_R16G16B16A16_UNORM; break;
    case D3DFMT_R16F: dxgi = DXGI_FORMAT_R16_FLOAT; break;
    case D3DFMT_G16R16F: dxgi = DXGI_FORMAT_R16G16_FLOAT; break;
    case D3DFMT_A16B16G16R16F: dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case D3DFMT_R32F: dxgi = DXGI_FORMAT_R32_FLOAT; break;
    case D3DFMT_G32R32F: dxgi = DXGI_FORMAT_R32G32_FLOAT; break;
    case D3DFMT_A32B32G32R32F: dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
    case D3DFMT_A8: dxgi = DXGI_FORMAT_A8_UNORM; break;
    // luminance shows as red
    case D3DFMT_L8: dxgi = DXGI_FORMAT_R8_UNORM; break;
    case D3DFMT_A8L8: dxgi = DXGI_FORMAT_R8G8_UNORM; break;
    case D3DFMT_L16: dxgi = DXGI_FORMAT_R16_UNORM; break;
    case D3DFMT_V8U8: dxgi = DXGI_FORMAT_R8G8_SNORM; break;
    case D3DFMT_Q8W8V8U8: dxgi = DXGI_FORMAT_R8G8B8A8_SNORM; break;
    case D3DFMT_V16U16: dxgi = DXGI_FORMAT_R16G16_SNORM; break;
    case D3DFMT_Q16W16V16U16: dxgi = DXGI_FORMAT_R16G16B16A16_SNORM; break;
    case D3DFMT_DXT1: dxgi = DXGI_FORMAT_BC1_UNORM; break;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3: dxgi = DXGI_FORMAT_BC2_UNORM; break;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5: dxgi = DXGI_FORMAT_BC3_UNORM; break;
    case MAKEFOURCC('A', 'T', 'I', '1'): dxgi = DXGI_FORMAT_BC4_UNORM; break;
    case MAKEFOURCC('A', 'T', 'I', '2'): dxgi = DXGI_FORMAT_BC5_UNORM; break;
    case D3DFMT_YUY2: dxgi = DXGI_FORMAT_YUY2; break;
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case MAKEFOURCC('D', 'F', '1', '6'): dxgi = DXGI_FORMAT_D16_UNORM; break;
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case MAKEFOURCC('I', 'N', 'T', 'Z'):
    case MAKEFOURCC('D', 'F', '2', '4'): dxgi = DXGI_FORMAT_D24_UNORM_S8_UINT; break;
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D32_LOCKABLE: dxgi = DXGI_FORMAT_D32_FLOAT; break;
    default: break;
  }

  if(dxgi != DXGI_FORMAT_UNKNOWN)
    return MakeResourceFormat(dxgi);

  ResourceFormat ret;

  switch((DWORD)format)
  {
    case D3DFMT_R8G8B8:
      ret.type = ResourceFormatType::Regular;
      ret.compCount = 3;
      ret.compByteWidth = 1;
      ret.compType = CompType::UNorm;
      ret.SetBGRAOrder(true);
      return ret;
    case D3DFMT_A2R10G10B10:
      ret = MakeResourceFormat(DXGI_FORMAT_R10G10B10A2_UNORM);
      ret.SetBGRAOrder(true);
      return ret;
    // palette indices, shown as integers. The palette is device state, it isn't applied.
    case D3DFMT_P8:
    case D3DFMT_A8P8:
      ret.type = ResourceFormatType::Regular;
      ret.compCount = uint8_t(format == D3DFMT_P8 ? 1 : 2);
      ret.compByteWidth = 1;
      ret.compType = CompType::UInt;
      return ret;
    // luminance in the low nibble shows as red, like L8, and alpha as green, like A8L8
    case D3DFMT_A4L4:
      ret.type = ResourceFormatType::R4G4;
      ret.compCount = 2;
      ret.compByteWidth = 1;
      ret.compType = CompType::UNorm;
      return ret;
    // the computed third channel isn't stored
    case D3DFMT_CxV8U8: return MakeResourceFormat(DXGI_FORMAT_R8G8_SNORM);
    // the 2-bit alpha is unsigned, which RenderDoc's signed 10:10:10:2 can't say
    case D3DFMT_A2W10V10U10:
      ret = MakeResourceFormat(DXGI_FORMAT_R10G10B10A2_UNORM);
      ret.compType = CompType::SNorm;
      return ret;
    case D3DFMT_D15S1:
      ret.type = ResourceFormatType::Undefined;
      ret.compCount = 2;
      ret.compByteWidth = 1;
      ret.compType = CompType::Depth;
      return ret;
    default: break;
  }

  // formats RenderDoc can't describe: R3G3B2, A8R3G3B2, L6V5U5, X8L8V8U8, UYVY, the packed RGBG
  // formats, A1 and vendor FOURCCs. The texture viewer decodes most of them (to the
  // channels D3D9 samples them as), so they're described as four unsigned normalised channels.
  ret.type = ResourceFormatType::Undefined;
  ret.compCount = 4;
  ret.compByteWidth = 1;
  ret.compType = CompType::UNorm;
  return ret;
}

bool IsDepthFormat(D3DFORMAT format)
{
  switch((DWORD)format)
  {
    case D3DFMT_D16:
    case D3DFMT_D15S1:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case D3DFMT_D32:
    case MAKEFOURCC('I', 'N', 'T', 'Z'):
    case MAKEFOURCC('D', 'F', '2', '4'):
    case MAKEFOURCC('D', 'F', '1', '6'): return true;
    default: return false;
  }
}

uint64_t SubresourceBytes(D3DFORMAT format, uint32_t width, uint32_t height, uint32_t depth)
{
  D3D9FormatBlock block = D3D9GetFormatBlock(format);
  if(block.Bytes == 0)
    return uint64_t(width) * height * depth * 4;
  return uint64_t((width + block.Width - 1) / block.Width) *
         ((height + block.Height - 1) / block.Height) * block.Bytes * depth;
}

// the format of a texture's top level, or of a surface
D3DFORMAT TextureFormat(WrappedD3D9Object *obj)
{
  D3DSURFACE_DESC desc = {};
  switch(obj->GetResourceType())
  {
    case Resource_Texture: ((WrappedD3DTexture9 *)obj)->GetReal()->GetLevelDesc(0, &desc); break;
    case Resource_CubeTexture:
      ((WrappedD3DCubeTexture9 *)obj)->GetReal()->GetLevelDesc(0, &desc);
      break;
    case Resource_VolumeTexture:
    {
      D3DVOLUME_DESC volDesc = {};
      ((WrappedD3DVolumeTexture9 *)obj)->GetReal()->GetLevelDesc(0, &volDesc);
      return volDesc.Format;
    }
    case Resource_Surface: ((WrappedD3DSurface9 *)obj)->GetReal()->GetDesc(&desc); break;
    default: return D3DFMT_UNKNOWN;
  }
  return desc.Format;
}

// Changes data where D3D9's layout differs from the format RenderDoc describes it with:
// - An X format's unused bits hold whatever was last written there, usually 0. RenderDoc describes
//   these formats with alpha in those bits (B8G8R8X8 reads its X as alpha too), so they're set to
//   what sampling the texture returns: 1.
// - ATI2 is described as BC5, but D3D9 samples the second half of each block as red and the first
//   as green, so the halves are swapped.
void MatchDescribedFormat(D3DFORMAT format, bytebuf &data)
{
  switch((DWORD)format)
  {
    case MAKEFOURCC('A', 'T', 'I', '2'):
      for(size_t i = 0; i + 15 < data.size(); i += 16)
        for(size_t j = 0; j < 8; j++)
          std::swap(data[i + j], data[i + j + 8]);
      break;
    case D3DFMT_X8R8G8B8:
    case D3DFMT_X8B8G8R8:
      for(size_t i = 0; i + 3 < data.size(); i += 4)
        data[i + 3] = 0xff;
      break;
    case D3DFMT_X1R5G5B5:
      for(size_t i = 0; i + 1 < data.size(); i += 2)
        data[i + 1] |= 0x80;
      break;
    case D3DFMT_X4R4G4B4:
      for(size_t i = 0; i + 1 < data.size(); i += 2)
        data[i + 1] |= 0xf0;
      break;
    default: break;
  }
}

// GetTextureData's remaps, the same mapping as the texture display, which other APIs remap through.
// D3D9 has no sRGB formats.
struct TexelRemap
{
  TexelRemap(const GetTextureDataParams &params) : remap(params.remap)
  {
    dstFmt.type = ResourceFormatType::Regular;
    dstFmt.compCount = 4;
    if(remap == RemapTexture::RGBA8)
    {
      dstFmt.compByteWidth = 1;
      dstFmt.compType = CompType::UNorm;
    }
    else
    {
      dstFmt.compByteWidth = remap == RemapTexture::RGBA16 ? 2 : 4;
      dstFmt.compType = CompType::Float;
    }
    dstSize = dstFmt.ElementSize();

    blackPoint = params.blackPoint;
    const float rangeSize = params.whitePoint - params.blackPoint;
    invRange = rangeSize != 0.0f ? 1.0f / rangeSize : 1.0f;
  }

  void Encode(FloatVector v, byte *out) const
  {
    float *comps = &v.x;
    for(int c = 0; c < 4; c++)
    {
      comps[c] = (comps[c] - blackPoint) * invRange;
      if(remap == RemapTexture::RGBA8)
        comps[c] = RDCCLAMP(comps[c], 0.0f, 1.0f);
    }
    EncodeFormattedComponents(dstFmt, v, out);
  }

  RemapTexture remap;
  ResourceFormat dstFmt;
  uint32_t dstSize;
  float blackPoint;
  float invRange;
};
};

D3D9Replay::D3D9Replay(WrappedD3DDevice9 *device, IDirect3D9 *d3d, HWND hiddenWindow)
    : m_pDevice(device), m_D3D(d3d), m_HiddenWindow(hiddenWindow)
{
  m_HighlightCache.driver = this;

  D3DADAPTER_IDENTIFIER9 ident = {};
  if(m_D3D && SUCCEEDED(m_D3D->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident)))
  {
    m_DriverInfo.vendor = GPUVendorFromPCIVendor(ident.VendorId);
    const uint64_t ver = (uint64_t)ident.DriverVersion.QuadPart;
    snprintf(m_DriverInfo.version, sizeof(m_DriverInfo.version), "%s %u.%u.%u.%u",
             ident.Description, uint32_t(ver >> 48) & 0xFFFF, uint32_t(ver >> 32) & 0xFFFF,
             uint32_t(ver >> 16) & 0xFFFF, uint32_t(ver) & 0xFFFF);
  }
}

void D3D9Replay::Shutdown()
{
  ReleaseDebugRender();
  ReleaseTargetShaders();

  for(auto it = m_OutputWindows.begin(); it != m_OutputWindows.end(); ++it)
    it->second.Release();
  m_OutputWindows.clear();

  m_pDevice->ReleaseReplayObjects();

  for(SDObject *&annotation : m_ConstantsAnnotations)
  {
    delete annotation;
    annotation = NULL;
  }

  // the last reference: everything replay created has been released
  ULONG refs = m_pDevice->Release();
  if(refs != 0)
    RDCWARN("D3D9 replay device still has %u references at shutdown", refs);
  m_pDevice = NULL;

  if(m_HiddenWindow)
    DestroyWindow(m_HiddenWindow);
  m_HiddenWindow = NULL;

  delete this;
}

ResourceDescription &D3D9Replay::GetResourceDesc(ResourceId id)
{
  auto it = m_ResourceIdx.find(id);
  if(it == m_ResourceIdx.end())
  {
    m_ResourceIdx[id] = m_Resources.size();
    m_Resources.push_back(ResourceDescription());
    m_Resources.back().resourceId = id;
    return m_Resources.back();
  }

  return m_Resources[it->second];
}

rdcarray<ResourceDescription> D3D9Replay::GetResources()
{
  return m_Resources;
}

rdcarray<DescriptorStoreDescription> D3D9Replay::GetDescriptorStores()
{
  return {};
}

rdcarray<BufferDescription> D3D9Replay::GetBuffers()
{
  rdcarray<BufferDescription> ret;

  for(const ResourceDescription &descr : m_Resources)
  {
    if(descr.type != ResourceType::Buffer)
      continue;

    BufferDescription buf = GetBuffer(descr.resourceId);
    if(buf.resourceId != ResourceId())
      ret.push_back(buf);
  }

  return ret;
}

BufferDescription D3D9Replay::GetBuffer(ResourceId id)
{
  BufferDescription ret;

  // the shader constant registers, presented as buffers (see d3d9_pipestate.cpp)
  if(ConstantsBufferIndex(id) >= 0)
    return GetConstantsBuffer(id);

  // post-transform vertices and DrawPrimitiveUP data (see d3d9_postvs.cpp)
  if(IsPostVSBuffer(id))
    return GetPostVSBuffer(id);

  D3D9ResourceManager *rm = m_pDevice->GetResourceManager();
  if(!rm->HasResource(id))
    return ret;

  WrappedD3D9Object *obj = GetWrappedObject(rm->GetResource(id));
  if(obj == NULL)
    return ret;

  if(obj->GetResourceType() == Resource_VertexBuffer)
  {
    D3DVERTEXBUFFER_DESC desc = {};
    ((WrappedD3DVertexBuffer9 *)obj)->GetReal()->GetDesc(&desc);
    ret.resourceId = id;
    ret.creationFlags = BufferCategory::Vertex;
    ret.length = desc.Size;
  }
  else if(obj->GetResourceType() == Resource_IndexBuffer)
  {
    D3DINDEXBUFFER_DESC desc = {};
    ((WrappedD3DIndexBuffer9 *)obj)->GetReal()->GetDesc(&desc);
    ret.resourceId = id;
    ret.creationFlags = BufferCategory::Index;
    ret.length = desc.Size;
  }

  return ret;
}

rdcarray<TextureDescription> D3D9Replay::GetTextures()
{
  rdcarray<TextureDescription> ret;

  for(const ResourceDescription &descr : m_Resources)
  {
    if(descr.type != ResourceType::Texture && descr.type != ResourceType::SwapchainImage)
      continue;

    TextureDescription tex = GetTexture(descr.resourceId);
    if(tex.resourceId != ResourceId())
      ret.push_back(tex);
  }

  return ret;
}

TextureDescription D3D9Replay::GetTexture(ResourceId id)
{
  TextureDescription ret = {};
  ret.resourceId = ResourceId();

  D3D9ResourceManager *rm = m_pDevice->GetResourceManager();
  if(!rm->HasResource(id))
    return ret;

  WrappedD3D9Object *obj = GetWrappedObject(rm->GetResource(id));
  if(obj == NULL)
    return ret;

  D3DSURFACE_DESC desc = {};
  D3DVOLUME_DESC volDesc = {};
  uint32_t mips = 1;

  switch(obj->GetResourceType())
  {
    case Resource_Texture:
    {
      IDirect3DTexture9 *tex = ((WrappedD3DTexture9 *)obj)->GetReal();
      tex->GetLevelDesc(0, &desc);
      mips = tex->GetLevelCount();
      ret.type = TextureType::Texture2D;
      ret.arraysize = 1;
      break;
    }
    case Resource_CubeTexture:
    {
      IDirect3DCubeTexture9 *tex = ((WrappedD3DCubeTexture9 *)obj)->GetReal();
      tex->GetLevelDesc(0, &desc);
      mips = tex->GetLevelCount();
      ret.type = TextureType::TextureCube;
      ret.cubemap = true;
      ret.arraysize = 6;
      break;
    }
    case Resource_VolumeTexture:
    {
      IDirect3DVolumeTexture9 *tex = ((WrappedD3DVolumeTexture9 *)obj)->GetReal();
      tex->GetLevelDesc(0, &volDesc);
      mips = tex->GetLevelCount();
      ret.type = TextureType::Texture3D;
      ret.arraysize = 1;
      break;
    }
    case Resource_Surface:
    {
      // texture levels are part of their texture
      WrappedD3D9Object *container = obj->GetContainer();
      if(container && container->GetResourceType() != Resource_SwapChain)
        return ret;

      ((WrappedD3DSurface9 *)obj)->GetReal()->GetDesc(&desc);
      ret.type = desc.MultiSampleType != D3DMULTISAMPLE_NONE ? TextureType::Texture2DMS
                                                             : TextureType::Texture2D;
      ret.arraysize = 1;
      if(container)
        ret.creationFlags |= TextureCategory::SwapBuffer;
      break;
    }
    default: return ret;
  }

  ret.resourceId = id;
  ret.mips = mips;

  if(obj->GetResourceType() == Resource_VolumeTexture)
  {
    ret.format = D3D9MakeResourceFormat(volDesc.Format);
    ret.dimension = 3;
    ret.width = volDesc.Width;
    ret.height = volDesc.Height;
    ret.depth = volDesc.Depth;
    ret.msSamp = 1;
    ret.creationFlags |= TextureCategory::ShaderRead;

    for(uint32_t m = 0; m < mips; m++)
      ret.byteSize += SubresourceBytes(volDesc.Format, RDCMAX(1U, ret.width >> m),
                                       RDCMAX(1U, ret.height >> m), RDCMAX(1U, ret.depth >> m));
    return ret;
  }

  ret.format = D3D9MakeResourceFormat(desc.Format);
  ret.dimension = 2;
  ret.width = desc.Width;
  ret.height = desc.Height;
  ret.depth = 1;
  ret.msSamp = RDCMAX(1U, (uint32_t)desc.MultiSampleType);
  ret.msQual = desc.MultiSampleQuality;

  if(obj->GetResourceType() != Resource_Surface)
    ret.creationFlags |= TextureCategory::ShaderRead;
  if(desc.Usage & D3DUSAGE_RENDERTARGET)
    ret.creationFlags |= TextureCategory::ColorTarget;
  if((desc.Usage & D3DUSAGE_DEPTHSTENCIL) || IsDepthFormat(desc.Format))
    ret.creationFlags |= TextureCategory::DepthTarget;

  for(uint32_t m = 0; m < mips; m++)
    ret.byteSize +=
        SubresourceBytes(desc.Format, RDCMAX(1U, ret.width >> m), RDCMAX(1U, ret.height >> m), 1) *
        ret.arraysize * ret.msSamp;

  return ret;
}

rdcarray<DebugMessage> D3D9Replay::GetDebugMessages()
{
  return m_pDevice->GetDebugMessages();
}

RDResult D3D9Replay::ReadLogInitialisation(RDCFile *rdc, bool storeStructuredBuffers)
{
  RDResult ret = m_pDevice->ReadLogInitialisation(rdc, storeStructuredBuffers);
  if(ret == ResultCode::Succeeded)
  {
    CreatePipelineStateResources();
    FinishUsage();
  }
  return ret;
}

void D3D9Replay::ReplayLog(uint32_t endEventID, ReplayLogType replayType)
{
  ClearReplayCaches();
  m_ReplayedEvent = endEventID;
  m_pDevice->ReplayLog(0, endEventID, replayType);
  // shaders that stopped replacing one before this replay (FreeTargetResource)
  ReleaseFreedTargetShaders();
}

SDFile *D3D9Replay::GetStructuredFile()
{
  return m_pDevice->GetStructuredFile();
}

void D3D9Replay::GetBufferData(ResourceId buff, uint64_t offset, uint64_t len, bytebuf &retData)
{
  retData.clear();

  if(ConstantsBufferIndex(buff) >= 0)
  {
    GetConstantsData(buff, offset, len, retData);
    return;
  }

  if(IsPostVSBuffer(buff))
  {
    GetPostVSBufferData(buff, offset, len, retData);
    return;
  }

  D3D9ResourceManager *rm = m_pDevice->GetResourceManager();
  if(!rm->HasResource(buff))
    return;

  D3D9SubresourceData sub;
  rdcstr err = m_pDevice->ReadSubresource(GetWrappedObject(rm->GetResource(buff)), 0, 0, sub);
  if(!err.empty())
  {
    RDCWARN("Couldn't read buffer %s: %s", ToStr(buff).c_str(), err.c_str());
    return;
  }

  if(offset >= sub.Data.size())
    return;

  uint64_t size = sub.Data.size() - offset;
  if(len > 0)
    size = RDCMIN(size, len);

  retData.assign(sub.Data.data() + offset, (size_t)size);
}

void D3D9Replay::GetTextureData(ResourceId tex, const Subresource &sub,
                                const GetTextureDataParams &params, bytebuf &data)
{
  data.clear();

  D3D9ResourceManager *rm = m_pDevice->GetResourceManager();
  if(!rm->HasResource(tex))
    return;

  WrappedD3D9Object *obj = GetWrappedObject(rm->GetResource(tex));
  if(obj == NULL)
    return;

  const TextureDescription desc = GetTexture(tex);

  // block compressed textures, and the formats RenderDoc can't describe (D3D9MakeResourceFormat),
  // are remapped from the texels picking decodes (GetDecoded), which match what the GPU samples.
  // YUV formats are (Cr, Y, Cb, 1), as picked. A volume's mip is returned with all its depth
  // slices, like its raw data.
  if(params.remap != RemapTexture::NoRemap &&
     (desc.format.BlockFormat() || desc.format.type == ResourceFormatType::Undefined ||
      desc.format.type == ResourceFormatType::YUV8))
  {
    const TexelRemap remap(params);
    const bool volume = obj->GetResourceType() == Resource_VolumeTexture;
    const uint32_t depth = volume ? RDCMAX(1U, desc.depth >> sub.mip) : 1;
    for(uint32_t z = 0; z < depth; z++)
    {
      const DecodedSubresource &decoded =
          GetDecoded(tex, Subresource(sub.mip, volume ? z : sub.slice, sub.sample));
      if(decoded.texels.empty())
      {
        RDCWARN("Couldn't decode %s for saving: %s", ToStr(tex).c_str(), decoded.error.c_str());
        data.clear();
        return;
      }

      const size_t offset = data.size();
      data.resize(offset + decoded.texels.size() * remap.dstSize);
      for(size_t i = 0; i < decoded.texels.size(); i++)
        remap.Encode(decoded.texels[i], data.data() + offset + i * remap.dstSize);
    }
    return;
  }

  D3D9SubresourceData contents;
  rdcstr err = m_pDevice->ReadSubresource(
      obj, obj->GetResourceType() == Resource_CubeTexture ? sub.slice : 0, sub.mip, contents);
  if(!err.empty())
  {
    RDCWARN("Couldn't read texture %s: %s", ToStr(tex).c_str(), err.c_str());
    return;
  }

  // read back depth stencils have one slice per sample, in D3D9's layout with depth in the high
  // bits. RenderDoc's D24S8 has the stencil in the high byte.
  if(desc.creationFlags & TextureCategory::DepthTarget)
  {
    const size_t slicePitch = size_t(contents.RowPitch) * contents.RowCount;
    const uint32_t slice = params.resolve ? 0 : RDCMIN(sub.sample, contents.Depth - 1);
    if(contents.Depth > 1 && slicePitch * (slice + 1) <= contents.Data.size())
      contents.Data = bytebuf(contents.Data.data() + slicePitch * slice, slicePitch);

    if(desc.format.type == ResourceFormatType::D24S8)
    {
      uint32_t *values = (uint32_t *)contents.Data.data();
      for(size_t i = 0; i < contents.Data.size() / 4; i++)
        values[i] = (values[i] >> 8) | (values[i] << 24);
    }
  }

  data.swap(contents.Data);

  MatchDescribedFormat(TextureFormat(obj), data);

  if(params.remap == RemapTexture::NoRemap)
    return;

  const ResourceFormat &fmt = desc.format;
  const bool defaultRange = params.blackPoint == 0.0f && params.whitePoint == 1.0f;

  // 8-bit RGBA only needs its channels put in order
  if(params.remap == RemapTexture::RGBA8 && defaultRange && fmt.type == ResourceFormatType::Regular &&
     fmt.compByteWidth == 1 && fmt.compCount == 4 && fmt.compType == CompType::UNorm)
  {
    if(fmt.BGRAOrder())
    {
      for(size_t i = 0; i + 3 < data.size(); i += 4)
        std::swap(data[i], data[i + 2]);
    }
    return;
  }

  // everything else is converted texel by texel on the CPU. The depth formats were put in
  // RenderDoc's layout above.
  const uint32_t srcSize = fmt.ElementSize();
  if(srcSize == 0)
  {
    RDCWARN("Converting %s textures for saving isn't supported yet", fmt.Name().c_str());
    return;
  }

  const TexelRemap remap(params);

  const size_t count = data.size() / srcSize;
  bytebuf converted;
  converted.resize(count * remap.dstSize);
  for(size_t i = 0; i < count; i++)
  {
    FloatVector v = DecodeFormattedComponents(fmt, data.data() + i * srcSize);
    // a missing alpha samples as 1, the decode leaves it 0 for the palette index formats
    if(fmt.type == ResourceFormatType::Regular && fmt.compCount < 4)
      v.w = 1.0f;
    remap.Encode(v, converted.data() + i * remap.dstSize);
  }

  data.swap(converted);
}

void D3D9Replay::ClearReplayCache()
{
  ClearPostVSCache();
}

void D3D9Replay::ReloadShaderDebugInformation()
{
}

rdcarray<PixelModification> D3D9Replay::PixelHistory(rdcarray<EventUsage> events, ResourceId target,
                                                     uint32_t x, uint32_t y, const Subresource &sub,
                                                     CompType typeCast)
{
  return {};
}

ShaderDebugTrace *D3D9Replay::DebugVertex(uint32_t eventId, uint32_t vertid, uint32_t instid,
                                          uint32_t idx, uint32_t view)
{
  return new ShaderDebugTrace;
}

ShaderDebugTrace *D3D9Replay::DebugPixel(uint32_t eventId, uint32_t x, uint32_t y,
                                         const DebugPixelInputs &inputs)
{
  return new ShaderDebugTrace;
}

ShaderDebugTrace *D3D9Replay::DebugThread(uint32_t eventId, const rdcfixedarray<uint32_t, 3> &groupid,
                                          const rdcfixedarray<uint32_t, 3> &threadid)
{
  return new ShaderDebugTrace;
}

ShaderDebugTrace *D3D9Replay::DebugMeshThread(uint32_t eventId,
                                              const rdcfixedarray<uint32_t, 3> &groupid,
                                              const rdcfixedarray<uint32_t, 3> &threadid)
{
  return new ShaderDebugTrace;
}

rdcarray<ShaderDebugState> D3D9Replay::ContinueDebug(ShaderDebugger *debugger)
{
  return {};
}

void D3D9Replay::FreeDebugger(ShaderDebugger *debugger)
{
}

RDResult D3D9Replay::FatalErrorCheck()
{
  return m_pDevice->FatalErrorCheck();
}

IReplayDriver *D3D9Replay::MakeDummyDriver()
{
  return new DummyDriver(this, {}, m_pDevice->DetachStructuredFile(), {});
}

///////////////////////////////////////////////////////////////////////////////
// output windows, as swapchains on the real device

void D3D9Replay::OutputWindow::Release()
{
  SAFE_RELEASE(depthStencil);
  SAFE_RELEASE(backBuffer);
  SAFE_RELEASE(swap);
}

bool D3D9Replay::CreateOutputSwapChain(OutputWindow &outw)
{
  outw.Release();

  if(outw.width <= 0 || outw.height <= 0)
    return false;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();
  HRESULT hr = S_OK;

  if(outw.wnd)
  {
    D3DPRESENT_PARAMETERS params = {};
    params.BackBufferWidth = (UINT)outw.width;
    params.BackBufferHeight = (UINT)outw.height;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = outw.wnd;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    hr = dev->CreateAdditionalSwapChain(&params, &outw.swap);
    if(SUCCEEDED(hr))
      hr = outw.swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &outw.backBuffer);
  }
  else
  {
    hr = dev->CreateRenderTarget((UINT)outw.width, (UINT)outw.height, D3DFMT_X8R8G8B8,
                                 D3DMULTISAMPLE_NONE, 0, FALSE, &outw.backBuffer, NULL);
  }

  if(SUCCEEDED(hr) && outw.depth)
    hr = dev->CreateDepthStencilSurface((UINT)outw.width, (UINT)outw.height, D3DFMT_D24S8,
                                        D3DMULTISAMPLE_NONE, 0, FALSE, &outw.depthStencil, NULL);

  if(FAILED(hr))
  {
    RDCERR("Couldn't create the output window's swapchain: %s", ToStr(hr).c_str());
    outw.Release();
    return false;
  }

  return true;
}

uint64_t D3D9Replay::MakeOutputWindow(WindowingData window, bool depth)
{
  OutputWindow outw;
  outw.depth = depth;

  if(window.system == WindowingSystem::Win32)
  {
    outw.wnd = window.win32.window;

    RECT rect = {};
    GetClientRect(outw.wnd, &rect);
    outw.width = rect.right - rect.left;
    outw.height = rect.bottom - rect.top;
  }
  else if(window.system == WindowingSystem::Headless)
  {
    outw.width = window.headless.width;
    outw.height = window.headless.height;
  }
  else
  {
    RDCERR("Unexpected window system %u", window.system);
    return 0;
  }

  CreateOutputSwapChain(outw);

  uint64_t id = m_OutputWindowID++;
  m_OutputWindows[id] = outw;
  return id;
}

void D3D9Replay::DestroyOutputWindow(uint64_t id)
{
  auto it = m_OutputWindows.find(id);
  if(it == m_OutputWindows.end())
    return;

  it->second.Release();
  m_OutputWindows.erase(it);

  if(m_CurrentOutputWindow == id)
    m_CurrentOutputWindow = 0;
}

bool D3D9Replay::CheckResizeOutputWindow(uint64_t id)
{
  auto it = m_OutputWindows.find(id);
  if(it == m_OutputWindows.end() || it->second.wnd == NULL)
    return false;

  OutputWindow &outw = it->second;

  RECT rect = {};
  GetClientRect(outw.wnd, &rect);
  const int32_t w = rect.right - rect.left, h = rect.bottom - rect.top;

  if(w == outw.width && h == outw.height)
    return false;

  outw.width = w;
  outw.height = h;
  CreateOutputSwapChain(outw);
  return true;
}

void D3D9Replay::GetOutputWindowDimensions(uint64_t id, int32_t &w, int32_t &h)
{
  w = h = 0;

  auto it = m_OutputWindows.find(id);
  if(it == m_OutputWindows.end())
    return;

  w = it->second.width;
  h = it->second.height;
}

void D3D9Replay::ClearOutputWindowColor(uint64_t id, FloatVector col)
{
  auto it = m_OutputWindows.find(id);
  if(it == m_OutputWindows.end() || it->second.backBuffer == NULL)
    return;

  // the colour is linear, D3D11's sRGB output windows encode it when they're cleared. The back
  // buffers here aren't sRGB and ColorFill writes the value as it is, so encode it first (and
  // clamp it, D3DCOLOR_COLORVALUE doesn't). ColorFill doesn't touch the device's state.
  auto encode = [](float f) { return ConvertLinearToSRGB(RDCCLAMP(f, 0.0f, 1.0f)); };
  m_pDevice->GetReal()->ColorFill(
      it->second.backBuffer, NULL,
      D3DCOLOR_COLORVALUE(encode(col.x), encode(col.y), encode(col.z), RDCCLAMP(col.w, 0.0f, 1.0f)));
}

void D3D9Replay::ClearOutputWindowDepth(uint64_t id, float depth, uint8_t stencil)
{
  auto it = m_OutputWindows.find(id);
  if(it == m_OutputWindows.end() || it->second.depthStencil == NULL || it->second.backBuffer == NULL)
    return;

  // binding our targets changes device state, which would be recorded into the application's state
  // block if one is being recorded at this event. Nothing is drawn then (see SaveReplayDrawState),
  // so there's nothing to clear either.
  D3D9DeviceState state;
  if(!m_pDevice->SaveReplayDrawState(state))
    return;

  IDirect3DDevice9 *dev = m_pDevice->GetReal();

  dev->SetRenderTarget(0, it->second.backBuffer);
  dev->SetDepthStencilSurface(it->second.depthStencil);

  // Clear is limited to the viewport and, when it's enabled, the scissor rect
  D3DVIEWPORT9 viewport = {};
  viewport.Width = (DWORD)it->second.width;
  viewport.Height = (DWORD)it->second.height;
  viewport.MaxZ = 1.0f;
  dev->SetViewport(&viewport);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

  dev->Clear(0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, depth, stencil);

  m_pDevice->RestoreReplayDrawState(state);
}

void D3D9Replay::BindOutputWindow(uint64_t id, bool depth)
{
  // what's drawn next goes to this window, with its depth stencil if it has one
  m_CurrentOutputWindow = id;
}

bool D3D9Replay::IsOutputWindowVisible(uint64_t id)
{
  auto it = m_OutputWindows.find(id);
  if(it == m_OutputWindows.end())
    return false;

  return it->second.wnd == NULL || IsWindowVisible(it->second.wnd) == TRUE;
}

void D3D9Replay::FlipOutputWindow(uint64_t id)
{
  auto it = m_OutputWindows.find(id);
  if(it == m_OutputWindows.end() || it->second.swap == NULL)
    return;

  it->second.swap->Present(NULL, NULL, NULL, NULL, 0);
}

///////////////////////////////////////////////////////////////////////////////
// creating the replay device

namespace
{
const wchar_t *HiddenWindowClass = L"renderdoc_d3d9_replay";

HWND CreateHiddenWindow()
{
  static bool registered = false;
  if(!registered)
  {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = HiddenWindowClass;
    registered = RegisterClassExW(&wc) != 0;
  }

  return CreateWindowExW(0, HiddenWindowClass, L"RenderDoc D3D9 replay", WS_OVERLAPPEDWINDOW, 0, 0,
                         64, 64, NULL, NULL, GetModuleHandleW(NULL), NULL);
}
};

RDResult D3D9_CreateReplayDevice(RDCFile *rdc, const ReplayOptions &opts, IReplayDriver **driver)
{
  RDCDEBUG("Creating a D3D9 replay device");

  if(rdc == NULL)
    RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                        "D3D9 can't be used to replay captures made with other APIs");

  int sectionIdx = rdc->SectionIndex(SectionType::FrameCapture);
  if(sectionIdx < 0)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "File does not contain captured API data");

  const uint64_t ver = rdc->GetSectionProperties(sectionIdx).version;
  if(!D3D9InitParams::IsSupportedVersion(ver))
    RETURN_ERROR_RESULT(ResultCode::APIIncompatibleVersion,
                        "D3D9 capture is incompatible version %llu, newest supported by this "
                        "build of RenderDoc is %llu",
                        ver, D3D9InitParams::CurrentVersion);

  D3D9InitParams initParams;
  {
    StreamReader *reader = rdc->ReadSection(sectionIdx);
    ReadSerialiser ser(reader, Ownership::Stream);
    ser.SetVersion(ver);

    SystemChunk chunk = ser.ReadChunk<SystemChunk>();
    if(chunk != SystemChunk::DriverInit)
      RETURN_ERROR_RESULT(ResultCode::FileCorrupted,
                          "Expected to get a DriverInit chunk, instead got %u", chunk);

    SERIALISE_ELEMENT(initParams);

    if(ser.IsErrored())
      return ser.GetError();
  }

  if(initParams.AdapterIdentifier.Description[0])
    RDCLOG("Capture was made on %s", initParams.AdapterIdentifier.Description);

  if(GetD3DCompiler() == NULL)
    RDCWARN("Couldn't load d3dcompiler_47.dll, depth stencils won't be written back");

  HMODULE lib = LoadLibraryA("d3d9.dll");
  if(lib == NULL)
    RETURN_ERROR_RESULT(ResultCode::APIInitFailed, "Failed to load d3d9.dll");

  // Captures replay on the kind of device they were made with. A plain D3D9 device keeps
  // D3DPOOL_MANAGED objects as they were, which is the only pool SetLOD works in. It can be lost
  // (another application going fullscreen, a display mode change), which replay reports as a
  // fatal error (FatalErrorCheck) rather than recreating everything in the default pool. D3D9Ex
  // devices are never lost, and D3D9Ex captures need one for their Ex calls.
  const bool useEx = initParams.Ex || D3D9_ForceReplayOnExDevice();
  RDCLOG("Replaying a %s capture on a %s device%s", initParams.Ex ? "D3D9Ex" : "D3D9",
         useEx ? "D3D9Ex" : "D3D9",
         !initParams.Ex && useEx ? " (D3D9_ForceReplayOnExDevice is set)" : "");

  IDirect3D9 *d3d = NULL;
  HRESULT hr = S_OK;
  if(useEx)
  {
    typedef HRESULT(WINAPI * PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex **);
    PFN_Direct3DCreate9Ex create9Ex =
        (PFN_Direct3DCreate9Ex)GetProcAddress(lib, "Direct3DCreate9Ex");
    if(create9Ex == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIInitFailed, "d3d9.dll has no Direct3DCreate9Ex");

    IDirect3D9Ex *d3dEx = NULL;
    hr = create9Ex(D3D_SDK_VERSION, &d3dEx);
    if(FAILED(hr) || d3dEx == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIInitFailed, "Direct3DCreate9Ex failed: %s",
                          ToStr(hr).c_str());
    d3d = d3dEx;
  }
  else
  {
    typedef IDirect3D9 *(WINAPI * PFN_Direct3DCreate9)(UINT);
    PFN_Direct3DCreate9 create9 = (PFN_Direct3DCreate9)GetProcAddress(lib, "Direct3DCreate9");
    if(create9 == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIInitFailed, "d3d9.dll has no Direct3DCreate9");

    d3d = create9(D3D_SDK_VERSION);
    if(d3d == NULL)
      RETURN_ERROR_RESULT(ResultCode::APIInitFailed, "Direct3DCreate9 failed");
  }

  HWND wnd = CreateHiddenWindow();
  if(wnd == NULL)
  {
    d3d->Release();
    RETURN_ERROR_RESULT(ResultCode::APIInitFailed, "Couldn't create the replay window");
  }

  // the device's own swapchain is never used, the capture's are created separately
  D3DPRESENT_PARAMETERS params = {};
  params.BackBufferWidth = 16;
  params.BackBufferHeight = 16;
  params.BackBufferFormat = D3DFMT_UNKNOWN;
  params.BackBufferCount = 1;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = wnd;
  params.Windowed = TRUE;
  params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  // mixed vertex processing, so that captures from software vertex processing devices (with
  // their larger constant files) replay
  const DWORD commonFlags =
      D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED | D3DCREATE_NOWINDOWCHANGES;

  auto createDevice = [&](DWORD vpFlags, IDirect3DDevice9 **dev) {
    if(useEx)
    {
      IDirect3DDevice9Ex *devEx = NULL;
      HRESULT ret = ((IDirect3D9Ex *)d3d)
                        ->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                         vpFlags | commonFlags, &params, NULL, &devEx);
      *dev = devEx;
      return ret;
    }
    return d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, vpFlags | commonFlags,
                             &params, dev);
  };

  IDirect3DDevice9 *dev = NULL;
  hr = createDevice(D3DCREATE_MIXED_VERTEXPROCESSING, &dev);
  if(FAILED(hr))
  {
    RDCWARN("Couldn't create a mixed vertex processing device (%s), trying hardware",
            ToStr((D3D9Result)hr).c_str());
    hr = createDevice(D3DCREATE_HARDWARE_VERTEXPROCESSING, &dev);
  }

  // a plain D3D9 device can't be created while another program has the display in exclusive
  // fullscreen (measured: D3DERR_DEVICELOST), a D3D9Ex one can
  if(hr == D3DERR_DEVICELOST && !useEx)
  {
    DestroyWindow(wnd);
    d3d->Release();
    RETURN_ERROR_RESULT(
        ResultCode::APIInitFailed,
        "Couldn't create a D3D9 device to replay with (D3DERR_DEVICELOST): another "
        "program probably has the display in exclusive fullscreen. Open the capture "
        "again once it has left fullscreen, or set D3D9_ForceReplayOnExDevice to "
        "replay on a D3D9Ex device, which ignores SetLOD");
  }

  if(FAILED(hr) || dev == NULL)
  {
    DestroyWindow(wnd);
    d3d->Release();
    RETURN_ERROR_RESULT(ResultCode::APIHardwareUnsupported,
                        "Couldn't create a %s device to replay with: %s", useEx ? "D3D9Ex" : "D3D9",
                        ToStr((D3D9Result)hr).c_str());
  }

  // the wrapped interfaces take over the references
  WrappedD3D9 *wrappedD3D = new WrappedD3D9(d3d);
  WrappedD3DDevice9 *wrapped = new WrappedD3DDevice9(dev, wrappedD3D, wnd, initParams);
  wrappedD3D->Release();

  D3D9Replay *replay = new D3D9Replay(wrapped, d3d, wnd);
  wrapped->SetReplay(replay, wnd, ver);

  *driver = replay;
  return ResultCode::Succeeded;
}

static DriverRegistration D3D9DriverRegistration(RDCDriver::D3D9, &D3D9_CreateReplayDevice);

// Structured export (e.g. renderdoccmd convert, or the python API's GetStructuredData) only needs
// to read the capture
RDResult D3D9_ProcessStructured(RDCFile *rdc, SDFile &output)
{
  int sectionIdx = rdc->SectionIndex(SectionType::FrameCapture);

  if(sectionIdx < 0)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "File does not contain captured API data");

  uint64_t version = rdc->GetSectionProperties(sectionIdx).version;

  if(!D3D9InitParams::IsSupportedVersion(version))
    RETURN_ERROR_RESULT(ResultCode::APIIncompatibleVersion,
                        "D3D9 capture is incompatible version %llu, newest supported by this "
                        "build of RenderDoc is %llu",
                        version, D3D9InitParams::CurrentVersion);

  WrappedD3DDevice9 device((D3D9InitParams()));

  device.SetStructuredExport(version);
  RDResult result = device.ReadLogInitialisation(rdc, true);

  if(result == ResultCode::Succeeded)
    device.GetStructuredFile()->Swap(output);

  return result;
}

static StructuredProcessRegistration D3D9ProcessRegistration(RDCDriver::D3D9,
                                                             &D3D9_ProcessStructured);
