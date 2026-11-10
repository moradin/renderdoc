// TEMPORARY probe, not committed: how many separate dirty rects the runtime keeps
#include "d3d9_test.h"

RD_TEST(D3D9_Dirty_Probe, D3D9GraphicsTest)
{
  static constexpr const char *Description = "probe";

  struct V
  {
    float x, y, z, rhw, u, v;
  };

  IDirect3DTexture9Ptr rt;
  IDirect3DSurface9Ptr rtSurf, sysmem;

  // reads a 64x64 texture back, returns its texels
  std::vector<DWORD> Read(IDirect3DTexture9 * tex)
  {
    IDirect3DSurface9Ptr prevRT;
    dev->GetRenderTarget(0, &prevRT);
    dev->SetRenderTarget(0, rtSurf);
    V q[4] = {{-0.5f, -0.5f, 0.5f, 1, 0, 0},
              {63.5f, -0.5f, 0.5f, 1, 1, 0},
              {-0.5f, 63.5f, 0.5f, 1, 0, 1},
              {63.5f, 63.5f, 0.5f, 1, 1, 1}};
    dev->BeginScene();
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetTexture(0, tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    dev->SetTexture(0, NULL);
    dev->EndScene();
    dev->SetRenderTarget(0, prevRT);
    dev->GetRenderTargetData(rtSurf, sysmem);
    D3DLOCKED_RECT l = {};
    sysmem->LockRect(&l, NULL, D3DLOCK_READONLY);
    std::vector<DWORD> ret(64 * 64);
    for(int y = 0; y < 64; y++)
      memcpy(&ret[y * 64], (byte *)l.pBits + y * l.Pitch, 64 * 4);
    sysmem->UnlockRect();
    return ret;
  }

  void Fill(IDirect3DTexture9 * tex, DWORD c, DWORD flags)
  {
    D3DLOCKED_RECT l = {};
    tex->LockRect(0, &l, NULL, flags);
    for(int y = 0; y < 64; y++)
      for(int x = 0; x < 64; x++)
        ((DWORD *)((byte *)l.pBits + y * l.Pitch))[x] = c;
    tex->UnlockRect(0);
  }

  int main()
  {
    if(!Init())
      return 3;
    dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt, NULL);
    rt->GetSurfaceLevel(0, &rtSurf);
    dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);

    // mode 0: AddDirtyRect, mode 1: LockRect (flags 0) of each rect
    for(int mode = 0; mode < 2; mode++)
    {
      for(int n = 1; n <= 40; n++)
      {
        IDirect3DTexture9Ptr s, d;
        dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s, NULL);
        dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d, NULL);
        Fill(s, 0xFF000000, 0);
        dev->UpdateTexture(s, d);
        Fill(s, 0xFFFFFFFF, D3DLOCK_NO_DIRTY_UPDATE);
        // n 1x1 rects on a diagonal-ish grid, far apart: (4i % 64, 4*(i/16)*... )
        for(int i = 0; i < n; i++)
        {
          RECT r = {LONG((i % 8) * 8), LONG((i / 8) * 8), LONG((i % 8) * 8 + 1), LONG((i / 8) * 8 + 1)};
          if(mode == 0)
          {
            s->AddDirtyRect(&r);
          }
          else
          {
            D3DLOCKED_RECT l = {};
            s->LockRect(0, &l, &r, 0);
            s->UnlockRect(0);
          }
        }
        dev->UpdateTexture(s, d);
        std::vector<DWORD> t = Read(d);
        int copied = 0;
        for(DWORD v : t)
          copied += (v == 0xFFFFFFFF);
        TEST_LOG("mode %s n %d: %d texels copied", mode ? "lock" : "adddirty", n, copied);
      }
    }


    // identical rects n times
    for(int n = 5; n <= 8; n++)
    {
      IDirect3DTexture9Ptr s, d;
      dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s, NULL);
      dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d, NULL);
      Fill(s, 0xFF000000, 0);
      dev->UpdateTexture(s, d);
      Fill(s, 0xFFFFFFFF, D3DLOCK_NO_DIRTY_UPDATE);
      for(int i = 0; i < n; i++)
      {
        RECT r = {0, 0, 1, 1};
        s->AddDirtyRect(&r);
      }
      dev->UpdateTexture(s, d);
      std::vector<DWORD> t = Read(d);
      int copied = 0;
      for(DWORD v : t)
        copied += (v == 0xFFFFFFFF);
      TEST_LOG("identical n %d: %d texels copied", n, copied);
    }
    // nested rects: a big one then 6 inside it
    {
      IDirect3DTexture9Ptr s, d;
      dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s, NULL);
      dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d, NULL);
      Fill(s, 0xFF000000, 0);
      dev->UpdateTexture(s, d);
      Fill(s, 0xFFFFFFFF, D3DLOCK_NO_DIRTY_UPDATE);
      RECT big = {0, 0, 32, 32};
      s->AddDirtyRect(&big);
      for(int i = 0; i < 6; i++)
      {
        RECT r = {i, i, i + 1, i + 1};
        s->AddDirtyRect(&r);
      }
      dev->UpdateTexture(s, d);
      std::vector<DWORD> t = Read(d);
      int copied = 0;
      for(DWORD v : t)
        copied += (v == 0xFFFFFFFF);
      TEST_LOG("big then 6 inside: %d texels copied", copied);
    }
    // cube: 4 rects on face 0 and 4 on face 1 -> per face count?
    {
      IDirect3DCubeTexture9Ptr s, d;
      dev->CreateCubeTexture(64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s, NULL);
      dev->CreateCubeTexture(64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d, NULL);
      IDirect3DTexture9Ptr sysTmp;
      for(int f = 0; f < 6; f++)
      {
        D3DLOCKED_RECT l = {};
        s->LockRect((D3DCUBEMAP_FACES)f, 0, &l, NULL, 0);
        for(int y = 0; y < 64; y++)
          for(int x = 0; x < 64; x++)
            ((DWORD *)((byte *)l.pBits + y * l.Pitch))[x] = 0xFF000000;
        s->UnlockRect((D3DCUBEMAP_FACES)f, 0);
      }
      dev->UpdateTexture(s, d);
      for(int f = 0; f < 6; f++)
      {
        D3DLOCKED_RECT l = {};
        s->LockRect((D3DCUBEMAP_FACES)f, 0, &l, NULL, D3DLOCK_NO_DIRTY_UPDATE);
        for(int y = 0; y < 64; y++)
          for(int x = 0; x < 64; x++)
            ((DWORD *)((byte *)l.pBits + y * l.Pitch))[x] = 0xFFFFFFFF;
        s->UnlockRect((D3DCUBEMAP_FACES)f, 0);
      }
      for(int f = 0; f < 2; f++)
        for(int i = 0; i < 4; i++)
        {
          RECT r = {i * 8, 0, i * 8 + 1, 1};
          s->AddDirtyRect((D3DCUBEMAP_FACES)f, &r);
        }
      dev->UpdateTexture(s, d);
      // read faces by copying to a sysmem cube? use GetCubeMapSurface + a render target copy isn't
      // possible from a non-RT default texture; draw each face with a cube lookup is long, so
      // instead StretchRect isn't allowed either. Use a lockable path: re-create d as dynamic.
      TEST_LOG("cube: see dynamic variant");
    }
    // cube with a dynamic default destination, readable by locking
    {
      IDirect3DCubeTexture9Ptr s, d;
      dev->CreateCubeTexture(64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s, NULL);
      HRESULT hr = dev->CreateCubeTexture(64, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d, NULL);
      for(int pass = 0; pass < 2; pass++)
      {
        for(int f = 0; f < 6; f++)
        {
          D3DLOCKED_RECT l = {};
          s->LockRect((D3DCUBEMAP_FACES)f, 0, &l, NULL, pass ? D3DLOCK_NO_DIRTY_UPDATE : 0);
          for(int y = 0; y < 64; y++)
            for(int x = 0; x < 64; x++)
              ((DWORD *)((byte *)l.pBits + y * l.Pitch))[x] = pass ? 0xFFFFFFFF : 0xFF000000;
          s->UnlockRect((D3DCUBEMAP_FACES)f, 0);
        }
        if(pass == 0)
          dev->UpdateTexture(s, d);
      }
      for(int f = 0; f < 2; f++)
        for(int i = 0; i < 4; i++)
        {
          RECT r = {i * 8, 0, i * 8 + 1, 1};
          s->AddDirtyRect((D3DCUBEMAP_FACES)f, &r);
        }
      dev->UpdateTexture(s, d);
      for(int f = 0; f < 6; f++)
      {
        D3DLOCKED_RECT l = {};
        d->LockRect((D3DCUBEMAP_FACES)f, 0, &l, NULL, D3DLOCK_READONLY);
        int copied = 0;
        for(int y = 0; y < 64; y++)
          for(int x = 0; x < 64; x++)
            copied += ((DWORD *)((byte *)l.pBits + y * l.Pitch))[x] == 0xFFFFFFFF;
        d->UnlockRect((D3DCUBEMAP_FACES)f, 0);
        TEST_LOG("cube dynamic (hr %x) 4+4 rects: face %d %d texels copied", hr, f, copied);
      }
    }
    // volume: n disjoint boxes, dynamic destination
    for(int n = 5; n <= 7; n++)
    {
      IDirect3DVolumeTexture9Ptr s, d;
      dev->CreateVolumeTexture(16, 16, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s, NULL);
      HRESULT hr = dev->CreateVolumeTexture(16, 16, 4, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d, NULL);
      for(int pass = 0; pass < 2; pass++)
      {
        D3DLOCKED_BOX l = {};
        s->LockBox(0, &l, NULL, pass ? D3DLOCK_NO_DIRTY_UPDATE : 0);
        for(int z = 0; z < 4; z++)
          for(int y = 0; y < 16; y++)
            for(int x = 0; x < 16; x++)
              ((DWORD *)((byte *)l.pBits + z * l.SlicePitch + y * l.RowPitch))[x] = pass ? 0xFFFFFFFF : 0xFF000000;
        s->UnlockBox(0);
        if(pass == 0)
          dev->UpdateTexture(s, d);
      }
      for(int i = 0; i < n; i++)
      {
        D3DBOX b = {UINT(i * 2), 0, UINT(i * 2 + 1), 1, UINT(i % 4), UINT(i % 4 + 1)};
        s->AddDirtyBox(&b);
      }
      dev->UpdateTexture(s, d);
      D3DLOCKED_BOX l = {};
      d->LockBox(0, &l, NULL, D3DLOCK_READONLY);
      int copied = 0;
      for(int z = 0; z < 4; z++)
        for(int y = 0; y < 16; y++)
          for(int x = 0; x < 16; x++)
            copied += ((DWORD *)((byte *)l.pBits + z * l.SlicePitch + y * l.RowPitch))[x] == 0xFFFFFFFF;
      d->UnlockBox(0);
      TEST_LOG("volume (hr %x) n %d boxes: %d texels copied", hr, n, copied);
    }

    // cube probes: rects per face given as a count for face 0 and face 1
    int cases[][2] = {{1, 0}, {2, 0}, {3, 0}, {6, 0}, {7, 0}, {3, 3}, {4, 3}};
    for(auto &cs : cases)
    {
      IDirect3DCubeTexture9Ptr s, d;
      dev->CreateCubeTexture(64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s, NULL);
      dev->CreateCubeTexture(64, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d, NULL);
      for(int pass = 0; pass < 2; pass++)
      {
        for(int f = 0; f < 6; f++)
        {
          D3DLOCKED_RECT l = {};
          s->LockRect((D3DCUBEMAP_FACES)f, 0, &l, NULL, pass ? D3DLOCK_NO_DIRTY_UPDATE : 0);
          for(int y = 0; y < 64; y++)
            for(int x = 0; x < 64; x++)
              ((DWORD *)((byte *)l.pBits + y * l.Pitch))[x] = pass ? 0xFFFFFFFF : 0xFF000000;
          s->UnlockRect((D3DCUBEMAP_FACES)f, 0);
        }
        if(pass == 0)
          dev->UpdateTexture(s, d);
      }
      for(int f = 0; f < 2; f++)
        for(int i = 0; i < cs[f]; i++)
        {
          RECT r = {i * 8, i * 2, i * 8 + 1, i * 2 + 1};
          s->AddDirtyRect((D3DCUBEMAP_FACES)f, &r);
        }
      dev->UpdateTexture(s, d);
      std::string out;
      for(int f = 0; f < 6; f++)
      {
        D3DLOCKED_RECT l = {};
        d->LockRect((D3DCUBEMAP_FACES)f, 0, &l, NULL, D3DLOCK_READONLY);
        int copied = 0;
        for(int y = 0; y < 64; y++)
          for(int x = 0; x < 64; x++)
            copied += ((DWORD *)((byte *)l.pBits + y * l.Pitch))[x] == 0xFFFFFFFF;
        d->UnlockRect((D3DCUBEMAP_FACES)f, 0);
        out += " " + std::to_string(copied);
      }
      TEST_LOG("cube %d+%d rects: per face%s", cs[0], cs[1], out.c_str());
    }
    // 2D with lock-on-surface of level 0 (via GetSurfaceLevel): does it dirty?
    {
      IDirect3DTexture9Ptr s, d;
      dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s, NULL);
      dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &d, NULL);
      Fill(s, 0xFF000000, 0);
      dev->UpdateTexture(s, d);
      Fill(s, 0xFFFFFFFF, D3DLOCK_NO_DIRTY_UPDATE);
      IDirect3DSurface9Ptr surf;
      s->GetSurfaceLevel(0, &surf);
      RECT r = {4, 4, 6, 6};
      D3DLOCKED_RECT l = {};
      surf->LockRect(&l, &r, 0);
      surf->UnlockRect();
      dev->UpdateTexture(s, d);
      std::vector<DWORD> t = Read(d);
      int copied = 0;
      for(DWORD v : t)
        copied += (v == 0xFFFFFFFF);
      TEST_LOG("surface lock of level 0 2x2: %d texels copied", copied);
    }
    // overlapping/adjacent: two rects touching
    return 0;
  }
};

REGISTER_TEST();
