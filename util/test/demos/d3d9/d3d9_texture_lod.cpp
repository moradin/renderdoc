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

RD_TEST(D3D9_Texture_LOD, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "SetLOD on managed 2D, cube, volume and vertex textures, set before the frame and changed in "
      "it. Each is sampled at LODs 0-7 into a row of a target, and the demo checks natively that "
      "the GPU read the levels SetLOD says.";

  // Each texture's levels are solid colours, which say the texture (blue) and level (red)
  static DWORD LevelColour(UINT tex, UINT level)
  {
    return 0xFF004000 | ((0x10 + 0x20 * level) << 16) | (tex + 1);
  }

  enum
  {
    Tex2D,
    TexCube,
    TexVolume,
    TexVertex,
    TexCount
  };

  struct TexInfo
  {
    const char *name;
    UINT size;
    UINT levels;
    // the LOD set before the frame, and the one the frame changes it to before the second pass,
    // and back after it
    DWORD before, inFrame;
  };

  const TexInfo Info[TexCount] = {
      {"2D", 64, 7, 2, 4},
      {"Cube", 32, 6, 1, 3},
      {"Volume", 16, 5, 3, 1},
      {"Vertex", 64, 7, 3, 5},
  };

  // the target: one row per texture, column c sampled at LOD c
  static const UINT Columns = 8, Width = 16, Height = TexCount;

  // the level the GPU reads at an integer LOD with MIPFILTER POINT: SetLOD's level is read as
  // level 0, and MAXMIPLEVEL counts from it
  static UINT ExpectedLevel(DWORD lod, DWORD maxMip, UINT levels, UINT column)
  {
    UINT base = lod < levels ? lod : levels - 1;
    UINT level = base + (column > maxMip ? column : maxMip);
    return level < levels ? level : levels - 1;
  }

  std::string PixelRow = R"EOSHADER(

sampler2D s2D : register(s0);
samplerCUBE sCube : register(s1);
sampler3D sVolume : register(s2);
float4 which : register(c0);

float4 main(float2 vpos : VPOS) : COLOR0
{
  float lod = floor(vpos.x);
  if(which.x == 0)
    return tex2Dlod(s2D, float4(0.5, 0.5, 0, lod));
  if(which.x == 1)
    return texCUBElod(sCube, float4(1, 0, 0, lod));
  return tex3Dlod(sVolume, float4(0.5, 0.5, 0.5, lod));
}

)EOSHADER";

  std::string QuadVS = R"EOSHADER(

float4 main(float4 pos : POSITION) : POSITION
{
  return pos;
}

)EOSHADER";

  // points at x = column + 1 in the vertex row, each with the vertex texture at LOD column
  std::string VertexRowVS = R"EOSHADER(

sampler2D sVertex : register(s0);
float4 row : register(c0);

struct v2f
{
  float4 pos : POSITION;
  float4 col : COLOR0;
};

v2f main(float4 pos : POSITION)
{
  v2f o;
  o.pos = float4(pos.x, row.x, 0.5, 1);
  o.col = tex2Dlod(sVertex, float4(0.5, 0.5, 0, pos.y));
  return o;
}

)EOSHADER";

  std::string ColourPS = R"EOSHADER(

