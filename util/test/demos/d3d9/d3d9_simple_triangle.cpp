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

RD_TEST(D3D9_Simple_Triangle, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Just draws a simple triangle, using shaders and a vertex declaration, and a second one "
      "using "
      "the fixed function pipeline with an FVF and DrawPrimitiveUP.";

  struct FFVertex
  {
    float x, y, z, rhw;
    D3DCOLOR col;
  };

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3DBlobPtr vsblob = Compile(D3D9DefaultVertex, "main", "vs_3_0");
    ID3DBlobPtr psblob = Compile(D3D9DefaultPixel, "main", "ps_3_0");

    IDirect3DVertexShader9Ptr vs = CreateVS(vsblob);
    IDirect3DPixelShader9Ptr ps = CreatePS(psblob);

    IDirect3DVertexBuffer9Ptr vb = MakeVB(DefaultTri, sizeof(DefaultTri));

    // pre-transformed vertices in the bottom-left corner
    const float w = (float)screenWidth, h = (float)screenHeight;
    const FFVertex ffTri[3] = {
        {w * 0.05f, h * 0.95f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 0, 255)},
        {w * 0.15f, h * 0.75f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 0, 255)},
        {w * 0.25f, h * 0.95f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 0, 255)},
    };

    while(Running())
    {
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      dev->BeginScene();

      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);

      pushMarker("Shader triangle");
      dev->SetVertexDeclaration(defaultDecl);
      dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
      dev->SetVertexShader(vs);
      dev->SetPixelShader(ps);
      dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
      popMarker();

      pushMarker("Fixed function triangle");
      dev->SetVertexShader(NULL);
      dev->SetPixelShader(NULL);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
      dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
      dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, ffTri, sizeof(FFVertex));
      popMarker();

      dev->EndScene();

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
