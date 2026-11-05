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

#include "3rdparty/fmt/core.h"
#include "d3d12_test.h"

RD_TEST(D3D12_Workgroup_Zoo, D3D12GraphicsTest)
{
  static constexpr const char *Description =
      "Test of behaviour around workgroup operations in shaders.";

  const std::string common = R"EOSHADER(

cbuffer rootconsts : register(b0)
{
  uint root_test;
  uint root_two;
}

uint GetTest() { return root_test; }

#define IsTest(x) (GetTest() == x)

)EOSHADER";

  const std::string compCommon = common + R"EOSHADER(

RWStructuredBuffer<float4> outbuf : register(u0);

static uint3 tid;
static uint flatId;

groupshared uint4 gsmUint4[1024];

void SetOutput(float4 val)
{
  outbuf[root_test * 1024 + tid.y * GROUP_SIZE_X + tid.x] = val;
}

void Init(float4 val)
{
  flatId = tid.z * GROUP_SIZE_X * GROUP_SIZE_Y + tid.y * GROUP_SIZE_X + tid.x;
  gsmUint4[flatId].xyz = tid;
  gsmUint4[flatId].z = tid.x;
  SetOutput(val);
}

)EOSHADER";

  const std::string testShader = compCommon + R"EOSHADER(

float4 funcD(uint id)
{
  return WaveActiveSum(id/2).xxxx;
}

float4 nestedFunc(uint id)
{
  float4 ret = funcD(id/3);
  ret.w = WaveActiveSum(id);
  return ret;
}

float4 funcA(uint id)
{
   return nestedFunc(id*2);
}

float4 funcB(uint id)
{
   return nestedFunc(id*4);
}

float4 funcTest(uint id)
{
  if ((id % 2) == 0)
  {
    return 0.xxxx;
  }
  else
  {
    float value = WaveActiveSum(id);
    if (id < 10)
    {
      return value.xxxx;
    }
    value += WaveActiveSum(id/2);
    return value.xxxx;
  }
}

float4 ComplexPartialReconvergence(uint id)
{
  float4 result = 0.0.xxxx;
  // Loops with different number of iterations per thread
  for (uint i = id; i < 23; i++)
  {
    result.x += WaveActiveSum(id);
  }

  if ((result.x < 5) || (id > 20))
  {
    result.y += WaveActiveSum(id);
    if (id < 25)
      result.z += WaveActiveSum(id);
  }
  else if (result.x < 10)
  {
    result.y += WaveActiveSum(id);

    if (result.x > 5)
      result.z += WaveActiveSum(id);
  }

  result.w += WaveActiveSum(id);

  return result;
}

[numthreads(GROUP_SIZE_X, GROUP_SIZE_Y, GROUP_SIZE_Z)]
void main(uint3 inTid : SV_GroupThreadID)
{
  tid = inTid;
  float4 testResult = 0.0f.xxxx;
  Init(testResult);
  uint id = flatId;

  if(IsTest(0))
  {
    testResult.x = id;
    AllMemoryBarrierWithGroupSync();
  }
  else if(IsTest(1))
  {
    testResult.x = WaveActiveSum(id);
    AllMemoryBarrierWithGroupSync();
  }
  else if(IsTest(2))
  {
    // Diverged threads which reconverge 
    if (id < 10)
    {
        // active threads 0-9
        testResult.x = WaveActiveSum(id);

        if ((id % 2) == 0)
          testResult.y = WaveActiveSum(id);
        else
          testResult.y = WaveActiveSum(id);

        testResult.x += WaveActiveSum(id);
    }
    else
    {
        // active threads 10...
        testResult.x = WaveActiveSum(id);
    }
    testResult.y = WaveActiveSum(id);
    AllMemoryBarrierWithGroupSync();
  }
  else if(IsTest(3))
  {
    // Converged threads calling a function 
    testResult = funcTest(id);
    testResult.y = WaveActiveSum(id);
    AllMemoryBarrierWithGroupSync();
  }
  else if(IsTest(4))
  {
    // Converged threads calling a function which has a nested function call in it
    testResult = nestedFunc(id);
    testResult.y = WaveActiveSum(id);
    AllMemoryBarrierWithGroupSync();
  }
  else if(IsTest(5))
  {
    // Diverged threads calling the same function
    if (id < 10)
    {
      testResult = funcD(id);
    }
    else
    {
      testResult = funcD(id);
    }
    testResult.y = WaveActiveSum(id);
    AllMemoryBarrierWithGroupSync();
  }
  else if(IsTest(6))
  {
    // Diverged threads calling the same function which has a nested function call in it
    if (id < 10)
    {
      testResult = funcA(id);
    }
    else
    {
      testResult = funcB(id);
    }
    testResult.y = WaveActiveSum(id);
    AllMemoryBarrierWithGroupSync();
  }
  else if(IsTest(7))
  {
    // Diverged threads which early exit
    if (id < 10)
    {
      testResult.x = WaveActiveSum(id+10);
      SetOutput(testResult);
      return;
    }
    testResult.x = WaveActiveSum(id);
  }
  else if(IsTest(8))
  {
     // Loops with different number of iterations per thread
    for (uint i = 0; i < id; i++)
    {
      testResult.x += WaveActiveSum(id);
    }
    AllMemoryBarrierWithGroupSync();
  }
  else if(IsTest(9))
  {
    // Query functions : unit tests
    testResult.x = float(WaveGetLaneCount());
    testResult.y = float(WaveGetLaneIndex());
    testResult.z = float(WaveIsFirstLane());
    AllMemoryBarrierWithGroupSync();
  }
  else if(IsTest(10))
  {
    testResult = ComplexPartialReconvergence(id);

    AllMemoryBarrierWithGroupSync();
  }

  SetOutput(testResult);
}

)EOSHADER";

  const std::string perfShader = compCommon + R"EOSHADER(