float4 main(float4 col : COLOR0) : COLOR0
{
  return col;
}

)EOSHADER";

  IDirect3DTexture9Ptr tex2D, texVertex;
  IDirect3DCubeTexture9Ptr texCube;
  IDirect3DVolumeTexture9Ptr texVolume;

  IDirect3DBaseTexture9 *Texture(int i)
  {
    switch(i)
    {
      case Tex2D: return tex2D;
      case TexCube: return texCube;
      case TexVolume: return texVolume;
      default: return texVertex;
    }
  }

  bool Fill()
  {
    for(int t : {Tex2D, TexVertex})
    {
      IDirect3DTexture9 *tex = t == Tex2D ? tex2D.GetInterfacePtr() : texVertex.GetInterfacePtr();
      for(UINT l = 0; l < tex->GetLevelCount(); l++)
      {
        D3DSURFACE_DESC desc;
        D3DLOCKED_RECT lr;
        if(FAILED(tex->GetLevelDesc(l, &desc)) || FAILED(tex->LockRect(l, &lr, NULL, 0)))
          return false;
        for(UINT y = 0; y < desc.Height; y++)
          for(UINT x = 0; x < desc.Width; x++)
            ((DWORD *)((byte *)lr.pBits + y * lr.Pitch))[x] = LevelColour(t, l);
        tex->UnlockRect(l);
      }
    }

    for(UINT f = 0; f < 6; f++)
    {
      for(UINT l = 0; l < texCube->GetLevelCount(); l++)
      {
        D3DSURFACE_DESC desc;
        D3DLOCKED_RECT lr;
        if(FAILED(texCube->GetLevelDesc(l, &desc)) ||
           FAILED(texCube->LockRect((D3DCUBEMAP_FACES)f, l, &lr, NULL, 0)))
          return false;
        for(UINT y = 0; y < desc.Height; y++)
          for(UINT x = 0; x < desc.Width; x++)
            ((DWORD *)((byte *)lr.pBits + y * lr.Pitch))[x] = LevelColour(TexCube, l);
        texCube->UnlockRect((D3DCUBEMAP_FACES)f, l);
      }
    }

    for(UINT l = 0; l < texVolume->GetLevelCount(); l++)
    {
      D3DVOLUME_DESC desc;
      D3DLOCKED_BOX lb;
      if(FAILED(texVolume->GetLevelDesc(l, &desc)) || FAILED(texVolume->LockBox(l, &lb, NULL, 0)))
        return false;
      for(UINT z = 0; z < desc.Depth; z++)
        for(UINT y = 0; y < desc.Height; y++)
          for(UINT x = 0; x < desc.Width; x++)
            ((DWORD *)((byte *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch))[x] =
                LevelColour(TexVolume, l);
      texVolume->UnlockBox(l);
    }

    return true;
  }

  void SetSamplers(DWORD maxMip)
  {
    const DWORD samplers[] = {0, 1, 2, D3DVERTEXTEXTURESAMPLER0};
    for(DWORD s : samplers)
    {
      dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
      dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, maxMip);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      dev->SetSamplerState(s, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    }
  }

  // checks a target's rows against the levels SetLOD and MAXMIPLEVEL say. The vertex row is
  // only drawn when vertex textures are supported.
  bool Check(IDirect3DSurface9 * target, IDirect3DSurface9 * sysmem, bool afterChange, DWORD maxMip,
             bool vertexRow, int &checked)
  {
    if(FAILED(dev->GetRenderTargetData(target, sysmem)))
    {
      TEST_ERROR("Couldn't read back the target");
      return false;
    }

    D3DLOCKED_RECT lr;
    if(FAILED(sysmem->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
      TEST_ERROR("Couldn't lock the readback surface");
      return false;
    }

    bool ok = true;
    for(int t = 0; t < TexCount; t++)
    {
      if(t == TexVertex && !vertexRow)
        continue;

      const TexInfo &info = Info[t];
      const DWORD lod = afterChange ? info.inFrame : info.before;
      const DWORD *row = (const DWORD *)((const byte *)lr.pBits + t * lr.Pitch);
      for(UINT c = 0; c < Columns; c++)
      {
        const UINT x = t == TexVertex ? c + 1 : c;
        const DWORD expected = LevelColour(t, ExpectedLevel(lod, maxMip, info.levels, c));
        checked++;
        if(row[x] != expected)
        {
          TEST_ERROR(
              "Frame %d: the %s texture with LOD %u and MAXMIPLEVEL %u at LOD %u read %08x, "
              "expected %08x",
              curFrame, info.name, lod, maxMip, c, row[x], expected);
          ok = false;
        }
      }
    }

    sysmem->UnlockRect();
    return ok;
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    D3DCAPS9 caps = {};
    dev->GetDeviceCaps(&caps);

    const bool vertexTextures = SUCCEEDED(
        d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, backbufferFmt,
                               D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8));
    if(!vertexTextures)
      TEST_WARN("A8R8G8B8 can't be a vertex texture, the vertex row isn't drawn");

    if(FAILED(dev->CreateTexture(Info[Tex2D].size, Info[Tex2D].size, Info[Tex2D].levels, 0,
                                 D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex2D, NULL)) ||
       FAILED(dev->CreateCubeTexture(Info[TexCube].size, Info[TexCube].levels, 0, D3DFMT_A8R8G8B8,
                                     D3DPOOL_MANAGED, &texCube, NULL)) ||
       FAILED(dev->CreateVolumeTexture(Info[TexVolume].size, Info[TexVolume].size,
                                       Info[TexVolume].size, Info[TexVolume].levels, 0,
                                       D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texVolume, NULL)) ||
       FAILED(dev->CreateTexture(Info[TexVertex].size, Info[TexVertex].size, Info[TexVertex].levels,
                                 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texVertex, NULL)) ||
       !Fill())
    {
      TEST_ERROR("Couldn't create the managed textures");
      return 4;
    }

    // the LODs the frame starts with
    for(int t = 0; t < TexCount; t++)
      Texture(t)->SetLOD(Info[t].before);

    IDirect3DTexture9Ptr targets[2];
    IDirect3DSurface9Ptr targetSurfs[2], sysmem;
    for(int i = 0; i < 2; i++)
    {
      if(FAILED(dev->CreateTexture(Width, Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                   D3DPOOL_DEFAULT, &targets[i], NULL)) ||
         FAILED(targets[i]->GetSurfaceLevel(0, &targetSurfs[i])))
      {
        TEST_ERROR("Couldn't create the targets");
        return 4;
      }
    }
    if(FAILED(dev->CreateOffscreenPlainSurface(Width, Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                               &sysmem, NULL)))
    {
      TEST_ERROR("Couldn't create the readback surface");
      return 4;
    }

    IDirect3DVertexShader9Ptr quadVS = CreateVS(Compile(QuadVS, "main", "vs_3_0"));
    IDirect3DPixelShader9Ptr rowPS = CreatePS(Compile(PixelRow, "main", "ps_3_0"));
    IDirect3DVertexShader9Ptr vertexVS = CreateVS(Compile(VertexRowVS, "main", "vs_3_0"));
    IDirect3DPixelShader9Ptr colourPS = CreatePS(Compile(ColourPS, "main", "ps_3_0"));
    if(!quadVS || !rowPS || !vertexVS || !colourPS)
    {
      TEST_ERROR("Couldn't create the shaders");
      return 4;
    }

    const D3DVERTEXELEMENT9 elems[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9Ptr decl;
    dev->CreateVertexDeclaration(elems, &decl);

    const float quad[] = {
        -1, -1, 0.5f, 1, 1, -1, 0.5f, 1, -1, 1, 0.5f, 1, 1, 1, 0.5f, 1,
    };
    IDirect3DVertexBuffer9Ptr quadVB = MakeVB(quad, sizeof(quad));

    // the vertex row's points: x in clip space for pixel column + 1, y the LOD
    float points[Columns * 4];
    for(UINT c = 0; c < Columns; c++)
    {
      points[c * 4 + 0] = 2.0f * float(c + 1) / float(Width) - 1.0f;
      points[c * 4 + 1] = float(c);
      points[c * 4 + 2] = 0.0f;
      points[c * 4 + 3] = 1.0f;
    }
    IDirect3DVertexBuffer9Ptr pointVB = MakeVB(points, sizeof(points));

    // the vertex row's y in clip space, for the middle of the last row with the whole target as
    // the viewport
    const float vertexRowY[4] = {1.0f - 2.0f * float(TexVertex) / float(Height), 0, 0, 0};

    int checked = 0;
    bool failed = false;

    while(Running())
    {
      // no depth stencil is bound after the first frame
      dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      IDirect3DSurface9Ptr backbuffer;
      dev->GetRenderTarget(0, &backbuffer);

      dev->SetDepthStencilSurface(NULL);
      dev->SetRenderState(D3DRS_ZENABLE, FALSE);
      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetVertexDeclaration(decl);

      dev->BeginScene();

      for(int pass = 0; pass < 2; pass++)
      {
        const DWORD maxMip = pass;

        if(pass == 1)
        {
          setMarker("SetLOD in the frame");
          for(int t = 0; t < TexCount; t++)
            Texture(t)->SetLOD(Info[t].inFrame);
        }

        pushMarker(pass == 0 ? "LOD set before the frame" : "LOD set in the frame");

        dev->SetRenderTarget(0, targetSurfs[pass]);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        SetSamplers(maxMip);

        dev->SetTexture(0, tex2D);
        dev->SetTexture(1, texCube);
        dev->SetTexture(2, texVolume);
        dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, NULL);

        // the pixel rows, each a quad over its row
        dev->SetVertexShader(quadVS);
        dev->SetPixelShader(rowPS);
        dev->SetStreamSource(0, quadVB, 0, sizeof(float) * 4);
        for(int t = Tex2D; t <= TexVolume; t++)
        {
          D3DVIEWPORT9 vp = {0, (DWORD)t, Columns, 1, 0.0f, 1.0f};
          dev->SetViewport(&vp);
          const float which[4] = {float(t), 0, 0, 0};
          dev->SetPixelShaderConstantF(0, which, 1);
          dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        }

        // the vertex row: points whose colour the vertex shader samples
        if(vertexTextures)
        {
          D3DVIEWPORT9 vp = {0, 0, Width, Height, 0.0f, 1.0f};
          dev->SetViewport(&vp);
          dev->SetTexture(0, NULL);
          dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, texVertex);
          dev->SetVertexShader(vertexVS);
          dev->SetPixelShader(colourPS);
          dev->SetVertexShaderConstantF(0, vertexRowY, 1);
          dev->SetStreamSource(0, pointVB, 0, sizeof(float) * 4);
          dev->DrawPrimitive(D3DPT_POINTLIST, 0, Columns);
        }

        popMarker();

        if(!Check(targetSurfs[pass], sysmem, pass == 1, maxMip, vertexTextures, checked))
          failed = true;
      }

      // back to the LODs the frame started with, so every frame starts the same. Replaying to the
      // second pass and then back to the first has to undo the SetLOD before it.
      setMarker("SetLOD back");
      for(int t = 0; t < TexCount; t++)
        Texture(t)->SetLOD(Info[t].before);

      dev->SetTexture(0, NULL);
      dev->SetTexture(1, NULL);
      dev->SetTexture(2, NULL);
      dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, NULL);
      dev->SetRenderTarget(0, backbuffer);

      dev->EndScene();

      if(curFrame == 10 && !failed)
        TEST_LOG("All %d samples read the levels SetLOD says", checked);

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
