#include "rtv/RayTracingPipeline.h"

#include "rtv/BufferUploader.h"
#include "rtv/Check.h"
#include "rtv/PipelineCache.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ShaderModule.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace rtv {

namespace {

void uploadSbtSection(
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    Buffer& buffer,
    VkStridedDeviceAddressRegionKHR& region,
    const uint8_t* handles,
    uint32_t firstGroup,
    uint32_t recordCount,
    uint32_t handleSize,
    VkDeviceSize stride,
    VkDeviceSize sectionSize,
    const char* debugName) {
    std::vector<uint8_t> data(static_cast<size_t>(sectionSize), 0);
    for (uint32_t record = 0; record < recordCount; ++record) {
        std::memcpy(
            data.data() + static_cast<size_t>(stride * record),
            handles + static_cast<size_t>(handleSize * (firstGroup + record)),
            handleSize);
    }
    buffer.create(allocator, BufferDesc{
        .size = sectionSize,
        .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = debugName,
    });
    uploader.uploadToBuffer(buffer, data.data(), static_cast<VkDeviceSize>(data.size()));
    region.deviceAddress = buffer.deviceAddress();
    region.stride = stride;
    region.size = stride * recordCount;
}

void uploadSbtRecords(
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    Buffer& buffer,
    VkStridedDeviceAddressRegionKHR& region,
    const uint8_t* handles,
    const std::vector<uint32_t>& groups,
    uint32_t handleSize,
    VkDeviceSize stride,
    VkDeviceSize sectionSize,
    const char* debugName) {
    std::vector<uint8_t> data(static_cast<size_t>(sectionSize), 0);
    for (uint32_t record = 0; record < static_cast<uint32_t>(groups.size()); ++record) {
        std::memcpy(
            data.data() + static_cast<size_t>(stride * record),
            handles + static_cast<size_t>(handleSize * groups[record]),
            handleSize);
    }
    buffer.create(allocator, BufferDesc{
        .size = sectionSize,
        .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = debugName,
    });
    uploader.uploadToBuffer(buffer, data.data(), static_cast<VkDeviceSize>(data.size()));
    region.deviceAddress = buffer.deviceAddress();
    region.stride = stride;
    region.size = stride * static_cast<VkDeviceSize>(groups.size());
}

void applyRayTracingPipelineFlags(
    VkRayTracingPipelineCreateInfoKHR& pipelineInfo,
    bool opacityMicromapsEnabled,
    bool allowMotionBlur) {
    if (opacityMicromapsEnabled) {
        pipelineInfo.flags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
    }
    if (allowMotionBlur) {
        pipelineInfo.flags |= VK_PIPELINE_CREATE_RAY_TRACING_ALLOW_MOTION_BIT_NV;
    }
}

template <typename Fn>
void timeRayTracingPipelineCreate(const std::string& label, Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    const auto end = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "Pipeline create: ray tracing " << label << " took " << ms << " ms\n";
}

} // namespace

