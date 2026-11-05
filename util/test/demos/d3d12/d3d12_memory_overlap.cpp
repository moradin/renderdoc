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

RD_TEST(D3D12_Memory_Overlap, D3D12GraphicsTest)
{
  static constexpr const char *Description =
      "Test that allocates a lot of 'virtual' memory that doesn't require much physical memory - "
      "overlapping resources and sparse buffers that are mostly unmapped.";

  std::string pixel = R"EOSHADER(

StructuredBuffer<uint> buffers[8000] : register(t0);

float4 main() : SV_Target0
{
  uint bufIdx = 0;
  uint byteOffset = 0;

  for(int i=0; i < 20; i++)
  {
    uint a = buffers[bufIdx][byteOffset/4];
    uint b = buffers[bufIdx][(byteOffset/4) + 1];

    if(a == 999999 && b == 999999)
      return float4(0, 1, 0, 1);

    bufIdx = a;
    byteOffset = b;
  }

	return float4(1, 0, 0, 1);
}

)EOSHADER";

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3DBlobPtr vsblob = Compile(D3DDefaultVertex, "main", "vs_6_0");
    ID3DBlobPtr psblob = Compile(pixel, "main", "ps_6_0");

    ID3D12RootSignaturePtr sig = MakeSig({
        tableParam(D3D12_SHADER_VISIBILITY_PIXEL, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 0, 8000, 0),
    });

    ID3D12PipelineStatePtr pso = MakePSO().RootSig(sig).InputLayout().VS(vsblob).PS(psblob);

    // 256MB heap gives us plenty of unique possibilities for buffers

    D3D12_HEAP_DESC heapDesc;
    heapDesc.SizeInBytes = 256 * 1024 * 1024;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    heapDesc.Alignment = 0;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapDesc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapDesc.Properties.CreationNodeMask = 1;
    heapDesc.Properties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resDesc;
    resDesc.Alignment = 0;
    resDesc.DepthOrArraySize = 1;
    resDesc.Width = heapDesc.SizeInBytes;
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.Height = 1;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.MipLevels = 1;
    resDesc.SampleDesc.Count = 1;
    resDesc.SampleDesc.Quality = 0;

    D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &resDesc);

    ID3D12HeapPtr heap;
    CHECK_HR(dev->CreateHeap(&heapDesc, __uuidof(ID3D12Heap), (void **)&heap));

    heapDesc.SizeInBytes = 128 * 1024 * 1024;
    ID3D12HeapPtr sparseHeap;
    CHECK_HR(dev->CreateHeap(&heapDesc, __uuidof(ID3D12Heap), (void **)&sparseHeap));

    std::vector<ID3D12ResourcePtr> bufs;

    // create a max-size buffer at many possible offsets. Saving the contents of each buffer
    // separately will result in a huge expansion of needed memory.
    // We cut down the number of resources a bit since tracking overlaps is expensive and this makes
    // the test run slower - no real application will have this amount of overlapping
    UINT64 offs = 0;
    while(resDesc.Width > 0)
    {
      ID3D12ResourcePtr buf;
      CHECK_HR(dev->CreatePlacedResource(heap, offs, &resDesc, D3D12_RESOURCE_STATE_COMMON, NULL,
                                         __uuidof(ID3D12Resource), (void **)&buf));
      bufs.push_back(buf);

      resDesc.Width -= D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT * 2;
      offs += D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT * 2;
    }

    const uint32_t firstSparse = (uint32_t)bufs.size();

    // create a few sparse buffers with only tiny amounts mapped - again saving the buffer contents
    // instead of backing memory will expand memory use
    resDesc.Width = 1536 * 1024 * 1024;

    std::vector<ID3D12ResourcePtr> sparsebufs;

    for(int i = 0; i < 20; i++)
    {
      ID3D12ResourcePtr buf;
      dev->CreateReservedResource(&resDesc, D3D12_RESOURCE_STATE_COMMON, NULL,
                                  __uuidof(ID3D12Resource), (void **)&buf);
      sparsebufs.push_back(buf);
      bufs.push_back(buf);
    }

    const uint32_t bigSparse = (uint32_t)bufs.size();

    // create a sparse buffer that uses the full 64-bit range
    {
      ID3D12ResourcePtr buf;

      resDesc.Width = 10ULL << 30;

      dev->CreateReservedResource(&resDesc, D3D12_RESOURCE_STATE_COMMON, NULL,
                                  __uuidof(ID3D12Resource), (void **)&buf);
      sparsebufs.push_back(buf);
      bufs.push_back(buf);
    }

    // we don't test textures currently as we save those entirely :)...

    for(size_t i = 0; i < bufs.size(); i++)
    {
      if(i == bigSparse)
      {
        // D3D APIs don't support creating views through this API that target more than the first 32-bit
        MakeSRV(bufs[i]).StructureStride(4).NumElements(1U << 29).CreateGPU(int(i));
      }
      else
      {
        MakeSRV(bufs[i]).StructureStride(4).CreateGPU(int(i));
      }
    }

    // we chase around a few buffers to ensure their data is correct. Starting from buffer 0, offset
    // 0 each time we read a pair of uints
    uint32_t data[] = {
        3,
        128,

        10,
        1024,

        firstSparse,
        1024 * 1024 + 64,

        firstSparse + 10,
        7 * 1024 * 1024 + 512,

        bigSparse,
        (1U << 28),

        // success value
        999999,
        999999,
    };

    ID3D12ResourcePtr uploadBuf = MakeBuffer().Upload().Data(data);

    // need to map the
    D3D12_TILED_RESOURCE_COORDINATE start = {
        (1024 * 1024) / 65536,
    };
    D3D12_TILE_REGION_SIZE size = {
        1, FALSE, 1, 1, 1,
    };
    UINT heapRangeStart = (92 * 1024 * 1024 + 128 * 1024) / 65536;
    UINT rangeTileCount = 1;
    queue->UpdateTileMappings(sparsebufs[0], 1, &start, &size, sparseHeap, 1, NULL, &heapRangeStart,
                              &rangeTileCount, D3D12_TILE_MAPPING_FLAG_NONE);

    start.X = (7 * 1024 * 1024) / 65536;
    heapRangeStart = (100 * 1024 * 1024 + 768 * 1024) / 65536;
    queue->UpdateTileMappings(sparsebufs[10], 1, &start, &size, sparseHeap, 1, NULL,
                              &heapRangeStart, &rangeTileCount, D3D12_TILE_MAPPING_FLAG_NONE);

    start.X = uint64_t(uint64_t(1U << 28) / 65536);
    heapRangeStart = (108 * 1024 * 1024) / 65536;
    queue->UpdateTileMappings(bufs[bigSparse], 1, &start, &size, sparseHeap, 1, NULL,
                              &heapRangeStart, &rangeTileCount, D3D12_TILE_MAPPING_FLAG_NONE);

    {
      ID3D12GraphicsCommandListPtr cmd = GetCommandBuffer();

      Reset(cmd);

      uint32_t bufIdx = 0;
      uint32_t byteOffset = 0;

      uint32_t srcOffs = 0;
      while(bufIdx != 999999)
      {
        cmd->CopyBufferRegion(bufs[bufIdx], byteOffset, uploadBuf, srcOffs, 8);
        bufIdx = data[srcOffs / sizeof(uint32_t)];
        byteOffset = data[srcOffs / sizeof(uint32_t) + 1];
        srcOffs += 8;
      }

      cmd->Close();

      Submit({cmd});
    }

    while(Running())
    {
      ID3D12GraphicsCommandListPtr cmd = GetCommandBuffer();

      Reset(cmd);

      cmd->SetDescriptorHeaps(1, &m_CBVUAVSRV.GetInterfacePtr());

      ID3D12ResourcePtr bb = StartUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      ClearRenderTargetView(cmd, BBRTV, {0.2f, 0.2f, 0.2f, 1.0f});

      cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      IASetVertexBuffer(cmd, DefaultTriVB, sizeof(DefaultA2V), 0);
      cmd->SetPipelineState(pso);
      cmd->SetGraphicsRootSignature(sig);
      cmd->SetGraphicsRootDescriptorTable(0, m_CBVUAVSRV->GetGPUDescriptorHandleForHeapStart());

      SetMainWindowViewScissor(cmd);

      OMSetRenderTargets(cmd, {BBRTV}, {});

      cmd->DrawInstanced(3, 1, 0, 0);

      FinishUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      cmd->Close();

      SubmitAndPresent({cmd});
    }

    return 0;
  }
};

REGISTER_TEST();
