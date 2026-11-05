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

#define INITGUID

#include "d3d9_test.h"
#include <stdio.h>
#include <algorithm>
#include "../win32/win32_platform.h"
#include "../win32/win32_window.h"

std::string D3D9DefaultVertex = R"EOSHADER(

struct vertin
{
	float3 pos : POSITION;
	float4 col : COLOR0;
	float2 uv : TEXCOORD0;
};

struct v2f
{
	float4 pos : POSITION;
	float4 col : COLOR0;
	float2 uv : TEXCOORD0;
};

v2f main(vertin IN)
{
	v2f OUT = (v2f)0;

	OUT.pos = float4(IN.pos.xyz, 1);
	OUT.col = IN.col;
	OUT.uv = IN.uv;

	return OUT;
}

)EOSHADER";

std::string D3D9DefaultPixel = R"EOSHADER(

struct v2f
{
	float4 col : COLOR0;
	float2 uv : TEXCOORD0;
};

float4 main(v2f IN) : COLOR0
{
	return IN.col;
}

)EOSHADER";

typedef IDirect3D9 *(WINAPI *PFN_DIRECT3D_CREATE9)(UINT);
typedef HRESULT(WINAPI *PFN_DIRECT3D_CREATE9_EX)(UINT, IDirect3D9Ex **);
typedef int(WINAPI *PFN_D3DPERF_BEGIN_EVENT)(D3DCOLOR, LPCWSTR);
typedef int(WINAPI *PFN_D3DPERF_END_EVENT)();
typedef void(WINAPI *PFN_D3DPERF_SET_MARKER)(D3DCOLOR, LPCWSTR);

namespace
{
HMODULE d3d9 = NULL;
HMODULE d3dcompiler = NULL;

PFN_DIRECT3D_CREATE9 dyn_Direct3DCreate9 = NULL;
PFN_DIRECT3D_CREATE9_EX dyn_Direct3DCreate9Ex = NULL;
PFN_D3DPERF_BEGIN_EVENT dyn_BeginEvent = NULL;
PFN_D3DPERF_END_EVENT dyn_EndEvent = NULL;
PFN_D3DPERF_SET_MARKER dyn_SetMarker = NULL;
pD3DCompile dyn_D3DCompile = NULL;
};

void D3D9GraphicsTest::Prepare(int argc, char **argv)
{
  GraphicsTest::Prepare(argc, argv);

  static bool prepared = false;

  if(!prepared)
  {
    prepared = true;

    d3d9 = LoadLibraryA("d3d9.dll");
    d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    if(!d3dcompiler)
      d3dcompiler = LoadLibraryA("d3dcompiler_46.dll");
    if(!d3dcompiler)
      d3dcompiler = LoadLibraryA("d3dcompiler_43.dll");

    if(d3d9)
    {
      dyn_Direct3DCreate9 = (PFN_DIRECT3D_CREATE9)GetProcAddress(d3d9, "Direct3DCreate9");
      dyn_Direct3DCreate9Ex = (PFN_DIRECT3D_CREATE9_EX)GetProcAddress(d3d9, "Direct3DCreate9Ex");
      dyn_BeginEvent = (PFN_D3DPERF_BEGIN_EVENT)GetProcAddress(d3d9, "D3DPERF_BeginEvent");
      dyn_EndEvent = (PFN_D3DPERF_END_EVENT)GetProcAddress(d3d9, "D3DPERF_EndEvent");
      dyn_SetMarker = (PFN_D3DPERF_SET_MARKER)GetProcAddress(d3d9, "D3DPERF_SetMarker");
    }

    if(d3dcompiler)
      dyn_D3DCompile = (pD3DCompile)GetProcAddress(d3dcompiler, "D3DCompile");
  }

  if(!d3d9)
    Avail = "d3d9.dll is not available";
  else if(!d3dcompiler)
    Avail = "d3dcompiler_XX.dll is not available";
  else if(!dyn_Direct3DCreate9 || !dyn_D3DCompile)
    Avail = "Missing required entry point";
  else if(useEx && !dyn_Direct3DCreate9Ex)
    Avail = "Direct3DCreate9Ex is not available";
}

