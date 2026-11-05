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

RD_TEST(D3D9_Reset_Capture, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Resets the device in the middle of captured frames. Frame 30 is captured with the in-app "
      "API and reset half way through, which should restart the capture after the reset. Frame "
      "60 is captured by triggering a capture on frame 59 and is also reset half way through, "
      "which should discard it and capture frame 61 instead.";

  void DrawTriangle(IDirect3DVertexBuffer9 * vb, IDirect3DVertexShader9 * vs,
                    IDirect3DPixelShader9 * ps)
  {
    // a reset sets everything back to defaults, so set all of it
    dev->BeginScene();
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetVertexDeclaration(defaultDecl);
    dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
    dev->SetVertexShader(vs);
    dev->SetPixelShader(ps);
    dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    dev->EndScene();
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    if(devEx)
    {
      TEST_ERROR("This test needs a non-Ex device");
      return 4;
    }

    if(!rdoc)
      TEST_WARN("Not running under RenderDoc, nothing will be captured");

    ID3DBlobPtr vsblob = Compile(D3D9DefaultVertex, "main", "vs_3_0");
    ID3DBlobPtr psblob = Compile(D3D9DefaultPixel, "main", "ps_3_0");
    IDirect3DVertexShader9Ptr vs = CreateVS(vsblob);
    IDirect3DPixelShader9Ptr ps = CreatePS(psblob);

    // managed, so it survives the resets
    IDirect3DVertexBuffer9Ptr vb = MakeVB(DefaultTri, sizeof(DefaultTri));

    uint32_t frame = 0;

    while(Running())
    {
      const bool resetFrame = (frame == 30 || frame == 60);

      if(frame == 30 && rdoc)
        rdoc->StartFrameCapture(NULL, NULL);
      if(frame == 59 && rdoc)
        rdoc->TriggerCapture();

      pushMarker(resetFrame ? "Before the reset" : "Frame");

      // red before the reset, blue after
      dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(0.6f, 0.1f, 0.1f, 1.0f), 1.0f, 0);
      DrawTriangle(vb, vs, ps);

      popMarker();

      if(resetFrame)
      {
        D3DPRESENT_PARAMETERS params = presentParams;
        HRESULT hr = dev->Reset(&params);
        if(FAILED(hr))
          TEST_ERROR("Reset failed: %x", hr);

        pushMarker("After the reset");
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(0.1f, 0.1f, 0.6f, 1.0f), 1.0f, 0);
        DrawTriangle(vb, vs, ps);
        popMarker();
      }

      if(frame == 30 && rdoc)
      {
        if(!rdoc->EndFrameCapture(NULL, NULL))
          TEST_ERROR("EndFrameCapture failed for the capture around the reset");
      }

      Present();
      frame++;
    }

    return 0;
  }
};

REGISTER_TEST();
