/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2020-2026 Baldur Karlsson
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

#include "d3d12_test.h"

RD_TEST(D3D12_PrimitiveID, D3D12GraphicsTest)
{
  static constexpr const char *Description =
      "Exercises pixel shader debugging with various primitive ID scenarios.";

  std::string common = R"EOSHADER(
struct v2f
{
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

struct prim2f
{
  v2f data;
  uint prim : SV_PrimitiveID;
};

)EOSHADER";

  std::string geomNoPrim = R"EOSHADER(

[maxvertexcount(6)]
void main(triangle v2f input[3], inout TriangleStream<v2f> TriStream)
{
  // Output the original triangle
  int i;
  for(i = 0; i < 3; i++)
  {
    v2f output = input[i];
    TriStream.Append(output);
  }
  TriStream.RestartStrip();

  // Output the original triangle, shifted to the right
  for(i = 0; i < 3; i++)
  {
    v2f output = input[i];
    output.pos.x += 0.5f;
    TriStream.Append(output);
  }
  TriStream.RestartStrip();
}

)EOSHADER";

  std::string geomPrim = R"EOSHADER(

[maxvertexcount(6)]
void main(triangle v2f input[3], inout TriangleStream<prim2f> TriStream)
{
  // Output the original triangle
  int i;
  for(i = 0; i < 3; i++)
  {
    prim2f output;
    output.prim = 2;
    output.data = input[i];
    TriStream.Append(output);
  }
  TriStream.RestartStrip();

  // Output the original triangle, shifted to the right
  for(i = 0; i < 3; i++)
  {
    prim2f output;
    output.prim = 3;
    output.data = input[i];
    output.data.pos.x += 0.5f;
    TriStream.Append(output);
  }
  TriStream.RestartStrip();
}

)EOSHADER";

  std::string pixelNoPrim = R"EOSHADER(

float4 main(in v2f IN) : SV_Target0
{
  float3 color = IN.col.bgr;
  color.r *= 0.5f;
  return float4(color.bgr, 1.0f);
}

)EOSHADER";

  std::string pixelPrim = R"EOSHADER(

float4 main(in prim2f IN) : SV_Target0
{
  float r = IN.prim;
  return float4(r / 4.0f, 1.0f, 0.0f, 1.0f);
}

)EOSHADER";

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3DBlobPtr vsBlob = Compile(D3DDefaultVertex, "main", "vs_4_0");
    ID3DBlobPtr gsNoPrimBlob = Compile(common + geomNoPrim, "main", "gs_5_0");
    ID3DBlobPtr gsPrimBlob = Compile(common + geomPrim, "main", "gs_5_0");
    ID3DBlobPtr psNoPrimBlob = Compile(common + pixelNoPrim, "main", "ps_5_0");
    ID3DBlobPtr psPrimBlob = Compile(common + pixelPrim, "main", "ps_5_0");

    ID3D12ResourcePtr vb = MakeBuffer().Data(DefaultTri);

    ID3D12RootSignaturePtr sig = MakeSig({});

    size_t countPSOs = 0;
    ID3D12PipelineStatePtr psos[8];
    psos[countPSOs++] = MakePSO().RootSig(sig).InputLayout().VS(vsBlob).PS(psNoPrimBlob);
    psos[countPSOs++] = MakePSO().RootSig(sig).InputLayout().VS(vsBlob).PS(psPrimBlob);
    psos[countPSOs++] =
        MakePSO().RootSig(sig).InputLayout().VS(vsBlob).GS(gsNoPrimBlob).PS(psNoPrimBlob);
    psos[countPSOs++] = MakePSO().RootSig(sig).InputLayout().VS(vsBlob).GS(gsPrimBlob).PS(psPrimBlob);
    size_t sm6start = countPSOs;

    if(m_DXILSupport)
    {
      ID3DBlobPtr vs6Blob = Compile(D3DDefaultVertex, "main", "vs_6_0");
      ID3DBlobPtr gs6NoPrimBlob = Compile(common + geomNoPrim, "main", "gs_6_0");
      ID3DBlobPtr gs6PrimBlob = Compile(common + geomPrim, "main", "gs_6_0");
      ID3DBlobPtr ps6NoPrimBlob = Compile(common + pixelNoPrim, "main", "ps_6_0");
      ID3DBlobPtr ps6PrimBlob = Compile(common + pixelPrim, "main", "ps_6_0");

      psos[countPSOs++] = MakePSO().RootSig(sig).InputLayout().VS(vs6Blob).PS(ps6NoPrimBlob);
      psos[countPSOs++] = MakePSO().RootSig(sig).InputLayout().VS(vs6Blob).PS(ps6PrimBlob);
      psos[countPSOs++] =
          MakePSO().RootSig(sig).InputLayout().VS(vs6Blob).GS(gs6NoPrimBlob).PS(ps6NoPrimBlob),
      psos[countPSOs++] =
          MakePSO().RootSig(sig).InputLayout().VS(vs6Blob).GS(gs6PrimBlob).PS(ps6PrimBlob);
    }

    ResourceBarrier(vb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    float halfWidth = (float)screenWidth * 0.5f;
    float halfHeight = (float)screenHeight * 0.5f;
    float quarterHeight = halfHeight * 0.5f;
    D3D12_VIEWPORT views[] = {
        {0.0f, 0.0f, halfWidth, quarterHeight, 0.0f, 1.0f},
        {halfWidth, 0.0f, halfWidth, quarterHeight, 0.0f, 1.0f},
        {0.0f, quarterHeight, halfWidth, quarterHeight, 0.0f, 1.0f},
        {halfWidth, quarterHeight, halfWidth, quarterHeight, 0.0f, 1.0f},
        {0.0f, 0.0f + halfHeight, halfWidth, quarterHeight, 0.0f, 1.0f},
        {halfWidth, 0.0f + halfHeight, halfWidth, quarterHeight, 0.0f, 1.0f},
        {0.0f, quarterHeight + halfHeight, halfWidth, quarterHeight, 0.0f, 1.0f},
        {halfWidth, quarterHeight + halfHeight, halfWidth, quarterHeight, 0.0f, 1.0f},
    };

    static_assert(ARRAYSIZE(psos) == ARRAYSIZE(views), "Mismatched array sizes");

    while(Running())
    {
      ID3D12GraphicsCommandListPtr cmd = GetCommandBuffer();
      Reset(cmd);

      ID3D12ResourcePtr bb = StartUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
      D3D12_CPU_DESCRIPTOR_HANDLE rtv =
          MakeRTV(bb).Format(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB).CreateCPU(0);

      cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      IASetVertexBuffer(cmd, vb, sizeof(DefaultA2V), 0);
      cmd->SetGraphicsRootSignature(sig);
      OMSetRenderTargets(cmd, {rtv}, {});
      ClearRenderTargetView(cmd, rtv, {0.2f, 0.2f, 0.2f, 1.0f});

      RSSetScissorRect(cmd, {0, 0, screenWidth, screenHeight});
      setMarker(cmd, "SM5.0");
      for(int i = 0; i < countPSOs; ++i)
      {
        if(i == sm6start)
          setMarker(cmd, "SM6.0");
        RSSetViewport(cmd, views[i]);
        cmd->SetPipelineState(psos[i]);
        cmd->DrawInstanced(3, 1, 0, 0);
      }

      FinishUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
      cmd->Close();
      Submit({cmd});
      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
