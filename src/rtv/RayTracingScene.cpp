#include "rtv/RayTracingScene.h"

#include "rtv/BufferUploader.h"
#include "rtv/Check.h"
#include "rtv/GpuScene.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/UploadContext.h"
#include "rtv/VulkanContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

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
    std::vector<uint32_t> primitiveCounts;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
};

struct BlasGeometryRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t alphaClass = kPrimitiveAlphaClassOpaque;
    bool opaqueTraversalSafe = false;
    bool useOpacityMicromap = false;
};

struct MeshGeometryRangeGpu {
    uint32_t offset = 0;
    uint32_t count = 0;
};

struct MeshBlasGeometryPlan {
    uint32_t meshBuildIndex = 0;
    std::vector<BlasGeometryRange> ranges;
};

struct MeshOpacityMicromapBuild {
    bool enabled = false;
    uint32_t meshBuildIndex = 0;
    VkMicromapUsageEXT usage{};
    std::vector<uint32_t> indexValues;
    std::vector<VkMicromapTriangleEXT> triangleArray;
    std::vector<uint8_t> packedData;
    Buffer indexBuffer;
    Buffer triangleArrayBuffer;
    Buffer dataBuffer;
    VkMicromapBuildSizesInfoEXT sizes{};
    VkMicromapEXT micromap = VK_NULL_HANDLE;
};

constexpr uint32_t kOpacity4StateTransparent = 0u;
constexpr uint32_t kOpacity4StateOpaque = 1u;
constexpr uint32_t kOpacity4StateUnknownOpaque = 3u;

uint32_t microTriangleCountForSubdivision(uint32_t subdivisionLevel) {
    if (subdivisionLevel >= 16u) {
        return 0u;
    }
    return 1u << (2u * subdivisionLevel);
}

uint32_t vulkanOpacity4State(uint8_t state) {
    switch (static_cast<OpacityMicromapCpuState>(state)) {
    case OpacityMicromapCpuState::Transparent:
        return kOpacity4StateTransparent;
    case OpacityMicromapCpuState::Opaque:
        return kOpacity4StateOpaque;
    case OpacityMicromapCpuState::Unknown:
    case OpacityMicromapCpuState::Mixed:
        return kOpacity4StateUnknownOpaque;
    }
    return kOpacity4StateUnknownOpaque;
}

uint32_t opacitySpecialIndex(int32_t value) {
    return static_cast<uint32_t>(value);
}

uint32_t primitiveAlphaClass(const GpuPrimitiveRecord& primitive) {
    return primitive.metadata.z;
}

bool primitiveOpaqueTraversalSafe(const GpuPrimitiveRecord& primitive) {
    return primitive.metadata.w != 0u && primitiveAlphaClass(primitive) == kPrimitiveAlphaClassOpaque;
}

void appendBlasGeometryRange(
    std::vector<BlasGeometryRange>& ranges,
    const GpuPrimitiveRecord& primitive,
    bool useOpacityMicromap) {
    const uint32_t alphaClass = primitiveAlphaClass(primitive);
    const bool opaqueSafe = primitiveOpaqueTraversalSafe(primitive);
    if (!ranges.empty()) {
        BlasGeometryRange& back = ranges.back();
        const bool contiguous = primitive.indexData.x == back.firstIndex + back.indexCount;
        if (contiguous &&
            back.alphaClass == alphaClass &&
            back.opaqueTraversalSafe == opaqueSafe &&
            back.useOpacityMicromap == useOpacityMicromap) {
            back.indexCount += primitive.indexData.y;
            return;
        }
    }
    ranges.push_back(BlasGeometryRange{
        .firstIndex = primitive.indexData.x,
        .indexCount = primitive.indexData.y,
        .alphaClass = alphaClass,
        .opaqueTraversalSafe = opaqueSafe,
        .useOpacityMicromap = useOpacityMicromap,
    });
}

