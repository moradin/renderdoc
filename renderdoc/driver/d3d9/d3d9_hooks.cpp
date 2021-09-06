/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2021 Baldur Karlsson
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

#include "driver/d3d11/d3d11_device.h"
#include "hooks/hooks.h"

#include "driver/dx/official/d3d9.h"
#include "d3d9_device.h"

typedef int(WINAPI *PFN_BEGIN_EVENT)(DWORD, WCHAR *);
typedef int(WINAPI *PFN_END_EVENT)();
typedef int(WINAPI *PFN_SET_MARKER_EVENT)(DWORD, WCHAR *);
typedef void(WINAPI *PFN_SET_OPTIONS)(DWORD);
typedef DWORD(WINAPI *PFN_GET_OPTIONS)();

typedef IDirect3D9 *(WINAPI *PFN_D3D9_CREATE)(UINT);
typedef HRESULT(WINAPI *PFN_D3D9_CREATEEX)(UINT, IDirect3D9Ex **);

class D3D9Hook : LibraryHook
{
public:
  void RegisterHooks()
  {
    RDCLOG("Registering D3D9 hooks");

    LibraryHooks::RegisterLibraryHook("d3d9.dll", NULL);

    PERF_BeginEvent.Register("d3d9.dll", "D3DPERF_BeginEvent", PERF_BeginEvent_hook);
    PERF_EndEvent.Register("d3d9.dll", "D3DPERF_EndEvent", PERF_EndEvent_hook);
    PERF_SetMarker.Register("d3d9.dll", "D3DPERF_SetMarker", PERF_SetMarker_hook);
    PERF_SetOptions.Register("d3d9.dll", "D3DPERF_SetOptions", PERF_SetOptions_hook);
    PERF_GetStatus.Register("d3d9.dll", "D3DPERF_GetStatus", PERF_GetStatus_hook);

    Create9.Register("d3d9.dll", "Direct3DCreate9", Create9_hook);
	Create9Ex.Register("d3d9.dll", "Direct3DCreate9Ex", Create9Ex_hook);
  }

private:
  static D3D9Hook d3d9hooks;

  // D3DPERF api
  HookedFunction<PFN_BEGIN_EVENT> PERF_BeginEvent;
  HookedFunction<PFN_END_EVENT> PERF_EndEvent;
  HookedFunction<PFN_SET_MARKER_EVENT> PERF_SetMarker;
  HookedFunction<PFN_SET_OPTIONS> PERF_SetOptions;
  HookedFunction<PFN_GET_OPTIONS> PERF_GetStatus;
  HookedFunction<PFN_D3D9_CREATE> Create9;
  HookedFunction<PFN_D3D9_CREATEEX> Create9Ex;

  static int WINAPI PERF_BeginEvent_hook(DWORD col, WCHAR *wszName)
  {
    int ret = WrappedID3D11Device::BeginEvent((uint32_t)col, wszName);

    d3d9hooks.PERF_BeginEvent()(col, wszName);

    return ret;
  }

  static int WINAPI PERF_EndEvent_hook()
  {
    int ret = WrappedID3D11Device::EndEvent();

    d3d9hooks.PERF_EndEvent()();

    return ret;
  }

  static void WINAPI PERF_SetMarker_hook(DWORD col, WCHAR *wszName)
  {
    WrappedID3D11Device::SetMarker((uint32_t)col, wszName);

    d3d9hooks.PERF_SetMarker()(col, wszName);
  }

  static void WINAPI PERF_SetOptions_hook(DWORD dwOptions)
  {
    if(dwOptions & 1)
      RDCLOG("Application requested not to be hooked via D3DPERF_SetOptions: no longer supported.");
  }

  static DWORD WINAPI PERF_GetStatus_hook() { return 1; }
  static IDirect3D9 *WINAPI Create9_hook(UINT SDKVersion)
  {
    RDCLOG("App creating d3d9 %x", SDKVersion);
    //*
    IDirect3D9 *realD3D = NULL;
    realD3D = d3d9hooks.Create9()(SDKVersion);
    if(realD3D != NULL)
    /*/
  // Creating the Ex object here causes some games to fail initializing
    IDirect3D9Ex *realD3D = NULL;
    HRESULT result = d3d9hooks.Create9Ex()(SDKVersion, &realD3D);
    if(result == S_OK && realD3D != NULL)
    //*/
    {
      return new WrappedD3D9(realD3D);
    }
    else
    {
      return NULL;
    }
  }

  static HRESULT WINAPI Create9Ex_hook(UINT SDKVersion, IDirect3D9Ex **ppD3D)
  {
    RDCLOG("App creating d3d9Ex %x", SDKVersion);

    IDirect3D9Ex *realD3D = NULL;
    HRESULT result = d3d9hooks.Create9Ex()(SDKVersion, &realD3D);

    if(result == S_OK)
    {
      *ppD3D = new WrappedD3D9(realD3D);
    }
    else
    {
      *ppD3D = NULL;
    }
    return result;
  }
};

D3D9Hook D3D9Hook::d3d9hooks;
