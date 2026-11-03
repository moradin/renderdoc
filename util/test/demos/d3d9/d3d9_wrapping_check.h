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

#pragma once

#include "d3d9_test.h"

// Checks object identity and lifetime rules that applications rely on. Run it natively and under
// RenderDoc: the CHECKs must pass in both, and the OBSERVE lines should print the same values.
// The window clears to green if every check passed and red otherwise.
struct D3D9WrappingCheckBase : D3D9GraphicsTest
{
  bool failed = false;
  D3DPOOL texPool = D3DPOOL_MANAGED;

#define CHECK(cond)                         \
  do                                        \
  {                                         \
    if(!(cond))                             \
    {                                       \
      if(!failed)                           \
        DEBUG_BREAK();                      \
      failed = true;                        \
      TEST_WARN("Check failed: %s", #cond); \
    }                                       \
  } while(0)

#define CHECK_HR(expr)                             \
  do                                               \
  {                                                \
    HRESULT check_hr = (expr);                     \
    if(FAILED(check_hr))                           \
    {                                              \
      if(!failed)                                  \
        DEBUG_BREAK();                             \
      failed = true;                               \
      TEST_WARN("%s failed: %x", #expr, check_hr); \
    }                                              \
  } while(0)

// behaviour we expect but that isn't clearly documented: log it so native and RenderDoc runs can
// be compared
#define OBSERVE(fmt, ...) TEST_LOG("Observation: " fmt, ##__VA_ARGS__)

  IDirect3DTexture9Ptr MakeTex(UINT w, UINT h, UINT levels = 1)
  {
    IDirect3DTexture9Ptr tex;
    CHECK_HR(dev->CreateTexture(w, h, levels, 0, D3DFMT_A8R8G8B8, texPool, &tex, NULL));
    return tex;
  }

  void CheckDeviceObjects()
  {
    TEST_LOG("Checking device-owned objects");

    IDirect3DSurface9Ptr bb1, bb2, rt, ds1, ds2;
    CHECK_HR(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb1));
    CHECK_HR(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb2));
    CHECK(bb1 && bb1 == bb2);

    CHECK_HR(dev->GetRenderTarget(0, &rt));
    CHECK(rt == bb1);

    CHECK_HR(dev->GetDepthStencilSurface(&ds1));
    CHECK_HR(dev->GetDepthStencilSurface(&ds2));
    CHECK(ds1 && ds1 == ds2);

    IDirect3DSwapChain9Ptr swap;
    CHECK_HR(dev->GetSwapChain(0, &swap));
    IDirect3DSurface9Ptr bb3;
    if(swap)
      CHECK_HR(swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb3));
    CHECK(bb3 == bb1);

    // the back buffer's container is its swapchain
    IDirect3DSwapChain9Ptr container;
    if(bb1)
      CHECK_HR(bb1->GetContainer(__uuidof(IDirect3DSwapChain9), (void **)&container));
    CHECK(container == swap);

    IDirect3DDevice9Ptr devFromSwap, devFromSurf;
    if(swap)
      CHECK_HR(swap->GetDevice(&devFromSwap));
    if(bb1)
      CHECK_HR(bb1->GetDevice(&devFromSurf));
    CHECK(devFromSwap == dev);
    CHECK(devFromSurf == dev);

    // COM identity
    IUnknown *unk1 = NULL, *unk2 = NULL;
    dev->QueryInterface(__uuidof(IUnknown), (void **)&unk1);
    dev->QueryInterface(__uuidof(IUnknown), (void **)&unk2);
    CHECK(unk1 && unk1 == unk2);
    if(unk1)
      unk1->Release();
    if(unk2)
      unk2->Release();

    IDirect3DDevice9Ptr devQI;
    CHECK_HR(dev->QueryInterface(__uuidof(IDirect3DDevice9), (void **)&devQI));
    CHECK(devQI == dev);

