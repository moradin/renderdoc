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

// a temporary probe: what a ps_3_0 receives from vs_3_0 outputs packed into one register

#include <stdio.h>
#include "../win32/win32_window.h"
#include "d3d9_test.h"

typedef HRESULT(WINAPI *pD3DAssemble)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
                                      const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
                                      UINT Flags, ID3DBlob **ppShader, ID3DBlob **ppErrorMsgs);

RD_TEST(D3D9_Packed_Probe, D3D9GraphicsTest)
{
  static constexpr const char *Description = "Probe of packed vs_3_0 output linkage";

  pD3DAssemble assemble = NULL;

  std::vector<DWORD> Assemble(const std::string &src)
  {
    ID3DBlobPtr code, errors;
    if(FAILED(assemble(src.c_str(), src.size(), NULL, NULL, NULL, 0, &code, &errors)))
    {
      TEST_LOG("assemble failed: %s", errors ? (const char *)errors->GetBufferPointer() : "");
      return {};
    }
    const DWORD *p = (const DWORD *)code->GetBufferPointer();
    return std::vector<DWORD>(p, p + code->GetBufferSize() / 4);
  }

  void Run(IDirect3DDevice9 * d, const char *devName)
  {
    // o1 = (1,2,3,4), o2 = (5,6,7,8), o3 = (9,10,11,12)
    const std::string vsHead =
        "vs_3_0\ndcl_position v0\ndcl_position o0\n"
        "def c0, 1, 2, 3, 4\ndef c1, 5, 6, 7, 8\ndef c2, 9, 10, 11, 12\n";
    const std::string vsBody = "mov o0, v0\n";
    struct
    {
      const char *name;
      const char *dcls;
      const char *writes;
    } vss[] = {
        {"tc0 o1.xy, tc1 o1.zw", "dcl_texcoord0 o1.xy\ndcl_texcoord1 o1.zw\n", "mov o1, c0\n"},
        {"tc1 o1.zw, tc0 o1.xy (declared in that order)",
         "dcl_texcoord1 o1.zw\ndcl_texcoord0 o1.xy\n", "mov o1, c0\n"},
        {"tc0 o1, tc1 o2 (unpacked)", "dcl_texcoord0 o1\ndcl_texcoord1 o2\n",
         "mov o1, c0\nmov o2, c1\n"},
        {"tc0 o1.xz, tc1 o1.yw", "dcl_texcoord0 o1.xz\ndcl_texcoord1 o1.yw\n", "mov o1, c0\n"},
        {"tc0 o1.x, tc1 o1.y, tc2 o1.zw",
         "dcl_texcoord0 o1.x\ndcl_texcoord1 o1.y\ndcl_texcoord2 o1.zw\n", "mov o1, c0\n"},
        {"tc0 o1.xy, tc1 o1.zw, tc2 o2",
         "dcl_texcoord0 o1.xy\ndcl_texcoord1 o1.zw\ndcl_texcoord2 o2\n", "mov o1, c0\nmov o2, c1\n"},
        {"tc2 o2, tc0 o1.xy, tc1 o3.zw (one each)",
         "dcl_texcoord2 o2\ndcl_texcoord0 o1.xy\ndcl_texcoord1 o3.zw\n",
         "mov o1.xy, c0\nmov o2, c1\nmov o3.zw, c2\n"},
        {"color0 o1.xyz, fog o1.w", "dcl_color0 o1.xyz\ndcl_fog o1.w\n", "mov o1, c0\n"},
    };
    struct
    {
      const char *name;
      const char *src;
    } pss[] = {
        {"tc0 v0.xy", "dcl_texcoord0 v0.xy\nmov r0.xy, v0\n"},
        {"tc0 v0.zw", "dcl_texcoord0 v0.zw\nmov r0.zw, v0\n"},
        {"tc0 v0", "dcl_texcoord0 v0\nmov r0, v0\n"},
        {"tc1 v0.xy", "dcl_texcoord1 v0.xy\nmov r0.xy, v0\n"},
        {"tc1 v0.zw", "dcl_texcoord1 v0.zw\nmov r0.zw, v0\n"},
        {"tc1 v0", "dcl_texcoord1 v0\nmov r0, v0\n"},
        {"tc2 v0", "dcl_texcoord2 v0\nmov r0, v0\n"},
        {"tc0 v0.xy, tc1 v0.zw", "dcl_texcoord0 v0.xy\ndcl_texcoord1 v0.zw\nmov r0, v0\n"},
        {"tc1 v0.xy, tc0 v0.zw", "dcl_texcoord1 v0.xy\ndcl_texcoord0 v0.zw\nmov r0, v0\n"},
        {"tc0 v0.xz, tc1 v0.yw", "dcl_texcoord0 v0.xz\ndcl_texcoord1 v0.yw\nmov r0, v0\n"},
        {"tc0 v3.xy, tc1 v5.xy", "dcl_texcoord0 v3.xy\ndcl_texcoord1 v5.xy\nmov r0.xy, v3\nmov r0.zw, v5.xyxy\n"},
        {"color0 v0.xyz", "dcl_color0 v0.xyz\nmov r0.xyz, v0\n"},
        {"fog v0.x", "dcl_fog v0.x\nmov r0.x, v0\n"},
        {"color0 v0.xyz, fog v1.x", "dcl_color0 v0.xyz\ndcl_fog v1.x\nmov r0.xyz, v0\nmov r0.w, v1.x\n"},
    };

    IDirect3DSurface9Ptr rt, read, old;
    d->CreateRenderTarget(1, 1, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    d->CreateOffscreenPlainSurface(1, 1, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &read, NULL);
    d->GetRenderTarget(0, &old);

    D3DVERTEXELEMENT9 el[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    IDirect3DVertexDeclaration9Ptr decl;
    d->CreateVertexDeclaration(el, &decl);
    float vert[3] = {0.0f, 0.0f, 0.5f};

    for(const auto &v : vss)
    {
      std::vector<DWORD> vt = Assemble(vsHead + v.dcls + vsBody + v.writes);
      IDirect3DVertexShader9Ptr vs;
      if(vt.empty() || FAILED(d->CreateVertexShader(vt.data(), &vs)))
      {
        TEST_LOG("%s: VS %s: couldn't create", devName, v.name);
        continue;
      }
      for(const auto &p : pss)
      {
        // the dcls first, then -1 in r0, then the moves
        std::string src = p.src;
        size_t split = src.find("mov");
        std::vector<DWORD> pt =
            Assemble("ps_3_0\n" + src.substr(0, split) + "def c0, -1, -1, -1, -1\nmov r0, c0\n" +
                     src.substr(split) + "mov oC0, r0\n");
        IDirect3DPixelShader9Ptr ps;
        if(pt.empty() || FAILED(d->CreatePixelShader(pt.data(), &ps)))
        {
          TEST_LOG("%s: PS %s: couldn't create", devName, p.name);
          continue;
        }
        d->SetRenderTarget(0, rt);
        d->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(7, 7, 7, 7), 1.0f, 0);
        D3DVIEWPORT9 vp = {0, 0, 1, 1, 0.0f, 1.0f};
        d->SetViewport(&vp);
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_LIGHTING, FALSE);
        float size = 4.0f;
        d->SetRenderState(D3DRS_POINTSIZE, *(DWORD *)&size);
        d->SetVertexDeclaration(decl);
        d->SetVertexShader(vs);
        d->SetPixelShader(ps);
        d->BeginScene();
        HRESULT hr = d->DrawPrimitiveUP(D3DPT_POINTLIST, 1, vert, sizeof(vert));
        d->EndScene();
        d->SetRenderTarget(0, old);
        d->SetVertexShader(NULL);
        d->SetPixelShader(NULL);

        float out[4] = {};
        D3DLOCKED_RECT lr = {};
        if(SUCCEEDED(d->GetRenderTargetData(rt, read)) &&
           SUCCEEDED(read->LockRect(&lr, NULL, D3DLOCK_READONLY)))
        {
          memcpy(out, lr.pBits, sizeof(out));
          read->UnlockRect();
        }
        TEST_LOG("%s: VS %-45s PS %-24s draw %08x: %g %g %g %g", devName, v.name, p.name, (UINT)hr,
                 out[0], out[1], out[2], out[3]);
      }
    }
  }

  int main()
  {
    if(!Init())
      return 3;

    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    assemble = d3dcompiler ? (pD3DAssemble)GetProcAddress(d3dcompiler, "D3DAssemble") : NULL;
    if(!assemble)
      return 4;

    TEST_LOG("-1 is what the PS left unwritten, 7/255 a pixel the draw didn't cover");
    Run(dev, "hw");

    Win32Window *win = (Win32Window *)MakeWindow(screenWidth, screenHeight, "sw");
    D3DPRESENT_PARAMETERS pp = presentParams;
    pp.hDeviceWindow = win->wnd;
    IDirect3DDevice9Ptr sw;
    if(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, win->wnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &sw)))
      Run(sw, "sw");
    sw = NULL;
    delete win;

    return 0;
  }
};

REGISTER_TEST();
