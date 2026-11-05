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

#include "d3d11_test.h"

RD_TEST(D3D11_D3DPERF_Markers, D3D11GraphicsTest)
{
  static constexpr const char *Description =
      "Draws three triangles inside markers made with d3d9.dll's D3DPERF functions, with no D3D9 "
      "device, and a fourth inside an ID3DUserDefinedAnnotation region to compare with. "
      "RenderDoc's D3D11 driver hooks the D3DPERF functions and records them as D3D11 markers.";

  typedef int(WINAPI * PFN_BEGIN_EVENT)(DWORD, LPCWSTR);
  typedef int(WINAPI * PFN_END_EVENT)();
  typedef void(WINAPI * PFN_SET_MARKER)(DWORD, LPCWSTR);

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    PFN_BEGIN_EVENT perfBegin =
        d3d9 ? (PFN_BEGIN_EVENT)GetProcAddress(d3d9, "D3DPERF_BeginEvent") : NULL;
    PFN_END_EVENT perfEnd = d3d9 ? (PFN_END_EVENT)GetProcAddress(d3d9, "D3DPERF_EndEvent") : NULL;
    PFN_SET_MARKER perfMarker =
        d3d9 ? (PFN_SET_MARKER)GetProcAddress(d3d9, "D3DPERF_SetMarker") : NULL;

    if(!perfBegin || !perfEnd || !perfMarker)
    {
      TEST_ERROR("Couldn't get the D3DPERF functions from d3d9.dll");
      return 4;
    }

    ID3DBlobPtr vsblob = Compile(D3DDefaultVertex, "main", "vs_4_0");
    ID3DBlobPtr psblob = Compile(D3DDefaultPixel, "main", "ps_4_0");

    CreateDefaultInputLayout(vsblob);

    ID3D11VertexShaderPtr vs = CreateVS(vsblob);
    ID3D11PixelShaderPtr ps = CreatePS(psblob);

    // four small triangles side by side, left to right in the order they're drawn
    DefaultA2V tris[12];
    for(int t = 0; t < 4; t++)
    {
      for(int v = 0; v < 3; v++)
      {
        tris[t * 3 + v] = DefaultTri[v];
        tris[t * 3 + v].pos.x = DefaultTri[v].pos.x * 0.2f - 0.75f + t * 0.5f;
        tris[t * 3 + v].pos.y = DefaultTri[v].pos.y * 0.4f;
      }
    }

    ID3D11BufferPtr vb = MakeBuffer().Vertex().Data(tris);

    bool loggedReturns = false;

    while(Running())
    {
      ClearRenderTargetView(bbRTV, {0.2f, 0.2f, 0.2f, 1.0f});

      IASetVertexBuffer(vb, sizeof(DefaultA2V), 0);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->IASetInputLayout(defaultLayout);

      ctx->VSSetShader(vs, NULL, 0);
      ctx->PSSetShader(ps, NULL, 0);

      RSSetViewport({0.0f, 0.0f, (float)screenWidth, (float)screenHeight, 0.0f, 1.0f});

      ctx->OMSetRenderTargets(1, &bbRTV.GetInterfacePtr(), NULL);

      // The capture's action tree should be:
      //   D3DPERF region
      //     Draw (first triangle)
      //     D3DPERF marker
      //     D3DPERF nested region
      //       Draw (second)
      //   Draw (third, outside any region)
      //   Annotation region
      //     Draw (fourth)
      int begin0 = perfBegin(0xffff0000, L"D3DPERF region");
      ctx->Draw(3, 0);
      perfMarker(0xff00ff00, L"D3DPERF marker");
      int begin1 = perfBegin(0xff0000ff, L"D3DPERF nested region");
      ctx->Draw(3, 3);
      int end1 = perfEnd();
      int end0 = perfEnd();

      ctx->Draw(3, 6);

      pushMarker("Annotation region");
      ctx->Draw(3, 9);
      popMarker();

      // D3D11_Refcount_Check relies on what these return, so the log shows them to compare a
      // native run with one under RenderDoc
      if(!loggedReturns)
      {
        TEST_LOG(
            "D3DPERF returns: BeginEvent %d, nested BeginEvent %d, nested EndEvent %d, "
            "EndEvent %d",
            begin0, begin1, end1, end0);
        loggedReturns = true;
      }

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