RayTracingPipeline::RayTracingPipeline(
    VkDevice device,
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties,
    const ShaderModule& raygenShader,
    const ShaderModule& primaryMissShader,
    const ShaderModule& shadowMissShader,
    const ShaderModule& closestHitShader,
    const ShaderModule& primaryAnyHitShader,
    const ShaderModule& shadowAnyHitShader,
    std::vector<VkDescriptorSetLayout> setLayouts,
    PipelineCache& pipelineCache,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    bool opacityMicromapsEnabled,
    bool allowMotionBlur,
    bool usePipelineCache)
    : device_(device) {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_), "vkCreatePipelineLayout(ray tracing)");

    std::array<VkPipelineShaderStageCreateInfo, 6> stages = {
        raygenShader.stageInfo(),
        primaryMissShader.stageInfo(),
        shadowMissShader.stageInfo(),
        closestHitShader.stageInfo(),
        primaryAnyHitShader.stageInfo(),
        shadowAnyHitShader.stageInfo(),
    };

    std::array<VkRayTracingShaderGroupCreateInfoKHR, 5> groups{};
    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[2].generalShader = 2;
    groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[3].generalShader = VK_SHADER_UNUSED_KHR;
    groups[3].closestHitShader = 3;
    groups[3].anyHitShader = 4;
    groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[4].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[4].generalShader = VK_SHADER_UNUSED_KHR;
    groups[4].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[4].anyHitShader = 5;
    groups[4].intersectionShader = VK_SHADER_UNUSED_KHR;

    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.groupCount = static_cast<uint32_t>(groups.size());
    pipelineInfo.pGroups = groups.data();
    pipelineInfo.maxPipelineRayRecursionDepth = 1;
    pipelineInfo.layout = layout_;
    applyRayTracingPipelineFlags(
        pipelineInfo,
        opacityMicromapsEnabled,
        allowMotionBlur);
    timeRayTracingPipelineCreate(raygenShader.debugName(), [&]() {
        checkVk(vkCreateRayTracingPipelinesKHR(
            device_,
            VK_NULL_HANDLE,
            usePipelineCache ? pipelineCache.handle() : VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            &pipeline_), "vkCreateRayTracingPipelinesKHR");
    });

    const uint32_t handleSize = properties.shaderGroupHandleSize;
    const uint32_t handleAlignment = properties.shaderGroupHandleAlignment;
    const uint32_t baseAlignment = properties.shaderGroupBaseAlignment;
    const VkDeviceSize stride = Buffer::alignUp(handleSize, handleAlignment);
    const VkDeviceSize raygenSectionSize = Buffer::alignUp(stride, baseAlignment);
    const VkDeviceSize missSectionSize = Buffer::alignUp(stride * 2u, baseAlignment);
    const VkDeviceSize hitSectionSize = Buffer::alignUp(stride * 2u, baseAlignment);

    std::vector<uint8_t> handles(handleSize * groups.size());
    checkVk(vkGetRayTracingShaderGroupHandlesKHR(
        device_,
        pipeline_,
        0,
        static_cast<uint32_t>(groups.size()),
        handles.size(),
        handles.data()), "vkGetRayTracingShaderGroupHandlesKHR");

    uploadSbtSection(allocator, uploader, raygenSbt_, raygenRegion_, handles.data(), 0u, 1u, handleSize, stride, raygenSectionSize, "raygen SBT");
    uploadSbtSection(allocator, uploader, missSbt_, missRegion_, handles.data(), 1u, 2u, handleSize, stride, missSectionSize, "miss SBT");
    uploadSbtSection(allocator, uploader, hitSbt_, hitRegion_, handles.data(), 3u, 2u, handleSize, stride, hitSectionSize, "hit SBT");
}

RayTracingPipeline::RayTracingPipeline(
    VkDevice device,
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties,
    const ShaderModule& raygenShader,
    const ShaderModule& primaryMissShader,
    const ShaderModule& shadowMissShader,
    const ShaderModule& terminalMissShader,
    const ShaderModule& closestHitShader,
    const ShaderModule& primaryAnyHitShader,
    const ShaderModule& shadowAnyHitShader,
    const ShaderModule& terminalClosestHitShader,
    const ShaderModule& terminalAnyHitShader,
    std::vector<VkDescriptorSetLayout> setLayouts,
    PipelineCache& pipelineCache,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    bool opacityMicromapsEnabled,
    bool usePipelineCache,
    bool deferSbtUpload)
    : device_(device) {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_), "vkCreatePipelineLayout(ray tracing native2b)");

    std::array<VkPipelineShaderStageCreateInfo, 9> stages = {
        raygenShader.stageInfo(),
        primaryMissShader.stageInfo(),
        shadowMissShader.stageInfo(),
        terminalMissShader.stageInfo(),
        closestHitShader.stageInfo(),
        primaryAnyHitShader.stageInfo(),
        shadowAnyHitShader.stageInfo(),
        terminalClosestHitShader.stageInfo(),
        terminalAnyHitShader.stageInfo(),
    };

    std::array<VkRayTracingShaderGroupCreateInfoKHR, 7> groups{};
    for (auto& group : groups) {
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[2].generalShader = 2;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[3].generalShader = 3;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[4].closestHitShader = 4;
    groups[4].anyHitShader = 5;
    groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[5].anyHitShader = 6;
    groups[6].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[6].closestHitShader = 7;
    groups[6].anyHitShader = 8;

    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.groupCount = static_cast<uint32_t>(groups.size());
    pipelineInfo.pGroups = groups.data();
    pipelineInfo.maxPipelineRayRecursionDepth = 1;
    pipelineInfo.layout = layout_;
    applyRayTracingPipelineFlags(pipelineInfo, opacityMicromapsEnabled, false);
    timeRayTracingPipelineCreate(raygenShader.debugName(), [&]() {
        checkVk(vkCreateRayTracingPipelinesKHR(
            device_, VK_NULL_HANDLE, usePipelineCache ? pipelineCache.handle() : VK_NULL_HANDLE,
            1, &pipelineInfo, nullptr, &pipeline_), "vkCreateRayTracingPipelinesKHR(native2b)");
    });

    const uint32_t handleSize = properties.shaderGroupHandleSize;
    const VkDeviceSize stride = Buffer::alignUp(handleSize, properties.shaderGroupHandleAlignment);
    const VkDeviceSize raygenSectionSize = Buffer::alignUp(stride, properties.shaderGroupBaseAlignment);
    const VkDeviceSize missSectionSize = Buffer::alignUp(stride * 3u, properties.shaderGroupBaseAlignment);
    const VkDeviceSize hitSectionSize = Buffer::alignUp(stride * 3u, properties.shaderGroupBaseAlignment);
    std::vector<uint8_t> handles(handleSize * groups.size());
    checkVk(vkGetRayTracingShaderGroupHandlesKHR(
        device_, pipeline_, 0, static_cast<uint32_t>(groups.size()), handles.size(), handles.data()),
        "vkGetRayTracingShaderGroupHandlesKHR(native2b)");
    if (deferSbtUpload) {
        deferredSbtHandles_ = std::move(handles);
        deferredSbtHandleSize_ = handleSize;
        deferredSbtStride_ = stride;
        deferredRaygenSectionSize_ = raygenSectionSize;
        deferredMissSectionSize_ = missSectionSize;
        deferredHitSectionSize_ = hitSectionSize;
    } else {
        uploadSbtSection(allocator, uploader, raygenSbt_, raygenRegion_, handles.data(), 0u, 1u, handleSize, stride, raygenSectionSize, "native2b raygen SBT");
        uploadSbtSection(allocator, uploader, missSbt_, missRegion_, handles.data(), 1u, 3u, handleSize, stride, missSectionSize, "native2b miss SBT");
        uploadSbtSection(allocator, uploader, hitSbt_, hitRegion_, handles.data(), 4u, 3u, handleSize, stride, hitSectionSize, "native2b hit SBT");
    }
}

