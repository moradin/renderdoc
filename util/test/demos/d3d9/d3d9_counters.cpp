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

RD_TEST(D3D9_Counters, D3D9GraphicsTest)
{
  static constexpr const char *Description =
      "Draws full screen quads cut down by scissor rects and the depth test, so that each passes "
      "a known number of samples, a full screen quad with an expensive pixel shader and a "
      "single pixel one, and a quad into a 4x multisampled target. Each draw is inside an "
      "occlusion query of the app's own, and on the first frame the counts the GPU gave are "
      "written to D3D9_Counters_reference.txt in its tmp folder (_reference_rdoc.txt under "
      "RenderDoc), for comparing with RenderDoc's samples passed counter. The heavy draw is also "
      "timed with timestamp queries, and the shortest time so far is in D3D9_Counters_timing.txt "
      "(_timing_rdoc.txt).";

  // the quad at the depth in c0.x
  const char *QuadVertex = R"EOSHADER(

float4 depth : register(c0);

float4 main(float3 pos : POSITION) : POSITION
{
  return float4(pos.xy, depth.x, 1);
}

)EOSHADER";

  const char *ColourPixel = R"EOSHADER(

float4 colour : register(c0);

float4 main() : COLOR0
{
  return colour;
}

)EOSHADER";

  // about 4000 iterations per pixel, so that the full screen draw takes far longer than any other
  const char *HeavyPixel = R"EOSHADER(

float4 seed : register(c0);

