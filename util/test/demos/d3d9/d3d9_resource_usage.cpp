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

RD_TEST(D3D9_Resource_Usage, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Uses resources in every way D3D9 can within a frame, and binds others that the draws don't "
      "use, for checking the resource usage RenderDoc records. Each resource has a size no other "
      "has (or, for the second render target, a format), so the test can find it. Before the first "
      "frame it checks natively whether a colour Clear with two render targets bound clears both, "
      "and writes what it found, and which optional features the GPU has, to "
      "D3D9_Resource_Usage_reference.txt in its tmp folder.";

  // samples a vertex texture when the GPU has vertex texture fetch
  const char *VertexVTF = R"EOSHADER(

sampler2D vtex : register(s0);
float4 offset : register(c0);

struct VOut
{
  float4 pos : POSITION;
  float4 col : COLOR0;
};

VOut main(float3 pos : POSITION, float4 col : COLOR0)
{
  VOut o;
  float h = tex2Dlod(vtex, float4(0.5f, 0.5f, 0.0f, 0.0f)).x;
  o.pos = float4(pos.xy + offset.xy + h * 0.0001f, 0.5f, 1.0f);
  o.col = col;
  return o;
}

)EOSHADER";

  const char *VertexPlain = R"EOSHADER(

float4 offset : register(c0);

struct VOut
{
  float4 pos : POSITION;
  float4 col : COLOR0;
};

VOut main(float3 pos : POSITION, float4 col : COLOR0)
{
  VOut o;
  o.pos = float4(pos.xy + offset.xy, 0.5f, 1.0f);
  o.col = col;
  return o;
}

)EOSHADER";

  // samples s0 only, though s1 has a texture too. Writes both render targets.
  const char *Pixel = R"EOSHADER(

sampler2D tex : register(s0);
float4 tint : register(c0);

struct POut
{
  float4 c0 : COLOR0;
  float4 c1 : COLOR1;
};