std::vector<MeshBlasGeometryPlan> buildBlasGeometryPlans(
    const GpuScene& scene,
    const std::vector<MeshOpacityMicromapBuild>& opacityMicromapBuilds,
    const OpacityMicromapBuildStats& opacityMicromapStats,
    std::vector<uint32_t>& geometryTriangleOffsets,
    std::vector<MeshGeometryRangeGpu>& meshGeometryRanges,
    RayTracingBlasGeometryStats& stats) {
    stats = {};
    const auto& rtMeshes = scene.rayTracingMeshes();
    const auto& primitiveRecords = scene.primitiveRecordsCpu();
    std::vector<MeshBlasGeometryPlan> plans(rtMeshes.size());
    meshGeometryRanges.assign(rtMeshes.size(), MeshGeometryRangeGpu{});

    for (uint32_t meshBuildIndex = 0; meshBuildIndex < rtMeshes.size(); ++meshBuildIndex) {
        const RayTracingMeshBuildInput& mesh = rtMeshes[meshBuildIndex];
        MeshBlasGeometryPlan plan;
        plan.meshBuildIndex = meshBuildIndex;
        const bool meshHasOmm = opacityMicromapStats.active &&
            meshBuildIndex < opacityMicromapBuilds.size() &&
            opacityMicromapBuilds[meshBuildIndex].enabled &&
            opacityMicromapBuilds[meshBuildIndex].micromap != VK_NULL_HANDLE;

        if (!primitiveRecords.empty() && mesh.primitiveOffset < primitiveRecords.size()) {
            const uint32_t primitiveEnd = std::min<uint32_t>(
                mesh.primitiveOffset + mesh.primitiveCount,
                static_cast<uint32_t>(primitiveRecords.size()));
            for (uint32_t primitiveIndex = mesh.primitiveOffset; primitiveIndex < primitiveEnd; ++primitiveIndex) {
                const GpuPrimitiveRecord& primitive = primitiveRecords[primitiveIndex];
                if (primitive.indexData.y < 3u) {
                    continue;
                }
                const bool useOmm = meshHasOmm && primitiveAlphaClass(primitive) == kPrimitiveAlphaClassAlphaTested;
                appendBlasGeometryRange(plan.ranges, primitive, useOmm);
            }
        }

        if (plan.ranges.empty() && mesh.indexCount >= 3u) {
            const uint32_t fallbackAlphaClass = mesh.containsBlendedGeometry ? kPrimitiveAlphaClassBlended :
                (mesh.containsAlphaTestedGeometry ? kPrimitiveAlphaClassAlphaTested : kPrimitiveAlphaClassOpaque);
            plan.ranges.push_back(BlasGeometryRange{
                .firstIndex = mesh.firstIndex,
                .indexCount = mesh.indexCount,
                .alphaClass = fallbackAlphaClass,
                .opaqueTraversalSafe = mesh.opaqueTraversalSafe && fallbackAlphaClass == kPrimitiveAlphaClassOpaque,
                .useOpacityMicromap = meshHasOmm && mesh.containsAlphaTestedGeometry && !mesh.containsBlendedGeometry,
            });
        }

        const uint32_t offsetBase = static_cast<uint32_t>(geometryTriangleOffsets.size());
        for (const BlasGeometryRange& range : plan.ranges) {
            geometryTriangleOffsets.push_back(range.firstIndex / 3u);
            ++stats.geometryCount;
            switch (range.alphaClass) {
            case kPrimitiveAlphaClassAlphaTested:
                ++stats.alphaTestedGeometryCount;
                break;
            case kPrimitiveAlphaClassBlended:
                ++stats.blendedGeometryCount;
                break;
            default:
                ++stats.opaqueGeometryCount;
                break;
            }
            if (range.useOpacityMicromap) {
                ++stats.opacityMicromapGeometryCount;
            }
        }
        if (mesh.meshIndex < meshGeometryRanges.size()) {
            meshGeometryRanges[mesh.meshIndex] = MeshGeometryRangeGpu{
                .offset = offsetBase,
                .count = static_cast<uint32_t>(plan.ranges.size()),
            };
        }
        plans[meshBuildIndex] = std::move(plan);
    }
    if (geometryTriangleOffsets.empty()) {
        geometryTriangleOffsets.push_back(0u);
    }
    return plans;
}

bool appendPackedOpacity4State(
    const OpacityMicromapCpuData& cpuData,
    const OpacityMicromapPrimitiveCpuData& primitive,
    uint32_t triangleIndex,
    uint32_t microTriangleCount,
    std::vector<uint8_t>& packedData,
    VkMicromapTriangleEXT& triangle) {
    const uint64_t firstState = static_cast<uint64_t>(primitive.stateOffset) +
        static_cast<uint64_t>(triangleIndex) * microTriangleCount;
    if (firstState + microTriangleCount > cpuData.microTriangleStates.size()) {
        return false;
    }

    const uint32_t dataOffset = static_cast<uint32_t>(packedData.size());
    const uint32_t packedBytes = (microTriangleCount + 3u) / 4u;
    packedData.resize(packedData.size() + packedBytes, 0u);
    for (uint32_t micro = 0; micro < microTriangleCount; ++micro) {
        const uint32_t encoded = vulkanOpacity4State(cpuData.microTriangleStates[static_cast<size_t>(firstState + micro)]);
        packedData[static_cast<size_t>(dataOffset) + micro / 4u] |= static_cast<uint8_t>(encoded << ((micro % 4u) * 2u));
    }

    triangle.dataOffset = dataOffset;
    triangle.subdivisionLevel = static_cast<uint16_t>(primitive.subdivisionLevel);
    triangle.format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT;
    return true;
}

