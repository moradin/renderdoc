/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
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

#include "hooks/hooks.h"
#include "d3d9_device.h"

// The D3DPERF_* marker functions are also exported from d3d9.dll, but they are hooked by the D3D11
// driver (driver/d3d11/d3d9_hooks.cpp) since D3D11 programs commonly use them for annotation. That
// hook passes them on to D3D9 devices too (D3D9_PERF_BeginEvent etc.). This hook only intercepts
// device creation. Both register d3d9.dll, which is fine since library hooks
// can have multiple registrations.

typedef IDirect3D9 *(WINAPI *PFN_D3D9_CREATE)(UINT);
typedef HRESULT(WINAPI *PFN_D3D9_CREATEEX)(UINT, IDirect3D9Ex **);

class D3D9CreateHook : LibraryHook
{
public:
  void RegisterHooks()
  {
    RDCLOG("Registering D3D9 device creation hooks");

    LibraryHooks::RegisterLibraryHook("d3d9.dll", NULL);

    Create9.Register("d3d9.dll", "Direct3DCreate9", Create9_hook);
    Create9Ex.Register("d3d9.dll", "Direct3DCreate9Ex", Create9Ex_hook);
  }

private:
  static D3D9CreateHook d3d9hooks;

  HookedFunction<PFN_D3D9_CREATE> Create9;
  HookedFunction<PFN_D3D9_CREATEEX> Create9Ex;

  static IDirect3D9 *WINAPI Create9_hook(UINT SDKVersion)
  {
    RDCLOG("App creating d3d9 %x", SDKVersion);

    // Always create the non-Ex object here. Creating an IDirect3D9Ex instead causes some games to
    // fail to initialise even though they succeed with plain IDirect3D9.
    IDirect3D9 *realD3D = d3d9hooks.Create9()(SDKVersion);

    if(realD3D == NULL)
      return NULL;

    return new WrappedD3D9(realD3D);
  }

  static HRESULT WINAPI Create9Ex_hook(UINT SDKVersion, IDirect3D9Ex **ppD3D)
  {
    RDCLOG("App creating d3d9Ex %x", SDKVersion);

    if(ppD3D == NULL)
      return d3d9hooks.Create9Ex()(SDKVersion, ppD3D);

    IDirect3D9Ex *realD3D = NULL;
    HRESULT result = d3d9hooks.Create9Ex()(SDKVersion, &realD3D);

    if(SUCCEEDED(result) && realD3D != NULL)
      *ppD3D = new WrappedD3D9(realD3D);
    else
      *ppD3D = NULL;

    return result;
  }
};

D3D9CreateHook D3D9CreateHook::d3d9hooks;