float4 main(float2 vpos : VPOS) : COLOR0
{
  float4 v = float4(vpos * 0.001f, 0.25f, 0.5f) + seed;
  [loop] for(int i = 0; i < 255; i++)
  {
    [loop] for(int j = 0; j < 16; j++)
      v = frac(v * 1.37f + 0.11f);
  }
  return v;
}

)EOSHADER";

  struct CountedDraw
  {
    const char *name;
    // what the draw should pass, worked out from its scissor rect and depth
    DWORD expected;
    IDirect3DQuery9Ptr query;
  };

  std::string ReferencePath()
  {
    return tmpPath + (rdoc ? "/D3D9_Counters_reference_rdoc.txt" : "/D3D9_Counters_reference.txt");
  }

  std::string TimingPath()
  {
    return tmpPath + (rdoc ? "/D3D9_Counters_timing_rdoc.txt" : "/D3D9_Counters_timing.txt");
  }

  template <typename T>
  bool ReadQuery(IDirect3DQuery9 * query, T & data)
  {
    HRESULT hr = S_FALSE;
    for(int tries = 0; tries < 1000000 && hr == S_FALSE; tries++)
      hr = query->GetData(&data, sizeof(data), D3DGETDATA_FLUSH);
    return hr == S_OK;
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3DBlobPtr vsblob = Compile(QuadVertex, "main", "vs_3_0");
    ID3DBlobPtr colourblob = Compile(ColourPixel, "main", "ps_3_0");
    ID3DBlobPtr heavyblob = Compile(HeavyPixel, "main", "ps_3_0");
    if(!vsblob || !colourblob || !heavyblob)
      return 4;

    IDirect3DVertexShader9Ptr vs = CreateVS(vsblob);
    IDirect3DPixelShader9Ptr colourPS = CreatePS(colourblob);
    IDirect3DPixelShader9Ptr heavyPS = CreatePS(heavyblob);

    const Vec3f quad[4] = {
        Vec3f(-1.0f, -1.0f, 0.0f),
        Vec3f(-1.0f, 1.0f, 0.0f),
        Vec3f(1.0f, -1.0f, 0.0f),
        Vec3f(1.0f, 1.0f, 0.0f),
    };
    IDirect3DVertexBuffer9Ptr vb = MakeVB(quad, sizeof(quad));

    const D3DVERTEXELEMENT9 elems[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9Ptr decl;
    if(FAILED(dev->CreateVertexDeclaration(elems, &decl)))
    {
      TEST_ERROR("CreateVertexDeclaration failed");
      return 4;
    }

    // a 64x64 4x multisampled target, drawn without a depth stencil, and resolved with StretchRect
    IDirect3DSurface9Ptr msaaRT, resolved;
    if(SUCCEEDED(d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                 TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL)))
    {
      dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &msaaRT,
                              NULL);
      dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved,
                              NULL);
    }
    if(!msaaRT || !resolved)
      TEST_WARN("No 4x multisampled X8R8G8B8 render target, the multisampled draw is left out");

    IDirect3DSurface9Ptr backbuffer, depthstencil;
    dev->GetRenderTarget(0, &backbuffer);
    dev->GetDepthStencilSurface(&depthstencil);

    const DWORD fullScreen = DWORD(screenWidth * screenHeight);

    // 64x32 pixels each. The second is entirely behind the first, and the third half behind it.
    const RECT firstRect = {10, 10, 74, 42};
    const RECT thirdRect = {42, 10, 106, 42};
    const RECT fullRect = {0, 0, screenWidth, screenHeight};
    const RECT pixelRect = {200, 150, 201, 151};
    const RECT msaaRect = {8, 8, 24, 24};

    CountedDraw draws[] = {
        {"Scissored quad", 64 * 32},
        {"Hidden quad", 0},
        {"Half hidden quad", 32 * 32},
        {"Heavy quad", fullScreen},
        {"Single pixel quad", 1},
        // four samples for each of the 16x16 pixels
        {"Multisampled quad", 16 * 16 * 4},
    };
    const size_t numDraws = msaaRT ? ARRAY_COUNT(draws) : ARRAY_COUNT(draws) - 1;

    for(size_t i = 0; i < numDraws; i++)
    {
      if(FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &draws[i].query)))
      {
        TEST_ERROR("Couldn't create an occlusion query");
        return 4;
      }
    }

    // timestamps around the heavy draw
    IDirect3DQuery9Ptr disjoint, freq, heavyBefore, heavyAfter;
    if(FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &disjoint)) ||
       FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &freq)) ||
       FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &heavyBefore)) ||
       FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &heavyAfter)))
      TEST_WARN("Timestamp queries aren't supported, the heavy draw isn't timed");
    double shortestHeavy = -1.0;

    bool wroteReference = false;
    bool reportedMismatch = false;

    while(Running())
    {
      dev->SetRenderTarget(0, backbuffer);
      dev->SetDepthStencilSurface(depthstencil);
      dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                 D3DCOLOR_COLORVALUE(0.2f, 0.2f, 0.2f, 1.0f), 1.0f, 0);

      dev->BeginScene();

      dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      dev->SetRenderState(D3DRS_LIGHTING, FALSE);
      dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
      dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
      dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
      dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
      dev->SetVertexDeclaration(decl);
      dev->SetStreamSource(0, vb, 0, sizeof(Vec3f));
      dev->SetVertexShader(vs);
      dev->SetPixelShader(colourPS);

      auto draw = [&](size_t idx, const RECT &scissor, float depth, Vec4f colour) {
        const float depthConst[4] = {depth, 0.0f, 0.0f, 0.0f};
        dev->SetVertexShaderConstantF(0, depthConst, 1);
        dev->SetPixelShaderConstantF(0, &colour.x, 1);
        dev->SetScissorRect(&scissor);

        pushMarker(draws[idx].name);
        draws[idx].query->Issue(D3DISSUE_BEGIN);
        dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        draws[idx].query->Issue(D3DISSUE_END);
        popMarker();
      };

      // writes depth 0.5 over firstRect
      draw(0, firstRect, 0.5f, Vec4f(1.0f, 0.0f, 0.0f, 1.0f));
      // behind it everywhere
      draw(1, firstRect, 0.75f, Vec4f(0.0f, 1.0f, 0.0f, 1.0f));
      // behind it where the two overlap (x 42-73), in front of the clear elsewhere (x 74-105)
      draw(2, thirdRect, 0.75f, Vec4f(0.0f, 0.0f, 1.0f, 1.0f));

      dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
      dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

      dev->SetPixelShader(heavyPS);
      if(heavyAfter)
      {
        disjoint->Issue(D3DISSUE_BEGIN);
        heavyBefore->Issue(D3DISSUE_END);
      }
      draw(3, fullRect, 0.5f, Vec4f(0.1f, 0.2f, 0.3f, 0.0f));
      if(heavyAfter)
      {
        heavyAfter->Issue(D3DISSUE_END);
        disjoint->Issue(D3DISSUE_END);
        freq->Issue(D3DISSUE_END);
      }
      draw(4, pixelRect, 0.5f, Vec4f(0.3f, 0.2f, 0.1f, 0.0f));

      if(msaaRT)
      {
        dev->SetRenderTarget(0, msaaRT);
        dev->SetDepthStencilSurface(NULL);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetPixelShader(colourPS);
        draw(5, msaaRect, 0.5f, Vec4f(1.0f, 1.0f, 0.0f, 1.0f));
      }

      dev->EndScene();

      if(msaaRT)
        dev->StretchRect(msaaRT, NULL, resolved, NULL, D3DTEXF_NONE);

      dev->SetRenderTarget(0, backbuffer);
      dev->SetDepthStencilSurface(depthstencil);

      Present();

      // the counts the GPU gave this frame
      std::string reference;
      for(size_t i = 0; i < numDraws; i++)
      {
        DWORD samples = 0;
        if(!ReadQuery(draws[i].query.GetInterfacePtr(), samples))
        {
          TEST_ERROR("Couldn't read the occlusion query of %s", draws[i].name);
          continue;
        }

        if(samples != draws[i].expected && !reportedMismatch)
        {
          TEST_ERROR("%s passed %u samples, expected %u", draws[i].name, samples, draws[i].expected);
          reportedMismatch = true;
        }

        reference += std::string(draws[i].name) + "\t" + std::to_string(samples) + "\n";
      }

      // the heavy draw's time, the shortest of the frames so far, since the GPU's clocks may still
      // be rising in the first ones
      if(heavyAfter)
      {
        BOOL isDisjoint = TRUE;
        UINT64 frequency = 0, before = 0, after = 0;
        if(!ReadQuery(disjoint.GetInterfacePtr(), isDisjoint) ||
           !ReadQuery(freq.GetInterfacePtr(), frequency) ||
           !ReadQuery(heavyBefore.GetInterfacePtr(), before) ||
           !ReadQuery(heavyAfter.GetInterfacePtr(), after))
        {
          TEST_ERROR("Couldn't read the timestamp queries");
        }
        else if(!isDisjoint && frequency > 0 && after >= before)
        {
          const double seconds = double(after - before) / double(frequency);
          if(shortestHeavy < 0.0 || seconds < shortestHeavy)
          {
            shortestHeavy = seconds;
            FILE *f = NULL;
            fopen_s(&f, TimingPath().c_str(), "w");
            if(f)
            {
              fprintf(f, "Heavy quad\t%.9g\nTimestamp frequency\t%llu\n", seconds, frequency);
              fclose(f);
            }
          }
        }
      }

      if(!wroteReference)
      {
        wroteReference = true;
        const std::string path = ReferencePath();
        FILE *f = NULL;
        fopen_s(&f, path.c_str(), "w");
        if(f)
        {
          fputs(reference.c_str(), f);
          fclose(f);
          TEST_LOG("Wrote the occlusion query counts to %s:\n%s", path.c_str(), reference.c_str());
        }
        else
        {
          TEST_ERROR("Couldn't write %s", path.c_str());
        }
      }
    }

    return 0;
  }
};

REGISTER_TEST();
