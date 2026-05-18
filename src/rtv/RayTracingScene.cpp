#include "rtv/RayTracingScene.h"

#include "rtv/BufferUploader.h"
#include "rtv/Check.h"
#include "rtv/GpuScene.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/UploadContext.h"
#include "rtv/VulkanContext.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace rtv {

namespace {

VkTransformMatrixKHR toVkTransform(const glm::mat4& transform) {
    VkTransformMatrixKHR result{};
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t col = 0; col < 4; ++col) {
            result.matrix[row][col] = transform[col][row];
        }
    }
    return result;
}

Buffer createScratch(ResourceAllocator& allocator, VkDeviceSize size, const char* debugName) {
    return Buffer(allocator, BufferDesc{
        .size = std::max<VkDeviceSize>(size, 4),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = debugName,
    });
}

void accelerationBuildBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

RayTracingScene::RayTracingScene(
    const VulkanContext& context,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    const GpuScene& scene) {
    build(context, allocator, uploader, scene);
}

void RayTracingScene::build(const VulkanContext& context, ResourceAllocator& allocator, BufferUploader& uploader, const GpuScene& scene) {
    if (!context.supportsHardwareRayTracing()) {
        throw std::runtime_error("Cannot build ray tracing scene because hardware RT is unavailable");
    }
    if (scene.rayTracingMeshes().empty() || scene.rayTracingInstances().empty()) {
        throw std::runtime_error("Cannot build ray tracing scene: no triangle meshes or instances");
    }

    const VkDevice device = context.device();
    const auto& asProps = context.rayTracingInfo().accelerationStructureProperties;
    const VkDeviceSize scratchAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;

    blases_.clear();
    blases_.resize(scene.rayTracingMeshes().size());
    accelerationStructureBytes_ = 0;

    VkCommandBuffer cmd = uploader.uploadContext().begin();
    std::vector<Buffer> scratchBuffers;
    scratchBuffers.reserve(scene.rayTracingMeshes().size() + 1u);

    for (const RayTracingMeshBuildInput& mesh : scene.rayTracingMeshes()) {
        if (mesh.meshIndex >= blases_.size() || mesh.vertexCount == 0 || mesh.indexCount < 3) {
            continue;
        }

        VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = scene.localVertices().deviceAddress();
        triangles.vertexStride = sizeof(GpuLocalVertex);
        triangles.maxVertex = mesh.firstVertex + mesh.vertexCount - 1u;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = scene.localIndices().deviceAddress() + sizeof(uint32_t) * mesh.firstIndex;

        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        // Only skip any-hit when the mesh is known to be alpha-free and double-sided.
        // Single-sided or alpha-tested meshes still need any-hit for material rules.
        geometry.flags = mesh.opaqueTraversalSafe ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        geometry.geometry.triangles = triangles;

        const uint32_t primitiveCount = mesh.indexCount / 3u;
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;

        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &buildInfo,
            &primitiveCount,
            &sizes);

        blases_[mesh.meshIndex].create(device, allocator, AccelerationStructureDesc{
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .size = sizes.accelerationStructureSize,
            .debugName = "ray tracing BLAS",
        });
        accelerationStructureBytes_ += sizes.accelerationStructureSize;

        scratchBuffers.push_back(createScratch(allocator, sizes.buildScratchSize + scratchAlignment, "ray tracing BLAS scratch"));
        Buffer& scratch = scratchBuffers.back();
        buildInfo.dstAccelerationStructure = blases_[mesh.meshIndex].handle();
        buildInfo.scratchData.deviceAddress = Buffer::alignUp(scratch.deviceAddress(), scratchAlignment);

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = primitiveCount;
        range.primitiveOffset = 0;
        range.firstVertex = 0;
        range.transformOffset = 0;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, ranges);
        accelerationBuildBarrier(cmd);
    }

    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(scene.rayTracingInstances().size());
    for (const RayTracingInstanceBuildInput& instance : scene.rayTracingInstances()) {
        if (!instance.visible || instance.meshIndex >= blases_.size() || blases_[instance.meshIndex].handle() == VK_NULL_HANDLE) {
            continue;
        }

        VkAccelerationStructureInstanceKHR vkInstance{};
        vkInstance.transform = toVkTransform(instance.transform);
        vkInstance.instanceCustomIndex = instance.instanceIndex;
        vkInstance.mask = 0xff;
        vkInstance.instanceShaderBindingTableRecordOffset = 0;
        vkInstance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        vkInstance.accelerationStructureReference = blases_[instance.meshIndex].deviceAddress();
        instances.push_back(vkInstance);
    }
    if (instances.empty()) {
        throw std::runtime_error("Cannot build ray tracing TLAS: no valid instances");
    }
    instanceCount_ = static_cast<uint32_t>(instances.size());

    instanceBuffer_.create(allocator, BufferDesc{
        .size = sizeof(VkAccelerationStructureInstanceKHR) * instances.size(),
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "ray tracing TLAS instances",
    });
    instanceBuffer_.write(instances.data(), sizeof(VkAccelerationStructureInstanceKHR) * instances.size());
    instanceBuffer_.flush();

    VkAccelerationStructureGeometryInstancesDataKHR instancesData{};
    instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = instanceBuffer_.deviceAddress();

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instancesData;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo,
        &instanceCount_,
        &sizes);

    tlas_.create(device, allocator, AccelerationStructureDesc{
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .size = sizes.accelerationStructureSize,
        .debugName = "ray tracing TLAS",
    });
    accelerationStructureBytes_ += sizes.accelerationStructureSize;

    scratchBuffers.push_back(createScratch(allocator, sizes.buildScratchSize + scratchAlignment, "ray tracing TLAS scratch"));
    Buffer& scratch = scratchBuffers.back();
    buildInfo.dstAccelerationStructure = tlas_.handle();
    buildInfo.scratchData.deviceAddress = Buffer::alignUp(scratch.deviceAddress(), scratchAlignment);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instanceCount_;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, ranges);
    accelerationBuildBarrier(cmd);

    uploader.uploadContext().submitAndWait(cmd);

    std::cout << "RT scene: BLAS=" << blases_.size()
              << " instances=" << instanceCount_
              << " AS memory=" << (static_cast<double>(accelerationStructureBytes_) / (1024.0 * 1024.0)) << " MB\n";
}

} // namespace rtv
