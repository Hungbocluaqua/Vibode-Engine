#include "rtv/RayTracingScene.h"

#include "rtv/BufferUploader.h"
#include "rtv/Check.h"
#include "rtv/GpuScene.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/UploadContext.h"
#include "rtv/VulkanContext.h"

#include <algorithm>
#include <chrono>
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

constexpr uint32_t instanceFlagVisible = 1u << 0u;
constexpr uint32_t instanceFlagVisibleToCamera = 1u << 1u;
constexpr uint32_t instanceFlagCastShadow = 1u << 2u;
constexpr uint8_t rayMaskCamera = 0x01u;
constexpr uint8_t rayMaskShadow = 0x02u;

struct BlasBuildSizes {
    uint32_t meshBuildIndex = 0;
    uint32_t primitiveCount = 0;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
};

uint32_t countValidRtInstances(const GpuScene& scene, size_t blasCount) {
    uint32_t count = 0;
    for (const RayTracingInstanceBuildInput& instance : scene.rayTracingInstances()) {
        if (instance.meshIndex < blasCount) {
            ++count;
        }
    }
    return count;
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

std::vector<VkAccelerationStructureInstanceKHR> buildVkInstances(
    const GpuScene& scene,
    const std::vector<AccelerationStructure>& blases) {
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(scene.rayTracingInstances().size());
    for (const RayTracingInstanceBuildInput& instance : scene.rayTracingInstances()) {
        if (instance.meshIndex >= blases.size() || blases[instance.meshIndex].handle() == VK_NULL_HANDLE) {
            continue;
        }

        VkAccelerationStructureInstanceKHR vkInstance{};
        vkInstance.transform = toVkTransform(instance.transform);
        vkInstance.instanceCustomIndex = instance.instanceIndex;
        uint8_t mask = 0u;
        const uint32_t flags = instance.flags == 0u
            ? (instanceFlagVisible | instanceFlagVisibleToCamera | instanceFlagCastShadow)
            : instance.flags;
        if (instance.visible && (flags & instanceFlagVisible) != 0u) {
            if ((flags & instanceFlagVisibleToCamera) != 0u) {
                mask |= rayMaskCamera;
            }
            if ((flags & instanceFlagCastShadow) != 0u) {
                mask |= rayMaskShadow;
            }
        }
        vkInstance.mask = mask;
        vkInstance.instanceShaderBindingTableRecordOffset = 0;
        vkInstance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        vkInstance.accelerationStructureReference = blases[instance.meshIndex].deviceAddress();
        instances.push_back(vkInstance);
    }
    return instances;
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

    std::vector<BlasBuildSizes> blasBuildSizes;
    blasBuildSizes.reserve(scene.rayTracingMeshes().size());
    VkDeviceSize maxBuildScratchSize = 0;
    const auto& rtMeshes = scene.rayTracingMeshes();
    for (uint32_t meshBuildIndex = 0; meshBuildIndex < rtMeshes.size(); ++meshBuildIndex) {
        const RayTracingMeshBuildInput& mesh = rtMeshes[meshBuildIndex];
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
        if (mesh.updateMode == AccelUpdateMode::RefitDeform) {
            buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        }
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

        maxBuildScratchSize = std::max(maxBuildScratchSize, sizes.buildScratchSize);
        blasBuildSizes.push_back(BlasBuildSizes{
            .meshBuildIndex = meshBuildIndex,
            .primitiveCount = primitiveCount,
            .sizes = sizes,
        });
    }

    if (blasBuildSizes.empty()) {
        throw std::runtime_error("Cannot build ray tracing scene: no valid BLAS inputs");
    }

    uint32_t plannedInstanceCount = countValidRtInstances(scene, blases_.size());
    if (plannedInstanceCount == 0) {
        throw std::runtime_error("Cannot build ray tracing TLAS: no valid instances");
    }

    VkAccelerationStructureGeometryInstancesDataKHR tlasInstancesDataForSizing{};
    tlasInstancesDataForSizing.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlasInstancesDataForSizing.arrayOfPointers = VK_FALSE;

    VkAccelerationStructureGeometryKHR tlasGeometryForSizing{};
    tlasGeometryForSizing.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeometryForSizing.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometryForSizing.geometry.instances = tlasInstancesDataForSizing;

    VkAccelerationStructureBuildGeometryInfoKHR tlasBuildInfoForSizing{};
    tlasBuildInfoForSizing.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlasBuildInfoForSizing.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuildInfoForSizing.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    tlasBuildInfoForSizing.geometryCount = 1;
    tlasBuildInfoForSizing.pGeometries = &tlasGeometryForSizing;

    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
    tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &tlasBuildInfoForSizing,
        &plannedInstanceCount,
        &tlasSizes);
    maxBuildScratchSize = std::max(maxBuildScratchSize, tlasSizes.buildScratchSize);

    Buffer buildScratch = createScratch(
        allocator,
        maxBuildScratchSize + scratchAlignment,
        "ray tracing AS build scratch arena");
    const VkDeviceAddress scratchAddress = Buffer::alignUp(buildScratch.deviceAddress(), scratchAlignment);

    VkCommandBuffer cmd = uploader.uploadContext().begin();

    for (const BlasBuildSizes& record : blasBuildSizes) {
        const RayTracingMeshBuildInput& mesh = rtMeshes[record.meshBuildIndex];
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
        geometry.flags = mesh.opaqueTraversalSafe ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        geometry.geometry.triangles = triangles;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        const bool allowBlasUpdate = mesh.updateMode == AccelUpdateMode::RefitDeform;
        if (allowBlasUpdate) {
            buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        }
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;

        blases_[mesh.meshIndex].create(device, allocator, AccelerationStructureDesc{
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .size = record.sizes.accelerationStructureSize,
            .allowUpdate = allowBlasUpdate,
            .debugName = "ray tracing BLAS",
        });
        accelerationStructureBytes_ += record.sizes.accelerationStructureSize;

        buildInfo.dstAccelerationStructure = blases_[mesh.meshIndex].handle();
        buildInfo.scratchData.deviceAddress = scratchAddress;

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = record.primitiveCount;
        range.primitiveOffset = 0;
        range.firstVertex = 0;
        range.transformOffset = 0;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, ranges);
        accelerationBuildBarrier(cmd);
    }

    std::vector<VkAccelerationStructureInstanceKHR> instances = buildVkInstances(scene, blases_);
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
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    tlas_.create(device, allocator, AccelerationStructureDesc{
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .size = tlasSizes.accelerationStructureSize,
        .allowUpdate = true,
        .debugName = "ray tracing TLAS",
    });
    accelerationStructureBytes_ += tlasSizes.accelerationStructureSize;
    tlasUpdateScratchSize_ = tlasSizes.updateScratchSize;
    tlasRefitScratch_ = createScratch(
        allocator,
        std::max<VkDeviceSize>(tlasUpdateScratchSize_, 4) + scratchAlignment,
        "ray tracing TLAS refit scratch");

    buildInfo.dstAccelerationStructure = tlas_.handle();
    buildInfo.scratchData.deviceAddress = scratchAddress;

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

