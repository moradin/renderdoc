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

RD_TEST(D3D9_Frame_Start_State, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Sets distinctive device state once before the first frame, on a pure device where possible, "
      "then draws the same two triangles as D3D9_Simple_Triangle every frame. A capture's "
      "BeginCaptureFrame chunk should hold the state set here. None of it changes what's drawn. "
      "Also checks that shader constants above c127 survive Present, where that can be read.";

  struct FFVertex
  {
    float x, y, z, rhw;
    D3DCOLOR col;
  };

  const float UpperVS[4] = {13.0f, 14.0f, 15.0f, 16.0f};
  const float UpperPS[4] = {17.0f, 18.0f, 19.0f, 20.0f};

  // checks that nothing between frames has changed the shader constants above c127, or copied
  // lower constants over them. Reading constants back isn't guaranteed to work on a pure device, so
  // the check is skipped if it fails. Some runtimes do return them. Under RenderDoc the real device
  // is never pure, so it always runs there.
  bool upperConstantsReported = false;

  void CheckUpperConstants()
  {
    if(upperConstantsReported)
      return;

    float vs200[4] = {}, vs138[4] = {}, ps180[4] = {}, ps148[4] = {};
    if(FAILED(dev->GetVertexShaderConstantF(200, vs200, 1)) ||
       FAILED(dev->GetVertexShaderConstantF(138, vs138, 1)) ||
       FAILED(dev->GetPixelShaderConstantF(180, ps180, 1)) ||
       FAILED(dev->GetPixelShaderConstantF(148, ps148, 1)))
      return;

    const float zero[4] = {};
    if(memcmp(vs200, UpperVS, sizeof(vs200)) != 0 || memcmp(vs138, zero, sizeof(vs138)) != 0 ||
       memcmp(ps180, UpperPS, sizeof(ps180)) != 0 || memcmp(ps148, zero, sizeof(ps148)) != 0)
    {
      TEST_ERROR(
          "Shader constants above c127 changed between frames: VS c200 %f, c138 %f, "
          "PS c180 %f, c148 %f",
          vs200[0], vs138[0], ps180[0], ps148[0]);
    }
    else
    {
      TEST_LOG("Shader constants above c127 survived the first frames");
    }
    upperConstantsReported = true;
  }

  static DWORD FloatBits(float f)
  {
    DWORD ret;
    memcpy(&ret, &f, sizeof(ret));
    return ret;
  }

  static D3DMATRIX Scale(float s)
  {
    D3DMATRIX m = {};
    m._11 = m._22 = m._33 = s;
    m._44 = 1.0f;
    return m;
  }

  // the state to look for in a capture. None of it affects the two draws: fog and the scissor test
  // stay disabled, the fixed function triangle is pre-transformed and unlit, and the shaders don't
  // read these constants.
  void SetFrameStartState()
  {
    dev->SetRenderState(D3DRS_FOGCOLOR, 0x00ABCDEF);
    dev->SetRenderState(D3DRS_FOGSTART, FloatBits(0.25f));
    dev->SetRenderState(D3DRS_AMBIENT, 0x00102030);
    dev->SetRenderState(D3DRS_STENCILREF, 0x5A);

    dev->SetSamplerState(3, D3DSAMP_MAXANISOTROPY, 7);
    dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER1, D3DSAMP_ADDRESSU, D3DTADDRESS_MIRROR);

    dev->SetTextureStageState(5, D3DTSS_TEXCOORDINDEX, 3);

    D3DMATRIX view = Scale(1.0f);
    view._41 = 1.0f;
    view._42 = 2.0f;
    view._43 = 3.0f;
    dev->SetTransform(D3DTS_VIEW, &view);
    D3DMATRIX world5 = Scale(2.0f);
    dev->SetTransform(D3DTS_WORLDMATRIX(5), &world5);

    RECT scissor = {1, 2, 3, 4};
    dev->SetScissorRect(&scissor);

    D3DMATERIAL9 mat = {};
    mat.Diffuse = {0.1f, 0.2f, 0.3f, 0.4f};
    mat.Power = 5.0f;
    dev->SetMaterial(&mat);

    D3DLIGHT9 light = {};
    light.Type = D3DLIGHT_POINT;
    light.Diffuse = {1.0f, 0.5f, 0.25f, 1.0f};
    light.Position = {10.0f, 20.0f, 30.0f};
    light.Range = 100.0f;
    light.Attenuation0 = 1.0f;
    dev->SetLight(7, &light);
    dev->LightEnable(7, TRUE);

    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Direction = {0.0f, -1.0f, 0.0f};
    dev->SetLight(42, &light);

    const float plane[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    dev->SetClipPlane(1, plane);

    const float vsF[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const int vsI[4] = {5, 6, 7, 8};
    const BOOL vsB = TRUE;
    dev->SetVertexShaderConstantF(10, vsF, 1);
    dev->SetVertexShaderConstantI(3, vsI, 1);
    dev->SetVertexShaderConstantB(2, &vsB, 1);

    const float psF[4] = {9.0f, 10.0f, 11.0f, 12.0f};
    dev->SetPixelShaderConstantF(20, psF, 1);

    // constants above c127, which RenderDoc's overlay used to overwrite every frame
    dev->SetVertexShaderConstantF(200, UpperVS, 1);
    dev->SetPixelShaderConstantF(180, UpperPS, 1);

    // palettes need driver support, so these can fail
    PALETTEENTRY pal[256] = {};
    pal[1] = {0x11, 0x22, 0x33, 0xFF};
    dev->SetPaletteEntries(3, pal);
    dev->SetCurrentTexturePalette(3);
  }

  int main()
  {
    extraBehaviorFlags = D3DCREATE_PUREDEVICE;

    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    if(behaviorFlags & D3DCREATE_PUREDEVICE)
    {
      // RenderDoc creates the real device without the flag, but must still report it
      D3DDEVICE_CREATION_PARAMETERS params = {};
      dev->GetCreationParameters(&params);
      if(params.BehaviorFlags & D3DCREATE_PUREDEVICE)
        TEST_LOG("Running on a pure device");
      else
        TEST_ERROR("GetCreationParameters lost D3DCREATE_PUREDEVICE");
    }
    else
    {
      TEST_LOG("Pure device not available, running on a software vertex processing device");
    }

    ID3DBlobPtr vsblob = Compile(D3D9DefaultVertex, "main", "vs_3_0");
    ID3DBlobPtr psblob = Compile(D3D9DefaultPixel, "main", "ps_3_0");

    IDirect3DVertexShader9Ptr vs = CreateVS(vsblob);
    IDirect3DPixelShader9Ptr ps = CreatePS(psblob);

    IDirect3DVertexBuffer9Ptr vb = MakeVB(DefaultTri, sizeof(DefaultTri));

    const float w = (float)screenWidth, h = (float)screenHeight;
    const FFVertex ffTri[3] = {
        {w * 0.05f, h * 0.95f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 0, 255)},
        {w * 0.15f, h * 0.75f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 0, 255)},
        {w * 0.25f, h * 0.95f, 0.5f, 1.0f, D3DCOLOR_XRGB(255, 0, 255)},
    };

    SetFrameStartState();

    while(Running())
    {
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      dev->BeginScene();

      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);

      dev->SetVertexDeclaration(defaultDecl);
      dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
      dev->SetVertexShader(vs);
      dev->SetPixelShader(ps);
      dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);

      dev->SetVertexShader(NULL);
      dev->SetPixelShader(NULL);
      dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
      dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
      dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, ffTri, sizeof(FFVertex));

      dev->EndScene();

      Present();

      // RenderDoc's overlay draws during the first Present, check once a few frames have gone by
      if(curFrame == 5)
        CheckUpperConstants();
    }

    return 0;
  }
};

REGISTER_TEST();