std::vector<MeshOpacityMicromapBuild> prepareOpacityMicromapBuilds(
    const VulkanContext& context,
    ResourceAllocator& allocator,
    const GpuScene& scene,
    RayTracingSceneBuildOptions options,
    OpacityMicromapBuildStats& stats) {
    stats = {};
    stats.requested = options.opacityMicromapsEnabled;
    stats.supported = context.supportsOpacityMicromaps();
    if (!options.opacityMicromapsEnabled) {
        stats.fallbackReason = "disabled";
        return {};
    }
    if (!context.supportsOpacityMicromaps()) {
        stats.fallbackReason = context.opacityMicromapInfo().disabledReason.empty()
            ? "unsupported"
            : context.opacityMicromapInfo().disabledReason;
        return {};
    }

    const OpacityMicromapCpuData& cpuData = scene.opacityMicromapData();
    if (cpuData.primitives.empty() || cpuData.microTriangleStates.empty()) {
        stats.fallbackReason = "no eligible alpha-tested primitives";
        return {};
    }

    std::unordered_map<uint64_t, const OpacityMicromapPrimitiveCpuData*> primitiveMap;
    primitiveMap.reserve(cpuData.primitives.size());
    for (const OpacityMicromapPrimitiveCpuData& primitive : cpuData.primitives) {
        const uint64_t key = (static_cast<uint64_t>(primitive.meshIndex) << 32u) | primitive.primitiveIndex;
        primitiveMap[key] = &primitive;
    }

    const auto& rtMeshes = scene.rayTracingMeshes();
    std::vector<MeshOpacityMicromapBuild> builds(rtMeshes.size());
    for (uint32_t meshBuildIndex = 0; meshBuildIndex < rtMeshes.size(); ++meshBuildIndex) {
        const RayTracingMeshBuildInput& mesh = rtMeshes[meshBuildIndex];
        const uint32_t primitiveCount = mesh.indexCount / 3u;
        if (!mesh.containsAlphaTestedGeometry || primitiveCount == 0) {
            continue;
        }

        MeshOpacityMicromapBuild build;
        build.meshBuildIndex = meshBuildIndex;
        const uint32_t defaultOpacityIndex = mesh.containsBlendedGeometry
            ? opacitySpecialIndex(VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE_EXT)
            : opacitySpecialIndex(VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE_EXT);
        build.indexValues.assign(primitiveCount, defaultOpacityIndex);
        uint32_t meshIndexedTriangleCount = 0;

        for (uint32_t localPrimitive = 0; localPrimitive < mesh.primitiveCount; ++localPrimitive) {
            const uint64_t key = (static_cast<uint64_t>(mesh.meshIndex) << 32u) | localPrimitive;
            auto found = primitiveMap.find(key);
            if (found == primitiveMap.end()) {
                continue;
            }
            const OpacityMicromapPrimitiveCpuData& primitive = *found->second;
            const uint32_t microTriangleCount = microTriangleCountForSubdivision(primitive.subdivisionLevel);
            if (microTriangleCount == 0u || primitive.triangleCount == 0u) {
                continue;
            }
            if (primitive.stateCount < primitive.triangleCount * microTriangleCount) {
                continue;
            }

            if (primitive.firstIndex < mesh.firstIndex) {
                continue;
            }
            const uint32_t meshTriangleOffset = (primitive.firstIndex - mesh.firstIndex) / 3u;
            for (uint32_t tri = 0; tri < primitive.triangleCount; ++tri) {
                const uint64_t stateBase = static_cast<uint64_t>(primitive.stateOffset) +
                    static_cast<uint64_t>(tri) * microTriangleCount;
                if (stateBase + microTriangleCount > cpuData.microTriangleStates.size()) {
                    continue;
                }

                bool allOpaque = true;
                bool allTransparent = true;
                for (uint32_t micro = 0; micro < microTriangleCount; ++micro) {
                    const auto state = static_cast<OpacityMicromapCpuState>(cpuData.microTriangleStates[static_cast<size_t>(stateBase + micro)]);
                    allOpaque &= state == OpacityMicromapCpuState::Opaque;
                    allTransparent &= state == OpacityMicromapCpuState::Transparent;
                }

                VkMicromapTriangleEXT triangle{};
                uint32_t indexValue = 0u;
                if (allOpaque) {
                    indexValue = opacitySpecialIndex(VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_OPAQUE_EXT);
                } else if (allTransparent) {
                    indexValue = opacitySpecialIndex(VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_TRANSPARENT_EXT);
                } else if (appendPackedOpacity4State(cpuData, primitive, tri, microTriangleCount, build.packedData, triangle)) {
                    indexValue = static_cast<uint32_t>(build.triangleArray.size());
                    build.triangleArray.push_back(triangle);
                    stats.packedMicroTriangleCount += microTriangleCount;
                } else {
                    indexValue = opacitySpecialIndex(VK_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE_EXT);
                }

                const uint32_t meshTriangle = meshTriangleOffset + tri;
                if (meshTriangle < build.indexValues.size()) {
                    build.indexValues[meshTriangle] = indexValue;
                    ++stats.indexedTriangleCount;
                    ++meshIndexedTriangleCount;
                }
            }
        }

        if (meshIndexedTriangleCount == 0 && build.triangleArray.empty()) {
            continue;
        }
        build.enabled = true;
        build.usage = VkMicromapUsageEXT{
            .count = static_cast<uint32_t>(build.triangleArray.size()),
            .subdivisionLevel = cpuData.stats.subdivisionLevel,
            .format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT,
        };
        stats.triangleArrayCount += static_cast<uint32_t>(build.triangleArray.size());

        const VkDeviceSize indexBytes = sizeof(uint32_t) * build.indexValues.size();
        build.indexBuffer.create(allocator, BufferDesc{
            .size = std::max<VkDeviceSize>(indexBytes, 4),
            .usage = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT |
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .memory = BufferMemory::Upload,
            .persistentMapped = true,
            .debugName = "opacity micromap AS index buffer",
        });
        build.indexBuffer.write(build.indexValues.data(), indexBytes);
        build.indexBuffer.flush(indexBytes);
        stats.buildInputBytes += indexBytes;

        if (!build.triangleArray.empty()) {
            const VkDeviceSize triangleBytes = sizeof(VkMicromapTriangleEXT) * build.triangleArray.size();
            build.triangleArrayBuffer.create(allocator, BufferDesc{
                .size = std::max<VkDeviceSize>(triangleBytes, 4),
                .usage = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .memory = BufferMemory::Upload,
                .persistentMapped = true,
                .debugName = "opacity micromap triangle array",
            });
            build.triangleArrayBuffer.write(build.triangleArray.data(), triangleBytes);
            build.triangleArrayBuffer.flush(triangleBytes);
            stats.buildInputBytes += triangleBytes;

            const VkDeviceSize dataBytes = build.packedData.size();
            build.dataBuffer.create(allocator, BufferDesc{
                .size = std::max<VkDeviceSize>(dataBytes, 4),
                .usage = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .memory = BufferMemory::Upload,
                .persistentMapped = true,
                .debugName = "opacity micromap packed data",
            });
            build.dataBuffer.write(build.packedData.data(), dataBytes);
            build.dataBuffer.flush(dataBytes);
            stats.buildInputBytes += dataBytes;

            VkMicromapBuildInfoEXT buildInfo{};
            buildInfo.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
            buildInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
            buildInfo.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
            buildInfo.usageCountsCount = 1;
            buildInfo.pUsageCounts = &build.usage;
            buildInfo.data.deviceAddress = build.dataBuffer.deviceAddress();
            buildInfo.triangleArray.deviceAddress = build.triangleArrayBuffer.deviceAddress();
            buildInfo.triangleArrayStride = sizeof(VkMicromapTriangleEXT);

            build.sizes.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT;
            vkGetMicromapBuildSizesEXT(
                context.device(),
                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                &buildInfo,
                &build.sizes);
            stats.micromapBytes += build.sizes.micromapSize;
            stats.buildScratchBytes = std::max(stats.buildScratchBytes, build.sizes.buildScratchSize);
        }

        builds[meshBuildIndex] = std::move(build);
        ++stats.meshCount;
    }

    if (stats.meshCount == 0 || stats.triangleArrayCount == 0) {
        stats.fallbackReason = "no mixed alpha-tested triangles required micromap storage";
        return builds;
    }
    stats.active = true;
    return builds;
}

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