[numthreads(GROUP_SIZE_X, GROUP_SIZE_Y, GROUP_SIZE_Z)]
void main(uint3 inTid : SV_DispatchThreadID)
{
  tid = inTid;
  float4 testResult = 0.0f.xxxx;
  Init(testResult);
  uint id = flatId;

  // TEST CASES:
  // 0: GPU math : loops 100
  // 1: CPU math : loops 100
  // 2: GPU math : loops 200
  // 3: CPU math : loops 200
  // 4: GPU math : loops 400
  // 5: CPU math : loops 400
  // 6: GPU math : loops 5000
  // 7: CPU math : loops 5000
  bool useCpu = GetTest() & 0x1;

  uint count = 0;
  {
    uint temp = GetTest() >> 1;
    if(temp == 0)
      count = 100U; 
    if(temp == 1)
      count = 200U; 
    if(temp == 2)
      count = 400U; 
    if(temp == 3)
      count = 5000U; 
  }

  if(useCpu)
  {
    for (uint i = 0; i < count; ++i)
    {
       gsmUint4[id].x += i;
       gsmUint4[id].y += i * i;
       testResult.x = testResult.x * testResult.x;
       testResult.x += dot(gsmUint4[id], gsmUint4[id]);
    }
  }
  else
  {
    for (uint i = 0; i < count; ++i)
    {
       gsmUint4[id].x += i;
       gsmUint4[id].y += i * i;
       testResult.x = pow(testResult.x, float(root_two));
       testResult.x += dot(gsmUint4[id], gsmUint4[id]);
    }
  }

  AllMemoryBarrierWithGroupSync();

  SetOutput(testResult);
}

)EOSHADER";

  void Prepare(int argc, char **argv)
  {
    D3D12GraphicsTest::Prepare(argc, argv);

    if(opts1.WaveLaneCountMax < 16)
      Avail = "Subgroup size is less than 16";

    bool supportSM60 = (m_HighestShaderModel >= D3D_SHADER_MODEL_6_0) && m_DXILSupport;
    if(!supportSM60)
      Avail = "SM 6.0 not supported";
  }

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3D12RootSignaturePtr sig = MakeSig({constParam(D3D12_SHADER_VISIBILITY_ALL, 0, 0, 2),
                                          uavParam(D3D12_SHADER_VISIBILITY_ALL, 0, 0)});

    int32_t numCompTests = 0;
    size_t pos = 0;
    while(pos != std::string::npos)
    {
      pos = testShader.find("IsTest(", pos);
      if(pos == std::string::npos)
        break;
      pos += sizeof("IsTest(") - 1;
      numCompTests = std::max(numCompTests, atoi(testShader.c_str() + pos) + 1);
    }

    const int32_t countPerfTests = 8;

    struct CompSize
    {
      int x, y, z;
    };
    CompSize compsizes[] = {
        {70, 1, 1},
    };
    std::string comppipe_name[ARRAY_COUNT(compsizes)];
    ID3D12PipelineStatePtr testPipes[ARRAY_COUNT(compsizes)];
    ID3D12PipelineStatePtr perfPipes[ARRAY_COUNT(compsizes)];

    std::string defines;

    for(int i = 0; i < ARRAY_COUNT(compsizes); i++)
    {
      std::string sizedefine;
      sizedefine =
          fmt::format("#define GROUP_SIZE_X {}\n#define GROUP_SIZE_Y {}\n#define GROUP_SIZE_Z {}",
                      compsizes[i].x, compsizes[i].y, compsizes[i].z);
      comppipe_name[i] = fmt::format("{}x{}x{}", compsizes[i].x, compsizes[i].y, compsizes[i].z);

      testPipes[i] =
          MakePSO().RootSig(sig).CS(Compile(defines + sizedefine + testShader, "main", "cs_6_0"));
      testPipes[i]->SetName(UTF82Wide(comppipe_name[i]).c_str());
      perfPipes[i] =
          MakePSO().RootSig(sig).CS(Compile(defines + sizedefine + perfShader, "main", "cs_6_0"));
      perfPipes[i]->SetName(UTF82Wide(comppipe_name[i]).c_str());
    }

    ID3D12ResourcePtr bufOut = MakeBuffer().Size(sizeof(Vec4f) * 1024 * numCompTests).UAV();
    D3D12ViewCreator uavView =
        MakeUAV(bufOut).Format(DXGI_FORMAT_R32_UINT).NumElements(4 * 1024 * numCompTests);
    D3D12_CPU_DESCRIPTOR_HANDLE uavcpu = uavView.CreateClearCPU(10);
    D3D12_GPU_DESCRIPTOR_HANDLE uavgpu = uavView.CreateGPU(10);

    bufOut->SetName(L"bufOut");

    while(Running())
    {
      ID3D12GraphicsCommandListPtr cmd = GetCommandBuffer();

      Reset(cmd);

      cmd->SetDescriptorHeaps(1, &m_CBVUAVSRV.GetInterfacePtr());

      ID3D12ResourcePtr bb = StartUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      ClearRenderTargetView(cmd, BBRTV, {0.2f, 0.2f, 0.2f, 1.0f});

      pushMarker(cmd, "Compute Tests");

      for(size_t p = 0; p < ARRAY_COUNT(testPipes); p++)
      {
        ResourceBarrier(cmd);

        UINT zero[4] = {};
        cmd->ClearUnorderedAccessViewUint(uavgpu, uavcpu, bufOut, zero, 0, NULL);

        ResourceBarrier(cmd);
        pushMarker(cmd, comppipe_name[p]);

        cmd->SetPipelineState(testPipes[p]);
        cmd->SetComputeRootSignature(sig);
        cmd->SetComputeRootUnorderedAccessView(1, bufOut->GetGPUVirtualAddress());

        for(int i = 0; i < numCompTests; i++)
        {
          cmd->SetComputeRoot32BitConstant(0, i, 0);
          cmd->SetComputeRoot32BitConstant(0, i + 1, 1);
          cmd->Dispatch(2, 1, 1);
        }

        popMarker(cmd);
      }

      popMarker(cmd);

      pushMarker(cmd, "Perf Tests");

      for(size_t p = 0; p < ARRAY_COUNT(testPipes); p++)
      {
        ResourceBarrier(cmd);

        UINT zero[4] = {};
        cmd->ClearUnorderedAccessViewUint(uavgpu, uavcpu, bufOut, zero, 0, NULL);

        ResourceBarrier(cmd);
        pushMarker(cmd, comppipe_name[p]);

        cmd->SetPipelineState(perfPipes[p]);
        cmd->SetComputeRootSignature(sig);
        cmd->SetComputeRootUnorderedAccessView(1, bufOut->GetGPUVirtualAddress());

        for(int i = 0; i < countPerfTests; ++i)
        {
          bool useCpu = (i & 0x1);
          int count = 0;
          {
            int temp = i >> 1;
            if(temp == 0)
              count = 100U;
            if(temp == 1)
              count = 200U;
            if(temp == 2)
              count = 400U;
            if(temp == 3)
              count = 5000U;
          }
          std::string perfTestName =
              fmt::format("{} Iterations {} Math", count, useCpu ? "CPU" : "GPU");
          pushMarker(cmd, perfTestName);
          int two = 2;
          cmd->SetComputeRoot32BitConstant(0, i, 0);
          cmd->SetComputeRoot32BitConstant(0, two, 1);
          cmd->Dispatch(2, 1, 1);
          popMarker(cmd);
        }

        popMarker(cmd);
      }

      popMarker(cmd);

      FinishUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      cmd->Close();

      SubmitAndPresent({cmd});
    }

    return 0;
  }
};

REGISTER_TEST();