D3DPRESENT_PARAMETERS D3D9GraphicsTest::MakePresentParams(GraphicsWindow *win)
{
  D3DPRESENT_PARAMETERS params = {};

  params.BackBufferWidth = screenWidth;
  params.BackBufferHeight = screenHeight;
  params.BackBufferFormat = backbufferFmt;
  params.BackBufferCount = 1;
  params.MultiSampleType = D3DMULTISAMPLE_NONE;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = ((Win32Window *)win)->wnd;
  params.Windowed = TRUE;
  params.EnableAutoDepthStencil = depthFmt != D3DFMT_UNKNOWN ? TRUE : FALSE;
  params.AutoDepthStencilFormat = depthFmt;
  params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  return params;
}

bool D3D9GraphicsTest::Init()
{
  if(!GraphicsTest::Init())
    return false;

  HRESULT hr = S_OK;

  if(useEx)
  {
    hr = dyn_Direct3DCreate9Ex(D3D_SDK_VERSION, &d3dEx);
    if(FAILED(hr) || !d3dEx)
    {
      TEST_ERROR("Direct3DCreate9Ex failed: %x", hr);
      return false;
    }
    d3d = d3dEx;
  }
  else
  {
    d3d.Attach(dyn_Direct3DCreate9(D3D_SDK_VERSION));
    if(!d3d)
    {
      TEST_ERROR("Direct3DCreate9 failed");
      return false;
    }
  }

  Win32Window *win = new Win32Window(screenWidth, screenHeight, screenTitle);
  mainWindow = win;

  presentParams = MakePresentParams(win);

  // prefer hardware vertex processing, fall back to software. Pure devices require hardware.
  const DWORD tries[] = {
      extraBehaviorFlags | D3DCREATE_HARDWARE_VERTEXPROCESSING,
      (extraBehaviorFlags & ~D3DCREATE_PUREDEVICE) | D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  hr = E_FAIL;
  for(DWORD flags : tries)
  {
    D3DPRESENT_PARAMETERS params = presentParams;

    if(useEx)
    {
      hr = d3dEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, win->wnd, flags, &params, NULL,
                                 &devEx);
      dev = devEx;
    }
    else
    {
      hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, win->wnd, flags, &params, &dev);
    }

    if(SUCCEEDED(hr))
    {
      behaviorFlags = flags;
      break;
    }
  }

  if(FAILED(hr) || !dev)
  {
    TEST_ERROR("CreateDevice failed: %x", hr);
    return false;
  }

  D3DADAPTER_IDENTIFIER9 ident = {};
  d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident);
  TEST_LOG("Running D3D9%s test on %s", useEx ? "Ex" : "", ident.Description);

  const D3DVERTEXELEMENT9 elems[] = {
      {0, offsetof(DefaultA2V, pos), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
       D3DDECLUSAGE_POSITION, 0},
      {0, offsetof(DefaultA2V, col), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
      {0, offsetof(DefaultA2V, uv), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
       D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };

  hr = dev->CreateVertexDeclaration(elems, &defaultDecl);
  if(FAILED(hr))
  {
    TEST_ERROR("CreateVertexDeclaration failed: %x", hr);
    return false;
  }

  return true;
}

void D3D9GraphicsTest::Shutdown()
{
  defaultDecl = NULL;
  devEx = NULL;
  dev = NULL;
  d3dEx = NULL;
  d3d = NULL;

  delete mainWindow;
  mainWindow = NULL;
}

GraphicsWindow *D3D9GraphicsTest::MakeWindow(int width, int height, const char *title)
{
  return new Win32Window(width, height, title);
}

ID3DBlobPtr D3D9GraphicsTest::Compile(std::string src, std::string entry, std::string profile)
{
  ID3DBlobPtr blob = NULL;
  ID3DBlobPtr error = NULL;

  HRESULT hr =
      dyn_D3DCompile(src.c_str(), src.length(), "", NULL, NULL, entry.c_str(), profile.c_str(),
                     D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG, 0, &blob, &error);

  if(FAILED(hr))
  {
    TEST_ERROR("Failed to compile shader, error %x / %s", hr,
               error ? (char *)error->GetBufferPointer() : "Unknown");
    return NULL;
  }

  return blob;
}

std::vector<DWORD> D3D9GraphicsTest::CompileLoop(std::string src, std::string profile, UINT reg)
{
  ID3DBlobPtr blob = Compile(src, "main", profile);
  if(!blob)
    return {};

  const DWORD *p = (const DWORD *)blob->GetBufferPointer();
  const std::vector<DWORD> t(p, p + blob->GetBufferSize() / 4);
  if(t.empty())
    return t;

  std::vector<DWORD> ret = {t[0]};
  bool found = false;
  for(size_t i = 1; i < t.size();)
  {
    const DWORD op = t[i] & 0xFFFF;
    // comments give their length in bits 16-30, instructions in bits 24-27 from shader model 2 on
    size_t len = op == 0xFFFE ? ((t[i] >> 16) & 0x7FFF) : ((t[i] >> 24) & 0xF);
    if(op == 0xFFFF)
      len = 0;
    // defi (48): its destination is the integer constant register (type 7)
    const bool defi = op == 48 && i + 1 < t.size() && ((t[i + 1] >> 28) & 0x7) == 7;
    const bool remove = defi && (t[i + 1] & 0x7FF) == reg;
    if(remove)
      found = true;
    else if(defi)
      TEST_WARN("The %s loop shader defines i%u, not i%u", profile.c_str(), t[i + 1] & 0x7FF, reg);
    if(!remove)
      ret.insert(ret.end(), t.begin() + i, t.begin() + std::min(t.size(), i + 1 + len));
    i += 1 + len;
  }
  if(!found)
    TEST_WARN("The %s loop shader has no defi i%u, so its loop count is fixed", profile.c_str(), reg);
  return ret;
}

IDirect3DVertexShader9Ptr D3D9GraphicsTest::CreateVS(ID3DBlobPtr blob)
{
  IDirect3DVertexShader9Ptr ret;
  if(!blob)
    return ret;
  HRESULT hr = dev->CreateVertexShader((const DWORD *)blob->GetBufferPointer(), &ret);
  if(FAILED(hr))
    TEST_ERROR("CreateVertexShader failed: %x", hr);
  return ret;
}

IDirect3DPixelShader9Ptr D3D9GraphicsTest::CreatePS(ID3DBlobPtr blob)
{
  IDirect3DPixelShader9Ptr ret;
  if(!blob)
    return ret;
  HRESULT hr = dev->CreatePixelShader((const DWORD *)blob->GetBufferPointer(), &ret);
  if(FAILED(hr))
    TEST_ERROR("CreatePixelShader failed: %x", hr);
  return ret;
}

IDirect3DVertexBuffer9Ptr D3D9GraphicsTest::MakeVB(const void *data, UINT size)
{
  IDirect3DVertexBuffer9Ptr vb;

  HRESULT hr = dev->CreateVertexBuffer(size, devEx ? D3DUSAGE_WRITEONLY : 0, 0,
                                       devEx ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &vb, NULL);
  if(FAILED(hr))
  {
    TEST_ERROR("CreateVertexBuffer failed: %x", hr);
    return NULL;
  }

  void *ptr = NULL;
  hr = vb->Lock(0, size, &ptr, 0);
  if(FAILED(hr))
  {
    TEST_ERROR("Lock failed: %x", hr);
    return NULL;
  }
  memcpy(ptr, data, size);
  vb->Unlock();

  return vb;
}

bool D3D9GraphicsTest::Running()
{
  if(!FrameLimit())
    return false;

  return mainWindow->Update();
}

void D3D9GraphicsTest::Present()
{
  dev->Present(NULL, NULL, NULL, NULL);
}

void D3D9GraphicsTest::pushMarker(const std::string &name)
{
  if(dyn_BeginEvent)
    dyn_BeginEvent(0xffffffff, UTF82Wide(name).c_str());
}

void D3D9GraphicsTest::setMarker(const std::string &name)
{
  if(dyn_SetMarker)
    dyn_SetMarker(0xffffffff, UTF82Wide(name).c_str());
}

void D3D9GraphicsTest::popMarker()
{
  if(dyn_EndEvent)
    dyn_EndEvent();
}
