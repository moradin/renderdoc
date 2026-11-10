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

RD_TEST(D3D9_Resource_Contents, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Uses objects in every pool that holds contents a capture must store: managed, default, "
      "dynamic and system memory buffers and textures, a render target texture, a multisampled "
      "render target, cube and volume textures, and DXT1 cube and volume textures. Some are "
      "partly overwritten every frame with a "
      "colour that alternates between frames, so a capture's initial contents can be told apart "
      "from what the frame writes. Also checks that UpdateTexture only copies the dirty region.";

  struct QuadVertex
  {
    float x, y, z, rhw;
    D3DCOLOR col;
    float u, v;
  };

  static const DWORD QuadFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

  // the colours to look for, as A8R8G8B8 DWORDs
  static constexpr DWORD ShadowColourA = 0xFF203040;
  static constexpr DWORD ShadowColourB = 0xFFA0B0C0;
  static constexpr DWORD SurfaceColourE = 0xFF5A5A5A;
  static constexpr DWORD RTColourC = 0xFFFF0000;
  static constexpr DWORD MSAAColour = 0xFF00FFFF;
  static constexpr DWORD EvenColour = 0xFF00FF00;
  static constexpr DWORD OddColour = 0xFF0000FF;

  // 0xFF111111 for level 0, 0xFF222222 for level 1...
  static DWORD ManagedLevelColour(UINT level)
  {
    return 0xFF000000 | (0x111111 * (level + 1));
  }

  DWORD ParityColour()
  {
    return (curFrame % 2) == 0 ? EvenColour : OddColour;
  }

  // fills a rect of a locked A8R8G8B8 level, pBits pointing at the rect's start
  static void FillLocked(const D3DLOCKED_RECT &locked, UINT width, UINT height, DWORD colour)
  {
    for(UINT y = 0; y < height; y++)
    {
      DWORD *row = (DWORD *)((byte *)locked.pBits + y * locked.Pitch);
      for(UINT x = 0; x < width; x++)
        row[x] = colour;
    }
  }

  // R5G6B5, as DXT1 stores its endpoints
  static WORD Colour565(UINT r5, UINT g6, UINT b5)
  {
    return WORD((r5 << 11) | (g6 << 5) | b5);
  }
  // DXT1 face i and volume slice i, which the test expands to 8 bits the same way
  static WORD DXTFaceColour(UINT face)
  {
    return Colour565(5 * (face + 1), 63 - 8 * face, 3 * face);
  }
  static WORD DXTSliceColour(UINT z)
  {
    return Colour565(0, 10 * (z + 1), 31 - 6 * z);
  }

  // fills DXT1 blocks with one colour: both endpoints the colour and every index 0, which every
  // decoder and GPU decodes exactly
  static void FillDXT1(byte * bits, UINT pitch, UINT blocksX, UINT blocksY, WORD colour)
  {
    for(UINT by = 0; by < blocksY; by++)
    {
      for(UINT bx = 0; bx < blocksX; bx++)
      {
        WORD *block = (WORD *)(bits + by * pitch + bx * 8);
        block[0] = block[1] = colour;
        block[2] = block[3] = 0;
      }
    }
  }

  IDirect3DTexture9Ptr MakeFilledTexture(UINT size, D3DPOOL pool, DWORD colour)
  {
    IDirect3DTexture9Ptr tex;
    HRESULT hr = dev->CreateTexture(size, size, 1, 0, D3DFMT_A8R8G8B8, pool, &tex, NULL);
    if(FAILED(hr))
    {
      TEST_ERROR("CreateTexture failed: %x", hr);
      return NULL;
    }

    D3DLOCKED_RECT locked = {};
    if(SUCCEEDED(tex->LockRect(0, &locked, NULL, 0)))
    {
      FillLocked(locked, size, size, colour);
      tex->UnlockRect(0);
    }
    return tex;
  }

  static QuadVertex Vert(float x, float y, float u, float v)
  {
    // pixel centres are at integer coordinates, offset so texels map exactly to pixels
    return {x - 0.5f, y - 0.5f, 0.5f, 1.0f, 0xFFFFFFFF, u, v};
  }

  static void MakeQuad(QuadVertex * verts, float x, float y, float size)
  {
    verts[0] = Vert(x, y, 0.0f, 0.0f);
    verts[1] = Vert(x + size, y, 1.0f, 0.0f);
    verts[2] = Vert(x, y + size, 0.0f, 1.0f);
    verts[3] = Vert(x + size, y, 1.0f, 0.0f);
    verts[4] = Vert(x + size, y + size, 1.0f, 1.0f);
    verts[5] = Vert(x, y + size, 0.0f, 1.0f);
  }

  void SetQuadState()
  {
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetFVF(QuadFVF);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  }

  // Draws the texture into a render target and reads back two pixels, to check what the runtime
  // copied in UpdateTexture: the dirty top-left quarter should have B, the rest A.
  void CheckDirtyRegionCopy(IDirect3DTexture9 * tex)
  {
    IDirect3DTexture9Ptr rt;
    IDirect3DSurface9Ptr rtSurf, sysmem, prevRT, prevDS;
    HRESULT hr = dev->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &rt, NULL);
    if(SUCCEEDED(hr))
      hr = rt->GetSurfaceLevel(0, &rtSurf);
    if(SUCCEEDED(hr))
      hr = dev->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem,
                                            NULL);
    if(FAILED(hr))
    {
      TEST_WARN("Couldn't set up the dirty region check: %x", hr);
      return;
    }

    D3DVIEWPORT9 prevVP = {};
    dev->GetViewport(&prevVP);
    dev->GetRenderTarget(0, &prevRT);
    dev->GetDepthStencilSurface(&prevDS);

    dev->SetRenderTarget(0, rtSurf);
    dev->SetDepthStencilSurface(NULL);

    QuadVertex quad[6];
    MakeQuad(quad, 0.0f, 0.0f, 16.0f);

    dev->BeginScene();
    SetQuadState();
    dev->SetTexture(0, tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(QuadVertex));
    dev->SetTexture(0, NULL);
    dev->EndScene();

    dev->SetRenderTarget(0, prevRT);
    dev->SetDepthStencilSurface(prevDS);
    dev->SetViewport(&prevVP);

    hr = dev->GetRenderTargetData(rtSurf, sysmem);
    D3DLOCKED_RECT locked = {};
    if(SUCCEEDED(hr))
      hr = sysmem->LockRect(&locked, NULL, D3DLOCK_READONLY);
    if(FAILED(hr))
    {
      TEST_WARN("Couldn't read back the dirty region check: %x", hr);
      return;
    }

    DWORD inside = ((DWORD *)((byte *)locked.pBits + 2 * locked.Pitch))[2];
    DWORD outside = ((DWORD *)((byte *)locked.pBits + 10 * locked.Pitch))[10];
    sysmem->UnlockRect();

    if(inside == ShadowColourB && outside == ShadowColourA)
      TEST_LOG("UpdateTexture copied only the dirty region");
    else
      TEST_ERROR(
          "UpdateTexture didn't copy only the dirty region: (2,2) is %08x, expected %08x. "
          "(10,10) is %08x, expected %08x",
          inside, ShadowColourB, outside, ShadowColourA);
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    D3DCAPS9 caps = {};
    dev->GetDeviceCaps(&caps);

    ID3DBlobPtr vsblob = Compile(D3D9DefaultVertex, "main", "vs_3_0");
    ID3DBlobPtr psblob = Compile(D3D9DefaultPixel, "main", "ps_3_0");
    IDirect3DVertexShader9Ptr vs = CreateVS(vsblob);
    IDirect3DPixelShader9Ptr ps = CreatePS(psblob);

    // managed vertex buffer with the default triangle
    IDirect3DVertexBuffer9Ptr managedVB = MakeVB(DefaultTri, sizeof(DefaultTri));

    // a write-only index buffer in the default pool, filled once
    IDirect3DIndexBuffer9Ptr staticIB;
    {
      HRESULT hr = dev->CreateIndexBuffer(3 * sizeof(uint16_t), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                          D3DPOOL_DEFAULT, &staticIB, NULL);
      if(FAILED(hr))
      {
        TEST_ERROR("CreateIndexBuffer failed: %x", hr);
        return 4;
      }
      uint16_t *idx = NULL;
      staticIB->Lock(0, 0, (void **)&idx, 0);
      idx[0] = 0;
      idx[1] = 1;
      idx[2] = 2;
      staticIB->Unlock();
    }

    // a dynamic vertex buffer, refilled every frame with DISCARD then appended to with NOOVERWRITE
    const UINT numQuads = 5;
    IDirect3DVertexBuffer9Ptr dynamicVB;
    if(FAILED(dev->CreateVertexBuffer(numQuads * 6 * sizeof(QuadVertex),
                                      D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, QuadFVF,
                                      D3DPOOL_DEFAULT, &dynamicVB, NULL)))
    {
      TEST_ERROR("Couldn't create the dynamic vertex buffer");
      return 4;
    }

    // a managed texture, each of its 4 levels a different colour. Level 0 is partly overwritten
    // each frame.
    IDirect3DTexture9Ptr managedTex;
    dev->CreateTexture(8, 8, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managedTex, NULL);
    for(UINT level = 0; level < 4; level++)
    {
      D3DLOCKED_RECT locked = {};
      managedTex->LockRect(level, &locked, NULL, 0);
      FillLocked(locked, 8 >> level, 8 >> level, ManagedLevelColour(level));
      managedTex->UnlockRect(level);
    }

    // a default pool texture that can only be filled through UpdateTexture/UpdateSurface. The
    // first update copies all of A, the second only the dirty top-left quarter of B.
    IDirect3DTexture9Ptr defaultTex, sysmemTex;
    dev->CreateTexture(16, 16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &defaultTex, NULL);
    dev->CreateTexture(16, 16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmemTex, NULL);
    for(UINT level = 0; level < 2; level++)
    {
      D3DLOCKED_RECT locked = {};
      sysmemTex->LockRect(level, &locked, NULL, 0);
      FillLocked(locked, 16 >> level, 16 >> level, ShadowColourA);
      sysmemTex->UnlockRect(level);
    }
    dev->UpdateTexture(sysmemTex, defaultTex);
    for(UINT level = 0; level < 2; level++)
    {
      D3DLOCKED_RECT locked = {};
      sysmemTex->LockRect(level, &locked, NULL, D3DLOCK_NO_DIRTY_UPDATE);
      FillLocked(locked, 16 >> level, 16 >> level, ShadowColourB);
      sysmemTex->UnlockRect(level);
    }
    RECT dirty = {0, 0, 8, 8};
    sysmemTex->AddDirtyRect(&dirty);
    dev->UpdateTexture(sysmemTex, defaultTex);

    CheckDirtyRegionCopy(defaultTex);

    // a system memory surface, uploaded into the default texture each frame with UpdateSurface
    IDirect3DSurface9Ptr sysmemSurf;
    dev->CreateOffscreenPlainSurface(4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmemSurf, NULL);
    {
      D3DLOCKED_RECT locked = {};
      sysmemSurf->LockRect(&locked, NULL, 0);
      FillLocked(locked, 4, 4, SurfaceColourE);
      sysmemSurf->UnlockRect();
    }

    // a render target texture, red with a corner filled each frame
    IDirect3DTexture9Ptr rtTex;
    IDirect3DSurface9Ptr rtTexSurf;
    dev->CreateTexture(32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtTex,
                       NULL);
    rtTex->GetSurfaceLevel(0, &rtTexSurf);
    dev->ColorFill(rtTexSurf, NULL, RTColourC);

    // a multisampled render target, resolved onto the back buffer each frame
    IDirect3DSurface9Ptr msaaRT;
    if(SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8,
                                                 TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL)) &&
       SUCCEEDED(dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0,
                                         FALSE, &msaaRT, NULL)))
      dev->ColorFill(msaaRT, NULL, MSAAColour);
    else
      TEST_WARN("4x MSAA isn't supported, skipping the multisampled render target");

    // a managed cube texture with a different colour on each face, only bound
    IDirect3DCubeTexture9Ptr cubeTex;
    if(SUCCEEDED(dev->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cubeTex, NULL)))
    {
      for(UINT face = 0; face < 6; face++)
      {
        D3DLOCKED_RECT locked = {};
        cubeTex->LockRect((D3DCUBEMAP_FACES)face, 0, &locked, NULL, 0);
        FillLocked(locked, 4, 4, 0xFF000000 | (0x10 * (face + 1)));
        cubeTex->UnlockRect((D3DCUBEMAP_FACES)face, 0);
      }
    }

    // a managed volume texture, each slice a different colour, only bound
    IDirect3DVolumeTexture9Ptr volumeTex;
    if((caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) &&
       SUCCEEDED(dev->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                          &volumeTex, NULL)))
    {
      D3DLOCKED_BOX locked = {};
      volumeTex->LockBox(0, &locked, NULL, 0);
      for(UINT z = 0; z < 4; z++)
      {
        D3DLOCKED_RECT slice = {locked.RowPitch, (byte *)locked.pBits + z * locked.SlicePitch};
        FillLocked(slice, 4, 4, 0xFF000000 | (0x100 * (z + 1)));
      }
      volumeTex->UnlockBox(0);
    }
    else
    {
      TEST_WARN("Volume textures aren't supported, skipping");
    }

    // a DXT1 cube texture and a DXT1 volume texture, each face and slice one colour, only bound.
    // Saving these as a PNG decodes them a face or a depth slice at a time.
    IDirect3DCubeTexture9Ptr dxtCubeTex;
    if(SUCCEEDED(dev->CreateCubeTexture(4, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &dxtCubeTex, NULL)))
    {
      for(UINT face = 0; face < 6; face++)
      {
        D3DLOCKED_RECT locked = {};
        dxtCubeTex->LockRect((D3DCUBEMAP_FACES)face, 0, &locked, NULL, 0);
        FillDXT1((byte *)locked.pBits, locked.Pitch, 1, 1, DXTFaceColour(face));
        dxtCubeTex->UnlockRect((D3DCUBEMAP_FACES)face, 0);
      }
    }
    else
    {
      TEST_WARN("DXT1 cube textures aren't supported, skipping");
    }

    IDirect3DVolumeTexture9Ptr dxtVolumeTex;
    if((caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) &&
       SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                        D3DRTYPE_VOLUMETEXTURE, D3DFMT_DXT1)) &&
       SUCCEEDED(dev->CreateVolumeTexture(4, 4, 4, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED,
                                          &dxtVolumeTex, NULL)))
    {
      D3DLOCKED_BOX locked = {};
      dxtVolumeTex->LockBox(0, &locked, NULL, 0);
      for(UINT z = 0; z < 4; z++)
        FillDXT1((byte *)locked.pBits + z * locked.SlicePitch, locked.RowPitch, 1, 1,
                 DXTSliceColour(z));
      dxtVolumeTex->UnlockBox(0);
    }
    else
    {
      TEST_WARN("DXT1 volume textures aren't supported, skipping");
    }

    // replaced every frame: used, released while still bound, then recreated with a new colour
    IDirect3DTexture9Ptr recycledTex = MakeFilledTexture(4, D3DPOOL_MANAGED, 0xFF000000);

    while(Running())
    {
      const DWORD parity = ParityColour();

      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      // rewrite the quads with DISCARD, then append the last one with NOOVERWRITE
      QuadVertex *verts = NULL;
      dynamicVB->Lock(0, 0, (void **)&verts, D3DLOCK_DISCARD);
      for(UINT q = 0; q < numQuads - 1; q++)
        MakeQuad(verts + q * 6, 10.0f + q * 74.0f, 10.0f, 64.0f);
      dynamicVB->Unlock();

      dynamicVB->Lock((numQuads - 1) * 6 * sizeof(QuadVertex), 6 * sizeof(QuadVertex),
                      (void **)&verts, D3DLOCK_NOOVERWRITE);
      MakeQuad(verts, 10.0f + (numQuads - 1) * 74.0f, 10.0f, 64.0f);
      dynamicVB->Unlock();

      // overwrite part of the managed texture's level 0 and the system memory surface
      {
        RECT rect = {2, 2, 6, 6};
        D3DLOCKED_RECT locked = {};
        managedTex->LockRect(0, &locked, &rect, 0);
        FillLocked(locked, 4, 4, parity);
        managedTex->UnlockRect(0);

        RECT surfRect = {1, 1, 3, 3};
        sysmemSurf->LockRect(&locked, &surfRect, 0);
        FillLocked(locked, 2, 2, parity);
        sysmemSurf->UnlockRect();
      }

      IDirect3DSurface9Ptr defaultLevel0;
      defaultTex->GetSurfaceLevel(0, &defaultLevel0);
      POINT dstPoint = {12, 12};
      dev->UpdateSurface(sysmemSurf, NULL, defaultLevel0, &dstPoint);

      dev->BeginScene();

      // the default triangle, from the managed vertex buffer and the default pool index buffer
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetVertexDeclaration(defaultDecl);
      dev->SetStreamSource(0, managedVB, 0, sizeof(DefaultA2V));
      dev->SetIndices(staticIB);
      dev->SetVertexShader(vs);
      dev->SetPixelShader(ps);
      dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);

      // one textured quad per texture. The cube and volume textures, and the DXT1 ones, are only
      // bound, on stages that are disabled.
      SetQuadState();
      dev->SetStreamSource(0, dynamicVB, 0, sizeof(QuadVertex));
      dev->SetTexture(1, cubeTex);
      dev->SetTexture(2, volumeTex);
      dev->SetTexture(3, dxtCubeTex);
      dev->SetTexture(4, dxtVolumeTex);

      IDirect3DBaseTexture9 *quadTextures[numQuads] = {managedTex, defaultTex, rtTex, recycledTex,
                                                       managedTex};
      for(UINT q = 0; q < numQuads; q++)
      {
        dev->SetTexture(0, quadTextures[q]);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, q * 6, 2);

        // released while still bound, so it dies when it's unbound below
        if(q == 3)
          recycledTex = NULL;
      }

      dev->SetTexture(0, NULL);
      dev->SetTexture(1, NULL);
      dev->SetTexture(2, NULL);
      dev->SetTexture(3, NULL);
      dev->SetTexture(4, NULL);

      dev->EndScene();

      // after the render target texture was used, fill its corner. The next frame's initial
      // contents have this colour, the frame after that writes the other one.
      RECT corner = {0, 0, 16, 16};
      dev->ColorFill(rtTexSurf, &corner, parity);

      // created during the frame, the capture has its creation and the data written to it
      recycledTex =
          MakeFilledTexture(4, D3DPOOL_MANAGED, 0xFF000000 | ((curFrame * 0x010101) & 0xFFFFFF));

      if(msaaRT)
      {
        IDirect3DSurface9Ptr bb;
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        RECT dst = {10, 84, 74, 148};
        dev->StretchRect(msaaRT, NULL, bb, &dst, D3DTEXF_NONE);
      }

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