bool RayTracingScene::refitTransforms(
    const VulkanContext& context,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    const GpuScene& scene) {
    if (!context.supportsHardwareRayTracing() || tlas_.handle() == VK_NULL_HANDLE || !tlas_.allowUpdate()) {
        return false;
    }

    const std::vector<VkAccelerationStructureInstanceKHR> instances = buildVkInstances(scene, blases_);
    if (instances.empty() || instances.size() != instanceCount_) {
        return false;
    }
    if (instanceBuffer_.handle() == VK_NULL_HANDLE ||
        instanceBuffer_.size() < sizeof(VkAccelerationStructureInstanceKHR) * instances.size()) {
        return false;
    }

    instanceBuffer_.write(instances.data(), sizeof(VkAccelerationStructureInstanceKHR) * instances.size());
    instanceBuffer_.flush(sizeof(VkAccelerationStructureInstanceKHR) * instances.size());

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
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    buildInfo.srcAccelerationStructure = tlas_.handle();
    buildInfo.dstAccelerationStructure = tlas_.handle();
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    const auto& asProps = context.rayTracingInfo().accelerationStructureProperties;
    const VkDeviceSize scratchAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;
    const VkDeviceSize scratchSize = std::max<VkDeviceSize>(tlasUpdateScratchSize_, 4) + scratchAlignment;
    if (tlasRefitScratch_.handle() == VK_NULL_HANDLE || tlasRefitScratch_.size() < scratchSize) {
        tlasRefitScratch_ = createScratch(allocator, scratchSize, "ray tracing TLAS refit scratch");
    }
    buildInfo.scratchData.deviceAddress = Buffer::alignUp(tlasRefitScratch_.deviceAddress(), scratchAlignment);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instanceCount_;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};

    const auto start = std::chrono::steady_clock::now();
    VkCommandBuffer cmd = uploader.uploadContext().begin();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, ranges);
    accelerationBuildBarrier(cmd);
    uploader.uploadContext().submitAndWait(cmd);
    const auto end = std::chrono::steady_clock::now();
    lastTlasRefitMs_ = std::chrono::duration<float, std::milli>(end - start).count();
    return true;
}

} // namespace rtv
