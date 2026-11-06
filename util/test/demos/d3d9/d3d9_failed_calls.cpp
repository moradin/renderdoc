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

RD_TEST(D3D9_Failed_Calls, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Makes calls that the D3D9 runtime might reject, each inside a marker of its own, between "
      "ordinary calls that succeed. On the first frame each call's result is written to "
      "D3D9_Failed_Calls_reference.txt in its tmp folder (_reference_rdoc.txt under RenderDoc), "
      "so that RenderDoc's debug messages can be checked against what failed natively.";

  struct Result
  {
    const char *marker;
    const char *call;
    HRESULT hr;
    bool changed;
  };

  std::vector<Result> results;

  // run one call inside a marker of its own, and keep its result
  template <typename F>
  void Case(const char *marker, const char *call, F f)
  {
    pushMarker(marker);
    HRESULT hr = f();
    popMarker();

    if(curFrame == 0)
    {
      results.push_back({marker, call, hr, false});
    }
    else
    {
      // the result should be the same on every frame, since the capture is of a later one
      for(Result &r : results)
      {
        if(!strcmp(r.marker, marker) && r.hr != hr && !r.changed)
        {
          TEST_WARN("%s: %s returned %08X on frame 0, %08X on frame %d", marker, call,
                    (uint32_t)r.hr, (uint32_t)hr, curFrame);
          r.changed = true;
        }
      }
    }
  }

  void WriteReference()
  {
    const std::string path = tmpPath + (rdoc ? "/D3D9_Failed_Calls_reference_rdoc.txt"
                                             : "/D3D9_Failed_Calls_reference.txt");
    FILE *f = NULL;
    fopen_s(&f, path.c_str(), "w");
    if(!f)
    {
      TEST_ERROR("Couldn't write %s", path.c_str());
      return;
    }
    for(const Result &r : results)
    {
      fprintf(f, "%s\t%s\t%08X\n", r.marker, r.call, (uint32_t)r.hr);
      TEST_LOG("%s: %s returned %08X", r.marker, r.call, (uint32_t)r.hr);
    }
    fclose(f);
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3DBlobPtr vsblob = Compile(D3D9DefaultVertex, "main", "vs_3_0");
    ID3DBlobPtr psblob = Compile(D3D9DefaultPixel, "main", "ps_3_0");
    if(!vsblob || !psblob)
      return 4;

    IDirect3DVertexShader9Ptr vs = CreateVS(vsblob);
    IDirect3DPixelShader9Ptr ps = CreatePS(psblob);
    IDirect3DVertexBuffer9Ptr vb = MakeVB(DefaultTri, sizeof(DefaultA2V) * 3);

    // two render targets of different sizes
    IDirect3DSurface9Ptr rtWide, rtSmall;
    dev->CreateRenderTarget(64, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rtWide, NULL);
    dev->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rtSmall, NULL);
    // and one as wide as rtWide but taller
    IDirect3DSurface9Ptr rtTall;
    dev->CreateRenderTarget(64, 36, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rtTall, NULL);

    // a depth stencil smaller than rtWide
    IDirect3DSurface9Ptr smallDepth;
    dev->CreateDepthStencilSurface(32, 16, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &smallDepth,
                                   NULL);

    // a default pool texture that isn't a render target, and its surface
    IDirect3DTexture9Ptr plainTex;
    IDirect3DSurface9Ptr plainSurf;
    dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &plainTex, NULL);
    if(plainTex)
      plainTex->GetSurfaceLevel(0, &plainSurf);

    // a second default pool texture, as UpdateTexture's destination
    IDirect3DTexture9Ptr plainTex2;
    dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &plainTex2, NULL);

    // a managed texture with one level, and its surface
    IDirect3DTexture9Ptr managedTex;
    IDirect3DSurface9Ptr managedSurf;
    dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managedTex, NULL);
    if(managedTex)
      managedTex->GetSurfaceLevel(0, &managedSurf);

    // a system memory surface smaller than rtWide
    IDirect3DSurface9Ptr sysSurf;
    dev->CreateOffscreenPlainSurface(32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysSurf, NULL);

    if(!rtWide || !rtSmall || !rtTall || !smallDepth || !plainSurf || !plainTex2 || !managedSurf ||
       !sysSurf)
    {
      TEST_ERROR("Couldn't create the resources");
      return 4;
    }

    IDirect3DSurface9Ptr backbuffer, depthstencil;
    dev->GetRenderTarget(0, &backbuffer);
    dev->GetDepthStencilSurface(&depthstencil);

    const float constant[4] = {1.0f, 0.0f, 0.0f, 1.0f};

    while(Running())
    {
      dev->SetRenderTarget(0, backbuffer);
      dev->SetDepthStencilSurface(depthstencil);
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(51, 51, 51), 1.0f, 0);

      // natively neither of these fails
      Case("Clear with a smaller second target", "Clear", [&]() {
        dev->SetRenderTarget(0, rtWide);
        dev->SetRenderTarget(1, rtSmall);
        dev->SetDepthStencilSurface(NULL);
        HRESULT hr = dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0);
        dev->SetRenderTarget(1, NULL);
        return hr;
      });

      Case("Clear with a bigger second target", "Clear", [&]() {
        dev->SetRenderTarget(0, rtSmall);
        dev->SetRenderTarget(1, rtWide);
        dev->SetDepthStencilSurface(NULL);
        HRESULT hr = dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0);
        dev->SetRenderTarget(1, NULL);
        return hr;
      });

      Case("Clear with a taller second target", "Clear", [&]() {
        dev->SetRenderTarget(0, rtWide);
        dev->SetRenderTarget(1, rtTall);
        dev->SetDepthStencilSurface(NULL);
        HRESULT hr = dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0);
        dev->SetRenderTarget(1, NULL);
        return hr;
      });

      Case("Clear with a smaller depth stencil", "Clear", [&]() {
        dev->SetRenderTarget(0, rtWide);
        dev->SetDepthStencilSurface(smallDepth);
        return dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(255, 0, 0),
                          1.0f, 0);
      });

      Case("SetRenderTarget index 4", "SetRenderTarget",
           [&]() { return dev->SetRenderTarget(4, rtWide); });

      Case("SetRenderTarget without render target usage", "SetRenderTarget",
           [&]() { return dev->SetRenderTarget(0, plainSurf); });

      Case("SetDepthStencilSurface with a colour target", "SetDepthStencilSurface",
           [&]() { return dev->SetDepthStencilSurface(rtSmall); });

      dev->SetRenderTarget(0, backbuffer);
      dev->SetDepthStencilSurface(depthstencil);

      // the runtime doesn't check a stage, sampler, render state or stream number that's out of
      // range: SetTexture(300, ...) and the like return S_OK, and a native run that made them
      // ended with heap corruption. An unaligned stream offset isn't rejected either, and
      // DrawIndexedPrimitive without an index buffer fails on the first frame and returns S_OK
      // after that.

      Case("SetVertexShaderConstantF past the end", "SetVertexShaderConstantF",
           [&]() { return dev->SetVertexShaderConstantF(300, constant, 1); });

      Case("SetPixelShaderConstantF past the end", "SetPixelShaderConstantF",
           [&]() { return dev->SetPixelShaderConstantF(224, constant, 1); });

      Case("EndScene without BeginScene", "EndScene", [&]() { return dev->EndScene(); });

      Case("BeginScene twice", "BeginScene", [&]() {
        dev->BeginScene();
        return dev->BeginScene();
      });

      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetVertexDeclaration(defaultDecl);
      dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
      dev->SetVertexShader(vs);
      dev->SetPixelShader(ps);
      pushMarker("Triangle");
      dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
      popMarker();

      dev->EndScene();

      Case("StretchRect outside the source", "StretchRect", [&]() {
        RECT src = {0, 0, 128, 128};
        return dev->StretchRect(rtWide, &src, rtSmall, NULL, D3DTEXF_NONE);
      });

      Case("ColorFill on a managed texture", "ColorFill",
           [&]() { return dev->ColorFill(managedSurf, NULL, D3DCOLOR_XRGB(0, 255, 0)); });

      Case("UpdateTexture between default pool textures", "UpdateTexture",
           [&]() { return dev->UpdateTexture(plainTex, plainTex2); });

      Case("GetRenderTargetData into a different size", "GetRenderTargetData",
           [&]() { return dev->GetRenderTargetData(rtWide, sysSurf); });

      Case("LockRect on a missing level", "LockRect", [&]() {
        D3DLOCKED_RECT lr = {};
        return managedTex->LockRect(3, &lr, NULL, 0);
      });

      // natively this succeeds
      Case("UnlockRect without a lock", "UnlockRect", [&]() { return managedTex->UnlockRect(0); });

      Case("CreateTexture with zero size", "CreateTexture", [&]() {
        IDirect3DTexture9Ptr tex;
        return dev->CreateTexture(0, 0, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
      });

      if(curFrame == 0)
        WriteReference();

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
