/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
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

RD_TEST(D3D12_Predication, D3D12GraphicsTest)
{
  static constexpr const char *Description =
      "Tests predication in D3D12 with queries both in and out of the capture";

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3DBlobPtr vsblob = Compile(D3DDefaultVertex, "main", "vs_4_0");
    ID3DBlobPtr psblob = Compile(D3DDefaultPixel, "main", "ps_4_0");

    D3D12_QUERY_HEAP_DESC desc = {};
    desc.Count = 4096;
    desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;

    ID3D12QueryHeapPtr qh;
    dev->CreateQueryHeap(&desc, __uuidof(ID3D12QueryHeap), (void **)&qh);

    desc.Count = 16;
    ID3D12QueryHeapPtr qh2;
    dev->CreateQueryHeap(&desc, __uuidof(ID3D12QueryHeap), (void **)&qh2);

    ID3D12CommandAllocatorPtr alloc2;
    CHECK_HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         __uuidof(ID3D12CommandAllocator), (void **)&alloc2));

    ID3D12ResourcePtr queryData = MakeBuffer().Size(sizeof(uint64_t) * (desc.Count + 1) * 3);

    uint64_t val = 1;
    SetBufferData(queryData, D3D12_RESOURCE_STATE_COMMON, (byte *)&val, sizeof(val));

    ID3D12ResourcePtr vb = MakeBuffer().Data(DefaultTri);

    ID3D12RootSignaturePtr sig = MakeSig({});

    ID3D12PipelineStatePtr pso = MakePSO().RootSig(sig).InputLayout().VS(vsblob).PS(psblob);

    ResourceBarrier(vb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    ID3D12ResourcePtr rtvtex = MakeTexture(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 4)
                                   .RTV()
                                   .InitialState(D3D12_RESOURCE_STATE_RENDER_TARGET);

    rtvtex->SetName(L"rtvtex");

    ID3D12ResourcePtr rtvMStex = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 4)
                                     .RTV()
                                     .Multisampled(4)
                                     .InitialState(D3D12_RESOURCE_STATE_RENDER_TARGET);

    rtvMStex->SetName(L"rtvMStex");

    ID3D12ResourcePtr dsvMStex = MakeTexture(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 4, 4)
                                     .DSV()
                                     .NoSRV()
                                     .Multisampled(4)
                                     .InitialState(D3D12_RESOURCE_STATE_COMMON);

    dsvMStex->SetName(L"dsvMStex");

    {
      ID3D12GraphicsCommandListPtr cmd = GetCommandBuffer();
      Reset(cmd);
      ClearRenderTargetView(cmd, MakeRTV(rtvtex).CreateCPU(1), {0.2f, 0.2f, 0.2f, 1.0f});
      ClearRenderTargetView(cmd, MakeRTV(rtvMStex).CreateCPU(2), {0.2f, 0.2f, 0.2f, 1.0f});

      ResourceBarrier(cmd, dsvMStex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);

      ClearDepthStencilView(cmd, dsvMStex, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.2f,
                            0x55);

      cmd->Close();

      Submit({cmd});
    }

    while(Running())
    {
      ID3D12GraphicsCommandListPtr cmd = GetCommandBuffer();
      ID3D12GraphicsCommandListPtr cmd2 = GetCommandBuffer();

      Reset(cmd);

      MakeRTV(rtvtex).CreateCPU(1);
      D3D12_CPU_DESCRIPTOR_HANDLE rtvMS = MakeRTV(rtvMStex).CreateCPU(2);
      MakeDSV(dsvMStex).CreateCPU(0);

      ID3D12ResourcePtr bb = StartUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      D3D12_CPU_DESCRIPTOR_HANDLE rtv =
          MakeRTV(bb).Format(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB).CreateCPU(0);

      ClearRenderTargetView(cmd, rtv, {0.2f, 0.2f, 0.2f, 1.0f});

      cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      IASetVertexBuffer(cmd, vb, sizeof(DefaultA2V), 0);
      cmd->SetPipelineState(pso);
      cmd->SetGraphicsRootSignature(sig);

      float w = (float)screenWidth;
      float h = (float)screenHeight;

      w /= 2.0f;
      h /= 2.0f;

      RSSetScissorRect(cmd, {0, 0, screenWidth, screenHeight});

      OMSetRenderTargets(cmd, {rtvMS}, MakeDSV(dsvMStex).CreateCPU(0));
      OMSetRenderTargets(cmd, {rtv}, {});

      int baseIdx = (curFrame % desc.Count);
      int prevIdx = ((curFrame - 1 + desc.Count) % desc.Count);
      int destIdx = 1 + baseIdx;

      cmd->BeginQuery(qh, D3D12_QUERY_TYPE_OCCLUSION, baseIdx);

      RSSetViewport(cmd, {0.0f, 0.0f, w, h, 0.0f, 1.0f});
      cmd->DrawInstanced(3, 1, 0, 0);

      cmd->EndQuery(qh, D3D12_QUERY_TYPE_OCCLUSION, baseIdx);

      ResourceBarrier(cmd, queryData, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);

      cmd->ResolveQueryData(qh, D3D12_QUERY_TYPE_OCCLUSION, baseIdx, 1, queryData,
                            sizeof(uint64_t) * destIdx * 3 + 8);

      if(curFrame > 1)
        cmd->ResolveQueryData(qh, D3D12_QUERY_TYPE_OCCLUSION, prevIdx, 1, queryData,
                              sizeof(uint64_t) * destIdx * 3 + 0);

      ResourceBarrier(cmd, queryData, D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

      cmd->SetPredication(queryData, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

      RSSetViewport(cmd, {w, 0.0f, w, h, 0.0f, 1.0f});
      cmd->DrawInstanced(3, 1, 0, 0);

      cmd->SetPredication(queryData, sizeof(uint64_t) * destIdx * 3 + 8,
                          D3D12_PREDICATION_OP_EQUAL_ZERO);

      RSSetViewport(cmd, {0.0f, h, w, h, 0.0f, 1.0f});
      cmd->DrawInstanced(3, 1, 0, 0);

      cmd->SetPredication(queryData, sizeof(uint64_t) * destIdx * 3 + 0,
                          D3D12_PREDICATION_OP_EQUAL_ZERO);

      RSSetViewport(cmd, {w, h, w, h, 0.0f, 1.0f});
      cmd->DrawInstanced(3, 1, 0, 0);

      cmd->SetPredication(queryData, sizeof(uint64_t) * destIdx * 3 + 16,
                          D3D12_PREDICATION_OP_EQUAL_ZERO);

      RSSetViewport(cmd, {w * 0.5f, h * 0.5f, w, h, 0.0f, 1.0f});
      cmd->DrawInstanced(3, 1, 0, 0);

      FinishUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      // check for queries being "imbalanced" between command buffers

      cmd2->Reset(alloc2, NULL);

      cmd2->BeginQuery(qh2, D3D12_QUERY_TYPE_OCCLUSION, 0);

      // this is fine, because the query is open on cmd2
      cmd->Close();

      cmd2->EndQuery(qh2, D3D12_QUERY_TYPE_OCCLUSION, 0);

      cmd2->Close();

      Submit({cmd, cmd2});

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
