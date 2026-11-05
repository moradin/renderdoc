// Native check: what LockRect reports as Pitch for 16x16 block textures, managed pool, level 0
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")
int main()
{
  HWND wnd = CreateWindowA("STATIC", "t", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, 0, 0, 0, 0);
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = wnd;
  IDirect3DDevice9 *dev = NULL;
  d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if(!dev) return 1;
  struct { D3DFORMAT f; const char *n; } fmts[] = {
      {D3DFMT_DXT1, "DXT1"}, {D3DFMT_DXT5, "DXT5"},
      {(D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1'), "ATI1"}, {(D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2'), "ATI2"}};
  UINT sizes[] = {16, 64};
  for(auto &f : fmts)
    for(UINT sz : sizes)
    {
      IDirect3DTexture9 *t = NULL;
      HRESULT hr = dev->CreateTexture(sz, sz, 1, 0, f.f, D3DPOOL_MANAGED, &t, NULL);
      D3DLOCKED_RECT lr = {};
      HRESULT hl = t ? t->LockRect(0, &lr, NULL, 0) : E_FAIL;
      printf("%s %ux%u: CreateTexture %08x LockRect %08x Pitch %d\n", f.n, sz, sz, (unsigned)hr, (unsigned)hl, lr.Pitch);
      if(t) { if(SUCCEEDED(hl)) t->UnlockRect(0); t->Release(); }
    }
  dev->Release();
  d3d->Release();
  return 0;
}
