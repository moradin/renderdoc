/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2025-2026 Baldur Karlsson
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

RD_TEST(D3D12_Blend, D3D12GraphicsTest)
{
  static constexpr const char *Description =
      "Draws a triangle repeatedly to test blending within a single drawcall";

  const DefaultA2V TemplateTriangleRed[3] = {
      {Vec3f(-0.5f, -0.5f, 0.0f), Vec4f(1 / 255.f, 0.0f, 0.0f, 1.0f), Vec2f(0.0f, 0.0f)},
      {Vec3f(0.0f, 0.5f, 0.0f), Vec4f(1 / 255.f, 0.0f, 0.0f, 1.0f), Vec2f(0.0f, 1.0f)},
      {Vec3f(0.5f, -0.5f, 0.0f), Vec4f(1 / 255.f, 0.0f, 0.0f, 1.0f), Vec2f(1.0f, 0.0f)},
  };
  const int TRIANGLES_RED_INDEX = 0;
  const int NUM_TRIANGLES_RED = 16;
  const DefaultA2V TemplateTriangleGreen[3] = {
      {Vec3f(-0.5f, -0.5f, 0.0f), Vec4f(0.0f, 1 / 255.f, 0.0f, 1.0f), Vec2f(0.0f, 0.0f)},
      {Vec3f(0.0f, 0.5f, 0.0f), Vec4f(0.0f, 1 / 255.f, 0.0f, 1.0f), Vec2f(0.0f, 1.0f)},
      {Vec3f(0.5f, -0.5f, 0.0f), Vec4f(0.0f, 1 / 255.f, 0.0f, 1.0f), Vec2f(1.0f, 0.0f)},
  };
  const int TRIANGLES_GREEN_INDEX = TRIANGLES_RED_INDEX + NUM_TRIANGLES_RED;
  const int NUM_TRIANGLES_GREEN = 255;
  const DefaultA2V TemplateTriangleBlue[3] = {
      {Vec3f(-0.5f, -0.5f, 0.0f), Vec4f(0.0f, 0.0f, 1 / 255.f, 1.0f), Vec2f(0.0f, 0.0f)},
      {Vec3f(0.0f, 0.5f, 0.0f), Vec4f(0.0f, 0.0f, 1 / 255.f, 1.0f), Vec2f(0.0f, 1.0f)},
      {Vec3f(0.5f, -0.5f, 0.0f), Vec4f(0.0f, 0.0f, 1 / 255.f, 1.0f), Vec2f(1.0f, 0.0f)},
  };
  const int TRIANGLES_BLUE_INDEX = TRIANGLES_GREEN_INDEX + NUM_TRIANGLES_GREEN;
  const int NUM_TRIANGLES_BLUE = 512;

  const int NUM_TRIANGLES_TOTAL = TRIANGLES_BLUE_INDEX + NUM_TRIANGLES_BLUE;

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    std::vector<DefaultA2V> triangles;
    triangles.reserve(3 * NUM_TRIANGLES_TOTAL);
    for(int i = 0; i < NUM_TRIANGLES_RED; i++)
    {
      triangles.push_back(TemplateTriangleRed[0]);
      triangles.push_back(TemplateTriangleRed[1]);
      triangles.push_back(TemplateTriangleRed[2]);
    }
    for(int i = 0; i < NUM_TRIANGLES_GREEN; i++)
    {
      triangles.push_back(TemplateTriangleGreen[0]);
      triangles.push_back(TemplateTriangleGreen[1]);
      triangles.push_back(TemplateTriangleGreen[2]);
    }
    for(int i = 0; i < NUM_TRIANGLES_BLUE; i++)
    {
      triangles.push_back(TemplateTriangleBlue[0]);
      triangles.push_back(TemplateTriangleBlue[1]);
      triangles.push_back(TemplateTriangleBlue[2]);
    }

    ID3D12ResourcePtr vb = MakeBuffer().Data(triangles);

    ID3D12ResourcePtr img = MakeTexture(DXGI_FORMAT_R32G32B32A32_FLOAT, screenWidth, screenHeight)
                                .RTV()
                                .InitialState(D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = MakeRTV(img).CreateCPU(5);

    ID3DBlobPtr vsblob = Compile(D3DDefaultVertex, "main", "vs_4_0");
    ID3DBlobPtr psblob = Compile(D3DDefaultPixel, "main", "ps_4_0");

    ID3D12RootSignaturePtr sig = MakeSig({});

    D3D12PSOCreator psoInfo =
        MakePSO().RootSig(sig).InputLayout().RTVs({DXGI_FORMAT_R32G32B32A32_FLOAT}).VS(vsblob).PS(psblob);

    psoInfo.GraphicsDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoInfo.GraphicsDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    psoInfo.GraphicsDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    psoInfo.GraphicsDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoInfo.GraphicsDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoInfo.GraphicsDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    psoInfo.GraphicsDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

    ID3D12PipelineStatePtr pso = psoInfo;

    while(Running())
    {
      ID3D12GraphicsCommandListPtr cmd = GetCommandBuffer();

      Reset(cmd);

      ID3D12ResourcePtr bb = StartUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      ClearRenderTargetView(cmd, BBRTV, {0.2f, 0.2f, 0.2f, 1.0f});

      pushMarker(cmd, "Clear");
      OMSetRenderTargets(cmd, {rtv}, {});
      ClearRenderTargetView(cmd, rtv, {0.0f, 0.0f, 0.0f, 1.0f});
      popMarker(cmd);

      cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      IASetVertexBuffer(cmd, vb, sizeof(DefaultA2V), 0);
      cmd->SetPipelineState(pso);
      cmd->SetGraphicsRootSignature(sig);

      SetMainWindowViewScissor(cmd);

      pushMarker(cmd, "Red: groups of repeated draws");
      for(int i = 1; i <= NUM_TRIANGLES_RED; i *= 2)
      {
        cmd->DrawInstanced(3 * i, 1, TRIANGLES_RED_INDEX, 0);
      }
      setMarker(cmd, "End of red");
      popMarker(cmd);
      pushMarker(cmd, "Green: 255 (the maximum we can handle) in a single drawcall");
      cmd->DrawInstanced(3 * NUM_TRIANGLES_GREEN, 1, 3 * TRIANGLES_GREEN_INDEX, 0);
      popMarker(cmd);
      pushMarker(cmd, "Blue: 512 (more than the maximum) in a single drawcall");
      cmd->DrawInstanced(3 * NUM_TRIANGLES_BLUE, 1, 3 * TRIANGLES_BLUE_INDEX, 0);
      popMarker(cmd);

      pushMarker(cmd, "Clear");
      ClearRenderTargetView(cmd, rtv, {0.0f, 0.0f, 0.0f, 1.0f});
      popMarker(cmd);

      pushMarker(cmd, "All of the above in a single drawcall");
      cmd->DrawInstanced(3 * NUM_TRIANGLES_TOTAL, 1, 3 * TRIANGLES_RED_INDEX, 0);
      popMarker(cmd);

      setMarker(cmd, "Test End");

      ResourceBarrier(cmd, img, D3D12_RESOURCE_STATE_RENDER_TARGET,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

      blitToSwap(cmd, img, bb, DXGI_FORMAT_R32G32B32A32_FLOAT);

      ResourceBarrier(cmd, img, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_RENDER_TARGET);

      FinishUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      cmd->Close();

      SubmitAndPresent({cmd});
    }

    return 0;
  }
};

REGISTER_TEST();
