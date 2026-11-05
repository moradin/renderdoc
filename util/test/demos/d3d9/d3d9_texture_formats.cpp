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

#include <stdarg.h>
#include <algorithm>
#include "d3d9_test.h"

RD_TEST(D3D9_Texture_Formats, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Creates a 16x16 texture in every format the GPU can sample, filled with pseudo-random "
      "data, and draws each one 1:1 with point sampling into an A32B32G32R32F render target, "
      "inside a marker named after the format and where it's drawn. The first frame's render "
      "target, which is what the GPU samples from each texture, is written next to the "
      "executable as D3D9_Texture_Formats_reference.txt (_reference_rdoc.txt under RenderDoc) "
      "for util/test/d3d9_formats_check.py to compare with RenderDoc's decoding.";

  enum class Fill
  {
    Bytes,
    Half,
    Float,
  };

  struct FormatInfo
  {
    D3DFORMAT fmt;
    const char *name;
    // bytes per block of blockW x blockH texels
    UINT blockBytes;
    UINT blockW, blockH;
    Fill fill;
  };

  struct Vert
  {
    float x, y, z, rhw;
    float u, v;
  };

  struct Entry
  {
    const FormatInfo *info;
    IDirect3DTexture9Ptr tex;
    UINT x, y;
    std::string marker;
  };

  const char *SamplePixel = R"EOSHADER(

sampler2D tex : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
  return tex2D(tex, uv);
}

)EOSHADER";

  static const UINT texSize = 16, cell = 20, cols = 12;

  uint32_t rngState = 0x12345678;

  uint32_t Random()
  {
    // xorshift32
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
  }

  // random data that's valid for the format: any bytes, or halfs and floats with magnitudes from
  // 1/32 to 2 (no infinities, NaNs or denormals)
  void FillRow(byte * dst, UINT bytes, Fill fill)
  {
    if(fill == Fill::Bytes)
    {
      for(UINT i = 0; i < bytes; i++)
        dst[i] = byte(Random() & 0xff);
    }
    else if(fill == Fill::Half)
    {
      for(UINT i = 0; i + 2 <= bytes; i += 2)
      {
        const uint32_t r = Random();
        const uint16_t h = uint16_t(((r >> 31) << 15) | ((10 + (r >> 10) % 6) << 10) | (r & 0x3ff));
        memcpy(dst + i, &h, sizeof(h));
      }
    }
    else
    {
      for(UINT i = 0; i + 4 <= bytes; i += 4)
      {
        const uint32_t r = Random();
        const uint32_t f = ((r >> 31) << 31) | ((122 + (r >> 23) % 6) << 23) | (r & 0x7fffff);
        memcpy(dst + i, &f, sizeof(f));
      }
    }
  }

  std::string Format(const char *f, ...)
  {
    char buf[256] = {};
    va_list args;
    va_start(args, f);
    vsnprintf(buf, sizeof(buf) - 1, f, args);
    va_end(args);
    return buf;
  }

  std::string ReferencePath()
  {
    std::string path =
        rdoc ? "D3D9_Texture_Formats_reference_rdoc.txt" : "D3D9_Texture_Formats_reference.txt";
    char exe[MAX_PATH] = {};
    if(GetModuleFileNameA(NULL, exe, MAX_PATH))
    {
      std::string e = exe;
      size_t slash = e.find_last_of("\\/");
      if(slash != std::string::npos)
        path = e.substr(0, slash + 1) + path;
    }
    return path;
  }

  // the render target's region for each format, as the GPU sampled it
  void WriteReference(IDirect3DSurface9 * rtSurf, IDirect3DSurface9 * readback,
                      const std::vector<Entry> &entries)
  {
    if(FAILED(dev->GetRenderTargetData(rtSurf, readback)))
    {
      TEST_ERROR("GetRenderTargetData failed, no reference written");
      return;
    }

    D3DLOCKED_RECT lr = {};
    if(FAILED(readback->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
      TEST_ERROR("Couldn't lock the render target's copy, no reference written");
      return;
    }

    const std::string path = ReferencePath();
    FILE *f = NULL;
    fopen_s(&f, path.c_str(), "w");
    if(f)
    {
      for(const Entry &e : entries)
      {
        fprintf(f, "format %s %u %u %u %u\n", e.info->name, e.x, e.y, texSize, texSize);
        for(UINT y = 0; y < texSize; y++)
        {
          const float *row = (const float *)((const byte *)lr.pBits + (e.y + y) * lr.Pitch) + e.x * 4;
          for(UINT i = 0; i < texSize * 4; i++)
            fprintf(f, i == 0 ? "%.9g" : " %.9g", row[i]);
          fprintf(f, "\n");
        }
      }
      fclose(f);
      TEST_LOG("Wrote the reference to %s", path.c_str());
    }
    else
    {
      TEST_ERROR("Couldn't open %s", path.c_str());
    }

    readback->UnlockRect();
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    const D3DFORMAT ATI1 = (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1');
    const D3DFORMAT ATI2 = (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2');

    // the names are what util/test/d3d9_formats_check.py looks up
    const FormatInfo formats[] = {
        {D3DFMT_R8G8B8, "R8G8B8", 3, 1, 1, Fill::Bytes},
        {D3DFMT_A8R8G8B8, "A8R8G8B8", 4, 1, 1, Fill::Bytes},
        {D3DFMT_X8R8G8B8, "X8R8G8B8", 4, 1, 1, Fill::Bytes},
        {D3DFMT_R5G6B5, "R5G6B5", 2, 1, 1, Fill::Bytes},
        {D3DFMT_X1R5G5B5, "X1R5G5B5", 2, 1, 1, Fill::Bytes},
        {D3DFMT_A1R5G5B5, "A1R5G5B5", 2, 1, 1, Fill::Bytes},
        {D3DFMT_A4R4G4B4, "A4R4G4B4", 2, 1, 1, Fill::Bytes},
        {D3DFMT_R3G3B2, "R3G3B2", 1, 1, 1, Fill::Bytes},
        {D3DFMT_A8, "A8", 1, 1, 1, Fill::Bytes},
        {D3DFMT_A8R3G3B2, "A8R3G3B2", 2, 1, 1, Fill::Bytes},
        {D3DFMT_X4R4G4B4, "X4R4G4B4", 2, 1, 1, Fill::Bytes},
        {D3DFMT_A2B10G10R10, "A2B10G10R10", 4, 1, 1, Fill::Bytes},
        {D3DFMT_A8B8G8R8, "A8B8G8R8", 4, 1, 1, Fill::Bytes},
        {D3DFMT_X8B8G8R8, "X8B8G8R8", 4, 1, 1, Fill::Bytes},
        {D3DFMT_G16R16, "G16R16", 4, 1, 1, Fill::Bytes},
        {D3DFMT_A2R10G10B10, "A2R10G10B10", 4, 1, 1, Fill::Bytes},
        {D3DFMT_A16B16G16R16, "A16B16G16R16", 8, 1, 1, Fill::Bytes},
        {D3DFMT_A8P8, "A8P8", 2, 1, 1, Fill::Bytes},
        {D3DFMT_P8, "P8", 1, 1, 1, Fill::Bytes},
        {D3DFMT_L8, "L8", 1, 1, 1, Fill::Bytes},
        {D3DFMT_A8L8, "A8L8", 2, 1, 1, Fill::Bytes},
        {D3DFMT_A4L4, "A4L4", 1, 1, 1, Fill::Bytes},
        {D3DFMT_V8U8, "V8U8", 2, 1, 1, Fill::Bytes},
        {D3DFMT_L6V5U5, "L6V5U5", 2, 1, 1, Fill::Bytes},
        {D3DFMT_X8L8V8U8, "X8L8V8U8", 4, 1, 1, Fill::Bytes},
        {D3DFMT_Q8W8V8U8, "Q8W8V8U8", 4, 1, 1, Fill::Bytes},
        {D3DFMT_V16U16, "V16U16", 4, 1, 1, Fill::Bytes},
        {D3DFMT_A2W10V10U10, "A2W10V10U10", 4, 1, 1, Fill::Bytes},
        {D3DFMT_CxV8U8, "CxV8U8", 2, 1, 1, Fill::Bytes},
        {D3DFMT_L16, "L16", 2, 1, 1, Fill::Bytes},
        {D3DFMT_Q16W16V16U16, "Q16W16V16U16", 8, 1, 1, Fill::Bytes},
        {D3DFMT_UYVY, "UYVY", 4, 2, 1, Fill::Bytes},
        {D3DFMT_YUY2, "YUY2", 4, 2, 1, Fill::Bytes},
        {D3DFMT_R8G8_B8G8, "R8G8_B8G8", 4, 2, 1, Fill::Bytes},
        {D3DFMT_G8R8_G8B8, "G8R8_G8B8", 4, 2, 1, Fill::Bytes},
        {D3DFMT_DXT1, "DXT1", 8, 4, 4, Fill::Bytes},
        {D3DFMT_DXT2, "DXT2", 16, 4, 4, Fill::Bytes},
        {D3DFMT_DXT3, "DXT3", 16, 4, 4, Fill::Bytes},
        {D3DFMT_DXT4, "DXT4", 16, 4, 4, Fill::Bytes},
        {D3DFMT_DXT5, "DXT5", 16, 4, 4, Fill::Bytes},
        {ATI1, "ATI1", 8, 4, 4, Fill::Bytes},
        {ATI2, "ATI2", 16, 4, 4, Fill::Bytes},
        {D3DFMT_R16F, "R16F", 2, 1, 1, Fill::Half},
        {D3DFMT_G16R16F, "G16R16F", 4, 1, 1, Fill::Half},
        {D3DFMT_A16B16G16R16F, "A16B16G16R16F", 8, 1, 1, Fill::Half},
        {D3DFMT_R32F, "R32F", 4, 1, 1, Fill::Float},
        {D3DFMT_G32R32F, "G32R32F", 8, 1, 1, Fill::Float},
        {D3DFMT_A32B32G32R32F, "A32B32G32R32F", 16, 1, 1, Fill::Float},
    };

    std::vector<Entry> entries;
    std::vector<std::string> unsupported;

    for(const FormatInfo &info : formats)
    {
      HRESULT hr = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                          D3DRTYPE_TEXTURE, info.fmt);
      if(FAILED(hr))
      {
        unsupported.push_back(std::string(info.name) + " (CheckDeviceFormat)");
        continue;
      }

      Entry e;
      e.info = &info;
      hr = dev->CreateTexture(texSize, texSize, 1, 0, info.fmt, D3DPOOL_MANAGED, &e.tex, NULL);
      if(FAILED(hr))
      {
        unsupported.push_back(Format("%s (CreateTexture %x)", info.name, hr));
        continue;
      }

      D3DLOCKED_RECT lr = {};
      hr = e.tex->LockRect(0, &lr, NULL, 0);
      if(FAILED(hr))
      {
        unsupported.push_back(Format("%s (LockRect %x)", info.name, hr));
        continue;
      }

      // Drivers report ATI1 and ATI2's Pitch as one byte per pixel column, less than a row of
      // their blocks, but the GPU reads the blocks tightly packed
      const UINT rowBytes = (texSize / info.blockW) * info.blockBytes;
      UINT pitch = (UINT)lr.Pitch;
      if(info.fmt == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1') ||
         info.fmt == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2'))
        pitch = std::max(pitch, rowBytes);

      // the same data on every run, for each format in the list whichever are supported
      rngState = 0x12345678u + uint32_t(&info - formats) * 0x9E3779B9u;
      for(UINT row = 0; row < texSize / info.blockH; row++)
        FillRow((byte *)lr.pBits + row * pitch, rowBytes, info.fill);

      e.tex->UnlockRect(0);

      const UINT slot = (UINT)entries.size();
      e.x = (slot % cols) * cell + 2;
      e.y = (slot / cols) * cell + 2;
      e.marker = Format("%s at %u,%u", info.name, e.x, e.y);
      entries.push_back(e);
    }

    for(const std::string &u : unsupported)
      TEST_LOG("Not supported: %s", u.c_str());
    TEST_LOG("%u formats supported, %u not", (uint32_t)entries.size(), (uint32_t)unsupported.size());

    IDirect3DPixelShader9Ptr ps = CreatePS(Compile(SamplePixel, "main", "ps_2_0"));

    const UINT rtWidth = cols * cell;
    const UINT rtHeight = std::max(1U, UINT(entries.size() + cols - 1) / cols) * cell;

    IDirect3DTexture9Ptr rt;
    IDirect3DSurface9Ptr rtSurf, readback, backBuffer;
    if(FAILED(dev->CreateTexture(rtWidth, rtHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F,
                                 D3DPOOL_DEFAULT, &rt, NULL)) ||
       FAILED(dev->CreateOffscreenPlainSurface(rtWidth, rtHeight, D3DFMT_A32B32G32R32F,
                                               D3DPOOL_SYSTEMMEM, &readback, NULL)))
    {
      TEST_ERROR("Couldn't create the A32B32G32R32F render target");
      return 4;
    }
    rt->GetSurfaceLevel(0, &rtSurf);
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);

    // the render target shown on the back buffer, scaled up as far as it fits
    const float scale = std::max(
        1.0f, floorf(std::min(float(screenWidth) / rtWidth, float(screenHeight) / rtHeight)));

    bool wroteReference = false;

    while(Running())
    {
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      dev->BeginScene();

      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

      dev->SetVertexShader(NULL);
      dev->SetPixelShader(ps);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

      for(const std::string &u : unsupported)
        setMarker("Not supported: " + u);

      dev->SetRenderTarget(0, rtSurf);
      dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);

      // each texel to one pixel: D3D9's pixel centres are on integer coordinates, so the quad is
      // half a pixel up and left
      for(const Entry &e : entries)
      {
        pushMarker(e.marker);
        const float x0 = float(e.x) - 0.5f, y0 = float(e.y) - 0.5f;
        const float x1 = x0 + texSize, y1 = y0 + texSize;
        const Vert quad[4] = {
            {x0, y0, 0.5f, 1.0f, 0.0f, 0.0f},
            {x1, y0, 0.5f, 1.0f, 1.0f, 0.0f},
            {x0, y1, 0.5f, 1.0f, 0.0f, 1.0f},
            {x1, y1, 0.5f, 1.0f, 1.0f, 1.0f},
        };
        dev->SetTexture(0, e.tex);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vert));
        popMarker();
      }

      pushMarker("Show the render target");
      dev->SetRenderTarget(0, backBuffer);
      {
        const float x1 = rtWidth * scale - 0.5f, y1 = rtHeight * scale - 0.5f;
        const Vert quad[4] = {
            {-0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
            {x1, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f},
            {-0.5f, y1, 0.5f, 1.0f, 0.0f, 1.0f},
            {x1, y1, 0.5f, 1.0f, 1.0f, 1.0f},
        };
        dev->SetTexture(0, rt);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vert));
        dev->SetTexture(0, NULL);
      }
      popMarker();

      dev->EndScene();

      if(!wroteReference)
      {
        WriteReference(rtSurf, readback, entries);
        wroteReference = true;
      }

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