    IDirect3D9Ptr d3dFromDev;
    CHECK_HR(dev->GetDirect3D(&d3dFromDev));
    CHECK(d3dFromDev == d3d);
  }

  void CheckChildren()
  {
    TEST_LOG("Checking child objects");

    IDirect3DTexture9Ptr tex = MakeTex(64, 64, 3);
    if(!tex)
      return;

    for(UINT level = 0; level < 3; level++)
    {
      IDirect3DSurface9Ptr s1, s2;
      CHECK_HR(tex->GetSurfaceLevel(level, &s1));
      CHECK_HR(tex->GetSurfaceLevel(level, &s2));
      CHECK(s1 && s1 == s2);

      IDirect3DTexture9Ptr container;
      if(s1)
        CHECK_HR(s1->GetContainer(__uuidof(IDirect3DTexture9), (void **)&container));
      CHECK(container == tex);
    }

    IDirect3DSurface9Ptr invalid;
    CHECK(FAILED(tex->GetSurfaceLevel(3, &invalid)));

    IDirect3DBaseTexture9Ptr base;
    CHECK_HR(tex->QueryInterface(__uuidof(IDirect3DBaseTexture9), (void **)&base));
    CHECK(base && (IDirect3DBaseTexture9 *)tex.GetInterfacePtr() == base.GetInterfacePtr());

    IDirect3DCubeTexture9Ptr cube;
    CHECK_HR(dev->CreateCubeTexture(16, 2, 0, D3DFMT_A8R8G8B8, texPool, &cube, NULL));
    if(cube)
    {
      IDirect3DSurface9Ptr f1, f2, f3;
      CHECK_HR(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_NEGATIVE_Y, 1, &f1));
      CHECK_HR(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_NEGATIVE_Y, 1, &f2));
      CHECK_HR(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_Z, 1, &f3));
      CHECK(f1 && f1 == f2 && f1 != f3);
    }

    IDirect3DVolumeTexture9Ptr vol;
    CHECK_HR(dev->CreateVolumeTexture(8, 8, 8, 2, 0, D3DFMT_A8R8G8B8, texPool, &vol, NULL));
    if(vol)
    {
      IDirect3DVolume9Ptr v1, v2;
      CHECK_HR(vol->GetVolumeLevel(1, &v1));
      CHECK_HR(vol->GetVolumeLevel(1, &v2));
      CHECK(v1 && v1 == v2);

      IDirect3DVolumeTexture9Ptr container;
      if(v1)
        CHECK_HR(v1->GetContainer(__uuidof(IDirect3DVolumeTexture9), (void **)&container));
      CHECK(container == vol);
    }

    // a surface keeps its texture alive
    IDirect3DSurface9Ptr level0;
    CHECK_HR(tex->GetSurfaceLevel(0, &level0));
    IDirect3DTexture9 *rawTex = tex;
    tex = NULL;
    D3DSURFACE_DESC desc = {};
    if(level0)
      CHECK_HR(level0->GetDesc(&desc));
    CHECK(desc.Width == 64);
    IDirect3DTexture9Ptr container;
    if(level0)
      CHECK_HR(level0->GetContainer(__uuidof(IDirect3DTexture9), (void **)&container));
    CHECK(container.GetInterfacePtr() == rawTex);
  }

  void CheckBindings()
  {
    TEST_LOG("Checking bindings");

    IDirect3DTexture9Ptr texA = MakeTex(4, 4), texB = MakeTex(4, 4);
    if(!texA || !texB)
      return;

    // bound objects stay alive, and the same pointer comes back
    {
      IDirect3DTexture9Ptr temp = MakeTex(8, 8);
      IDirect3DTexture9 *raw = temp;
      CHECK_HR(dev->SetTexture(3, temp));
      OBSERVE("bound texture refcount %u", GetRefcount(raw));
      temp = NULL;

      IDirect3DBaseTexture9Ptr got;
      CHECK_HR(dev->GetTexture(3, &got));
      CHECK(got.GetInterfacePtr() == (IDirect3DBaseTexture9 *)raw);
      D3DSURFACE_DESC desc = {};
      if(got)
        CHECK_HR(((IDirect3DTexture9 *)got.GetInterfacePtr())->GetLevelDesc(0, &desc));
      CHECK(desc.Width == 8);
      CHECK_HR(dev->SetTexture(3, NULL));
    }

    // vertex texture samplers are separate slots
    CHECK_HR(dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, texB));
    {
      IDirect3DBaseTexture9Ptr vtex, ptex;
      CHECK_HR(dev->GetTexture(D3DVERTEXTEXTURESAMPLER0, &vtex));
      CHECK_HR(dev->GetTexture(0, &ptex));
      CHECK(vtex.GetInterfacePtr() == (IDirect3DBaseTexture9 *)texB.GetInterfacePtr());
      CHECK(ptex == NULL);
    }
    CHECK_HR(dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, NULL));

    // render targets
    IDirect3DSurface9Ptr bb, rt;
    CHECK_HR(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
    {
      IDirect3DTexture9Ptr rtTex;
      CHECK_HR(dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                  D3DPOOL_DEFAULT, &rtTex, NULL));
      IDirect3DSurface9Ptr rtSurf;
      if(rtTex)
        CHECK_HR(rtTex->GetSurfaceLevel(0, &rtSurf));
      if(rtSurf)
      {
        CHECK_HR(dev->SetRenderTarget(0, rtSurf));
        CHECK_HR(dev->GetRenderTarget(0, &rt));
        CHECK(rt == rtSurf);
        rt = NULL;
        CHECK_HR(dev->SetRenderTarget(0, bb));
      }

      IDirect3DSurface9Ptr none;
      HRESULT hr = dev->GetRenderTarget(1, &none);
      CHECK(FAILED(hr) && none == NULL);
      OBSERVE("GetRenderTarget on an unbound slot returns %x", hr);
    }

    // streams, indices, shaders and declarations
    IDirect3DVertexBuffer9Ptr vb = MakeVB(DefaultTri, sizeof(DefaultTri));
    CHECK_HR(dev->SetStreamSource(2, vb, 12, 24));
    {
      IDirect3DVertexBuffer9Ptr got;
      UINT offs = 0, stride = 0;
      CHECK_HR(dev->GetStreamSource(2, &got, &offs, &stride));
      CHECK(got == vb && offs == 12 && stride == 24);
    }
    CHECK_HR(dev->SetStreamSource(2, NULL, 0, 0));

    IDirect3DIndexBuffer9Ptr ib;
    CHECK_HR(dev->CreateIndexBuffer(64, 0, D3DFMT_INDEX16,
                                    devEx ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &ib, NULL));
    CHECK_HR(dev->SetIndices(ib));
    {
      IDirect3DIndexBuffer9Ptr got;
      CHECK_HR(dev->GetIndices(&got));
      CHECK(got == ib);
    }

    IDirect3DVertexShader9Ptr vs = CreateVS(Compile(D3D9DefaultVertex, "main", "vs_3_0"));
    IDirect3DPixelShader9Ptr ps = CreatePS(Compile(D3D9DefaultPixel, "main", "ps_3_0"));
    CHECK_HR(dev->SetVertexShader(vs));
    CHECK_HR(dev->SetPixelShader(ps));
    {
      IDirect3DVertexShader9Ptr gotVS;
      IDirect3DPixelShader9Ptr gotPS;
      CHECK_HR(dev->GetVertexShader(&gotVS));
      CHECK_HR(dev->GetPixelShader(&gotPS));
      CHECK(gotVS == vs && gotPS == ps);
    }

    CHECK_HR(dev->SetVertexDeclaration(defaultDecl));
    {
      IDirect3DVertexDeclaration9Ptr got;
      CHECK_HR(dev->GetVertexDeclaration(&got));
      CHECK(got == defaultDecl);
    }

    CHECK_HR(dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
    {
      DWORD fvf = 0;
      CHECK_HR(dev->GetFVF(&fvf));
      CHECK(fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE));

      IDirect3DVertexDeclaration9Ptr got1, got2;
      HRESULT hr = dev->GetVertexDeclaration(&got1);
      dev->GetVertexDeclaration(&got2);
      OBSERVE("declaration after SetFVF: hr %x, %s, %s", hr, got1 ? "non-NULL" : "NULL",
              got1 == got2 ? "stable" : "changed");
      CHECK(got1 != defaultDecl);
      if(got1)
      {
        UINT numElems = 0;
        CHECK_HR(got1->GetDeclaration(NULL, &numElems));
        OBSERVE("FVF declaration has %u elements", numElems);
      }
    }

    // UP draws unbind stream 0 (and the index buffer for indexed draws)
    CHECK_HR(dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V)));
    CHECK_HR(dev->SetVertexShader(NULL));
    CHECK_HR(dev->SetPixelShader(NULL));
    CHECK_HR(dev->BeginScene());
    const float verts[3][4] = {};
    const WORD idxs[3] = {0, 1, 2};
    CHECK_HR(dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE));
    CHECK_HR(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, verts, sizeof(verts[0])));
    {
      IDirect3DVertexBuffer9Ptr got;
      UINT offs = 1, stride = 1;
      CHECK_HR(dev->GetStreamSource(0, &got, &offs, &stride));
      CHECK(got == NULL);
    }
    CHECK_HR(dev->SetIndices(ib));
    CHECK_HR(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, idxs, D3DFMT_INDEX16, verts,
                                         sizeof(verts[0])));
    {
      IDirect3DIndexBuffer9Ptr got;
      CHECK_HR(dev->GetIndices(&got));
      CHECK(got == NULL);
    }
    CHECK_HR(dev->EndScene());
  }

  void CheckStateBlocks()
  {
    TEST_LOG("Checking state blocks");

    IDirect3DTexture9Ptr texA = MakeTex(4, 4), texB = MakeTex(4, 4), texC = MakeTex(4, 4);
    if(!texA || !texB || !texC)
      return;

    auto boundTex = [this](DWORD stage) {
      IDirect3DBaseTexture9Ptr got;
      dev->GetTexture(stage, &got);
      return got.GetInterfacePtr();
    };
    auto isTex = [](IDirect3DBaseTexture9 *a, IDirect3DTexture9Ptr &b) {
      return a == (IDirect3DBaseTexture9 *)b.GetInterfacePtr();
    };

    // D3DSBT_ALL captures textures
    CHECK_HR(dev->SetTexture(0, texA));
    IDirect3DStateBlock9Ptr all;
    CHECK_HR(dev->CreateStateBlock(D3DSBT_ALL, &all));
    CHECK_HR(dev->SetTexture(0, texB));
    if(all)
      CHECK_HR(all->Apply());
    CHECK(isTex(boundTex(0), texA));

    // Capture updates the snapshot
    CHECK_HR(dev->SetTexture(0, texC));
    if(all)
    {
      CHECK_HR(all->Capture());
      CHECK_HR(dev->SetTexture(0, texB));
      CHECK_HR(all->Apply());
    }
    CHECK(isTex(boundTex(0), texC));

    // the state block keeps captured objects alive
    {
      IDirect3DTexture9Ptr temp = MakeTex(16, 16);
      IDirect3DTexture9 *raw = temp;
      CHECK_HR(dev->SetTexture(0, temp));
      IDirect3DStateBlock9Ptr sb;
      CHECK_HR(dev->CreateStateBlock(D3DSBT_ALL, &sb));
      CHECK_HR(dev->SetTexture(0, NULL));
      temp = NULL;
      if(sb)
        CHECK_HR(sb->Apply());
      IDirect3DBaseTexture9 *got = boundTex(0);
      CHECK(got == (IDirect3DBaseTexture9 *)raw);
      D3DSURFACE_DESC desc = {};
      if(got)
        CHECK_HR(((IDirect3DTexture9 *)got)->GetLevelDesc(0, &desc));
      CHECK(desc.Width == 16);
      CHECK_HR(dev->SetTexture(0, NULL));
    }

    // pixel and vertex state blocks
    {
      CHECK_HR(dev->SetTexture(0, texA));
      IDirect3DStateBlock9Ptr pixel, vertex;
      CHECK_HR(dev->CreateStateBlock(D3DSBT_PIXELSTATE, &pixel));
      CHECK_HR(dev->CreateStateBlock(D3DSBT_VERTEXSTATE, &vertex));
      CHECK_HR(dev->SetTexture(0, texB));
      if(pixel)
        CHECK_HR(pixel->Apply());
      OBSERVE("D3DSBT_PIXELSTATE %s textures", isTex(boundTex(0), texA) ? "restores" : "ignores");
      CHECK_HR(dev->SetTexture(0, texB));
      if(vertex)
        CHECK_HR(vertex->Apply());
      OBSERVE("D3DSBT_VERTEXSTATE %s textures", isTex(boundTex(0), texA) ? "restores" : "ignores");

      CHECK_HR(dev->SetVertexDeclaration(defaultDecl));
      IDirect3DStateBlock9Ptr vertex2;
      CHECK_HR(dev->CreateStateBlock(D3DSBT_VERTEXSTATE, &vertex2));
      CHECK_HR(dev->SetFVF(D3DFVF_XYZ));
      if(vertex2)
        CHECK_HR(vertex2->Apply());
      IDirect3DVertexDeclaration9Ptr decl;
      dev->GetVertexDeclaration(&decl);
      OBSERVE("D3DSBT_VERTEXSTATE %s the vertex declaration",
              decl == defaultDecl ? "restores" : "ignores");
    }

    // recorded state blocks only contain what was set while recording
    {
      CHECK_HR(dev->SetTexture(0, texA));
      CHECK_HR(dev->SetTexture(1, texA));

      CHECK_HR(dev->BeginStateBlock());
      CHECK_HR(dev->SetTexture(1, texB));
      OBSERVE("while recording, GetTexture returns the %s texture",
              isTex(boundTex(1), texB) ? "recorded" : "previous");
      IDirect3DStateBlock9Ptr recorded;
      CHECK_HR(dev->EndStateBlock(&recorded));

      // recording doesn't apply the state
      CHECK(isTex(boundTex(1), texA));

      CHECK_HR(dev->SetTexture(0, texC));
      CHECK_HR(dev->SetTexture(1, texC));
      if(recorded)
        CHECK_HR(recorded->Apply());
      CHECK(isTex(boundTex(1), texB));
      CHECK(isTex(boundTex(0), texC));

      IDirect3DDevice9Ptr sbDev;
      if(recorded)
        CHECK_HR(recorded->GetDevice(&sbDev));
      CHECK(sbDev == dev);
    }

    CHECK_HR(dev->SetTexture(0, NULL));
    CHECK_HR(dev->SetTexture(1, NULL));
  }

  void CheckOperations()
  {
    TEST_LOG("Checking operations on wrapped objects");

    IDirect3DSurface9Ptr bb;
    CHECK_HR(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));

    IDirect3DSurface9Ptr rt;
    CHECK_HR(
        dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL));
    if(rt)
    {
      CHECK_HR(dev->ColorFill(rt, NULL, D3DCOLOR_XRGB(255, 0, 0)));
      CHECK_HR(dev->StretchRect(rt, NULL, bb, NULL, D3DTEXF_NONE));

      // standalone surfaces have the device as their container
      IDirect3DDevice9Ptr container;
      CHECK_HR(rt->GetContainer(__uuidof(IDirect3DDevice9), (void **)&container));
      CHECK(container == dev);

      IDirect3DSurface9Ptr sysmem;
      CHECK_HR(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem,
                                                NULL));
      if(sysmem)
      {
        CHECK_HR(dev->GetRenderTargetData(rt, sysmem));
        D3DLOCKED_RECT locked = {};
        CHECK_HR(sysmem->LockRect(&locked, NULL, D3DLOCK_READONLY));
        if(locked.pBits)
        {
          CHECK((((uint32_t *)locked.pBits)[0] & 0xffffff) == 0xff0000);
          sysmem->UnlockRect();
        }
      }
    }

    // UpdateTexture from system memory
    IDirect3DTexture9Ptr src, dst;
    CHECK_HR(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, NULL));
    CHECK_HR(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, NULL));
    if(src && dst)
      CHECK_HR(dev->UpdateTexture(src, dst));

    // queries
    IDirect3DQuery9Ptr query;
    CHECK_HR(dev->CreateQuery(D3DQUERYTYPE_EVENT, NULL));
    CHECK_HR(dev->CreateQuery(D3DQUERYTYPE_EVENT, &query));
    if(query)
    {
      CHECK(query->GetType() == D3DQUERYTYPE_EVENT);
      CHECK_HR(query->Issue(D3DISSUE_END));
      BOOL done = FALSE;
      for(int i = 0; i < 10000 && query->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE;
          i++)
        Sleep(0);
    }

    // additional swapchains aren't counted or reachable through the device
    const UINT numSwaps = dev->GetNumberOfSwapChains();
    {
      D3DPRESENT_PARAMETERS params = presentParams;
      params.EnableAutoDepthStencil = FALSE;
      IDirect3DSwapChain9Ptr extra;
      CHECK_HR(dev->CreateAdditionalSwapChain(&params, &extra));
      CHECK(dev->GetNumberOfSwapChains() == numSwaps);

      IDirect3DSwapChain9Ptr implicitSwap;
      CHECK_HR(dev->GetSwapChain(0, &implicitSwap));
      CHECK(implicitSwap != extra);

      IDirect3DSwapChain9Ptr outOfRange;
      CHECK(FAILED(dev->GetSwapChain(numSwaps, &outOfRange)));

      if(extra)
      {
        IDirect3DSurface9Ptr b1, b2;
        CHECK_HR(extra->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &b1));
        CHECK_HR(extra->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &b2));
        CHECK(b1 && b1 == b2 && b1 != bb);
      }
    }
  }

  void CheckRefcounts()
  {
    TEST_LOG("Checking refcounts");

    const ULONG devRefs = GetRefcount(dev.GetInterfacePtr());

    IDirect3DTexture9 *tex = NULL;
    CHECK_HR(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, texPool, &tex, NULL));
    if(!tex)
      return;

    OBSERVE("device refcount %u -> %u after creating a texture", devRefs,
            GetRefcount(dev.GetInterfacePtr()));

    CHECK(tex->AddRef() == 2);

    IDirect3DSurface9 *surf = NULL;
    CHECK_HR(tex->GetSurfaceLevel(0, &surf));
    OBSERVE("texture refcount with its surface held: %u", GetRefcount(tex));
    if(surf)
      OBSERVE("surface Release() returned %u", surf->Release());

    CHECK(tex->Release() == 1);
    CHECK(tex->Release() == 0);

    OBSERVE("device refcount back to %u", GetRefcount(dev.GetInterfacePtr()));
  }

  void CheckReset()
  {
    TEST_LOG("Checking Reset");

    // everything in D3DPOOL_DEFAULT has to be released before a (non-Ex) Reset, including what we
    // hold on to. Leave a managed texture bound, which is allowed.
    IDirect3DTexture9Ptr managed;
    if(!devEx)
    {
      managed = MakeTex(4, 4);
      CHECK_HR(dev->SetTexture(0, managed));
    }

    dev->SetRenderTarget(1, NULL);
    dev->SetStreamSource(0, NULL, 0, 0);
    dev->SetIndices(NULL);

    D3DPRESENT_PARAMETERS params = presentParams;
    HRESULT hr = devEx ? devEx->ResetEx(&params, NULL) : dev->Reset(&params);
    CHECK_HR(hr);

    if(SUCCEEDED(hr))
    {
      IDirect3DSurface9Ptr bb, rt, ds;
      CHECK_HR(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
      CHECK_HR(dev->GetRenderTarget(0, &rt));
      CHECK(bb && rt == bb);

      CHECK_HR(dev->GetDepthStencilSurface(&ds));
      CHECK(ds != NULL);

      // state goes back to defaults
      IDirect3DBaseTexture9Ptr tex;
      CHECK_HR(dev->GetTexture(0, &tex));
      CHECK(tex == NULL);
    }
  }

  int main()
  {
    if(!Init())
      return 3;

    if(devEx)
      texPool = D3DPOOL_DEFAULT;

    CheckDeviceObjects();
    CheckChildren();
    CheckBindings();
    CheckStateBlocks();
    CheckOperations();
    CheckRefcounts();
    CheckReset();

    // re-run the device object checks after the reset
    CheckDeviceObjects();

    if(failed)
      TEST_ERROR("Some checks failed");
    else
      TEST_LOG("All checks passed");

    while(Running())
    {
      dev->Clear(0, NULL, D3DCLEAR_TARGET,
                 failed ? D3DCOLOR_XRGB(255, 0, 0) : D3DCOLOR_XRGB(0, 255, 0), 1.0f, 0);

      Present();
    }

    return failed ? 5 : 0;
  }

#undef CHECK
#undef CHECK_HR
#undef OBSERVE
};