VkAccelerationStructureMotionInstanceNV makeVkMotionInstance(
    const RayTracingInstanceBuildInput& instance,
    const AccelerationStructure& blas) {
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

    VkAccelerationStructureMotionInstanceNV vkInstance{};
    vkInstance.type = VK_ACCELERATION_STRUCTURE_MOTION_INSTANCE_TYPE_MATRIX_MOTION_NV;
    vkInstance.flags = 0;
    VkAccelerationStructureMatrixMotionInstanceNV& matrix = vkInstance.data.matrixMotionInstance;
    matrix.transformT0 = toVkTransform(instance.previousTransform);
    matrix.transformT1 = toVkTransform(instance.transform);
    matrix.instanceCustomIndex = instance.instanceIndex;
    matrix.mask = mask;
    matrix.instanceShaderBindingTableRecordOffset = 0;
    matrix.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    matrix.accelerationStructureReference = blas.deviceAddress();
    return vkInstance;
}

float transformMaxDelta(const glm::mat4& a, const glm::mat4& b) {
    float maxDelta = 0.0f;
    for (uint32_t col = 0; col < 4; ++col) {
        for (uint32_t row = 0; row < 4; ++row) {
            maxDelta = std::max(maxDelta, std::abs(a[col][row] - b[col][row]));
        }
    }
    return maxDelta;
}

RayTracingMotionInstanceStats collectMotionInstanceStats(
    const GpuScene& scene,
    bool active,
    uint32_t previousRefitCount) {
    RayTracingMotionInstanceStats stats{};
    stats.active = active;
    stats.tlasRefitCount = previousRefitCount;
    for (const RayTracingInstanceBuildInput& instance : scene.rayTracingInstances()) {
        ++stats.instanceCount;
        const float delta = transformMaxDelta(instance.transform, instance.previousTransform);
        stats.maxTransformDelta = std::max(stats.maxTransformDelta, delta);
        if (delta > 0.00001f) {
            ++stats.movingInstanceCount;
        } else {
            ++stats.staticInstanceCount;
        }
    }
    return stats;
}

