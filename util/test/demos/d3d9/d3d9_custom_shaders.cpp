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

#define CHECK_HR(expr)                                    \
  do                                                      \
  {                                                       \
    HRESULT check_hr = (expr);                            \
    if(FAILED(check_hr))                                  \
      TEST_ERROR("%s failed: %x", #expr, (UINT)check_hr); \
  } while(0)

// The textures, each a size no other has so the test can find it (util/test/tests/D3D9/
// D3D9_Custom_Shaders.py has the same list):
// - "Default 2D": A8R8G8B8 64x32, 3 mips, D3DPOOL_DEFAULT. Sampled directly by the texture viewer.
// - "Managed 2D": A8R8G8B8 48x24, 2 mips, D3DPOOL_MANAGED. Decoded on the CPU.
// - "L8": L8 16x8, D3DPOOL_DEFAULT. Sampled directly, as (L, L, L, 1).
// - "Cube": A8R8G8B8 16x16, 2 mips, D3DPOOL_DEFAULT.
// - "Volume": A8R8G8B8 16x8x4, 2 mips, D3DPOOL_DEFAULT.
// - "Render Target": an A8R8G8B8 40x20 render target surface, ColorFilled in two halves each frame.
//   The texture viewer copies it into a texture.
// - "Depth": a D24S8 40x20 depth stencil, cleared in two halves each frame. Decoded on the CPU.
//
// Before the first frame, natively, each texture's subresources are drawn with SampleBody (the
// shader the test builds as a custom shader) and what the GPU returned at a few texels is written
// to D3D9_Custom_Shaders_reference.txt. The render target is copied into a texture first, the
// depth stencil's values are the ones it's cleared to. PositionBody is drawn into a 16x8 target
// the same way. Both bodies are written next to it for the test to build, with the RD_ functions
// that RenderDoc's prefix has defined here by NativePrefix.
RD_TEST(D3D9_Custom_Shaders, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Textures of each kind the texture viewer samples differently, and a reference of what the "
      "GPU samples from them with the shader the test then uses as a custom display shader.";

  // the RD_ functions that RenderDoc's prefix defines, from constants we set
  const std::string NativePrefix = R"EOSHADER(
float4 NativeTexDim : register(c0);
float4 NativeSelected : register(c1);

#define RD_TextureType_2D 2
#define RD_TextureType_3D 3
#define RD_TextureType_Cube 8

uint RD_TextureType() { return (uint)max(NativeSelected.w, 0.0f); }
uint RD_SelectedMip() { return (uint)max(NativeSelected.x, 0.0f); }
uint RD_SelectedSliceFace() { return (uint)max(NativeSelected.y, 0.0f); }
uint4 RD_TexDim() { return (uint4)max(NativeTexDim, 0.0f); }
)EOSHADER";

  const std::string SampleBody = R"EOSHADER(
sampler2D tex2d : register(s0);
samplerCUBE texCube : register(s1);
sampler3D tex3d : register(s2);

float3 CubeDir(uint face, float2 uv)
{
  float2 st = uv * 2.0f - 1.0f;
  if(face == 0)
    return float3(1.0f, -st.y, -st.x);
  if(face == 1)
    return float3(-1.0f, -st.y, st.x);
  if(face == 2)
    return float3(st.x, 1.0f, st.y);
  if(face == 3)
    return float3(st.x, -1.0f, -st.y);
  if(face == 4)
    return float3(st.x, -st.y, 1.0f);
  return float3(-st.x, -st.y, -1.0f);
}

float4 main(float4 pos : SV_Position, float4 uv : TEXCOORD0) : SV_Target0
{
  float mip = (float)RD_SelectedMip();
  if(RD_TextureType() == RD_TextureType_Cube)
    return texCUBElod(texCube, float4(CubeDir(RD_SelectedSliceFace(), uv.xy), mip));
  if(RD_TextureType() == RD_TextureType_3D)
  {
    float depth = max(1.0f, floor((float)RD_TexDim().z / exp2(mip)));
    return tex3Dlod(tex3d, float4(uv.xy, ((float)RD_SelectedSliceFace() + 0.5f) / depth, mip));
  }
  return tex2Dlod(tex2d, float4(uv.xy, 0, mip));
}
)EOSHADER";

  // uv's z and w should be 0, any other value shows up multiplied by 1000
  const std::string PositionBody = R"EOSHADER(
