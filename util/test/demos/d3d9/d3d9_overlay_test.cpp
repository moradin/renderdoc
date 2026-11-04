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

// The layout, in fractions of the window (util/test/d3d9_overlay_check.py uses the same numbers):
// - "Setup": the top half (y < 0.5) gets depth 0.5 and stencil 1, the bottom half keeps the
//   clear's depth 1.0 and stencil 0.
// - "Depth Test": a vs_3_0 quad at x 0.05-0.3, y 0.1-0.9, depth 0.7, ZFUNC LESS. Fails in the top
//   half, passes in the bottom.
// - "Depth Bias": a fixed function quad at x 0.35-0.6, y 0.1-0.9, depth 0.45 with D3DRS_DEPTHBIAS
//   0.1, ZFUNC LESS. Fails in the top half only because of the bias.
// - "Stencil Test": a fixed function quad at x 0.65-0.95, y 0.1-0.9, depth test off, STENCILFUNC
//   EQUAL 1. Passes in the top half, fails in the bottom.
// - "Backface Cull": one draw of two triangles with D3DCULL_CCW: a clockwise one around
//   (0.25, 0.8), kept, and a counter-clockwise one around (0.75, 0.8), culled.
// - "Viewport Scissor": a vs_3_0 quad covering the viewport, which is the bottom right quarter
//   (x 0.5-1, y 0.5-1), with the scissor rect at x 0.6-0.9, y 0.6-0.9.
// - "MSAA": a 256x256 4x multisampled target and depth stencil, cleared, then a fixed function
//   triangle (128,20) (236,236) (20,236) at depth 0.5 with ZFUNC LESS. Skipped without 4x MSAA.
RD_TEST(D3D9_Overlay_Test, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Draws for checking the texture viewer's overlays: a depth test that fails and passes, one "
      "that only fails because of the depth bias, a stencil test, backface culling, a viewport "
      "with a scissor rect, and a multisampled target. Each is in its own marker.";

  struct FFVertex
  {
    float x, y, z, rhw;
    D3DCOLOR col;
  };

  static const DWORD FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

  // a rectangle in fractions of the window, as a triangle strip
  void FFRect(float x0, float y0, float x1, float y1, float z, D3DCOLOR col)
  {
    const float w = (float)screenWidth, h = (float)screenHeight;
    // pixel centres are at integer coordinates
    const FFVertex verts[4] = {
        {x0 * w - 0.5f, y0 * h - 0.5f, z, 1.0f, col},
        {x1 * w - 0.5f, y0 * h - 0.5f, z, 1.0f, col},
        {x0 * w - 0.5f, y1 * h - 0.5f, z, 1.0f, col},
        {x1 * w - 0.5f, y1 * h - 0.5f, z, 1.0f, col},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(FFVertex));
  }

  void SetFixedFunction()
  {
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetFVF(FVF);
  }

  // a quad in clip space, as a triangle list
  static void ClipQuad(DefaultA2V * verts, float x0, float y0, float x1, float y1, float z, Vec4f col)
  {
    verts[0] = {Vec3f(x0, y0, z), col, Vec2f(0.0f, 0.0f)};
    verts[1] = {Vec3f(x1, y0, z), col, Vec2f(1.0f, 0.0f)};
    verts[2] = {Vec3f(x0, y1, z), col, Vec2f(0.0f, 1.0f)};
    verts[3] = {Vec3f(x1, y0, z), col, Vec2f(1.0f, 0.0f)};
    verts[4] = {Vec3f(x1, y1, z), col, Vec2f(1.0f, 1.0f)};
    verts[5] = {Vec3f(x0, y1, z), col, Vec2f(0.0f, 1.0f)};
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3DBlobPtr vsblob = Compile(D3D9DefaultVertex, "main", "vs_3_0");
    ID3DBlobPtr psblob = Compile(D3D9DefaultPixel, "main", "ps_3_0");

    IDirect3DVertexShader9Ptr vs = CreateVS(vsblob);
    IDirect3DPixelShader9Ptr ps = CreatePS(psblob);

    // the depth test quad (x 0.05-0.3, y 0.1-0.9 in the window), then a quad covering the viewport
    DefaultA2V quads[12];
    ClipQuad(quads, -0.9f, 0.8f, -0.4f, -0.8f, 0.7f, Vec4f(0.0f, 0.5f, 0.0f, 1.0f));
    ClipQuad(quads + 6, -1.0f, 1.0f, 1.0f, -1.0f, 0.5f, Vec4f(0.5f, 0.5f, 0.0f, 1.0f));
    IDirect3DVertexBuffer9Ptr vb = MakeVB(quads, sizeof(quads));

    const bool msaaSupported =
        SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8,
                                                  TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL)) &&
        SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24S8,
                                                  TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL));

    IDirect3DSurface9Ptr backBuffer, autoDepth;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    dev->GetDepthStencilSurface(&autoDepth);

    IDirect3DSurface9Ptr msaaRT, msaaDepth;
    if(msaaSupported)
    {
      dev->CreateRenderTarget(256, 256, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE,
                              &msaaRT, NULL);
      dev->CreateDepthStencilSurface(256, 256, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE,
                                     &msaaDepth, NULL);
    }
    if(!msaaRT || !msaaDepth)
      TEST_WARN("4x MSAA isn't supported for A8R8G8B8 and D24S8, skipping it");

    const float w = (float)screenWidth, h = (float)screenHeight;

    float depthBias = 0.1f;
    DWORD depthBiasBits;
    memcpy(&depthBiasBits, &depthBias, sizeof(depthBiasBits));

    while(Running())
    {
      dev->SetRenderTarget(0, backBuffer);
      dev->SetDepthStencilSurface(autoDepth);

      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      dev->BeginScene();

      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
      dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

      pushMarker("Setup");
      SetFixedFunction();
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
      dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
      dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
      dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
      dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
      dev->SetRenderState(D3DRS_STENCILREF, 1);
      FFRect(0.0f, 0.0f, 1.0f, 0.5f, 0.5f, D3DCOLOR_COLORVALUE(0.1f, 0.1f, 0.1f, 1.0f));
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
      popMarker();

      pushMarker("Depth Test");
      dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
      dev->SetVertexDeclaration(defaultDecl);
      dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
      dev->SetVertexShader(vs);
      dev->SetPixelShader(ps);
      dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
      popMarker();

      pushMarker("Depth Bias");
      SetFixedFunction();
      dev->SetRenderState(D3DRS_DEPTHBIAS, depthBiasBits);
      FFRect(0.35f, 0.1f, 0.6f, 0.9f, 0.45f, D3DCOLOR_COLORVALUE(0.0f, 0.0f, 0.5f, 1.0f));
      dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
      popMarker();

      pushMarker("Stencil Test");
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
      dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
      dev->SetRenderState(D3DRS_STENCILREF, 1);
      FFRect(0.65f, 0.1f, 0.95f, 0.9f, 0.5f, D3DCOLOR_COLORVALUE(0.5f, 0.0f, 0.5f, 1.0f));
      dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      popMarker();

      pushMarker("Backface Cull");
      {
        const D3DCOLOR col = D3DCOLOR_COLORVALUE(0.0f, 0.5f, 0.5f, 1.0f);
        // clockwise on screen (y goes down), so a front face, then counter-clockwise
        const FFVertex tris[6] = {
            {0.25f * w, 0.6f * h, 0.5f, 1.0f, col}, {0.4f * w, 0.9f * h, 0.5f, 1.0f, col},
            {0.1f * w, 0.9f * h, 0.5f, 1.0f, col},  {0.75f * w, 0.6f * h, 0.5f, 1.0f, col},
            {0.6f * w, 0.9f * h, 0.5f, 1.0f, col},  {0.9f * w, 0.9f * h, 0.5f, 1.0f, col},
        };
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, tris, sizeof(FFVertex));
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      }
      popMarker();

      pushMarker("Viewport Scissor");
      {
        const D3DVIEWPORT9 view = {
            DWORD(0.5f * w), DWORD(0.5f * h), DWORD(0.5f * w), DWORD(0.5f * h), 0.0f, 1.0f};
        const RECT scissor = {LONG(0.6f * w), LONG(0.6f * h), LONG(0.9f * w), LONG(0.9f * h)};
        dev->SetViewport(&view);
        dev->SetScissorRect(&scissor);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        dev->SetVertexDeclaration(defaultDecl);
        dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, 6, 2);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        const D3DVIEWPORT9 full = {0, 0, (DWORD)screenWidth, (DWORD)screenHeight, 0.0f, 1.0f};
        dev->SetViewport(&full);
      }
      popMarker();

      if(msaaRT && msaaDepth)
      {
        pushMarker("MSAA");
        dev->SetRenderTarget(0, msaaRT);
        dev->SetDepthStencilSurface(msaaDepth);
        dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                   D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);
        SetFixedFunction();
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        const D3DCOLOR col = D3DCOLOR_COLORVALUE(1.0f, 0.5f, 0.0f, 1.0f);
        const FFVertex tri[3] = {
            {128.0f, 20.0f, 0.5f, 1.0f, col},
            {236.0f, 236.0f, 0.5f, 1.0f, col},
            {20.0f, 236.0f, 0.5f, 1.0f, col},
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(FFVertex));
        dev->SetRenderTarget(0, backBuffer);
        dev->SetDepthStencilSurface(autoDepth);
        popMarker();
      }

      dev->EndScene();

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