RayTracingPipeline::RayTracingPipeline(
    VkDevice device,
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& properties,
    const ShaderModule& primaryRaygenShader,
    const ShaderModule& shadowRaygenShader,
    const ShaderModule& primaryMissShader,
    const ShaderModule& shadowMissShader,
    const ShaderModule& closestHitShader,
    const ShaderModule& primaryAnyHitShader,
    const ShaderModule& shadowAnyHitShader,
    std::vector<VkDescriptorSetLayout> setLayouts,
    PipelineCache& pipelineCache,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    bool opacityMicromapsEnabled,
    bool allowMotionBlur,
    bool usePipelineCache)
    : device_(device) {
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    checkVk(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_), "vkCreatePipelineLayout(ray tracing multi-raygen)");

    std::array<VkPipelineShaderStageCreateInfo, 7> stages = {
        primaryRaygenShader.stageInfo(),
        primaryMissShader.stageInfo(),
        shadowMissShader.stageInfo(),
        closestHitShader.stageInfo(),
        primaryAnyHitShader.stageInfo(),
        shadowAnyHitShader.stageInfo(),
        shadowRaygenShader.stageInfo(),
    };

    std::array<VkRayTracingShaderGroupCreateInfoKHR, 6> groups{};
    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[2].generalShader = 2;
    groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[3].generalShader = VK_SHADER_UNUSED_KHR;
    groups[3].closestHitShader = 3;
    groups[3].anyHitShader = 4;
    groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[4].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[4].generalShader = VK_SHADER_UNUSED_KHR;
    groups[4].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[4].anyHitShader = 5;
    groups[4].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[5].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[5].generalShader = 6;
    groups[5].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[5].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[5].intersectionShader = VK_SHADER_UNUSED_KHR;

    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.groupCount = static_cast<uint32_t>(groups.size());
    pipelineInfo.pGroups = groups.data();
    pipelineInfo.maxPipelineRayRecursionDepth = 1;
    pipelineInfo.layout = layout_;
    applyRayTracingPipelineFlags(
        pipelineInfo,
        opacityMicromapsEnabled,
        allowMotionBlur);
    timeRayTracingPipelineCreate(primaryRaygenShader.debugName(), [&]() {
        checkVk(vkCreateRayTracingPipelinesKHR(
            device_,
            VK_NULL_HANDLE,
            usePipelineCache ? pipelineCache.handle() : VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            &pipeline_), "vkCreateRayTracingPipelinesKHR(multi-raygen)");
    });

    const uint32_t handleSize = properties.shaderGroupHandleSize;
    const uint32_t handleAlignment = properties.shaderGroupHandleAlignment;
    const uint32_t baseAlignment = properties.shaderGroupBaseAlignment;
    const VkDeviceSize stride = Buffer::alignUp(handleSize, handleAlignment);
    const VkDeviceSize raygenStride = Buffer::alignUp(handleSize, baseAlignment);
    const VkDeviceSize raygenSectionSize = Buffer::alignUp(raygenStride * 2u, baseAlignment);
    const VkDeviceSize missSectionSize = Buffer::alignUp(stride * 2u, baseAlignment);
    const VkDeviceSize hitSectionSize = Buffer::alignUp(stride * 2u, baseAlignment);

    std::vector<uint8_t> handles(handleSize * groups.size());
    checkVk(vkGetRayTracingShaderGroupHandlesKHR(
        device_,
        pipeline_,
        0,
        static_cast<uint32_t>(groups.size()),
        handles.size(),
        handles.data()), "vkGetRayTracingShaderGroupHandlesKHR(multi-raygen)");

    uploadSbtRecords(allocator, uploader, raygenSbt_, raygenRegion_, handles.data(), {0u, 5u}, handleSize, raygenStride, raygenSectionSize, "raygen SBT");
    uploadSbtSection(allocator, uploader, missSbt_, missRegion_, handles.data(), 1u, 2u, handleSize, stride, missSectionSize, "miss SBT");
    uploadSbtSection(allocator, uploader, hitSbt_, hitRegion_, handles.data(), 3u, 2u, handleSize, stride, hitSectionSize, "hit SBT");
}