std::vector<VkAccelerationStructureMotionInstanceNV> buildVkMotionInstances(
    const GpuScene& scene,
    const std::vector<AccelerationStructure>& blases) {
    std::vector<VkAccelerationStructureMotionInstanceNV> instances;
    instances.reserve(scene.rayTracingInstances().size());
    for (const RayTracingInstanceBuildInput& instance : scene.rayTracingInstances()) {
        if (instance.meshIndex >= blases.size() || blases[instance.meshIndex].handle() == VK_NULL_HANDLE) {
            continue;
        }
        instances.push_back(makeVkMotionInstance(instance, blases[instance.meshIndex]));
    }
    return instances;
}

} // namespace

RayTracingScene::RayTracingScene(
    const VulkanContext& context,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    const GpuScene& scene,
    RayTracingSceneBuildOptions options) {
    build(context, allocator, uploader, scene, options);
}

void micromapBuildBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
    barrier.srcAccessMask = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

RayTracingScene::~RayTracingScene() {
    destroyOpacityMicromaps();
}

void RayTracingScene::destroyOpacityMicromaps() {
    if (opacityMicromapDevice_ != VK_NULL_HANDLE) {
        for (VkMicromapEXT micromap : opacityMicromaps_) {
            if (micromap != VK_NULL_HANDLE) {
                vkDestroyMicromapEXT(opacityMicromapDevice_, micromap, nullptr);
            }
        }
    }
    opacityMicromaps_.clear();
    opacityMicromapBuffers_.clear();
    opacityMicromapDevice_ = VK_NULL_HANDLE;
}

