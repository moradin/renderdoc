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
      "the fixed function pipeline with an FVF and DrawPrimitiveUP. A third, in the bottom right, "
      "has a pixel shader that reads a float, an int and a bool constant.";

  struct FFVertex
  {
    float x, y, z, rhw;
    D3DCOLOR col;
  };

  // reads c5 and c9, loops i0 times and branches on b3. The app sets i0 to 4, so the loop gives
  // 1.875 * c9 = (0.75, 0.375, 0), and with b3 TRUE that's multiplied by c5 = (0.5, 2, 1): the
  // triangle is (0.375, 0.75, 0), or 96, 191, 0 in the back buffer. If b3 were FALSE it would be
  // 191, 96, 0, and if the loop didn't run, black.
  const char *ConstantsPixel = R"EOSHADER(

float4 tint : register(c5);
float4 stepColour : register(c9);
bool useTint : register(b3);

float4 main() : COLOR0
{
  float4 ret = float4(0, 0, 0, 1);
  // 4 is the count the app sets in i0 (see CompileLoop)
  [loop] for(int i = 0; i < 4; i++)
    ret.rgb = ret.rgb * 0.5f + stepColour.rgb;
  if(useTint)
    ret.rgb *= tint.rgb;
  return ret;
}

)EOSHADER";

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

    IDirect3DPixelShader9Ptr constantsPS;
    const std::vector<DWORD> constantsTokens = CompileLoop(ConstantsPixel, "ps_3_0", 0);
    if(!constantsTokens.empty() &&
       FAILED(dev->CreatePixelShader(constantsTokens.data(), &constantsPS)))
      TEST_ERROR("CreatePixelShader failed for the constants shader");

    // in the bottom right, clear of the other two
    const DefaultA2V constantsTri[3] = {
        {Vec3f(0.5f, -0.9f, 0.0f), Vec4f(1.0f, 1.0f, 1.0f, 1.0f), Vec2f(0.0f, 0.0f)},
        {Vec3f(0.7f, -0.5f, 0.0f), Vec4f(1.0f, 1.0f, 1.0f, 1.0f), Vec2f(0.0f, 1.0f)},
        {Vec3f(0.9f, -0.9f, 0.0f), Vec4f(1.0f, 1.0f, 1.0f, 1.0f), Vec2f(1.0f, 0.0f)},
    };
    IDirect3DVertexBuffer9Ptr constantsVB = MakeVB(constantsTri, sizeof(constantsTri));

    const float tint[4] = {0.5f, 2.0f, 1.0f, 1.0f};
    const float stepColour[4] = {0.4f, 0.2f, 0.0f, 0.0f};
    const int loopCount[4] = {4, 0, 1, 0};
    const BOOL useTint = TRUE;

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

      if(constantsPS)
      {
        pushMarker("Pixel shader constants");
        dev->SetVertexDeclaration(defaultDecl);
        dev->SetStreamSource(0, constantsVB, 0, sizeof(DefaultA2V));
        dev->SetVertexShader(vs);
        dev->SetPixelShader(constantsPS);
        dev->SetPixelShaderConstantF(5, tint, 1);
        dev->SetPixelShaderConstantF(9, stepColour, 1);
        dev->SetPixelShaderConstantI(0, loopCount, 1);
        dev->SetPixelShaderConstantB(3, &useTint, 1);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
        popMarker();
      }

      dev->EndScene();

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
