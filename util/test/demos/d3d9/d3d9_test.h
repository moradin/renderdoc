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

#include "../test_common.h"

// d3d_helpers.h includes comdef.h, which must come before d3d9.h
#include "../dx/d3d_helpers.h"
#include "dx/official/d3d9.h"

COM_SMARTPTR(ID3DBlob);
// the D3D9 interface smart pointers (IDirect3DDevice9Ptr etc) come from d3d9.h, since comdef.h is
// included first

// shader model 3 versions of the default shaders, for DefaultA2V vertices
extern std::string D3D9DefaultVertex;
extern std::string D3D9DefaultPixel;

struct D3D9GraphicsTest : public GraphicsTest
{
  static const TestAPI API = TestAPI::D3D9;

  void Prepare(int argc, char **argv);
  bool Init();
  void Shutdown();
  GraphicsWindow *MakeWindow(int width, int height, const char *title);

  D3DPRESENT_PARAMETERS MakePresentParams(GraphicsWindow *win);

  ID3DBlobPtr Compile(std::string src, std::string entry, std::string profile);
  IDirect3DVertexShader9Ptr CreateVS(ID3DBlobPtr blob);
  IDirect3DPixelShader9Ptr CreatePS(ID3DBlobPtr blob);

  // fxc won't compile a loop over an int uniform bound to an i register, so loops have a literal
  // count. This compiles a shader with one, and removes the defi fxc writes for the count to
  // i<reg>, so that the count comes from Set*ShaderConstantI(reg, ...) as it would with a uniform.
  // The app has to set the same count. Returns no tokens if the shader doesn't compile.
  std::vector<DWORD> CompileLoop(std::string src, std::string profile, UINT reg = 0);

  // a vertex buffer containing the given vertices, in the managed pool (or default pool on Ex
  // devices, which don't support managed)
  IDirect3DVertexBuffer9Ptr MakeVB(const void *data, UINT size);

  bool Running();
  void Present();

  void pushMarker(const std::string &name);
  void setMarker(const std::string &name);
  void popMarker();

  // configuration, set before Init()
  bool useEx = false;
  DWORD extraBehaviorFlags = 0;
  D3DFORMAT backbufferFmt = D3DFMT_X8R8G8B8;
  D3DFORMAT depthFmt = D3DFMT_D24S8;

  GraphicsWindow *mainWindow = NULL;

  IDirect3D9Ptr d3d;
  IDirect3D9ExPtr d3dEx;
  IDirect3DDevice9Ptr dev;
  IDirect3DDevice9ExPtr devEx;

  D3DPRESENT_PARAMETERS presentParams = {};
  DWORD behaviorFlags = 0;

  // vertex declaration for DefaultA2V
  IDirect3DVertexDeclaration9Ptr defaultDecl;
};