void RayTracingScene::build(
    const VulkanContext& context,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    const GpuScene& scene,
    RayTracingSceneBuildOptions options) {
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
    destroyOpacityMicromaps();
    accelerationStructureBytes_ = 0;
    opacityMicromapStats_ = {};
    motionBlurActive_ = options.motionBlurEnabled && context.supportsRayTracingMotionBlur();
    motionInstanceStats_ = {};

    std::vector<MeshOpacityMicromapBuild> opacityMicromapBuilds = prepareOpacityMicromapBuilds(
        context,
        allocator,
        scene,
        options,
        opacityMicromapStats_);
    if (opacityMicromapStats_.active) {
        opacityMicromapDevice_ = device;
        opacityMicromaps_.reserve(opacityMicromapStats_.meshCount);
        opacityMicromapBuffers_.reserve(opacityMicromapStats_.meshCount);
        for (MeshOpacityMicromapBuild& build : opacityMicromapBuilds) {
            if (!build.enabled || build.triangleArray.empty() || build.sizes.micromapSize == 0) {
                continue;
            }
            Buffer storage(allocator, BufferDesc{
                .size = build.sizes.micromapSize,
                .usage = VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT,
                .memory = BufferMemory::GpuOnly,
                .debugName = "opacity micromap storage",
            });
            VkMicromapCreateInfoEXT createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT;
            createInfo.buffer = storage.handle();
            createInfo.size = build.sizes.micromapSize;
            createInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
            checkVk(vkCreateMicromapEXT(device, &createInfo, nullptr, &build.micromap), "vkCreateMicromapEXT");
            allocator.setDebugName(VK_OBJECT_TYPE_MICROMAP_EXT, reinterpret_cast<uint64_t>(build.micromap), "opacity micromap");
            opacityMicromaps_.push_back(build.micromap);
            opacityMicromapBuffers_.push_back(std::move(storage));
        }
        opacityMicromapStats_.micromapCount = static_cast<uint32_t>(opacityMicromaps_.size());
        if (opacityMicromapStats_.micromapCount == 0) {
            opacityMicromapStats_.active = false;
            opacityMicromapStats_.fallbackReason = "micromap storage allocation produced no active micromaps";
        }
    }

    std::vector<uint32_t> geometryTriangleOffsets;
    std::vector<MeshGeometryRangeGpu> meshGeometryRanges;
    std::vector<MeshBlasGeometryPlan> blasGeometryPlans = buildBlasGeometryPlans(
        scene,
        opacityMicromapBuilds,
        opacityMicromapStats_,
        geometryTriangleOffsets,
        meshGeometryRanges,
        blasGeometryStats_);
    geometryTriangleOffsetsBuffer_.create(allocator, BufferDesc{
        .size = sizeof(uint32_t) * geometryTriangleOffsets.size(),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "ray tracing geometry triangle offsets",
    });
    geometryTriangleOffsetsBuffer_.write(
        geometryTriangleOffsets.data(),
        sizeof(uint32_t) * geometryTriangleOffsets.size());
    geometryTriangleOffsetsBuffer_.flush(sizeof(uint32_t) * geometryTriangleOffsets.size());

    if (meshGeometryRanges.empty()) {
        meshGeometryRanges.push_back(MeshGeometryRangeGpu{.offset = 0u, .count = 1u});
    }
    meshGeometryRangesBuffer_.create(allocator, BufferDesc{
        .size = sizeof(MeshGeometryRangeGpu) * meshGeometryRanges.size(),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "ray tracing mesh geometry ranges",
    });
    meshGeometryRangesBuffer_.write(
        meshGeometryRanges.data(),
        sizeof(MeshGeometryRangeGpu) * meshGeometryRanges.size());
    meshGeometryRangesBuffer_.flush(sizeof(MeshGeometryRangeGpu) * meshGeometryRanges.size());

    std::vector<BlasBuildSizes> blasBuildSizes;
    blasBuildSizes.reserve(scene.rayTracingMeshes().size());
    VkDeviceSize maxBuildScratchSize = opacityMicromapStats_.active ? opacityMicromapStats_.buildScratchBytes : 0;
    const auto& rtMeshes = scene.rayTracingMeshes();
    for (uint32_t meshBuildIndex = 0; meshBuildIndex < rtMeshes.size(); ++meshBuildIndex) {
        const RayTracingMeshBuildInput& mesh = rtMeshes[meshBuildIndex];
        if (mesh.meshIndex >= blases_.size() || mesh.vertexCount == 0 || mesh.indexCount < 3) {
            continue;
        }

        const MeshBlasGeometryPlan& plan = meshBuildIndex < blasGeometryPlans.size()
            ? blasGeometryPlans[meshBuildIndex]
            : MeshBlasGeometryPlan{};
        if (plan.ranges.empty()) {
            continue;
        }
        std::vector<VkAccelerationStructureGeometryTrianglesDataKHR> triangleDatas;
        std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> opacityInfos;
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<uint32_t> primitiveCounts;
        triangleDatas.reserve(plan.ranges.size());
        opacityInfos.reserve(plan.ranges.size());
        geometries.reserve(plan.ranges.size());
        primitiveCounts.reserve(plan.ranges.size());

        for (const BlasGeometryRange& range : plan.ranges) {
            VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
            triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            triangles.vertexData.deviceAddress = scene.localVertices().deviceAddress();
            triangles.vertexStride = sizeof(GpuLocalVertex);
            triangles.maxVertex = mesh.firstVertex + mesh.vertexCount - 1u;
            triangles.indexType = VK_INDEX_TYPE_UINT32;
            triangles.indexData.deviceAddress = scene.localIndices().deviceAddress() + sizeof(uint32_t) * range.firstIndex;

            if (range.useOpacityMicromap &&
                meshBuildIndex < opacityMicromapBuilds.size() &&
                opacityMicromapBuilds[meshBuildIndex].micromap != VK_NULL_HANDLE) {
                MeshOpacityMicromapBuild& opacityBuild = opacityMicromapBuilds[meshBuildIndex];
                opacityInfos.push_back(VkAccelerationStructureTrianglesOpacityMicromapEXT{});
                VkAccelerationStructureTrianglesOpacityMicromapEXT& opacityInfo = opacityInfos.back();
                opacityInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
                opacityInfo.indexType = VK_INDEX_TYPE_UINT32;
                opacityInfo.indexBuffer.deviceAddress = opacityBuild.indexBuffer.deviceAddress() +
                    sizeof(uint32_t) * ((range.firstIndex - mesh.firstIndex) / 3u);
                opacityInfo.indexStride = sizeof(uint32_t);
                opacityInfo.baseTriangle = 0;
                opacityInfo.usageCountsCount = 1;
                opacityInfo.pUsageCounts = &opacityBuild.usage;
                opacityInfo.micromap = opacityBuild.micromap;
                triangles.pNext = &opacityInfo;
            }

            triangleDatas.push_back(triangles);
            VkAccelerationStructureGeometryKHR geometry{};
            geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geometry.flags = range.opaqueTraversalSafe ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
            geometry.geometry.triangles = triangleDatas.back();
            geometries.push_back(geometry);
            primitiveCounts.push_back(range.indexCount / 3u);
        }

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        if (std::any_of(plan.ranges.begin(), plan.ranges.end(), [](const BlasGeometryRange& range) { return range.useOpacityMicromap; })) {
            buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_BIT_EXT;
        }
        if (mesh.updateMode == AccelUpdateMode::RefitDeform) {
            buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        }
        buildInfo.geometryCount = static_cast<uint32_t>(geometries.size());
        buildInfo.pGeometries = geometries.data();

        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &buildInfo,
            primitiveCounts.data(),
            &sizes);

        maxBuildScratchSize = std::max(maxBuildScratchSize, sizes.buildScratchSize);
        blasBuildSizes.push_back(BlasBuildSizes{
            .meshBuildIndex = meshBuildIndex,
            .primitiveCounts = primitiveCounts,
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
    if (motionBlurActive_) {
        tlasBuildInfoForSizing.flags |= VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV;
    }
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

    const auto opacityBuildStart = std::chrono::steady_clock::now();
    bool recordedOpacityMicromapBuilds = false;
    if (opacityMicromapStats_.active) {
        for (MeshOpacityMicromapBuild& opacityBuild : opacityMicromapBuilds) {
            if (!opacityBuild.enabled || opacityBuild.triangleArray.empty() || opacityBuild.micromap == VK_NULL_HANDLE) {
                continue;
            }
            VkMicromapBuildInfoEXT micromapBuildInfo{};
            micromapBuildInfo.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
            micromapBuildInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
            micromapBuildInfo.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
            micromapBuildInfo.dstMicromap = opacityBuild.micromap;
            micromapBuildInfo.usageCountsCount = 1;
            micromapBuildInfo.pUsageCounts = &opacityBuild.usage;
            micromapBuildInfo.data.deviceAddress = opacityBuild.dataBuffer.deviceAddress();
            micromapBuildInfo.scratchData.deviceAddress = scratchAddress;
            micromapBuildInfo.triangleArray.deviceAddress = opacityBuild.triangleArrayBuffer.deviceAddress();
            micromapBuildInfo.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
            vkCmdBuildMicromapsEXT(cmd, 1, &micromapBuildInfo);
            recordedOpacityMicromapBuilds = true;
        }
        if (recordedOpacityMicromapBuilds) {
            micromapBuildBarrier(cmd);
        }
    }

    for (const BlasBuildSizes& record : blasBuildSizes) {
        const RayTracingMeshBuildInput& mesh = rtMeshes[record.meshBuildIndex];
        const MeshBlasGeometryPlan& plan = blasGeometryPlans[record.meshBuildIndex];
        std::vector<VkAccelerationStructureGeometryTrianglesDataKHR> triangleDatas;
        std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> opacityInfos;
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        triangleDatas.reserve(plan.ranges.size());
        opacityInfos.reserve(plan.ranges.size());
        geometries.reserve(plan.ranges.size());
        ranges.reserve(plan.ranges.size());

        for (const BlasGeometryRange& geometryRange : plan.ranges) {
            VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
            triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            triangles.vertexData.deviceAddress = scene.localVertices().deviceAddress();
            triangles.vertexStride = sizeof(GpuLocalVertex);
            triangles.maxVertex = mesh.firstVertex + mesh.vertexCount - 1u;
            triangles.indexType = VK_INDEX_TYPE_UINT32;
            triangles.indexData.deviceAddress = scene.localIndices().deviceAddress() + sizeof(uint32_t) * geometryRange.firstIndex;

            if (geometryRange.useOpacityMicromap &&
                record.meshBuildIndex < opacityMicromapBuilds.size() &&
                opacityMicromapBuilds[record.meshBuildIndex].micromap != VK_NULL_HANDLE) {
                MeshOpacityMicromapBuild& opacityBuild = opacityMicromapBuilds[record.meshBuildIndex];
                opacityInfos.push_back(VkAccelerationStructureTrianglesOpacityMicromapEXT{});
                VkAccelerationStructureTrianglesOpacityMicromapEXT& opacityInfo = opacityInfos.back();
                opacityInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
                opacityInfo.indexType = VK_INDEX_TYPE_UINT32;
                opacityInfo.indexBuffer.deviceAddress = opacityBuild.indexBuffer.deviceAddress() +
                    sizeof(uint32_t) * ((geometryRange.firstIndex - mesh.firstIndex) / 3u);
                opacityInfo.indexStride = sizeof(uint32_t);
                opacityInfo.baseTriangle = 0;
                opacityInfo.usageCountsCount = 1;
                opacityInfo.pUsageCounts = &opacityBuild.usage;
                opacityInfo.micromap = opacityBuild.micromap;
                triangles.pNext = &opacityInfo;
            }

            triangleDatas.push_back(triangles);
            VkAccelerationStructureGeometryKHR geometry{};
            geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geometry.flags = geometryRange.opaqueTraversalSafe ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
            geometry.geometry.triangles = triangleDatas.back();
            geometries.push_back(geometry);

            VkAccelerationStructureBuildRangeInfoKHR range{};
            range.primitiveCount = geometryRange.indexCount / 3u;
            range.primitiveOffset = 0;
            range.firstVertex = 0;
            range.transformOffset = 0;
            ranges.push_back(range);
        }

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        if (std::any_of(plan.ranges.begin(), plan.ranges.end(), [](const BlasGeometryRange& range) { return range.useOpacityMicromap; })) {
            buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_BIT_EXT;
        }
        const bool allowBlasUpdate = mesh.updateMode == AccelUpdateMode::RefitDeform;
        if (allowBlasUpdate) {
            buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        }
        buildInfo.geometryCount = static_cast<uint32_t>(geometries.size());
        buildInfo.pGeometries = geometries.data();

        blases_[mesh.meshIndex].create(device, allocator, AccelerationStructureDesc{
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .size = record.sizes.accelerationStructureSize,
            .allowUpdate = allowBlasUpdate,
            .debugName = "ray tracing BLAS",
        });
        accelerationStructureBytes_ += record.sizes.accelerationStructureSize;

        buildInfo.dstAccelerationStructure = blases_[mesh.meshIndex].handle();
        buildInfo.scratchData.deviceAddress = scratchAddress;

        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePtrs;
        rangePtrs.reserve(ranges.size());
        for (const VkAccelerationStructureBuildRangeInfoKHR& range : ranges) {
            rangePtrs.push_back(&range);
        }
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, rangePtrs.data());
        accelerationBuildBarrier(cmd);
    }

    const std::vector<VkAccelerationStructureInstanceKHR> instances = motionBlurActive_ ? std::vector<VkAccelerationStructureInstanceKHR>{} : buildVkInstances(scene, blases_);
    const std::vector<VkAccelerationStructureMotionInstanceNV> motionInstances = motionBlurActive_ ? buildVkMotionInstances(scene, blases_) : std::vector<VkAccelerationStructureMotionInstanceNV>{};
    const uint32_t builtInstanceCount = static_cast<uint32_t>(motionBlurActive_ ? motionInstances.size() : instances.size());
    if (builtInstanceCount == 0) {
        throw std::runtime_error("Cannot build ray tracing TLAS: no valid instances");
    }
    instanceCount_ = builtInstanceCount;
    motionInstanceStats_ = collectMotionInstanceStats(scene, motionBlurActive_, 0u);

    const VkDeviceSize instanceBytes = motionBlurActive_
        ? sizeof(VkAccelerationStructureMotionInstanceNV) * motionInstances.size()
        : sizeof(VkAccelerationStructureInstanceKHR) * instances.size();

    instanceBuffer_.create(allocator, BufferDesc{
        .size = instanceBytes,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory = BufferMemory::Upload,
        .persistentMapped = true,
        .debugName = "ray tracing TLAS instances",
    });
    instanceBuffer_.write(motionBlurActive_ ? static_cast<const void*>(motionInstances.data()) : static_cast<const void*>(instances.data()), instanceBytes);
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
    if (motionBlurActive_) {
        buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV;
    }
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    tlas_.create(device, allocator, AccelerationStructureDesc{
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .size = tlasSizes.accelerationStructureSize,
        .createFlags = motionBlurActive_ ? VK_ACCELERATION_STRUCTURE_CREATE_MOTION_BIT_NV : 0u,
        .motionMaxInstances = motionBlurActive_ ? instanceCount_ : 0u,
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
    if (recordedOpacityMicromapBuilds) {
        const auto opacityBuildEnd = std::chrono::steady_clock::now();
        opacityMicromapStats_.buildMs = std::chrono::duration<float, std::milli>(opacityBuildEnd - opacityBuildStart).count();
    }

    std::cout << "RT scene: BLAS=" << blases_.size()
              << " instances=" << instanceCount_
              << " geometries=" << blasGeometryStats_.geometryCount
              << " AS memory=" << (static_cast<double>(accelerationStructureBytes_) / (1024.0 * 1024.0)) << " MB";
    if (motionBlurActive_) {
        std::cout << " motion_blur=active";
    }
    if (opacityMicromapStats_.requested) {
        std::cout << " OMM=" << (opacityMicromapStats_.active ? "active" : "fallback")
                  << " micromaps=" << opacityMicromapStats_.micromapCount
                  << " storage=" << (static_cast<double>(opacityMicromapStats_.micromapBytes) / 1024.0) << " KiB";
        if (!opacityMicromapStats_.fallbackReason.empty()) {
            std::cout << " reason=" << opacityMicromapStats_.fallbackReason;
        }
    }
    std::cout << '\n';
}

bool RayTracingScene::refitTransforms(
    const VulkanContext& context,
    ResourceAllocator& allocator,
    BufferUploader& uploader,
    const GpuScene& scene) {
    if (!context.supportsHardwareRayTracing() || tlas_.handle() == VK_NULL_HANDLE || !tlas_.allowUpdate()) {
        return false;
    }

    const std::vector<VkAccelerationStructureInstanceKHR> instances = motionBlurActive_ ? std::vector<VkAccelerationStructureInstanceKHR>{} : buildVkInstances(scene, blases_);
    const std::vector<VkAccelerationStructureMotionInstanceNV> motionInstances = motionBlurActive_ ? buildVkMotionInstances(scene, blases_) : std::vector<VkAccelerationStructureMotionInstanceNV>{};
    const uint32_t builtInstanceCount = static_cast<uint32_t>(motionBlurActive_ ? motionInstances.size() : instances.size());
    if (builtInstanceCount == 0 || builtInstanceCount != instanceCount_) {
        return false;
    }
    const VkDeviceSize instanceBytes = motionBlurActive_
        ? sizeof(VkAccelerationStructureMotionInstanceNV) * motionInstances.size()
        : sizeof(VkAccelerationStructureInstanceKHR) * instances.size();
    if (instanceBuffer_.handle() == VK_NULL_HANDLE ||
        instanceBuffer_.size() < instanceBytes) {
        return false;
    }

    instanceBuffer_.write(motionBlurActive_ ? static_cast<const void*>(motionInstances.data()) : static_cast<const void*>(instances.data()), instanceBytes);
    instanceBuffer_.flush(instanceBytes);

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
    if (motionBlurActive_) {
        buildInfo.flags |= VK_BUILD_ACCELERATION_STRUCTURE_MOTION_BIT_NV;
    }
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
    motionInstanceStats_ = collectMotionInstanceStats(
        scene,
        motionBlurActive_,
        motionInstanceStats_.tlasRefitCount + 1u);
    return true;
}

} // namespace rtv
