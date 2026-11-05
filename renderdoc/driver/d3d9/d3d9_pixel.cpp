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

#include <math.h>
#include "compressonator/CMP_Core.h"
#include "maths/half_convert.h"
#include "d3d9_contents.h"
#include "d3d9_device.h"
#include "d3d9_replay.h"
#include "d3d9_resources.h"

// Pixel queries
// -------------
// PickPixel, GetMinMax and GetHistogram work on the CPU, over a subresource decoded to floats
// (GetDecoded). The contents come from ReadSubresource, which reads depth stencils that can't be
// locked with the depth tests and resolves multisampled render targets, so this works for
// everything D3D9 can read. Textures it can't read (D3DPOOL_DEFAULT ones replay couldn't make
// dynamic) are drawn through the display shader into a float render target instead.
//
// Texels are decoded to the channels the format is described with (D3D9MakeResourceFormat), which
// is how D3D9 samples them where that's different: luminance is red, A8L8's alpha is green,
// L6V5U5 and X8L8V8U8 are (U, V, L, 1). Depth is red and stencil green, as stencil / 255 like the
// other APIs. YUV formats are (Cr, Y, Cb, 1), which the display decodes to RGB when asked.
//
// The decoded data is cached per subresource until the next replay.

namespace
{
float UNorm(uint32_t v, uint32_t bits)
{
  return float(double(v) / double((uint64_t(1) << bits) - 1));
}

float SNorm(uint32_t v, uint32_t bits)
{
  // sign extend
  const int32_t s = int32_t(v << (32 - bits)) >> (32 - bits);
  const float maxVal = float((1 << (bits - 1)) - 1);
  return RDCMAX(float(s) / maxVal, -1.0f);
}

uint16_t Read16(const byte *p)
{
  uint16_t ret;
  memcpy(&ret, p, sizeof(ret));
  return ret;
}

uint32_t Read32(const byte *p)
{
  uint32_t ret;
  memcpy(&ret, p, sizeof(ret));
  return ret;
}

float ReadFloat(const byte *p)
{
  float ret;
  memcpy(&ret, p, sizeof(ret));
  return ret;
}

float Half(const byte *p)
{
  return ConvertFromHalf(Read16(p));
}

// one texel of a format with whole bytes per pixel
bool DecodeTexel(D3DFORMAT format, const byte *p, FloatVector &out)
{
  out = FloatVector(0.0f, 0.0f, 0.0f, 1.0f);

  switch((DWORD)format)
  {
    // 8 bits
    case D3DFMT_L8: out.x = UNorm(p[0], 8); return true;
    case D3DFMT_A8: out.w = UNorm(p[0], 8); return true;
    case D3DFMT_P8: out.x = float(p[0]); return true;
    case D3DFMT_A4L4:
      out.x = UNorm(p[0] & 0xF, 4);
      out.y = UNorm(p[0] >> 4, 4);
      return true;
    case D3DFMT_R3G3B2:
      out.x = UNorm((p[0] >> 5) & 0x7, 3);
      out.y = UNorm((p[0] >> 2) & 0x7, 3);
      out.z = UNorm(p[0] & 0x3, 2);
      return true;

    // 16 bits
    case D3DFMT_R5G6B5:
    {
      const uint16_t v = Read16(p);
      out.x = UNorm((v >> 11) & 0x1F, 5);
      out.y = UNorm((v >> 5) & 0x3F, 6);
      out.z = UNorm(v & 0x1F, 5);
      return true;
    }
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    {
      const uint16_t v = Read16(p);
      out.x = UNorm((v >> 10) & 0x1F, 5);
      out.y = UNorm((v >> 5) & 0x1F, 5);
      out.z = UNorm(v & 0x1F, 5);
      if(format == D3DFMT_A1R5G5B5)
        out.w = float(v >> 15);
      return true;
    }
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A4R4G4B4:
    {
      const uint16_t v = Read16(p);
      out.x = UNorm((v >> 8) & 0xF, 4);
      out.y = UNorm((v >> 4) & 0xF, 4);
      out.z = UNorm(v & 0xF, 4);
      if(format == D3DFMT_A4R4G4B4)
        out.w = UNorm(v >> 12, 4);
      return true;
    }
    case D3DFMT_A8R3G3B2:
      out.x = UNorm((p[0] >> 5) & 0x7, 3);
      out.y = UNorm((p[0] >> 2) & 0x7, 3);
      out.z = UNorm(p[0] & 0x3, 2);
      out.w = UNorm(p[1], 8);
      return true;
    case D3DFMT_A8P8:
      out.x = float(p[0]);
      out.y = float(p[1]);
      return true;
    case D3DFMT_A8L8:
      out.x = UNorm(p[0], 8);
      out.y = UNorm(p[1], 8);
      return true;
    case D3DFMT_V8U8:
      out.x = SNorm(p[0], 8);
      out.y = SNorm(p[1], 8);
      return true;
    case D3DFMT_CxV8U8:
      out.x = SNorm(p[0], 8);
      out.y = SNorm(p[1], 8);
      out.z = sqrtf(RDCMAX(0.0f, 1.0f - out.x * out.x - out.y * out.y));
      return true;
    case D3DFMT_L6V5U5:
    {
      const uint16_t v = Read16(p);
      out.x = SNorm(v & 0x1F, 5);
      out.y = SNorm((v >> 5) & 0x1F, 5);
      out.z = UNorm(v >> 10, 6);
      return true;
    }
    case D3DFMT_L16: out.x = UNorm(Read16(p), 16); return true;
    case D3DFMT_R16F: out.x = Half(p); return true;

    // 24 bits
    case D3DFMT_R8G8B8:
      out.x = UNorm(p[2], 8);
      out.y = UNorm(p[1], 8);
      out.z = UNorm(p[0], 8);
      return true;

    // 32 bits
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
      out.x = UNorm(p[2], 8);
      out.y = UNorm(p[1], 8);
      out.z = UNorm(p[0], 8);
      if(format == D3DFMT_A8R8G8B8)
        out.w = UNorm(p[3], 8);
      return true;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
      out.x = UNorm(p[0], 8);
      out.y = UNorm(p[1], 8);
      out.z = UNorm(p[2], 8);
      if(format == D3DFMT_A8B8G8R8)
        out.w = UNorm(p[3], 8);
      return true;
    case D3DFMT_A2B10G10R10:
    {
      const uint32_t v = Read32(p);
      out.x = UNorm(v & 0x3FF, 10);
      out.y = UNorm((v >> 10) & 0x3FF, 10);
      out.z = UNorm((v >> 20) & 0x3FF, 10);
      out.w = UNorm(v >> 30, 2);
      return true;
    }
    case D3DFMT_A2R10G10B10:
    {
      const uint32_t v = Read32(p);
      out.z = UNorm(v & 0x3FF, 10);
      out.y = UNorm((v >> 10) & 0x3FF, 10);
      out.x = UNorm((v >> 20) & 0x3FF, 10);
      out.w = UNorm(v >> 30, 2);
      return true;
    }
    case D3DFMT_A2W10V10U10:
    {
      const uint32_t v = Read32(p);
      out.x = SNorm(v & 0x3FF, 10);
      out.y = SNorm((v >> 10) & 0x3FF, 10);
      out.z = SNorm((v >> 20) & 0x3FF, 10);
      out.w = UNorm(v >> 30, 2);
      return true;
    }
    case D3DFMT_G16R16:
      out.x = UNorm(Read16(p), 16);
      out.y = UNorm(Read16(p + 2), 16);
      return true;
    case D3DFMT_V16U16:
      out.x = SNorm(Read16(p), 16);
      out.y = SNorm(Read16(p + 2), 16);
      return true;
    case D3DFMT_Q8W8V8U8:
      out.x = SNorm(p[0], 8);
      out.y = SNorm(p[1], 8);
      out.z = SNorm(p[2], 8);
      out.w = SNorm(p[3], 8);
      return true;
    case D3DFMT_X8L8V8U8:
      out.x = SNorm(p[0], 8);
      out.y = SNorm(p[1], 8);
      out.z = UNorm(p[2], 8);
      return true;
    case D3DFMT_G16R16F:
      out.x = Half(p);
      out.y = Half(p + 2);
      return true;
    case D3DFMT_R32F: out.x = ReadFloat(p); return true;

    // 64 bits
    case D3DFMT_A16B16G16R16:
      out.x = UNorm(Read16(p), 16);
      out.y = UNorm(Read16(p + 2), 16);
      out.z = UNorm(Read16(p + 4), 16);
      out.w = UNorm(Read16(p + 6), 16);
      return true;
    case D3DFMT_Q16W16V16U16:
      out.x = SNorm(Read16(p), 16);
      out.y = SNorm(Read16(p + 2), 16);
      out.z = SNorm(Read16(p + 4), 16);
      out.w = SNorm(Read16(p + 6), 16);
      return true;
    case D3DFMT_A16B16G16R16F:
      out.x = Half(p);
      out.y = Half(p + 2);
      out.z = Half(p + 4);
      out.w = Half(p + 6);
      return true;
    case D3DFMT_G32R32F:
      out.x = ReadFloat(p);
      out.y = ReadFloat(p + 4);
      return true;

    // 128 bits
    case D3DFMT_A32B32G32R32F:
      out.x = ReadFloat(p);
      out.y = ReadFloat(p + 4);
      out.z = ReadFloat(p + 8);
      out.w = ReadFloat(p + 12);
      return true;

    // depth stencils, as they're locked or read back (d3d9_depth.cpp): depth in the high bits
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case MAKEFOURCC('D', 'F', '1', '6'): out.x = UNorm(Read16(p), 16); return true;
    case D3DFMT_D15S1:
    {
      const uint16_t v = Read16(p);
      out.x = UNorm(v >> 1, 15);
      out.y = float(v & 0x1) / 255.0f;
      return true;
    }
    case D3DFMT_D24S8:
    case D3DFMT_D24X4S4:
    case MAKEFOURCC('I', 'N', 'T', 'Z'):
    {
      const uint32_t v = Read32(p);
      out.x = UNorm(v >> 8, 24);
      out.y = float(v & (format == D3DFMT_D24X4S4 ? 0xF : 0xFF)) / 255.0f;
      return true;
    }
    // D32 is read back to 24 bits
    case D3DFMT_D24X8:
    case D3DFMT_D32:
    case MAKEFOURCC('D', 'F', '2', '4'): out.x = UNorm(Read32(p) >> 8, 24); return true;
    case D3DFMT_D32_LOCKABLE: out.x = UNorm(Read32(p), 32); return true;
    case D3DFMT_D32F_LOCKABLE: out.x = ReadFloat(p); return true;

    default: return false;
  }
}

// formats whose blocks hold two pixels sharing their chroma: (pixel 0, pixel 1)
bool DecodePair(D3DFORMAT format, const byte *p, FloatVector &a, FloatVector &b)
{
  switch((DWORD)format)
  {
    // Y0 U Y1 V, shown as (Cr, Y, Cb)
    case D3DFMT_YUY2:
      a = FloatVector(UNorm(p[3], 8), UNorm(p[0], 8), UNorm(p[1], 8), 1.0f);
      b = FloatVector(UNorm(p[3], 8), UNorm(p[2], 8), UNorm(p[1], 8), 1.0f);
      return true;
    // U Y0 V Y1
    case D3DFMT_UYVY:
      a = FloatVector(UNorm(p[2], 8), UNorm(p[1], 8), UNorm(p[0], 8), 1.0f);
      b = FloatVector(UNorm(p[2], 8), UNorm(p[3], 8), UNorm(p[0], 8), 1.0f);
      return true;
    // D3D9 names list the most significant byte first, so R8G8_B8G8 is G0 R G1 B in memory
    case D3DFMT_R8G8_B8G8:
      a = FloatVector(UNorm(p[1], 8), UNorm(p[0], 8), UNorm(p[3], 8), 1.0f);
      b = FloatVector(UNorm(p[1], 8), UNorm(p[2], 8), UNorm(p[3], 8), 1.0f);
      return true;
    // R G0 B G1
    case D3DFMT_G8R8_G8B8:
      a = FloatVector(UNorm(p[0], 8), UNorm(p[1], 8), UNorm(p[2], 8), 1.0f);
      b = FloatVector(UNorm(p[0], 8), UNorm(p[3], 8), UNorm(p[2], 8), 1.0f);
      return true;
    default: return false;
  }
}

// 4x4 blocks: fills 16 texels, row by row
bool DecodeBlock(D3DFORMAT format, const byte *p, FloatVector texels[16])
{
  byte rgba[64] = {};
  switch((DWORD)format)
  {
    case D3DFMT_DXT1: DecompressBlockBC1(p, rgba, NULL); break;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3: DecompressBlockBC2(p, rgba, NULL); break;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5: DecompressBlockBC3(p, rgba, NULL); break;
    case MAKEFOURCC('A', 'T', 'I', '1'):
    {
      byte r[16] = {};
      DecompressBlockBC4(p, r, NULL);
      for(int i = 0; i < 16; i++)
        texels[i] = FloatVector(UNorm(r[i], 8), 0.0f, 0.0f, 1.0f);
      return true;
    }
    case MAKEFOURCC('A', 'T', 'I', '2'):
    {
      byte r[16] = {}, g[16] = {};
      DecompressBlockBC5(p, r, g, NULL);
      for(int i = 0; i < 16; i++)
        texels[i] = FloatVector(UNorm(r[i], 8), UNorm(g[i], 8), 0.0f, 1.0f);
      return true;
    }
    default: return false;
  }

  for(int i = 0; i < 16; i++)
    texels[i] = FloatVector(UNorm(rgba[i * 4 + 0], 8), UNorm(rgba[i * 4 + 1], 8),
                            UNorm(rgba[i * 4 + 2], 8), UNorm(rgba[i * 4 + 3], 8));
  return true;
}

// decodes one slice of a subresource as ReadSubresource returned it
rdcstr DecodeSubresource(D3DFORMAT format, const D3D9SubresourceData &data, uint32_t slice,
                         rdcarray<FloatVector> &texels)
{
  const D3D9FormatBlock block = D3D9GetFormatBlock(format);
  if(block.Bytes == 0)
    return StringFormat::Fmt("%s can't be decoded", ToStr(format).c_str());

  const uint32_t width = data.Width, height = data.Height;
  const size_t slicePitch = size_t(data.RowPitch) * data.RowCount;

  const uint32_t blocksX = (width + block.Width - 1) / block.Width;
  const uint32_t blocksY = (height + block.Height - 1) / block.Height;

  if(data.RowCount < blocksY || data.RowPitch < blocksX * block.Bytes ||
     data.Data.size() < slicePitch * (slice + 1))
    return "The contents read back are smaller than the subresource";

  texels.resize(size_t(width) * height);

  const byte *base = data.Data.data() + slicePitch * slice;

  if(block.Width == 1 && block.Height == 1)
  {
    for(uint32_t y = 0; y < height; y++)
    {
      const byte *row = base + size_t(y) * data.RowPitch;
      for(uint32_t x = 0; x < width; x++)
        if(!DecodeTexel(format, row + x * block.Bytes, texels[y * width + x]))
          return StringFormat::Fmt("%s can't be decoded", ToStr(format).c_str());
    }
    return rdcstr();
  }

  if(block.Width == 2 && block.Height == 1)
  {
    for(uint32_t y = 0; y < height; y++)
    {
      const byte *row = base + size_t(y) * data.RowPitch;
      for(uint32_t bx = 0; bx < blocksX; bx++)
      {
        FloatVector a, b;
        if(!DecodePair(format, row + bx * block.Bytes, a, b))
          return StringFormat::Fmt("%s can't be decoded", ToStr(format).c_str());
        texels[y * width + bx * 2] = a;
        if(bx * 2 + 1 < width)
          texels[y * width + bx * 2 + 1] = b;
      }
    }
    return rdcstr();
  }

  if(block.Width == 4 && block.Height == 4)
  {
    FloatVector decoded[16];
    for(uint32_t by = 0; by < blocksY; by++)
    {
      const byte *row = base + size_t(by) * data.RowPitch;
      for(uint32_t bx = 0; bx < blocksX; bx++)
      {
        if(!DecodeBlock(format, row + bx * block.Bytes, decoded))
          return StringFormat::Fmt("%s can't be decoded", ToStr(format).c_str());

        for(uint32_t py = 0; py < 4 && by * 4 + py < height; py++)
          for(uint32_t px = 0; px < 4 && bx * 4 + px < width; px++)
            texels[(by * 4 + py) * width + bx * 4 + px] = decoded[py * 4 + px];
      }
    }
    return rdcstr();
  }

  return StringFormat::Fmt("%s can't be decoded", ToStr(format).c_str());
}
};