void RayTracingPipeline::finalizeDeferredSbtUpload(ResourceAllocator& allocator, BufferUploader& uploader) {
    if (deferredSbtHandles_.empty()) {
        return;
    }
    uploadSbtSection(
        allocator, uploader, raygenSbt_, raygenRegion_, deferredSbtHandles_.data(),
        0u, 1u, deferredSbtHandleSize_, deferredSbtStride_, deferredRaygenSectionSize_, "native2b raygen SBT");
    uploadSbtSection(
        allocator, uploader, missSbt_, missRegion_, deferredSbtHandles_.data(),
        1u, 3u, deferredSbtHandleSize_, deferredSbtStride_, deferredMissSectionSize_, "native2b miss SBT");
    uploadSbtSection(
        allocator, uploader, hitSbt_, hitRegion_, deferredSbtHandles_.data(),
        4u, 3u, deferredSbtHandleSize_, deferredSbtStride_, deferredHitSectionSize_, "native2b hit SBT");
    deferredSbtHandles_.clear();
    deferredSbtHandles_.shrink_to_fit();
}

RayTracingPipeline::~RayTracingPipeline() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
}

void RayTracingPipeline::bind(VkCommandBuffer commandBuffer) const {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline_);
}

void RayTracingPipeline::traceRays(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height, uint32_t raygenIndex) const {
    VkStridedDeviceAddressRegionKHR raygenRegion = raygenRegion_;
    if (raygenRegion.stride > 0) {
        const VkDeviceSize offset = raygenRegion.stride * raygenIndex;
        if (offset + raygenRegion.stride > raygenRegion.size) {
            throw std::runtime_error("Ray tracing raygen SBT index out of range");
        }
        raygenRegion.deviceAddress += offset;
        raygenRegion.size = raygenRegion.stride;
    }
    vkCmdTraceRaysKHR(commandBuffer, &raygenRegion, &missRegion_, &hitRegion_, &callableRegion_, width, height, 1);
}

void RayTracingPipeline::traceRaysIndirect(
    VkCommandBuffer commandBuffer,
    const Buffer& indirectBuffer,
    VkDeviceSize indirectOffset,
    uint32_t raygenIndex) const {
    VkStridedDeviceAddressRegionKHR raygenRegion = raygenRegion_;
    if (raygenRegion.stride > 0) {
        const VkDeviceSize offset = raygenRegion.stride * raygenIndex;
        if (offset + raygenRegion.stride > raygenRegion.size) {
            throw std::runtime_error("Ray tracing raygen SBT index out of range");
        }
        raygenRegion.deviceAddress += offset;
        raygenRegion.size = raygenRegion.stride;
    }
    vkCmdTraceRaysIndirectKHR(
        commandBuffer,
        &raygenRegion,
        &missRegion_,
        &hitRegion_,
        &callableRegion_,
        indirectBuffer.deviceAddress() + indirectOffset);
}

} // namespace rtv
