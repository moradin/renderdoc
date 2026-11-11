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

RD_TEST(D3D9_Capture_Edge_Cases, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Things a capture has to handle beyond plain calls: depth stencils that can't be locked "
      "(the automatic one, a multisampled one, D16 and INTZ), state blocks created before the "
      "frame and used in it, a lock held across Present, GDI drawing with GetDC, "
      "GetFrontBufferData, and D3DPERF markers. Checks that reading depth stencils and state "
      "blocks during a capture leaves the application's state alone.";

  struct Vertex
  {
    float x, y, z, rhw;
    D3DCOLOR col;
    float u, v;
  };

  static const DWORD FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

  static constexpr DWORD EvenColour = 0xFF00FF00;
  static constexpr DWORD OddColour = 0xFF0000FF;

  // frames drawn so far
  uint32_t frame = 0;

  DWORD ParityColour()
  {
    return (frame % 2) == 0 ? EvenColour : OddColour;
  }

  // a rectangle at depth z, in pixels
  static void MakeRect(Vertex * verts, float x0, float y0, float x1, float y1, float z, D3DCOLOR col)
  {
    // pixel centres are at integer coordinates
    x0 -= 0.5f;
    y0 -= 0.5f;
    x1 -= 0.5f;
    y1 -= 0.5f;
    verts[0] = {x0, y0, z, 1.0f, col, 0.0f, 0.0f};
    verts[1] = {x1, y0, z, 1.0f, col, 1.0f, 0.0f};
    verts[2] = {x0, y1, z, 1.0f, col, 0.0f, 1.0f};
    verts[3] = {x1, y1, z, 1.0f, col, 1.0f, 1.0f};
  }

  void DrawRect(float x0, float y0, float x1, float y1, float z, D3DCOLOR col)
  {
    Vertex verts[4];
    MakeRect(verts, x0, y0, x1, y1, z, col);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(Vertex));
  }

  void SetDrawState()
  {
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetFVF(FVF);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  }

  // depth and stencil written with an always-passing test
  void SetDepthStencilWrite(DWORD stencilRef)
  {
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    dev->SetRenderState(D3DRS_STENCILREF, stencilRef);
    dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
  }

  void CheckRenderState(const char *when, D3DRENDERSTATETYPE state, DWORD expected)
  {
    DWORD value = 0;
    dev->GetRenderState(state, &value);
    if(value != expected)
      TEST_ERROR("%s: render state %u is %08x, expected %08x", when, (uint32_t)state, value,
                 expected);
  }

  void CheckVSConstant(const char *when, UINT reg, const float *expected)
  {
    float value[4] = {};
    dev->GetVertexShaderConstantF(reg, value, 1);
    if(memcmp(value, expected, sizeof(value)) != 0)
      TEST_ERROR("%s: c%u is (%f, %f, %f, %f), expected (%f, %f, %f, %f)", when, reg, value[0],
                 value[1], value[2], value[3], expected[0], expected[1], expected[2], expected[3]);
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    const bool msaaSupported =
        SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8,
                                                  TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL)) &&
        SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24S8,
                                                  TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL));

    const D3DFORMAT INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
    const bool intzSupported =
        SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                         D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, INTZ));

    IDirect3DSurface9Ptr backBuffer, autoDepth;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    dev->GetDepthStencilSurface(&autoDepth);

    // a multisampled target with its own depth stencil, drawn to every frame
    IDirect3DSurface9Ptr msaaRT, msaaDepth;
    if(msaaSupported)
    {
      dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &msaaRT,
                              NULL);
      dev->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE,
                                     &msaaDepth, NULL);
    }
    if(!msaaRT || !msaaDepth)
      TEST_WARN("4x MSAA isn't supported for A8R8G8B8 and D24S8, skipping it");

    // depth stencils filled once and only bound in the frame: D16, and INTZ if supported
    IDirect3DTexture9Ptr smallRTTex;
    IDirect3DSurface9Ptr smallRT, d16Depth, intzDepth;
    IDirect3DTexture9Ptr intzTex;
    dev->CreateTexture(32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                       &smallRTTex, NULL);
    smallRTTex->GetSurfaceLevel(0, &smallRT);
    dev->CreateDepthStencilSurface(32, 32, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, &d16Depth,
                                   NULL);
    if(intzSupported && SUCCEEDED(dev->CreateTexture(32, 32, 1, D3DUSAGE_DEPTHSTENCIL, INTZ,
                                                     D3DPOOL_DEFAULT, &intzTex, NULL)))
      intzTex->GetSurfaceLevel(0, &intzDepth);
    else
      TEST_WARN("INTZ isn't supported, skipping it");

    dev->SetRenderTarget(0, smallRT);
    dev->SetDepthStencilSurface(d16Depth);
    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 0.5f, 0);
    if(intzDepth)
    {
      dev->SetDepthStencilSurface(intzDepth);
      dev->Clear(0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.375f, 0x0F);
    }
    dev->SetRenderTarget(0, backBuffer);
    dev->SetDepthStencilSurface(autoDepth);

    // State blocks made before the frame. One is recorded, one captures the pixel states and one
    // everything.
    IDirect3DTexture9Ptr stateTex;
    dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &stateTex, NULL);

    IDirect3DStateBlock9Ptr recordedSB, pixelSB, allSB;
    const float recordedConst[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    dev->BeginStateBlock();
    dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF336699);
    dev->SetTexture(0, stateTex);
    dev->SetVertexShaderConstantF(20, recordedConst, 1);
    dev->EndStateBlock(&recordedSB);

    dev->SetRenderState(D3DRS_ALPHAREF, 0x42);
    dev->CreateStateBlock(D3DSBT_PIXELSTATE, &pixelSB);
    dev->SetRenderState(D3DRS_ALPHAREF, 0);

    dev->CreateStateBlock(D3DSBT_ALL, &allSB);

    // locked at the end of each frame, unlocked at the start of the next
    IDirect3DTexture9Ptr spanTex;
    dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &spanTex, NULL);

    // drawn to with GDI
    IDirect3DSurface9Ptr gdiSurf;
    dev->CreateOffscreenPlainSurface(32, 32, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &gdiSurf, NULL);
    {
      D3DLOCKED_RECT locked = {};
      gdiSurf->LockRect(&locked, NULL, 0);
      for(UINT y = 0; y < 32; y++)
        for(UINT x = 0; x < 32; x++)
          ((DWORD *)((byte *)locked.pBits + y * locked.Pitch))[x] = 0x00808080;
      gdiSurf->UnlockRect();
    }

    // the front buffer is the size of the display
    IDirect3DSurface9Ptr frontCopy;
    {
      D3DDISPLAYMODE mode = {};
      dev->GetDisplayMode(0, &mode);
      dev->CreateOffscreenPlainSurface(mode.Width, mode.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                       &frontCopy, NULL);
    }

    // A 64x64 D24S8 depth stencil holding 4096 depths, cleared a pixel at a time before the first
    // frame. The frame tests the same depths against it with ZFUNC EQUAL, writing green where it
    // passes, so replay only draws all green if it wrote every depth back to the level it had.
    // Pixel 0 is 0.375 and pixel 1 is 0.75000006, whose levels (0x600000 and 0xC00000) the formula
    // captures used before version 0x3 never wrote. Made after everything else, which keeps the
    // other resources' names.
    IDirect3DSurface9Ptr levelsDepth, levelsRT, levelsSys;
    IDirect3DTexture9Ptr levelsRTTex, levelsTex;
    IDirect3DVertexShader9Ptr levelsVS;
    IDirect3DPixelShader9Ptr levelsPS;
    IDirect3DVertexDeclaration9Ptr levelsDecl;
    if(SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
                                        D3DRTYPE_TEXTURE, D3DFMT_R32F)))
    {
      dev->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                     &levelsDepth, NULL);
      dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                         &levelsRTTex, NULL);
      levelsRTTex->GetSurfaceLevel(0, &levelsRT);
      dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &levelsSys, NULL);
      dev->CreateTexture(64, 64, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &levelsTex, NULL);

      // the depths: the two above, round numbers, then each level's own depth (as the formula
      // computes it) for levels across the range, depths between levels, and any fp32 in [0, 1]
      std::vector<float> depths(64 * 64);
      const uint32_t specials[] = {0x3EC00000, 0x3F400001, 0x00000000, 0x3F800000,
                                   0x3E800000, 0x3F000000, 0x3F400000, 0x3EAAAAAB};
      uint32_t seed = 0x1234567;
      for(uint32_t i = 0; i < 64 * 64; i++)
      {
        seed = seed * 1664525U + 1013904223U;
        const uint32_t r = seed >> 8;
        uint32_t bits = 0;
        float d = 0.0f;
        if(i < 8)
        {
          bits = specials[i];
          memcpy(&d, &bits, 4);
        }
        else if(i % 3 == 0)
        {
          d = float(r + (r >= 0x800000 ? 1 : 0)) / 16777216.0f;
        }
        else if(i % 3 == 1)
        {
          d = float(r) * (1.0f / 16777215.0f);
        }
        else
        {
          bits = seed % 0x3F800001U;
          memcpy(&d, &bits, 4);
        }
        depths[i] = d;
      }

      D3DLOCKED_RECT locked = {};
      levelsTex->LockRect(0, &locked, NULL, 0);
      for(UINT y = 0; y < 64; y++)
        memcpy((byte *)locked.pBits + y * locked.Pitch, &depths[y * 64], 64 * sizeof(float));
      levelsTex->UnlockRect(0);

      dev->SetRenderTarget(0, levelsRT);
      dev->SetDepthStencilSurface(levelsDepth);
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_STENCIL, 0xFF000000, 1.0f, 0);
      for(UINT i = 0; i < 64 * 64; i++)
      {
        D3DRECT r = {LONG(i % 64), LONG(i / 64), LONG(i % 64 + 1), LONG(i / 64 + 1)};
        dev->Clear(1, &r, D3DCLEAR_ZBUFFER, 0, depths[i], 0);
      }
      dev->SetRenderTarget(0, backBuffer);
      dev->SetDepthStencilSurface(autoDepth);

      levelsVS = CreateVS(Compile(R"(
float4 main(float4 pos : POSITION) : POSITION
{
  return pos;
}
)",
                                  "main", "vs_3_0"));
      levelsPS = CreatePS(Compile(R"(
sampler2D depths : register(s0);

float4 main(float2 vpos : VPOS, out float depth : DEPTH) : COLOR0
{
  depth = tex2Dlod(depths, float4((vpos + 0.5f) / 64.0f, 0, 0)).r;
  return float4(0, 1, 0, 1);
}
)",
                                  "main", "ps_3_0"));
      D3DVERTEXELEMENT9 elements[] = {
          {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          D3DDECL_END(),
      };
      dev->CreateVertexDeclaration(elements, &levelsDecl);
    }
    else
    {
      TEST_WARN("R32F textures aren't supported, skipping the depth levels");
    }

    bool spanLocked = false;

    while(Running())
    {
      if(spanLocked)
      {
        spanTex->UnlockRect(0);
        spanLocked = false;
      }

      pushMarker("Depth");

      // the automatic depth stencil: cleared, then two rects with different depth and stencil
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      dev->BeginScene();

      SetDrawState();
      SetDepthStencilWrite(0x5A);
      DrawRect(0, 0, 100, 100, 0.25f, 0xFFFF0000);
      SetDepthStencilWrite(0xA5);
      DrawRect(100, 0, 200, 100, 0.75f, 0xFF00FF00);

      setMarker("Multisampled");

      // the lower left half of the multisampled one
      if(msaaRT && msaaDepth)
      {
        dev->SetRenderTarget(0, msaaRT);
        dev->SetDepthStencilSurface(msaaDepth);
        dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.0f,
                   0);
        SetDepthStencilWrite(0x33);
        Vertex tri[3] = {
            {-0.5f, -0.5f, 0.5f, 1.0f, 0xFFFFFF00, 0, 0},
            {63.5f, 63.5f, 0.5f, 1.0f, 0xFFFFFF00, 0, 0},
            {-0.5f, 63.5f, 0.5f, 1.0f, 0xFFFFFF00, 0, 0},
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
      }

      // Bind D16 and INTZ without changing them. The draw makes the capture read them, inside
      // this scene, so check our state survives that.
      dev->SetRenderTarget(0, smallRT);
      D3DVIEWPORT9 smallVP = {0, 0, 32, 32, 0.0f, 1.0f};
      dev->SetViewport(&smallVP);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF010203);
      const float sentinel[4] = {200.0f, 201.0f, 202.0f, 203.0f};
      dev->SetVertexShaderConstantF(200, sentinel, 1);

      IDirect3DSurface9 *readDepths[] = {d16Depth, intzDepth};
      for(IDirect3DSurface9 *ds : readDepths)
      {
        if(!ds)
          continue;
        dev->SetDepthStencilSurface(ds);
        DrawRect(0, 0, 32, 32, 0.5f, 0xFF808080);

        const char *when = ds == d16Depth ? "After drawing with D16" : "After drawing with INTZ";
        CheckRenderState(when, D3DRS_ZENABLE, D3DZB_FALSE);
        CheckRenderState(when, D3DRS_TEXTUREFACTOR, 0xFF010203);
        CheckRenderState(when, D3DRS_CULLMODE, D3DCULL_NONE);
        CheckVSConstant(when, 200, sentinel);

        D3DVIEWPORT9 vp = {};
        dev->GetViewport(&vp);
        if(memcmp(&vp, &smallVP, sizeof(vp)) != 0)
          TEST_ERROR("%s: viewport is %u,%u %ux%u, expected 0,0 32x32", when, vp.X, vp.Y, vp.Width,
                     vp.Height);
      }

      // the depths tested against the depth stencil holding them, green where they're equal
      if(levelsDepth)
      {
        pushMarker("Depth levels");
        dev->SetRenderTarget(0, levelsRT);
        dev->SetDepthStencilSurface(levelsDepth);
        D3DVIEWPORT9 levelsVP = {0, 0, 64, 64, 0.0f, 1.0f};
        dev->SetViewport(&levelsVP);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetVertexShader(levelsVS);
        dev->SetPixelShader(levelsPS);
        dev->SetVertexDeclaration(levelsDecl);
        dev->SetTexture(0, levelsTex);
        // half a pixel up and left, as D3D9's pixel centres are at integer coordinates
        const float h = 1.0f / 64.0f;
        const float quad[] = {
            -1 - h, -1 + h, 0.5f, 1, -1 - h, 1 + h, 0.5f, 1,
            1 - h,  -1 + h, 0.5f, 1, 1 - h,  1 + h, 0.5f, 1,
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 4 * sizeof(float));
        dev->SetTexture(0, NULL);
        dev->SetVertexShader(NULL);
        dev->SetPixelShader(NULL);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        popMarker();
      }

      dev->SetRenderTarget(0, backBuffer);
      dev->SetDepthStencilSurface(autoDepth);

      popMarker();

      pushMarker("State blocks");

      // the recorded state block sets the texture factor
      recordedSB->Apply();
      CheckRenderState("After applying the recorded state block", D3DRS_TEXTUREFACTOR, 0xFF336699);

      // Capture replaces the pixel state block's contents, so the capture reads them first.
      // Applying it to read them must not leave its values behind.
      pixelSB->Apply();
      CheckRenderState("After applying the pixel state block", D3DRS_ALPHAREF,
                       (frame == 0) ? 0x42 : ((frame % 2) == 0 ? 0x20 : 0x10));
      const DWORD alphaRef = (frame % 2) == 0 ? 0x10 : 0x20;
      dev->SetRenderState(D3DRS_ALPHAREF, alphaRef);
      dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFF040506);
      pixelSB->Capture();
      CheckRenderState("After capturing the pixel state block", D3DRS_ALPHAREF, alphaRef);
      CheckRenderState("After capturing the pixel state block", D3DRS_TEXTUREFACTOR, 0xFF040506);

      // Reading a D3DSBT_ALL state block applies it, which sets every constant. The capture has to
      // put c200 back.
      dev->SetVertexShaderConstantF(200, sentinel, 1);
      allSB->Capture();
      CheckVSConstant("After capturing the D3DSBT_ALL state block", 200, sentinel);

      popMarker();

      // the texture locked across the previous Present, and GDI drawing
      SetDrawState();
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      dev->SetTexture(0, spanTex);
      DrawRect(200, 0, 264, 64, 0.5f, 0xFFFFFFFF);
      dev->SetTexture(0, NULL);

      dev->EndScene();

      // natively every depth should pass
      if(levelsDepth && frame == 0)
      {
        dev->GetRenderTargetData(levelsRT, levelsSys);
        D3DLOCKED_RECT locked = {};
        levelsSys->LockRect(&locked, NULL, D3DLOCK_READONLY);
        uint32_t green = 0;
        for(UINT y = 0; y < 64; y++)
          for(UINT x = 0; x < 64; x++)
            green += ((DWORD *)((byte *)locked.pBits + y * locked.Pitch))[x] == 0xFF00FF00 ? 1 : 0;
        levelsSys->UnlockRect();
        if(green != 64 * 64)
          TEST_ERROR("Only %u of the 4096 depths passed ZFUNC EQUAL natively", green);
        else
          TEST_LOG("All 4096 depths passed ZFUNC EQUAL natively");
      }

      HDC dc = NULL;
      if(SUCCEEDED(gdiSurf->GetDC(&dc)))
      {
        HBRUSH brush =
            CreateSolidBrush((frame % 2) == 0 ? RGB(0x12, 0x34, 0x56) : RGB(0x65, 0x43, 0x21));
        RECT rect = {0, 0, 16, 16};
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
        gdiSurf->ReleaseDC(dc);
      }
      else if(frame == 0)
      {
        TEST_WARN("GetDC failed on the system memory surface");
      }

      dev->GetFrontBufferData(0, frontCopy);

      // locked now, written, and unlocked after the Present
      D3DLOCKED_RECT locked = {};
      if(SUCCEEDED(spanTex->LockRect(0, &locked, NULL, 0)))
      {
        for(UINT y = 0; y < 4; y++)
          for(UINT x = 0; x < 4; x++)
            ((DWORD *)((byte *)locked.pBits + y * locked.Pitch))[x] = ParityColour();
        spanLocked = true;
      }

      Present();
      frame++;
    }

    if(spanLocked)
      spanTex->UnlockRect(0);

    return 0;
  }
};

REGISTER_TEST();