D3D9Replay::DecodedSubresource &D3D9Replay::GetDecoded(ResourceId id, const Subresource &sub)
{
  DecodedKey key;
  key.id = id;
  key.mip = sub.mip;
  key.slice = sub.slice;
  // ~0U asks for the samples resolved: render targets are resolved when they're read, depth
  // stencils show their first sample
  key.sample = sub.sample == ~0U ? 0 : sub.sample;

  auto it = m_Decoded.find(key);
  if(it != m_Decoded.end())
    return it->second;

  DecodedSubresource &ret = m_Decoded[key];

  D3D9ResourceManager *rm = m_pDevice->GetResourceManager();
  WrappedD3D9Object *obj = rm->HasResource(id) ? GetWrappedObject(rm->GetResource(id)) : NULL;
  if(obj == NULL)
  {
    ret.error = "Unknown texture";
    return ret;
  }

  D3DFORMAT format = D3DFMT_UNKNOWN;
  uint32_t face = 0;

  switch(obj->GetResourceType())
  {
    case Resource_Texture:
    {
      D3DSURFACE_DESC desc = {};
      ((WrappedD3DTexture9 *)obj)->GetReal()->GetLevelDesc(0, &desc);
      format = desc.Format;
      break;
    }
    case Resource_CubeTexture:
    {
      D3DSURFACE_DESC desc = {};
      ((WrappedD3DCubeTexture9 *)obj)->GetReal()->GetLevelDesc(0, &desc);
      format = desc.Format;
      face = RDCMIN(sub.slice, 5U);
      break;
    }
    case Resource_VolumeTexture:
    {
      D3DVOLUME_DESC desc = {};
      ((WrappedD3DVolumeTexture9 *)obj)->GetReal()->GetLevelDesc(0, &desc);
      format = desc.Format;
      break;
    }
    case Resource_Surface:
    {
      D3DSURFACE_DESC desc = {};
      ((WrappedD3DSurface9 *)obj)->GetReal()->GetDesc(&desc);
      format = desc.Format;
      break;
    }
    default: ret.error = "Not a texture"; return ret;
  }

  D3D9SubresourceData data;
  rdcstr err = m_pDevice->ReadSubresource(obj, face, sub.mip, data);

  if(err.empty())
  {
    // volumes have a slice per depth, multisampled depth stencils one per sample
    uint32_t slice = 0;
    if(obj->GetResourceType() == Resource_VolumeTexture)
      slice = RDCMIN(sub.slice, RDCMAX(1U, data.Depth) - 1);
    else if(data.Depth > 1)
      slice = RDCMIN(key.sample, data.Depth - 1);

    err = DecodeSubresource(format, data, slice, ret.texels);
    if(err.empty())
    {
      ret.width = data.Width;
      ret.height = data.Height;
      return ret;
    }
    ret.texels.clear();
  }

  // what can't be read on the CPU may still be sampled
  if(ReadDecodedFromGPU(id, sub, ret))
  {
    ret.error.clear();
    return ret;
  }

  ret.width = ret.height = 0;
  ret.texels.clear();
  ret.error = err;
  RDCWARN("Couldn't decode %s mip %u slice %u: %s", ToStr(id).c_str(), sub.mip, sub.slice,
          err.c_str());
  return ret;
}

