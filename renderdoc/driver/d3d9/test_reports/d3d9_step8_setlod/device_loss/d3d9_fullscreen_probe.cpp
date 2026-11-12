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

// a temporary probe: takes the display with an exclusive fullscreen D3D9 device for a few seconds,
// to see whether another process's windowed plain D3D9 device (RenderDoc's replay device) is lost

#include "d3d9_test.h"
#include "../win32/win32_window.h"

RD_TEST(D3D9_Fullscreen_Probe, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Probe: exclusive fullscreen for a few seconds";

  int main()
  {
    // the framework's window and windowed device, which is released for the fullscreen one
    if(!Init())
      return 3;

    dev = NULL;

    // a mode other than the desktop's, so the display mode really changes
    D3DDISPLAYMODE desktop = {};
    d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &desktop);
    D3DDISPLAYMODE mode = {};
    bool found = false;
    UINT count = d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
    for(UINT i = 0; i < count && !found; i++)
    {
      d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &mode);
      found = mode.Width == 1280 && mode.Height == 720;
    }
    if(!found)
      mode = desktop;
    TEST_LOG("Desktop %ux%u, going fullscreen at %ux%u %uHz", desktop.Width, desktop.Height,
             mode.Width, mode.Height, mode.RefreshRate);

    HWND wnd = ((Win32Window *)mainWindow)->wnd;

    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = mode.Width;
    pp.BackBufferHeight = mode.Height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = FALSE;
    pp.FullScreen_RefreshRateInHz = mode.RefreshRate;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    IDirect3DDevice9Ptr full;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &full);
    if(FAILED(hr))
    {
      TEST_ERROR("Couldn't create the fullscreen device: %x", hr);
      return 4;
    }
    TEST_LOG("Fullscreen device created");

    DWORD start = GetTickCount();
    int frames = 0;
    while(GetTickCount() - start < 4000)
    {
      MSG msg;
      while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
      {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
      full->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, (frames * 2) & 0xFF, 64), 1.0f, 0);
      hr = full->Present(NULL, NULL, NULL, NULL);
      if(FAILED(hr) && frames == 0)
        TEST_WARN("Present returned %x", hr);
      frames++;
    }
    TEST_LOG("Presented %d frames fullscreen", frames);

    full = NULL;
    return 0;
  }
};

REGISTER_TEST();
