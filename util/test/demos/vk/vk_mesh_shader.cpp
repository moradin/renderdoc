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

#include "vk_test.h"

RD_TEST(VK_Mesh_Shader, VulkanGraphicsTest)
{
  static constexpr const char *Description = "Draws geometry using mesh shader pipeline.";

  std::string task = R"EOSHADER(

#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

struct Inner
{
  uint a;
  uint b;
  uint c;
};

struct PayLoad
{
  uint padArr[4];
  uint pad;
  Inner inner;
  Inner innerArr[4];
  uint tri[4];
};

taskPayloadSharedEXT PayLoad payLoad;

void main()
{
  for (int i = 0; i < 4; ++i)
    payLoad.tri[i] = i;

  for (int i = 0; i < 4; ++i)
    payLoad.padArr[i] = 1000 + i;

  payLoad.pad = 123;

  for (int i = 0; i < 4; ++i)
  {
    payLoad.innerArr[i].a = 10*i + 0;
    payLoad.innerArr[i].b = 10*i + 1;
    payLoad.innerArr[i].c = 10*i + 2;
  }

  payLoad.inner.a = 500;
  payLoad.inner.b = 501;
  payLoad.inner.c = 502;

  EmitMeshTasksEXT(4, 1, 1);
}

)EOSHADER";

  std::string task_mesh = R"EOSHADER(

#version 460
#extension GL_EXT_mesh_shader : require

struct Inner
{
  uint a;
  uint b;
  uint c;
};

struct PayLoad
{
  uint padArr[4];
  uint pad;
  Inner inner;
  Inner innerArr[4];
  uint tri[4];
};

taskPayloadSharedEXT PayLoad payLoad;

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(triangles, max_vertices = 3, max_primitives = 1) out;
layout(location = 0) out vec4 outColor[];

void main()
{
  uint triangleCount = 1;
  uint vertexCount = 3 * triangleCount;
  SetMeshOutputsEXT(vertexCount, triangleCount);

  uint dtid = gl_GlobalInvocationID.x;
  uint tri = payLoad.tri[dtid];
  uint vertIdx = 0;
  vec4 org = vec4(-0.65, 0.0, 0.0, 0.0) + vec4(0.42, 0.0, 0.0, 0.0) * tri;

  uint vert0 = 0 + vertIdx;
  uint vert1 = 1 + vertIdx;
  uint vert2 = 2 + vertIdx;

  gl_MeshVerticesEXT[vert0].gl_Position = vec4(-0.2, -0.2, 0.0, 1.0) + org;
  gl_MeshVerticesEXT[vert1].gl_Position = vec4(0.0, 0.2, 0.0, 1.0) + org;
  gl_MeshVerticesEXT[vert2].gl_Position = vec4(0.2, -0.2, 0.0, 1.0) + org;

  outColor[vert0] = vec4(0.0, 0.0, 1.0, 1.0);
  outColor[vert1] = vec4(0.0, 0.0, 1.0, 1.0);
  outColor[vert2] = vec4(0.0, 0.0, 1.0, 1.0);

  gl_PrimitiveTriangleIndicesEXT[0] =  uvec3(vert0, vert1, vert2);
}

)EOSHADER";

  std::string simple_mesh = R"EOSHADER(

#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(triangles, max_vertices = 6, max_primitives = 2) out;
layout(location = 0) out vec4 outColor[];

void main()
{
  uint triangleCount = 2;
  uint vertexCount = 3 * triangleCount;

  SetMeshOutputsEXT(vertexCount, triangleCount);

  for (uint i = 0; i < 2; ++i)
  {
    uint vertIdx = i * 3;
    uint tri = i + 2 * gl_WorkGroupID.x;
    vec4 org = vec4(-0.65, +0.65, 0.0, 0.0) + vec4(0.42, 0.0, 0.0, 0.0) * tri;

    uint vert0 = 0 + vertIdx;
    uint vert1 = 1 + vertIdx;
    uint vert2 = 2 + vertIdx;

    gl_MeshVerticesEXT[vert0].gl_Position = vec4(-0.2, -0.2, 0.0, 1.0) + org;
    gl_MeshVerticesEXT[vert1].gl_Position = vec4(0.0, 0.2, 0.0, 1.0) + org;
    gl_MeshVerticesEXT[vert2].gl_Position = vec4(0.2, -0.2, 0.0, 1.0) + org;

    outColor[vert0] = vec4(1.0, 0.0, 0.0, 1.0);
    outColor[vert1] = vec4(1.0, 0.0, 0.0, 1.0);
    outColor[vert2] = vec4(1.0, 0.0, 0.0, 1.0);

    gl_PrimitiveTriangleIndicesEXT[i] =  uvec3(vert0, vert1, vert2);
  }
}

)EOSHADER";

  std::string point_mesh = R"EOSHADER(

#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(points, max_vertices = 2, max_primitives = 2) out;
layout(location = 0) out vec4 outColor[];

void main()
{
  uint primCount = 2;
  uint vertexCount = 1 * primCount;

  SetMeshOutputsEXT(vertexCount, primCount);

  for (uint i = 0; i < primCount; ++i)
  {
    uint vertIdx = i * 1;
    uint tri = i + 2 * gl_WorkGroupID.x;
    vec4 org = vec4(0.21, 0.0, 0.0, 0.0) * tri;

    uint vert0 = 0 + vertIdx;

    gl_MeshVerticesEXT[vert0].gl_Position = vec4(-0.4, -0.4, 0.0, 1.0) + org;
    gl_MeshVerticesEXT[vert0].gl_PointSize = 20.0f;

    outColor[vert0] = vec4(0.0, 1.0, 0.0, 1.0);

    gl_PrimitivePointIndicesEXT[i] = vert0;
  }
}

)EOSHADER";

  std::string pixel = R"EOSHADER(