void D3D9Replay::PickPixel(ResourceId texture, uint32_t x, uint32_t y, const Subresource &sub,
                           CompType typeCast, float pixel[4])
{
  pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0.0f;

  const DecodedSubresource &decoded = GetDecoded(texture, sub);
  if(decoded.texels.empty())
    return;

  x = RDCMIN(x, decoded.width - 1);
  y = RDCMIN(y, decoded.height - 1);

  const FloatVector &v = decoded.texels[size_t(y) * decoded.width + x];
  pixel[0] = v.x;
  pixel[1] = v.y;
  pixel[2] = v.z;
  pixel[3] = v.w;
}

bool D3D9Replay::GetMinMax(ResourceId texid, const Subresource &sub, CompType typeCast,
                           float *minval, float *maxval)
{
  for(int c = 0; c < 4; c++)
  {
    minval[c] = 0.0f;
    maxval[c] = 1.0f;
  }

  const DecodedSubresource &decoded = GetDecoded(texid, sub);
  if(decoded.texels.empty())
    return false;

  float mins[4] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
  float maxs[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};

  for(const FloatVector &v : decoded.texels)
  {
    const float comps[4] = {v.x, v.y, v.z, v.w};
    for(int c = 0; c < 4; c++)
    {
      // NaNs don't count, like the GPU's min and max
      if(comps[c] != comps[c])
        continue;
      mins[c] = RDCMIN(mins[c], comps[c]);
      maxs[c] = RDCMAX(maxs[c], comps[c]);
    }
  }

  for(int c = 0; c < 4; c++)
  {
    if(mins[c] <= maxs[c])
    {
      minval[c] = mins[c];
      maxval[c] = maxs[c];
    }
  }

  return true;
}

bool D3D9Replay::GetHistogram(ResourceId texid, const Subresource &sub, CompType typeCast,
                              float minval, float maxval, const rdcfixedarray<bool, 4> &channels,
                              rdcarray<uint32_t> &histogram)
{
  const uint32_t numBuckets = 256;
  histogram.fill(numBuckets, 0);

  if(minval >= maxval)
    return false;

  const DecodedSubresource &decoded = GetDecoded(texid, sub);
  if(decoded.texels.empty())
    return false;

  // like the other APIs, values equal to the maximum go in the last bucket
  maxval += maxval * 1e-6f;
  const float invRange = 1.0f / (maxval - minval);

  for(const FloatVector &v : decoded.texels)
  {
    const float comps[4] = {v.x, v.y, v.z, v.w};
    for(int c = 0; c < 4; c++)
    {
      if(!channels[c])
        continue;

      const float n = (comps[c] - minval) * invRange;
      // also skips NaNs
      if(!(n >= 0.0f))
        continue;

      const float bucket = floorf(n * float(numBuckets));
      if(bucket < float(numBuckets))
        histogram[(size_t)bucket]++;
    }
  }

  return true;
}
