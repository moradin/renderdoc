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

RD_TEST(D3D9_Dirty_Regions, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "UpdateTexture copies only the source's dirty region. Each frame starts with system memory "
      "textures whose dirty regions were set at the end of the frame before: two rects of a 2D "
      "texture with two levels, two rects on one cube face (copied as the box around them) and "
      "another whole face, two boxes of a volume, and 8x8 textures with no dirty region, 6 rects "
      "and 7 rects (which makes all of it dirty). The frame copies each into a default pool "
      "texture, then locks the 2D texture with and without D3DLOCK_NO_DIRTY_UPDATE and on level "
      "1, and copies it again. Every frame reads the default pool textures back on the GPU and "
      "checks them against what the dirty regions say should have been copied.";

  static const UINT Size = 16;
  static const UINT CubeSize = 8;
  static const UINT VolSize = 8;
  static const UINT VolDepth = 4;
  static const UINT SmallSize = 8;

  static constexpr DWORD Base = 0xFF203040;
  // the end of each frame fills the sources with one of these, alternating, so each frame starts
  // with the other one
  const DWORD FrameColours[2] = {0xFF00FF00, 0xFF0000FF};
  // and the frame's locks write one of these
  const DWORD LockColours[2] = {0xFFFF00FF, 0xFFFFFF00};

  // A texture's contents on the CPU: each subresource (level, or cube face) is a w x h x d array
  struct Image
  {
    UINT w = 0, h = 0, d = 1;
    std::vector<DWORD> texels;

    void Init(UINT width, UINT height, UINT depth, DWORD colour)
    {
      w = width;
      h = height;
      d = depth;
      texels.assign(w * h * d, colour);
    }
    DWORD &At(UINT x, UINT y, UINT z = 0) { return texels[(z * h + y) * w + x]; }
    void Fill(const D3DBOX &box, DWORD colour)
    {
      for(UINT z = box.Front; z < box.Back; z++)
        for(UINT y = box.Top; y < box.Bottom; y++)
          for(UINT x = box.Left; x < box.Right; x++)
            At(x, y, z) = colour;
    }
    void CopyFrom(Image &src, const D3DBOX &box)
    {
      for(UINT z = box.Front; z < box.Back; z++)
        for(UINT y = box.Top; y < box.Bottom; y++)
          for(UINT x = box.Left; x < box.Right; x++)
            At(x, y, z) = src.At(x, y, z);
    }
  };

  static D3DBOX Box(UINT l, UINT t, UINT r, UINT b, UINT f = 0, UINT bk = 1)
  {
    D3DBOX ret = {l, t, r, b, f, bk};
    return ret;
  }

  // a level-0 region at a level, rounding outwards, as the runtime copies it
  static D3DBOX AtLevel(const D3DBOX &box, UINT level)
  {
    const UINT round = (1U << level) - 1;
    return Box(box.Left >> level, box.Top >> level, (box.Right + round) >> level,
               (box.Bottom + round) >> level, box.Front >> level, (box.Back + round) >> level);
  }

  static RECT Rect(const D3DBOX &box)
  {
    RECT r = {(LONG)box.Left, (LONG)box.Top, (LONG)box.Right, (LONG)box.Bottom};
    return r;
  }

  // The regions, which the test in util/test/tests/D3D9/D3D9_Dirty_Regions.py repeats. Dirty at
  // the start of each frame: 2D and volume textures keep up to 6 boxes, a 7th makes the whole
  // texture dirty. A cube face keeps the box around everything added to it.
  const std::vector<D3DBOX> Carried = {Box(0, 0, 8, 8), Box(10, 12, 13, 15)};
  const std::vector<D3DBOX> CubeCarried = {Box(0, 0, 2, 2), Box(5, 5, 7, 7)};
  const D3DBOX CubeCarriedBounds = Box(0, 0, 7, 7);
  const UINT CubeCarriedFace = 1;
  const UINT CubeWholeFace = 2;
  const std::vector<D3DBOX> VolCarried = {Box(0, 0, 4, 4, 0, 2), Box(6, 6, 8, 8, 3, 4)};
  // the small 2D textures: no rects, 6 rects, and 7 rects
  static const UINT NumSmall = 3;
  std::vector<D3DBOX> SmallRects(UINT count)
  {
    std::vector<D3DBOX> ret;
    for(UINT i = 0; i < count; i++)
      ret.push_back(Box(i, (i * 3) % SmallSize, i + 1, (i * 3) % SmallSize + 1));
    return ret;
  }
  const UINT SmallCounts[NumSmall] = {0, 6, 7};
  // locked in the frame: dirty, not dirty (D3DLOCK_NO_DIRTY_UPDATE), and on level 1
  const D3DBOX Locked = Box(8, 8, 12, 12);
  const D3DBOX LockedNoDirty = Box(12, 0, 16, 4);
  const D3DBOX LockedLevel1 = Box(0, 6, 2, 8);

  struct QuadVertex
  {
    float x, y, z, rhw;
    float u, v, w;
  };

  static const DWORD QuadFVF = D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0);

  // a quad of w x h pixels at (x, y), with texture coordinates c[0..3] at its top-left, top-right,
  // bottom-left and bottom-right pixel corners
  static void MakeQuad(QuadVertex * verts, float x, float y, float w, float h, const float c[4][3])
  {
    const float px[4] = {x, x + w, x, x + w};
    const float py[4] = {y, y, y + h, y + h};
    const int order[6] = {0, 1, 2, 1, 3, 2};
    for(int i = 0; i < 6; i++)
    {
      const int v = order[i];
      verts[i] = {px[v] - 0.5f, py[v] - 0.5f, 0.5f, 1.0f, c[v][0], c[v][1], c[v][2]};
    }
  }

  void SetQuadState(IDirect3DBaseTexture9 * tex, UINT level)
  {
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetFVF(QuadFVF);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, level);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    dev->SetTexture(0, tex);
  }

  IDirect3DTexture9Ptr readbackRT;
  IDirect3DSurface9Ptr readbackSurf, readbackSysmem;

  // Reads part of a texture back on the GPU: draws a quad of w x h pixels with the corners'
  // coordinates, point sampled from one level, and compares it with the expected image (the
  // subresource of 'want' at depth slice z). Returns false and logs the first difference.
  bool CheckOnGPU(const char *what, IDirect3DBaseTexture9 *tex, UINT level, UINT w, UINT h,
                  const float corners[4][3], Image &want, UINT z)
  {
    IDirect3DSurface9Ptr prevRT, prevDS;
    dev->GetRenderTarget(0, &prevRT);
    dev->GetDepthStencilSurface(&prevDS);
    D3DVIEWPORT9 prevVP = {};
    dev->GetViewport(&prevVP);

    dev->SetRenderTarget(0, readbackSurf);
    dev->SetDepthStencilSurface(NULL);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);

    QuadVertex quad[6];
    MakeQuad(quad, 0.0f, 0.0f, (float)w, (float)h, corners);

    dev->BeginScene();
    SetQuadState(tex, level);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(QuadVertex));
    dev->SetTexture(0, NULL);
    dev->EndScene();

    dev->SetRenderTarget(0, prevRT);
    dev->SetDepthStencilSurface(prevDS);
    dev->SetViewport(&prevVP);

    D3DLOCKED_RECT locked = {};
    HRESULT hr = dev->GetRenderTargetData(readbackSurf, readbackSysmem);
    if(SUCCEEDED(hr))
      hr = readbackSysmem->LockRect(&locked, NULL, D3DLOCK_READONLY);
    if(FAILED(hr))
    {
      TEST_ERROR("Couldn't read back %s: %x", what, hr);
      return false;
    }

    bool ok = true;
    for(UINT y = 0; y < h && ok; y++)
    {
      const DWORD *row = (const DWORD *)((byte *)locked.pBits + y * locked.Pitch);
      for(UINT x = 0; x < w && ok; x++)
      {
        if(row[x] != want.At(x, y, z))
        {
          TEST_ERROR("Frame %d: %s at %u,%u is %08x, expected %08x", curFrame, what, x, y, row[x],
                     want.At(x, y, z));
          ok = false;
        }
      }
    }
    readbackSysmem->UnlockRect();
    return ok;
  }

  bool Check2D(const char *what, IDirect3DTexture9 *tex, std::vector<Image> &levels)
  {
    bool ok = true;
    for(UINT level = 0; level < levels.size(); level++)
    {
      const float c[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
      std::string name = std::string(what) + " level " + std::to_string(level);
      ok &= CheckOnGPU(name.c_str(), tex, level, levels[level].w, levels[level].h, c, levels[level],
                       0);
    }
    return ok;
  }

  bool CheckCube(IDirect3DCubeTexture9 * tex, std::vector<Image> & faces)
  {
    bool ok = true;
    for(UINT face = 0; face < 6; face++)
    {
      // the direction of each corner of the face, for s and t from -1 to 1 across it
      float c[4][3];
      for(int i = 0; i < 4; i++)
      {
        const float s = (i & 1) ? 1.0f : -1.0f;
        const float t = (i & 2) ? 1.0f : -1.0f;
        float *d = c[i];
        switch(face)
        {
          case 0: d[0] = 1, d[1] = -t, d[2] = -s; break;
          case 1: d[0] = -1, d[1] = -t, d[2] = s; break;
          case 2: d[0] = s, d[1] = 1, d[2] = t; break;
          case 3: d[0] = s, d[1] = -1, d[2] = -t; break;
          case 4: d[0] = s, d[1] = -t, d[2] = 1; break;
          default: d[0] = -s, d[1] = -t, d[2] = -1; break;
        }
      }
      std::string name = "The cube texture face " + std::to_string(face);
      ok &= CheckOnGPU(name.c_str(), tex, 0, CubeSize, CubeSize, c, faces[face], 0);
    }
    return ok;
  }

  bool CheckVolume(IDirect3DVolumeTexture9 * tex, Image & vol)
  {
    bool ok = true;
    for(UINT z = 0; z < VolDepth; z++)
    {
      const float w = (z + 0.5f) / VolDepth;
      const float c[4][3] = {{0, 0, w}, {1, 0, w}, {0, 1, w}, {1, 1, w}};
      std::string name = "The volume texture slice " + std::to_string(z);
      ok &= CheckOnGPU(name.c_str(), tex, 0, VolSize, VolSize, c, vol, z);
    }
    return ok;
  }

  // fills part of a locked A8R8G8B8 2D level, pBits pointing at the rect's start
  static void FillLocked(void *bits, UINT pitch, UINT slicePitch, const D3DBOX &box, DWORD colour)
  {
    for(UINT z = 0; z < box.Back - box.Front; z++)
      for(UINT y = 0; y < box.Bottom - box.Top; y++)
      {
        DWORD *row = (DWORD *)((byte *)bits + z * slicePitch + y * pitch);
        for(UINT x = 0; x < box.Right - box.Left; x++)
          row[x] = colour;
      }
  }

  void Lock2D(IDirect3DTexture9 * tex, Image & img, UINT level, const D3DBOX &box, DWORD flags,
              DWORD colour)
  {
    RECT rect = Rect(box);
    D3DLOCKED_RECT locked = {};
    if(FAILED(tex->LockRect(level, &locked, &rect, flags)))
    {
      TEST_ERROR("LockRect failed");
      return;
    }
    FillLocked(locked.pBits, locked.Pitch, 0, box, colour);
    tex->UnlockRect(level);
    img.Fill(box, colour);
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    D3DCAPS9 caps = {};
    dev->GetDeviceCaps(&caps);

    if(FAILED(dev->CreateTexture(Size, Size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                 D3DPOOL_DEFAULT, &readbackRT, NULL)) ||
       FAILED(readbackRT->GetSurfaceLevel(0, &readbackSurf)) ||
       FAILED(dev->CreateOffscreenPlainSurface(Size, Size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                               &readbackSysmem, NULL)))
    {
      TEST_ERROR("Couldn't create the readback render target");
      return 4;
    }

    // each source is a system memory texture copied into a default pool texture. What they hold
    // is tracked on the CPU too, in src*/dst*.
    IDirect3DTexture9Ptr src2D, dst2D, srcSmall[NumSmall], dstSmall[NumSmall];
    IDirect3DCubeTexture9Ptr srcCube, dstCube;
    IDirect3DVolumeTexture9Ptr srcVol, dstVol;

    std::vector<Image> src2DImg(2), dst2DImg(2);
    std::vector<std::vector<Image>> srcSmallImg(NumSmall, std::vector<Image>(1)),
        dstSmallImg(NumSmall, std::vector<Image>(1));
    std::vector<Image> srcCubeImg(6), dstCubeImg(6);
    Image srcVolImg, dstVolImg;

    if(FAILED(dev->CreateTexture(Size, Size, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src2D, NULL)) ||
       FAILED(dev->CreateTexture(Size, Size, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst2D, NULL)) ||
       FAILED(dev->CreateCubeTexture(CubeSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &srcCube,
                                     NULL)) ||
       FAILED(dev->CreateCubeTexture(CubeSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dstCube,
                                     NULL)))
    {
      TEST_ERROR("Couldn't create the textures");
      return 4;
    }

    for(UINT i = 0; i < NumSmall; i++)
    {
      if(FAILED(dev->CreateTexture(SmallSize, SmallSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                   &srcSmall[i], NULL)) ||
         FAILED(dev->CreateTexture(SmallSize, SmallSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                   &dstSmall[i], NULL)))
      {
        TEST_ERROR("Couldn't create the textures");
        return 4;
      }
      srcSmallImg[i][0].Init(SmallSize, SmallSize, 1, Base);
      dstSmallImg[i][0].Init(SmallSize, SmallSize, 1, Base);
    }

    if(!(caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) ||
       FAILED(dev->CreateVolumeTexture(VolSize, VolSize, VolDepth, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_SYSTEMMEM, &srcVol, NULL)) ||
       FAILED(dev->CreateVolumeTexture(VolSize, VolSize, VolDepth, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_DEFAULT, &dstVol, NULL)))
    {
      TEST_WARN("Volume textures aren't supported, skipping");
      srcVol = dstVol = NULL;
    }

    for(UINT level = 0; level < 2; level++)
    {
      src2DImg[level].Init(Size >> level, Size >> level, 1, Base);
      dst2DImg[level].Init(Size >> level, Size >> level, 1, Base);
    }
    for(UINT face = 0; face < 6; face++)
    {
      srcCubeImg[face].Init(CubeSize, CubeSize, 1, Base);
      dstCubeImg[face].Init(CubeSize, CubeSize, 1, Base);
    }
    srcVolImg.Init(VolSize, VolSize, VolDepth, Base);
    dstVolImg.Init(VolSize, VolSize, VolDepth, Base);

    // fills every source with a colour without adding to its dirty region
    auto fillSources = [&](DWORD colour) {
      for(UINT level = 0; level < 2; level++)
        Lock2D(src2D, src2DImg[level], level, Box(0, 0, Size >> level, Size >> level),
               D3DLOCK_NO_DIRTY_UPDATE, colour);
      for(UINT i = 0; i < NumSmall; i++)
        Lock2D(srcSmall[i], srcSmallImg[i][0], 0, Box(0, 0, SmallSize, SmallSize),
               D3DLOCK_NO_DIRTY_UPDATE, colour);
      for(UINT face = 0; face < 6; face++)
      {
        D3DLOCKED_RECT locked = {};
        srcCube->LockRect((D3DCUBEMAP_FACES)face, 0, &locked, NULL, D3DLOCK_NO_DIRTY_UPDATE);
        FillLocked(locked.pBits, locked.Pitch, 0, Box(0, 0, CubeSize, CubeSize), colour);
        srcCube->UnlockRect((D3DCUBEMAP_FACES)face, 0);
        srcCubeImg[face].Fill(Box(0, 0, CubeSize, CubeSize), colour);
      }
      if(srcVol)
      {
        D3DLOCKED_BOX locked = {};
        srcVol->LockBox(0, &locked, NULL, D3DLOCK_NO_DIRTY_UPDATE);
        const D3DBOX all = Box(0, 0, VolSize, VolSize, 0, VolDepth);
        FillLocked(locked.pBits, locked.RowPitch, locked.SlicePitch, all, colour);
        srcVol->UnlockBox(0);
        srcVolImg.Fill(all, colour);
      }
    };

    // the first copies are whole, since new textures are entirely dirty
    fillSources(Base);
    dev->UpdateTexture(src2D, dst2D);
    for(UINT i = 0; i < NumSmall; i++)
      dev->UpdateTexture(srcSmall[i], dstSmall[i]);
    dev->UpdateTexture(srcCube, dstCube);
    if(srcVol)
      dev->UpdateTexture(srcVol, dstVol);

    // what the end of each frame does for the next: new contents that aren't dirty, then the
    // regions the next frame's copies should take
    auto prepareNextFrame = [&](DWORD colour) {
      fillSources(colour);
      for(const D3DBOX &box : Carried)
      {
        RECT r = Rect(box);
        src2D->AddDirtyRect(&r);
      }
      for(const D3DBOX &box : CubeCarried)
      {
        RECT r = Rect(box);
        srcCube->AddDirtyRect((D3DCUBEMAP_FACES)CubeCarriedFace, &r);
      }
      srcCube->AddDirtyRect((D3DCUBEMAP_FACES)CubeWholeFace, NULL);
      if(srcVol)
      {
        for(const D3DBOX &box : VolCarried)
          srcVol->AddDirtyBox(&box);
      }
      for(UINT i = 0; i < NumSmall; i++)
      {
        for(const D3DBOX &box : SmallRects(SmallCounts[i]))
        {
          RECT r = Rect(box);
          srcSmall[i]->AddDirtyRect(&r);
        }
      }
    };

    prepareNextFrame(FrameColours[1]);

    int checkedFrames = 0;
    bool failed = false;

    while(Running())
    {
      const DWORD lockColour = LockColours[curFrame % 2];

      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      // a read-only lock of level 0 doesn't add to the dirty region
      {
        D3DLOCKED_RECT locked = {};
        src2D->LockRect(0, &locked, NULL, D3DLOCK_READONLY);
        src2D->UnlockRect(0);
      }

      // the regions dirty since the end of the last frame
      setMarker("UpdateTexture with the carried dirty regions");
      dev->UpdateTexture(src2D, dst2D);
      for(UINT level = 0; level < 2; level++)
        for(const D3DBOX &box : Carried)
          dst2DImg[level].CopyFrom(src2DImg[level], AtLevel(box, level));

      dev->UpdateTexture(srcCube, dstCube);
      dstCubeImg[CubeCarriedFace].CopyFrom(srcCubeImg[CubeCarriedFace], CubeCarriedBounds);
      dstCubeImg[CubeWholeFace].CopyFrom(srcCubeImg[CubeWholeFace], Box(0, 0, CubeSize, CubeSize));

      if(srcVol)
      {
        dev->UpdateTexture(srcVol, dstVol);
        for(const D3DBOX &box : VolCarried)
          dstVolImg.CopyFrom(srcVolImg, box);
      }

      // nothing dirty, 6 rects, and 7 rects which make all of it dirty
      for(UINT i = 0; i < NumSmall; i++)
      {
        dev->UpdateTexture(srcSmall[i], dstSmall[i]);
        if(SmallCounts[i] > 6)
          dstSmallImg[i][0].CopyFrom(srcSmallImg[i][0], Box(0, 0, SmallSize, SmallSize));
        else
          for(const D3DBOX &box : SmallRects(SmallCounts[i]))
            dstSmallImg[i][0].CopyFrom(srcSmallImg[i][0], box);
      }

      // locks in the frame: only the first adds to the dirty region
      Lock2D(src2D, src2DImg[0], 0, Locked, 0, lockColour);
      Lock2D(src2D, src2DImg[0], 0, LockedNoDirty, D3DLOCK_NO_DIRTY_UPDATE, lockColour);
      Lock2D(src2D, src2DImg[1], 1, LockedLevel1, 0, lockColour);

      setMarker("UpdateTexture with the frame's locks");
      dev->UpdateTexture(src2D, dst2D);
      for(UINT level = 0; level < 2; level++)
        dst2DImg[level].CopyFrom(src2DImg[level], AtLevel(Locked, level));

      // show the 2D texture's level 0
      {
        dev->BeginScene();
        const float c[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
        QuadVertex quad[6];
        MakeQuad(quad, 10.0f, 10.0f, 128.0f, 128.0f, c);
        SetQuadState(dst2D, 0);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quad, sizeof(QuadVertex));
        dev->SetTexture(0, NULL);
        dev->EndScene();
      }

      // what the GPU has against what the dirty regions say
      bool ok = Check2D("The 2D texture", dst2D, dst2DImg);
      for(UINT i = 0; i < NumSmall; i++)
      {
        std::string name = "The texture with " + std::to_string(SmallCounts[i]) + " dirty rects";
        ok &= Check2D(name.c_str(), dstSmall[i], dstSmallImg[i]);
      }
      ok &= CheckCube(dstCube, dstCubeImg);
      if(dstVol)
        ok &= CheckVolume(dstVol, dstVolImg);
      if(ok)
        checkedFrames++;
      else
        failed = true;

      if(curFrame == 10 && !failed)
        TEST_LOG("UpdateTexture copied the dirty regions in all %d frames", checkedFrames);

      prepareNextFrame(FrameColours[curFrame % 2]);

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
