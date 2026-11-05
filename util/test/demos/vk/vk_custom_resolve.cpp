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

#include "vk_test.h"

std::string resolveShader = R"EOSHADER(

#version 460 core

layout (input_attachment_index = 0, binding = 0) uniform subpassInputMS msaaColour;

layout(location = 0, index = 0) out vec4 Color;

void main()
{
	Color = vec4(0.0, 0.0, 0.0, 0.0);
  vec4 s0 = subpassLoad(msaaColour,0);
  vec4 s1 = subpassLoad(msaaColour,1);
  vec4 s2 = subpassLoad(msaaColour,2);
  vec4 s3 = subpassLoad(msaaColour,3);
  Color = (s0 + s1 + s2 + s3 ) / 16.0;
  if (s0 != s1)
    Color = vec4(1.0, 0.0, 0.0, 1.0);
  if (s0 != s2)
    Color = vec4(0.0, 1.0, 0.0, 1.0);
  if (s0 != s3)
    Color = vec4(0.0, 0.0, 1.0, 1.0);
  Color.a = 1.0;
}

)EOSHADER";

RD_TEST(VK_Custom_Resolve, VulkanGraphicsTest)
{
  static constexpr const char *Description = "Test capture and replay of VK_EXT_custom_resolve";

  void Prepare(int argc, char **argv)
  {
    devExts.push_back(VK_EXT_CUSTOM_RESOLVE_EXTENSION_NAME);
    devExts.push_back(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);

    VulkanGraphicsTest::Prepare(argc, argv);

    if(!Avail.empty())
      return;

    // Dynamic rendering without using extension
    if(devVersion < VK_MAKE_VERSION(1, 3, 0))
    {
      Avail = "Vulkan device version isn't 1.3+";
      return;
    }

    static VkPhysicalDeviceVulkan13Features vk13feats = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };

    getPhysFeatures2(&vk13feats);
    if(vk13feats.dynamicRendering == VK_FALSE)
    {
      Avail = "Vulkan device doesn't support dynamicRendering";
      return;
    }

    vk13feats.dynamicRendering = VK_TRUE;
    devInfoNext = &vk13feats;

    static VkPhysicalDeviceCustomResolveFeaturesEXT customResolveFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_RESOLVE_FEATURES_EXT};

    getPhysFeatures2(&customResolveFeatures);
    customResolveFeatures.pNext = (void *)devInfoNext;
    customResolveFeatures.customResolve = VK_TRUE;
    devInfoNext = &customResolveFeatures;

    static VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR dynRenderLocalReadFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};

    getPhysFeatures2(&dynRenderLocalReadFeatures);
    dynRenderLocalReadFeatures.pNext = (void *)devInfoNext;
    dynRenderLocalReadFeatures.dynamicRenderingLocalRead = VK_TRUE;
    devInfoNext = &dynRenderLocalReadFeatures;
  }

  int main()
  {
    // initialise, create window, create context, etc
    if(!Init())
      return 3;

    vkh::RenderPassCreator renderPassCreateInfo;
    // MSAA Colour pass
    renderPassCreateInfo.attachments.push_back(vkh::AttachmentDescription(
        mainWindow->format, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_SAMPLE_COUNT_4_BIT));
    // Resolve output
    renderPassCreateInfo.attachments.push_back(vkh::AttachmentDescription(
        mainWindow->format, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE));

    renderPassCreateInfo.addSubpass({VkAttachmentReference({0, VK_IMAGE_LAYOUT_GENERAL})},
                                    VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED);
    // Resolve subpass with VK_SUBPASS_DESCRIPTION_CUSTOM_RESOLVE_BIT_EXT
    // Color attachment 1. Input attachment 0
    renderPassCreateInfo.addSubpass({VkAttachmentReference({1, VK_IMAGE_LAYOUT_GENERAL})},
                                    VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED, {},
                                    {VkAttachmentReference({0, VK_IMAGE_LAYOUT_GENERAL})});
    renderPassCreateInfo.subpasses.back().flags |= VK_SUBPASS_DESCRIPTION_CUSTOM_RESOLVE_BIT_EXT;

    renderPassCreateInfo.dependencies.push_back(vkh::SubpassDependency(
        VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_NONE, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_NONE, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT));
    renderPassCreateInfo.dependencies.push_back(vkh::SubpassDependency(
        0, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT));

    VkRenderPass msaaRP = createRenderPass(renderPassCreateInfo);

    VkPipelineLayout layout = createPipelineLayout(vkh::PipelineLayoutCreateInfo());
    vkh::GraphicsPipelineCreateInfo colPipeCreateInfo;

    colPipeCreateInfo.layout = layout;
    colPipeCreateInfo.renderPass = msaaRP;
    colPipeCreateInfo.multisampleState.sampleShadingEnable = VK_FALSE;
    colPipeCreateInfo.multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;

    colPipeCreateInfo.vertexInputState.vertexBindingDescriptions = {vkh::vertexBind(0, DefaultA2V)};
    colPipeCreateInfo.vertexInputState.vertexAttributeDescriptions = {
        vkh::vertexAttr(0, 0, DefaultA2V, pos),
        vkh::vertexAttr(1, 0, DefaultA2V, col),
        vkh::vertexAttr(2, 0, DefaultA2V, uv),
    };

    colPipeCreateInfo.stages = {
        CompileShaderModule(VKDefaultVertex, ShaderLang::glsl, ShaderStage::vert, "main"),
        CompileShaderModule(VKDefaultPixel, ShaderLang::glsl, ShaderStage::frag, "main"),
    };

    VkPipeline pipeCol = createGraphicsPipeline(colPipeCreateInfo);

    VkDescriptorSetLayout resSetlayout =
        createDescriptorSetLayout(vkh::DescriptorSetLayoutCreateInfo({
            {0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        }));
    VkDescriptorSet resDescset = allocateDescriptorSet(resSetlayout);
    VkDescriptorSet dynResDescset = allocateDescriptorSet(resSetlayout);

    VkPipelineLayout resLayout = createPipelineLayout(vkh::PipelineLayoutCreateInfo({resSetlayout}));
    vkh::GraphicsPipelineCreateInfo resPipeCreateInfo;

    resPipeCreateInfo.layout = resLayout;
    resPipeCreateInfo.renderPass = msaaRP;
    resPipeCreateInfo.multisampleState.sampleShadingEnable = VK_FALSE;
    resPipeCreateInfo.multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    resPipeCreateInfo.inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    resPipeCreateInfo.stages = {
        CompileShaderModule(VKFullscreenQuadVertex, ShaderLang::glsl, ShaderStage::vert, "main"),
        CompileShaderModule(resolveShader, ShaderLang::glsl, ShaderStage::frag, "main"),
    };

    resPipeCreateInfo.subpass = 1;
    VkPipeline pipeRes = createGraphicsPipeline(resPipeCreateInfo);

    VkPipelineRenderingCreateInfoKHR dynPipeRendInfo = {};
    dynPipeRendInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    dynPipeRendInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    dynPipeRendInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    VkFormat outFormats[] = {mainWindow->format};
    dynPipeRendInfo.pColorAttachmentFormats = outFormats;
    dynPipeRendInfo.colorAttachmentCount = ARRAY_COUNT(outFormats);

    VkCustomResolveCreateInfoEXT customResolveCreateInfo = {};
    customResolveCreateInfo.sType = VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT;
    customResolveCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    customResolveCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    VkFormat colourFormats[] = {mainWindow->format};
    customResolveCreateInfo.pColorAttachmentFormats = colourFormats;
    customResolveCreateInfo.colorAttachmentCount = ARRAY_COUNT(colourFormats);
    dynPipeRendInfo.pNext = &customResolveCreateInfo;

    colPipeCreateInfo.pNext = &dynPipeRendInfo;
    colPipeCreateInfo.renderPass = VK_NULL_HANDLE;
    customResolveCreateInfo.customResolve = VK_FALSE;

    VkPipeline dynColPipe = createGraphicsPipeline(colPipeCreateInfo);

    resPipeCreateInfo.pNext = &dynPipeRendInfo;
    resPipeCreateInfo.renderPass = VK_NULL_HANDLE;
    resPipeCreateInfo.subpass = 0;
    customResolveCreateInfo.customResolve = VK_TRUE;

    VkPipeline dynResPipe = createGraphicsPipeline(resPipeCreateInfo);

    AllocatedImage msaaImg(
        this,
        vkh::ImageCreateInfo(mainWindow->scissor.extent.width, mainWindow->scissor.extent.height, 0,
                             mainWindow->format,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             1, 1, VK_SAMPLE_COUNT_4_BIT),
        VmaAllocationCreateInfo({0, VMA_MEMORY_USAGE_GPU_ONLY}));

    VkImageView msaaRTV = createImageView(
        vkh::ImageViewCreateInfo(msaaImg.image, VK_IMAGE_VIEW_TYPE_2D, mainWindow->format));
    setName(msaaImg.image, "MSAA Image");

    AllocatedImage resImg(
        this,
        vkh::ImageCreateInfo(mainWindow->scissor.extent.width, mainWindow->scissor.extent.height, 0,
                             mainWindow->format,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT),
        VmaAllocationCreateInfo({0, VMA_MEMORY_USAGE_GPU_ONLY}));
    setName(resImg.image, "Resolve Image");

    VkImageView resRTV = createImageView(
        vkh::ImageViewCreateInfo(resImg.image, VK_IMAGE_VIEW_TYPE_2D, mainWindow->format));

    VkRenderingAttachmentInfo colAtt = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
        NULL,
        msaaRTV,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_RESOLVE_MODE_CUSTOM_BIT_EXT,
        resRTV,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_DONT_CARE,
        vkh::ClearValue(0.6f, 0.2f, 0.2f, 1.0f),
    };

    VkRenderingInfo dynRendInfo = {
        VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
        NULL,
        VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT,
        mainWindow->scissor,
        1,
        0,
        1,
        &colAtt,
        NULL,
        NULL,
    };

    VkFramebuffer msaaFB = createFramebuffer(vkh::FramebufferCreateInfo(
        msaaRP, {msaaRTV, resRTV},
        {mainWindow->scissor.extent.width, mainWindow->scissor.extent.height}));

    AllocatedBuffer vb(
        this,
        vkh::BufferCreateInfo(sizeof(DefaultTri),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT),
        VmaAllocationCreateInfo({0, VMA_MEMORY_USAGE_CPU_TO_GPU}));

    vb.upload(DefaultTri);

    vkh::updateDescriptorSets(
        device,
        {
            vkh::WriteDescriptorSet(resDescset, 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                                    {
                                        vkh::DescriptorImageInfo(msaaRTV, VK_IMAGE_LAYOUT_GENERAL),
                                    }),
            vkh::WriteDescriptorSet(dynResDescset, 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                                    {
                                        vkh::DescriptorImageInfo(msaaRTV, VK_IMAGE_LAYOUT_GENERAL),
                                    }),
        });

    while(Running())
    {
      VkCommandBuffer cmd = GetCommandBuffer();

      vkBeginCommandBuffer(cmd, vkh::CommandBufferBeginInfo());

      VkImage swapimg = StartUsingBackbuffer(cmd, VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

      pushMarker(cmd, "RenderPass");
      pushMarker(cmd, "Clear");
      vkh::cmdPipelineBarrier(
          cmd, {
                   vkh::ImageMemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_GENERAL, resImg.image),
               });

      vkCmdClearColorImage(cmd, resImg.image, VK_IMAGE_LAYOUT_GENERAL,
                           vkh::ClearColorValue(0.5f, 0.0f, 0.0f, 1.0f), 1,
                           vkh::ImageSubresourceRange());

      vkh::cmdPipelineBarrier(
          cmd, {
                   vkh::ImageMemoryBarrier(
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, resImg.image),
               });

      vkh::cmdPipelineBarrier(
          cmd, {
                   vkh::ImageMemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_GENERAL, msaaImg.image),
               });

      vkCmdClearColorImage(cmd, msaaImg.image, VK_IMAGE_LAYOUT_GENERAL,
                           vkh::ClearColorValue(0.2f, 0.5f, 0.2f, 1.0f), 1,
                           vkh::ImageSubresourceRange());

      vkh::cmdPipelineBarrier(
          cmd, {
                   vkh::ImageMemoryBarrier(
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, msaaImg.image),
               });

      popMarker(cmd);

      vkCmdBeginRenderPass(cmd, vkh::RenderPassBeginInfo(msaaRP, msaaFB, mainWindow->scissor),
                           VK_SUBPASS_CONTENTS_INLINE);

      mainWindow->setViewScissor(cmd);
      vkh::cmdBindVertexBuffers(cmd, 0, {vb.buffer}, {0});
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeCol);
      setMarker(cmd, "MSAA Draw");
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
      vkh::cmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resLayout, 0, {resDescset},
                                 {});
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeRes);
      setMarker(cmd, "MSAA Resolve");
      vkCmdDraw(cmd, 4, 1, 0, 0);

      vkCmdEndRenderPass(cmd);
      popMarker(cmd);

      pushMarker(cmd, "Dynamic");
      pushMarker(cmd, "Clear");
      vkh::cmdPipelineBarrier(
          cmd, {
                   vkh::ImageMemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_GENERAL, resImg.image),
               });

      vkCmdClearColorImage(cmd, resImg.image, VK_IMAGE_LAYOUT_GENERAL,
                           vkh::ClearColorValue(0.0f, 0.0f, 0.5f, 1.0f), 1,
                           vkh::ImageSubresourceRange());
      vkh::cmdPipelineBarrier(
          cmd, {
                   vkh::ImageMemoryBarrier(
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, resImg.image),
               });

      vkh::cmdPipelineBarrier(
          cmd, {
                   vkh::ImageMemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_GENERAL, msaaImg.image),
               });

      vkCmdClearColorImage(cmd, msaaImg.image, VK_IMAGE_LAYOUT_GENERAL,
                           vkh::ClearColorValue(0.2f, 0.2f, 0.5f, 1.0f), 1,
                           vkh::ImageSubresourceRange());

      vkh::cmdPipelineBarrier(
          cmd, {
                   vkh::ImageMemoryBarrier(
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, msaaImg.image),
               });

      popMarker(cmd);
      vkCmdBeginRendering(cmd, &dynRendInfo);
      mainWindow->setViewScissor(cmd);
      vkh::cmdBindVertexBuffers(cmd, 0, {vb.buffer}, {0});
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dynColPipe);
      setMarker(cmd, "MSAA Draw");
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkh::cmdPipelineBarrier(
          cmd,
          {
              vkh::ImageMemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                      VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                      VK_IMAGE_LAYOUT_GENERAL, msaaImg.image),
          },
          {}, {}, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT);
      vkh::cmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resLayout, 0,
                                 {dynResDescset}, {});
      vkCmdBeginCustomResolveEXT(cmd, NULL);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dynResPipe);
      setMarker(cmd, "MSAA Resolve");
      vkCmdDraw(cmd, 4, 1, 0, 0);
      vkCmdEndRendering(cmd);
      popMarker(cmd);

      vkh::cmdPipelineBarrier(
          cmd, {
                   vkh::ImageMemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                           msaaImg.image),
               });
      vkh::cmdPipelineBarrier(
          cmd,
          {vkh::ImageMemoryBarrier(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, resImg.image)});

      blitToSwap(cmd, resImg.image, VK_IMAGE_LAYOUT_GENERAL, swapimg,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

      FinishUsingBackbuffer(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

      vkEndCommandBuffer(cmd);

      Submit(0, 1, {cmd});

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