POut main(float4 col : COLOR0)
{
  POut o;
  o.c0 = tex2D(tex, float2(0.5f, 0.5f)) * col * tint;
  o.c1 = col;
  return o;
}

)EOSHADER";

  struct FFVertex
  {
    float x, y, z, rhw;
    D3DCOLOR col;
    float u, v;
  };

  IDirect3DTexture9Ptr MakeTexture(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, DWORD usage = 0,
                                   UINT levels = 1)
  {
    IDirect3DTexture9Ptr tex;
    if(FAILED(dev->CreateTexture(w, h, levels, usage, fmt, pool, &tex, NULL)))
    {
      TEST_ERROR("Couldn't create a %ux%u texture", w, h);
      return NULL;
    }
    if(pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM)
    {
      D3DLOCKED_RECT lr = {};
      if(SUCCEEDED(tex->LockRect(0, &lr, NULL, 0)))
      {
        // both formats used here are 4 bytes a texel
        for(UINT y = 0; y < h; y++)
          memset((byte *)lr.pBits + y * lr.Pitch, 0xC0, w * 4);
        tex->UnlockRect(0);
      }
    }
    return tex;
  }

  IDirect3DVertexBuffer9Ptr MakeBuffer(UINT size, const void *data, UINT dataSize)
  {
    IDirect3DVertexBuffer9Ptr vb;
    if(FAILED(dev->CreateVertexBuffer(size, 0, 0, D3DPOOL_MANAGED, &vb, NULL)))
    {
      TEST_ERROR("Couldn't create a %u byte vertex buffer", size);
      return NULL;
    }
    void *p = NULL;
    if(SUCCEEDED(vb->Lock(0, 0, &p, 0)))
    {
      memset(p, 0, size);
      memcpy(p, data, dataSize);
      vb->Unlock();
    }
    return vb;
  }

  // the first texel of a render target, or ~0U if it can't be read
  DWORD ReadFirstTexel(IDirect3DSurface9 * rt)
  {
    D3DSURFACE_DESC desc = {};
    rt->GetDesc(&desc);
    IDirect3DSurface9Ptr readback;
    if(FAILED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                               D3DPOOL_SYSTEMMEM, &readback, NULL)))
      return ~0U;

    DWORD ret = ~0U;
    D3DLOCKED_RECT lr = {};
    if(SUCCEEDED(dev->GetRenderTargetData(rt, readback)) &&
       SUCCEEDED(readback->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
      ret = *(const DWORD *)lr.pBits & 0xFFFFFF;
      readback->UnlockRect();
    }
    return ret;
  }

  // whether a colour Clear with render targets 0 and 1 bound clears target 1 as well: 1 if it
  // does, 0 if it doesn't, -1 if that couldn't be checked
  int ClearClearsSecondTarget(IDirect3DSurface9 * rt0, IDirect3DSurface9 * rt1)
  {
    IDirect3DSurface9Ptr prevRT, prevDS;
    dev->GetRenderTarget(0, &prevRT);
    dev->GetDepthStencilSurface(&prevDS);

    // make both green on their own, then clear with both bound to red
    dev->SetDepthStencilSurface(NULL);
    dev->SetRenderTarget(0, rt1);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0);
    dev->SetRenderTarget(0, rt0);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0);
    dev->SetRenderTarget(1, rt1);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0);
    dev->SetRenderTarget(1, NULL);
    dev->SetRenderTarget(0, prevRT);
    dev->SetDepthStencilSurface(prevDS);

    const DWORD first = ReadFirstTexel(rt0), second = ReadFirstTexel(rt1);
    TEST_LOG("After the clears render target 0 is 0x%06x and render target 1 0x%06x", first, second);
    if(first != 0xFF0000)
    {
      TEST_ERROR("Render target 0 wasn't cleared to red");
      return -1;
    }
    if(second == 0xFF0000)
      return 1;
    if(second == 0x00FF00)
      return 0;
    TEST_ERROR("Render target 1 is neither colour");
    return -1;
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    D3DCAPS9 caps = {};
    dev->GetDeviceCaps(&caps);
    if(caps.NumSimultaneousRTs < 2)
    {
      TEST_ERROR("Two simultaneous render targets aren't supported");
      return 4;
    }

    D3DDISPLAYMODE mode = {};
    d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
    const bool vtf = SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                      mode.Format, D3DUSAGE_QUERY_VERTEXTEXTURE,
                                                      D3DRTYPE_TEXTURE, D3DFMT_R32F));
    // S_OK, not D3DOK_NOAUTOGEN, means the mips are really generated
    const bool autogen = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format,
                                                D3DUSAGE_AUTOGENMIPMAP | D3DUSAGE_RENDERTARGET,
                                                D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8) == D3D_OK;
    const bool msaa = SUCCEEDED(d3d->CheckDeviceMultiSampleType(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL));

    ID3DBlobPtr vsblob = Compile(vtf ? VertexVTF : VertexPlain, "main", "vs_3_0");
    ID3DBlobPtr psblob = Compile(Pixel, "main", "ps_3_0");
    if(!vsblob || !psblob)
      return 4;
    IDirect3DVertexShader9Ptr vs = CreateVS(vsblob);
    IDirect3DPixelShader9Ptr ps = CreatePS(psblob);

    // textures, each a size no other resource has
    IDirect3DTexture9Ptr texPS = MakeTexture(8, 8, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    IDirect3DTexture9Ptr texUnusedPS = MakeTexture(12, 12, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    IDirect3DTexture9Ptr texVS = vtf ? MakeTexture(4, 4, D3DFMT_R32F, D3DPOOL_MANAGED) : NULL;
    IDirect3DTexture9Ptr texFFP = MakeTexture(20, 20, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    IDirect3DTexture9Ptr texFFPDisabled = MakeTexture(28, 28, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    IDirect3DTexture9Ptr lockTex = MakeTexture(44, 44, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    IDirect3DTexture9Ptr sysTex = MakeTexture(36, 36, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM);
    IDirect3DTexture9Ptr defTex = MakeTexture(36, 36, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT);
    IDirect3DTexture9Ptr rt0Tex =
        MakeTexture(64, 32, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET);
    // render targets bound together must be the same size, so this one is told apart by its format
    IDirect3DTexture9Ptr rt1Tex =
        MakeTexture(64, 32, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET);
    IDirect3DTexture9Ptr resolveTex =
        msaa ? MakeTexture(40, 40, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET) : NULL;
    IDirect3DTexture9Ptr mipTex =
        autogen ? MakeTexture(128, 128, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                              D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP, 0)
                : NULL;

    IDirect3DSurface9Ptr rt0, rt1, resolveSurf, mipSurf;
    rt0Tex->GetSurfaceLevel(0, &rt0);
    rt1Tex->GetSurfaceLevel(0, &rt1);
    if(resolveTex)
      resolveTex->GetSurfaceLevel(0, &resolveSurf);
    if(mipTex)
      mipTex->GetSurfaceLevel(0, &mipSurf);

    IDirect3DSurface9Ptr depth, copyDst, sysSurf, msaaRT;
    dev->CreateDepthStencilSurface(72, 40, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth, NULL);
    dev->CreateRenderTarget(64, 44, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &copyDst, NULL);
    dev->CreateOffscreenPlainSurface(64, 44, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysSurf, NULL);
    if(msaa)
      dev->CreateRenderTarget(40, 40, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &msaaRT,
                              NULL);
    if(!depth || !copyDst || !sysSurf || (msaa && !msaaRT))
    {
      TEST_ERROR("Couldn't create the surfaces");
      return 4;
    }

    // buffers, each a size no other has
    const Vec3f positions[4] = {
        Vec3f(-0.5f, -0.5f, 0.0f),
        Vec3f(-0.5f, 0.5f, 0.0f),
        Vec3f(0.5f, -0.5f, 0.0f),
        Vec3f(0.5f, 0.5f, 0.0f),
    };
    const DWORD colours[4] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
    IDirect3DVertexBuffer9Ptr vb0 = MakeBuffer(1024, positions, sizeof(positions));
    IDirect3DVertexBuffer9Ptr vb1 = MakeBuffer(1152, colours, sizeof(colours));
    IDirect3DVertexBuffer9Ptr vbUnused = MakeBuffer(1280, colours, sizeof(colours));

    IDirect3DIndexBuffer9Ptr ib;
    dev->CreateIndexBuffer(640, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL);
    {
      const WORD indices[6] = {0, 1, 2, 2, 1, 3};
      void *p = NULL;
      if(ib && SUCCEEDED(ib->Lock(0, 0, &p, 0)))
      {
        memset(p, 0, 640);
        memcpy(p, indices, sizeof(indices));
        ib->Unlock();
      }
    }
    if(!vb0 || !vb1 || !vbUnused || !ib)
    {
      TEST_ERROR("Couldn't create the buffers");
      return 4;
    }

    const D3DVERTEXELEMENT9 elems[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9Ptr decl;
    dev->CreateVertexDeclaration(elems, &decl);

    const FFVertex ffQuad[4] = {
        {4.0f, 4.0f, 0.5f, 1.0f, 0xffffffff, 0.0f, 0.0f},
        {60.0f, 4.0f, 0.5f, 1.0f, 0xffffffff, 1.0f, 0.0f},
        {4.0f, 28.0f, 0.5f, 1.0f, 0xffffffff, 0.0f, 1.0f},
        {60.0f, 28.0f, 0.5f, 1.0f, 0xffffffff, 1.0f, 1.0f},
    };

    IDirect3DSurface9Ptr backbuffer, autoDepth;
    dev->GetRenderTarget(0, &backbuffer);
    dev->GetDepthStencilSurface(&autoDepth);

    const int clearsBoth = ClearClearsSecondTarget(rt0, rt1);
    TEST_LOG("A colour Clear with two render targets bound %s the second",
             clearsBoth == 1   ? "clears"
             : clearsBoth == 0 ? "doesn't clear"
                               : "couldn't check");

    {
      const std::string path = tmpPath + (rdoc ? "/D3D9_Resource_Usage_reference_rdoc.txt"
                                               : "/D3D9_Resource_Usage_reference.txt");
      FILE *f = NULL;
      fopen_s(&f, path.c_str(), "w");
      if(f)
      {
        fprintf(f, "Clear clears render target 1\t%d\nVertex texture fetch\t%d\n", clearsBoth,
                vtf ? 1 : 0);
        fprintf(f, "Autogen mipmaps\t%d\nMultisampling\t%d\n", autogen ? 1 : 0, msaa ? 1 : 0);
        fclose(f);
      }
      else
      {
        TEST_ERROR("Couldn't write %s", path.c_str());
      }
    }

    const float offset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    while(Running())
    {
      pushMarker("Clear two targets");
      dev->SetRenderTarget(0, rt0);
      dev->SetRenderTarget(1, rt1);
      dev->SetDepthStencilSurface(depth);
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                 D3DCOLOR_XRGB(32, 32, 32), 1.0f, 0);
      popMarker();

      pushMarker("Lock vertex buffer");
      {
        void *p = NULL;
        if(SUCCEEDED(vb1->Lock(0, sizeof(colours), &p, 0)))
        {
          memcpy(p, colours, sizeof(colours));
          vb1->Unlock();
        }
      }
      popMarker();

      pushMarker("Lock texture");
      {
        D3DLOCKED_RECT lr = {};
        if(SUCCEEDED(lockTex->LockRect(0, &lr, NULL, 0)))
        {
          *(DWORD *)lr.pBits = 0xff123456;
          lockTex->UnlockRect(0);
        }
      }
      popMarker();

      dev->BeginScene();

      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      dev->SetVertexDeclaration(decl);
      dev->SetStreamSource(0, vb0, 0, sizeof(Vec3f));
      dev->SetStreamSource(1, vb1, 0, sizeof(DWORD));
      dev->SetStreamSource(2, vbUnused, 0, sizeof(DWORD));
      dev->SetIndices(ib);
      dev->SetVertexShader(vs);
      dev->SetPixelShader(ps);
      dev->SetVertexShaderConstantF(0, offset, 1);
      dev->SetPixelShaderConstantF(0, tint, 1);
      dev->SetTexture(0, texPS);
      dev->SetTexture(1, texUnusedPS);
      dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, texVS);

      pushMarker("Shader draw");
      dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
      popMarker();

      // the index buffer is still bound, and depth testing is off
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      pushMarker("Non-indexed draw");
      dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
      popMarker();

      // stage 0 samples texFFP, stage 1 is disabled so stage 2's texture isn't read
      dev->SetRenderTarget(1, NULL);
      dev->SetVertexShader(NULL);
      dev->SetPixelShader(NULL);
      dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, NULL);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
      dev->SetTexture(0, texFFP);
      dev->SetTexture(1, NULL);
      dev->SetTexture(2, texFFPDisabled);
      dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
      dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
      dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
      dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      dev->SetTextureStageState(2, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      pushMarker("Fixed function draw");
      dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, ffQuad, sizeof(FFVertex));
      popMarker();

      if(msaaRT)
      {
        dev->SetRenderTarget(0, msaaRT);
        dev->SetDepthStencilSurface(NULL);
        pushMarker("Multisampled draw");
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, ffQuad, sizeof(FFVertex));
        popMarker();
      }

      dev->SetTexture(0, NULL);
      dev->SetTexture(2, NULL);
      dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);

      dev->EndScene();

      if(msaaRT)
      {
        pushMarker("Resolve");
        dev->StretchRect(msaaRT, NULL, resolveSurf, NULL, D3DTEXF_NONE);
        popMarker();
      }

      pushMarker("StretchRect");
      dev->StretchRect(rt0, NULL, copyDst, NULL, D3DTEXF_NONE);
      popMarker();

      pushMarker("GetRenderTargetData");
      dev->GetRenderTargetData(copyDst, sysSurf);
      popMarker();

      pushMarker("ColorFill");
      dev->ColorFill(copyDst, NULL, D3DCOLOR_XRGB(0, 0, 255));
      popMarker();

      pushMarker("UpdateTexture");
      sysTex->AddDirtyRect(NULL);
      dev->UpdateTexture(sysTex, defTex);
      popMarker();

      if(mipTex)
      {
        pushMarker("Generate mips");
        dev->ColorFill(mipSurf, NULL, D3DCOLOR_XRGB(0, 255, 255));
        mipTex->GenerateMipSubLevels();
        popMarker();
      }

      dev->SetRenderTarget(0, backbuffer);
      dev->SetDepthStencilSurface(autoDepth);
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(51, 51, 51), 1.0f, 0);

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