float4 main(float4 pos : SV_Position, float4 uv : TEXCOORD0) : SV_Target0
{
  return float4(pos.xy, uv.xy + uv.zw * 1000.0f);
}
)EOSHADER";

  // a full target quad with D3D9's half pixel offset. Like the texture viewer's: uv in TEXCOORD0,
  // with z and w 0.
  const std::string QuadVS = R"EOSHADER(
float4 halfPixel : register(c0);

struct v2f
{
  float4 pos : POSITION;
  float4 uv : TEXCOORD0;
};

v2f main(float4 pos : POSITION)
{
  v2f OUT;
  OUT.pos = float4(pos.xy + halfPixel.xy, 0, 1);
  OUT.uv = float4(pos.zw, 0, 0);
  return OUT;
}
)EOSHADER";

  struct Texel
  {
    uint8_t r, g, b, a;
  };

  static Texel Pattern(UINT x, UINT y, UINT slice, UINT mip, UINT seed)
  {
    return {
        uint8_t((x * 16 + seed * 7) & 0xFF),
        uint8_t((y * 16 + mip * 64) & 0xFF),
        uint8_t((slice * 40 + seed * 30 + 5) & 0xFF),
        uint8_t((255 - x * 8 - y * 4) & 0xFF),
    };
  }

  static DWORD PatternARGB(UINT x, UINT y, UINT slice, UINT mip, UINT seed)
  {
    Texel t = Pattern(x, y, slice, mip, seed);
    return D3DCOLOR_ARGB(t.a, t.r, t.g, t.b);
  }

  void FillRect(D3DLOCKED_RECT & locked, UINT w, UINT h, UINT slice, UINT mip, UINT seed)
  {
    for(UINT y = 0; y < h; y++)
    {
      DWORD *row = (DWORD *)((byte *)locked.pBits + y * locked.Pitch);
      for(UINT x = 0; x < w; x++)
        row[x] = PatternARGB(x, y, slice, mip, seed);
    }
  }

  IDirect3DTexture9Ptr Make2D(UINT w, UINT h, UINT mips, D3DPOOL pool, UINT seed)
  {
    IDirect3DTexture9Ptr fill;
    CHECK_HR(dev->CreateTexture(w, h, mips, 0, D3DFMT_A8R8G8B8,
                                pool == D3DPOOL_DEFAULT ? D3DPOOL_SYSTEMMEM : pool, &fill, NULL));
    for(UINT m = 0; m < mips; m++)
    {
      D3DLOCKED_RECT locked = {};
      CHECK_HR(fill->LockRect(m, &locked, NULL, 0));
      FillRect(locked, std::max(1U, w >> m), std::max(1U, h >> m), 0, m, seed);
      fill->UnlockRect(m);
    }
    if(pool != D3DPOOL_DEFAULT)
      return fill;

    IDirect3DTexture9Ptr ret;
    CHECK_HR(dev->CreateTexture(w, h, mips, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ret, NULL));
    CHECK_HR(dev->UpdateTexture(fill, ret));
    return ret;
  }

  static uint8_t PatternL8(UINT x, UINT y)
  {
    return uint8_t((x * 16 + y * 8 + 3) & 0xFF);
  }

  struct Native
  {
    IDirect3DVertexShader9Ptr vs;
    IDirect3DPixelShader9Ptr samplePS, positionPS;
    IDirect3DVertexDeclaration9Ptr decl;
    FILE *f = NULL;
  } native;

  // draws ps over a w x h float target and returns its texels
  std::vector<Vec4f> DrawNative(IDirect3DPixelShader9 * ps, UINT w, UINT h)
  {
    std::vector<Vec4f> ret;

    IDirect3DSurface9Ptr rt, readback;
    CHECK_HR(dev->CreateRenderTarget(w, h, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt,
                                     NULL));
    CHECK_HR(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM,
                                              &readback, NULL));

    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(NULL);
    const D3DVIEWPORT9 view = {0, 0, w, h, 0.0f, 1.0f};
    dev->SetViewport(&view);

    const float halfPixel[4] = {-1.0f / float(w), 1.0f / float(h), 0.0f, 0.0f};
    dev->SetVertexShaderConstantF(0, halfPixel, 1);
    dev->SetVertexShader(native.vs);
    dev->SetPixelShader(ps);
    dev->SetVertexDeclaration(native.decl);

    const Vec4f quad[4] = {
        Vec4f(-1.0f, 1.0f, 0.0f, 0.0f),
        Vec4f(1.0f, 1.0f, 1.0f, 0.0f),
        Vec4f(-1.0f, -1.0f, 0.0f, 1.0f),
        Vec4f(1.0f, -1.0f, 1.0f, 1.0f),
    };
    dev->BeginScene();
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vec4f));
    dev->EndScene();

    CHECK_HR(dev->GetRenderTargetData(rt, readback));
    D3DLOCKED_RECT locked = {};
    CHECK_HR(readback->LockRect(&locked, NULL, D3DLOCK_READONLY));
    ret.resize(w * h);
    for(UINT y = 0; y < h; y++)
      memcpy(&ret[y * w], (byte *)locked.pBits + y * locked.Pitch, w * sizeof(Vec4f));
    readback->UnlockRect();

    return ret;
  }

  void WriteTexels(const char *name, UINT mip, UINT slice, UINT w, UINT h,
                   const std::vector<Vec4f> &texels)
  {
    const UINT probes[][2] = {
        {0, 0}, {w - 1, 0}, {w / 2, h / 2}, {0, h - 1}, {w - 1, h - 1}, {w / 3, h / 4},
    };
    for(const auto &p : probes)
    {
      const Vec4f &v = texels[p[1] * w + p[0]];
      fprintf(native.f, "%s\t%u\t%u\t%u\t%u\t%f\t%f\t%f\t%f\n", name, mip, slice, p[0], p[1], v.x,
              v.y, v.z, v.w);
    }
  }

  // draws SampleBody over one subresource, with the texture at the sampler of its type
  void SampleNative(const char *name, IDirect3DBaseTexture9 *tex, float type, UINT w, UINT h,
                    UINT depth, UINT mips, UINT mip, UINT slice)
  {
    for(DWORD s = 0; s < 3; s++)
    {
      dev->SetTexture(s, NULL);
      dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    }
    dev->SetTexture(type == 8.0f ? 1 : type == 3.0f ? 2 : 0, tex);

    const float consts[8] = {
        float(w), float(h), float(depth), float(mips), float(mip), float(slice), 0.0f, type,
    };
    dev->SetPixelShaderConstantF(0, consts, 2);

    const UINT mw = std::max(1U, w >> mip), mh = std::max(1U, h >> mip);
    WriteTexels(name, mip, slice, mw, mh, DrawNative(native.samplePS, mw, mh));

    for(DWORD s = 0; s < 3; s++)
      dev->SetTexture(s, NULL);
  }

  void WriteFile(const std::string &name, const std::string &contents)
  {
    const std::string path = tmpPath + "/" + name;
    FILE *f = NULL;
    fopen_s(&f, path.c_str(), "w");
    if(!f)
    {
      TEST_ERROR("Couldn't write %s", path.c_str());
      return;
    }
    fputs(contents.c_str(), f);
    fclose(f);
  }

  void FillTargets(IDirect3DSurface9 * rt, IDirect3DSurface9 * depth,
                   IDirect3DSurface9 * backBuffer, IDirect3DSurface9 * autoDepth)
  {
    const RECT left = {0, 0, 20, 20}, right = {20, 0, 40, 20};
    dev->ColorFill(rt, &left, D3DCOLOR_ARGB(255, 0x33, 0x66, 0x99));
    dev->ColorFill(rt, &right, D3DCOLOR_ARGB(0x80, 0xCC, 0x00, 0x11));

    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(depth);
    const D3DRECT dleft = {0, 0, 20, 20}, dright = {20, 0, 40, 20};
    dev->Clear(1, &dleft, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.25f, 7);
    dev->Clear(1, &dright, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.75f, 200);
    dev->SetRenderTarget(0, backBuffer);
    dev->SetDepthStencilSurface(autoDepth);
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3DBlobPtr vsblob = Compile(D3D9DefaultVertex, "main", "vs_3_0");
    ID3DBlobPtr psblob = Compile(D3D9DefaultPixel, "main", "ps_3_0");
    ID3DBlobPtr quadvs = Compile(QuadVS, "main", "vs_3_0");
    ID3DBlobPtr sampleps = Compile(NativePrefix + SampleBody, "main", "ps_3_0");
    ID3DBlobPtr positionps = Compile(PositionBody, "main", "ps_3_0");
    if(!vsblob || !psblob || !quadvs || !sampleps || !positionps)
      return 4;

    IDirect3DVertexShader9Ptr vs = CreateVS(vsblob);
    IDirect3DPixelShader9Ptr ps = CreatePS(psblob);
    IDirect3DVertexBuffer9Ptr vb = MakeVB(DefaultTri, sizeof(DefaultA2V) * 3);

    native.vs = CreateVS(quadvs);
    native.samplePS = CreatePS(sampleps);
    native.positionPS = CreatePS(positionps);
    const D3DVERTEXELEMENT9 quadElements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    CHECK_HR(dev->CreateVertexDeclaration(quadElements, &native.decl));

    IDirect3DTexture9Ptr default2D = Make2D(64, 32, 3, D3DPOOL_DEFAULT, 1);
    IDirect3DTexture9Ptr managed2D = Make2D(48, 24, 2, D3DPOOL_MANAGED, 2);

    IDirect3DTexture9Ptr l8;
    {
      IDirect3DTexture9Ptr fill;
      CHECK_HR(dev->CreateTexture(16, 8, 1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &fill, NULL));
      D3DLOCKED_RECT locked = {};
      CHECK_HR(fill->LockRect(0, &locked, NULL, 0));
      for(UINT y = 0; y < 8; y++)
        for(UINT x = 0; x < 16; x++)
          ((byte *)locked.pBits)[y * locked.Pitch + x] = PatternL8(x, y);
      fill->UnlockRect(0);
      CHECK_HR(dev->CreateTexture(16, 8, 1, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &l8, NULL));
      CHECK_HR(dev->UpdateTexture(fill, l8));
    }

    IDirect3DCubeTexture9Ptr cube;
    {
      IDirect3DCubeTexture9Ptr fill;
      CHECK_HR(dev->CreateCubeTexture(16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &fill, NULL));
      for(UINT face = 0; face < 6; face++)
      {
        for(UINT m = 0; m < 2; m++)
        {
          D3DLOCKED_RECT locked = {};
          CHECK_HR(fill->LockRect((D3DCUBEMAP_FACES)face, m, &locked, NULL, 0));
          FillRect(locked, 16 >> m, 16 >> m, face, m, 3);
          fill->UnlockRect((D3DCUBEMAP_FACES)face, m);
        }
      }
      CHECK_HR(dev->CreateCubeTexture(16, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube, NULL));
      CHECK_HR(dev->UpdateTexture(fill, cube));
    }

    IDirect3DVolumeTexture9Ptr volume;
    {
      IDirect3DVolumeTexture9Ptr fill;
      CHECK_HR(dev->CreateVolumeTexture(16, 8, 4, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &fill,
                                        NULL));
      for(UINT m = 0; m < 2; m++)
      {
        D3DLOCKED_BOX locked = {};
        CHECK_HR(fill->LockBox(m, &locked, NULL, 0));
        const UINT w = 16 >> m, h = 8 >> m, d = 4 >> m;
        for(UINT z = 0; z < d; z++)
        {
          for(UINT y = 0; y < h; y++)
          {
            DWORD *row =
                (DWORD *)((byte *)locked.pBits + z * locked.SlicePitch + y * locked.RowPitch);
            for(UINT x = 0; x < w; x++)
              row[x] = PatternARGB(x, y, z, m, 4);
          }
        }
        fill->UnlockBox(m);
      }
      CHECK_HR(dev->CreateVolumeTexture(16, 8, 4, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &volume,
                                        NULL));
      CHECK_HR(dev->UpdateTexture(fill, volume));
    }

    IDirect3DSurface9Ptr rt, depth;
    CHECK_HR(
        dev->CreateRenderTarget(40, 20, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL));
    CHECK_HR(dev->CreateDepthStencilSurface(40, 20, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                            &depth, NULL));

    IDirect3DSurface9Ptr backBuffer, autoDepth;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    dev->GetDepthStencilSurface(&autoDepth);

    FillTargets(rt, depth, backBuffer, autoDepth);

    // the native reference, before the first frame
    if(!rdoc)
    {
      WriteFile("D3D9_Custom_Shaders_sample.hlsl", SampleBody);
      WriteFile("D3D9_Custom_Shaders_position.hlsl", PositionBody);

      const std::string path = tmpPath + "/D3D9_Custom_Shaders_reference.txt";
      fopen_s(&native.f, path.c_str(), "w");
      if(!native.f)
      {
        TEST_ERROR("Couldn't write %s", path.c_str());
        return 5;
      }

      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);

      for(UINT m = 0; m < 3; m++)
        SampleNative("Default 2D", default2D, 2.0f, 64, 32, 1, 3, m, 0);
      for(UINT m = 0; m < 2; m++)
        SampleNative("Managed 2D", managed2D, 2.0f, 48, 24, 1, 2, m, 0);
      SampleNative("L8", l8, 2.0f, 16, 8, 1, 1, 0, 0);
      for(UINT face = 0; face < 6; face++)
        for(UINT m = 0; m < 2; m++)
          SampleNative("Cube", cube, 8.0f, 16, 16, 6, 2, m, face);
      for(UINT m = 0; m < 2; m++)
        for(UINT z = 0; z < (4U >> m); z++)
          SampleNative("Volume", volume, 3.0f, 16, 8, 4, 2, m, z);

      // the render target, sampled from a copy as the texture viewer does
      {
        IDirect3DTexture9Ptr copy;
        IDirect3DSurface9Ptr copySurf;
        CHECK_HR(dev->CreateTexture(40, 20, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &copy, NULL));
        CHECK_HR(copy->GetSurfaceLevel(0, &copySurf));
        CHECK_HR(dev->StretchRect(rt, NULL, copySurf, NULL, D3DTEXF_NONE));
        SampleNative("Render Target", copy, 2.0f, 40, 20, 1, 1, 0, 0);
      }

      // the depth stencil can't be sampled, these are the values it's cleared to: depth in red,
      // stencil in green as RenderDoc decodes it
      {
        const UINT probes[][2] = {{0, 0}, {19, 19}, {20, 0}, {39, 19}};
        for(const auto &p : probes)
        {
          const bool left = p[0] < 20;
          fprintf(native.f, "Depth\t0\t0\t%u\t%u\t%f\t%f\t%f\t%f\n", p[0], p[1],
                  left ? 0.25f : 0.75f, (left ? 7.0f : 200.0f) / 255.0f, 0.0f, 1.0f);
        }
      }

      // the position shader, over a 16x8 target like L8's
      {
        std::vector<Vec4f> texels = DrawNative(native.positionPS, 16, 8);
        WriteTexels("Position", 0, 0, 16, 8, texels);
      }

      fclose(native.f);
      native.f = NULL;

      dev->SetRenderTarget(0, backBuffer);
      dev->SetDepthStencilSurface(autoDepth);
      const D3DVIEWPORT9 full = {0, 0, (DWORD)screenWidth, (DWORD)screenHeight, 0.0f, 1.0f};
      dev->SetViewport(&full);
    }

    while(Running())
    {
      dev->SetRenderTarget(0, backBuffer);
      dev->SetDepthStencilSurface(autoDepth);
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      dev->BeginScene();

      pushMarker("Fill Targets");
      FillTargets(rt, depth, backBuffer, autoDepth);
      popMarker();

      // the textures are all bound to the draw, so they're in the capture
      pushMarker("Triangle");
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetTexture(0, default2D);
      dev->SetTexture(1, managed2D);
      dev->SetTexture(2, l8);
      dev->SetTexture(3, cube);
      dev->SetTexture(4, volume);
      dev->SetVertexDeclaration(defaultDecl);
      dev->SetStreamSource(0, vb, 0, sizeof(DefaultA2V));
      dev->SetVertexShader(vs);
      dev->SetPixelShader(ps);
      dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
      for(DWORD s = 0; s < 5; s++)
        dev->SetTexture(s, NULL);
      popMarker();

      dev->EndScene();

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