#version 460

layout(location = 0) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
  outColor = inColor;
}

)EOSHADER";

  void Prepare(int argc, char **argv)
  {
    devExts.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);

    VulkanGraphicsTest::Prepare(argc, argv);

    if(!Avail.empty())
      return;

    if(devVersion < VK_API_VERSION_1_1)
    {
      Avail = "Vulkan device version isn't 1.1";
      return;
    }

    static VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};

    getPhysFeatures2(&meshShaderFeatures);

    if(!meshShaderFeatures.meshShader)
    {
      Avail = "Mesh Shader feature 'meshShader' not available\n";
      return;
    }

    if(!meshShaderFeatures.taskShader)
    {
      Avail = "Mesh Shader feature 'taskShader' not available";
      return;
    }

    meshShaderFeatures.multiviewMeshShader = VK_FALSE;
    meshShaderFeatures.primitiveFragmentShadingRateMeshShader = VK_FALSE;
    meshShaderFeatures.meshShaderQueries = VK_FALSE;

    devInfoNext = &meshShaderFeatures;
  }

  int main()
  {
    // initialise, create window, create context, etc
    if(!Init())
      return 3;

    VkPipelineLayout layout = createPipelineLayout(
        vkh::PipelineLayoutCreateInfo({}, {vkh::PushConstantRange(VK_SHADER_STAGE_ALL, 0, 8)}));

    vkh::GraphicsPipelineCreateInfo pipeCreateInfo;
    VkGraphicsPipelineCreateInfo *vkPipeCreateInfo = NULL;

    pipeCreateInfo.layout = layout;
    pipeCreateInfo.renderPass = mainWindow->rp;

    VkPipeline pipelines[3];
    int countTasks[3];

    pipeCreateInfo.stages = {
        CompileShaderModule(simple_mesh, ShaderLang::glsl, ShaderStage::mesh, "main", {},
                            SPIRVTarget::vulkan12),
        CompileShaderModule(pixel, ShaderLang::glsl, ShaderStage::frag, "main"),
    };

    vkPipeCreateInfo = pipeCreateInfo;
    vkPipeCreateInfo->pVertexInputState = NULL;
    vkPipeCreateInfo->pInputAssemblyState = NULL;

    pipelines[0] = createGraphicsPipeline(vkPipeCreateInfo);
    countTasks[0] = 2;

    pipeCreateInfo.stages = {
        CompileShaderModule(task, ShaderLang::glsl, ShaderStage::task, "main", {},
                            SPIRVTarget::vulkan12),
        CompileShaderModule(task_mesh, ShaderLang::glsl, ShaderStage::mesh, "main", {},
                            SPIRVTarget::vulkan12),
        CompileShaderModule(pixel, ShaderLang::glsl, ShaderStage::frag, "main"),
    };

    vkPipeCreateInfo = pipeCreateInfo;
    vkPipeCreateInfo->pVertexInputState = NULL;
    vkPipeCreateInfo->pInputAssemblyState = NULL;

    pipelines[1] = createGraphicsPipeline(vkPipeCreateInfo);
    countTasks[1] = 1;

    pipeCreateInfo.stages = {
        CompileShaderModule(point_mesh, ShaderLang::glsl, ShaderStage::mesh, "main", {},
                            SPIRVTarget::vulkan12),
        CompileShaderModule(pixel, ShaderLang::glsl, ShaderStage::frag, "main"),
    };

    vkPipeCreateInfo = pipeCreateInfo;
    vkPipeCreateInfo->pVertexInputState = NULL;
    vkPipeCreateInfo->pInputAssemblyState = NULL;

    pipelines[2] = createGraphicsPipeline(vkPipeCreateInfo);
    countTasks[2] = 3;

    while(Running())
    {
      VkCommandBuffer cmd = GetCommandBuffer();

      vkBeginCommandBuffer(cmd, vkh::CommandBufferBeginInfo());

      VkImage swapimg =
          StartUsingBackbuffer(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);

      vkCmdClearColorImage(cmd, swapimg, VK_IMAGE_LAYOUT_GENERAL,
                           vkh::ClearColorValue(0.2f, 0.2f, 0.2f, 1.0f), 1,
                           vkh::ImageSubresourceRange());

      vkCmdBeginRenderPass(
          cmd, vkh::RenderPassBeginInfo(mainWindow->rp, mainWindow->GetFB(), mainWindow->scissor),
          VK_SUBPASS_CONTENTS_INLINE);

      setMarker(cmd, "Mesh Shaders");
      for(size_t i = 0; i < ARRAY_COUNT(pipelines); ++i)
      {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[i]);
        vkCmdSetViewport(cmd, 0, 1, &mainWindow->viewport);
        vkCmdSetScissor(cmd, 0, 1, &mainWindow->scissor);

        vkCmdDrawMeshTasksEXT(cmd, countTasks[i], 1, 1);
      }

      vkCmdEndRenderPass(cmd);

      FinishUsingBackbuffer(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);

      vkEndCommandBuffer(cmd);

      Submit(0, 1, {cmd});

      Present();
    }
    return 0;
  }
};

REGISTER_TEST();
