#pragma once

#include "rtv/AccelerationStructure.h"
#include "rtv/Buffer.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <string>
#include <vector>

namespace rtv {

class BufferUploader;
class GpuScene;
class ResourceAllocator;
class VulkanContext;

struct RayTracingSceneBuildOptions {
    bool opacityMicromapsEnabled = false;
    bool motionBlurEnabled = false;
};

struct OpacityMicromapBuildStats {
    bool requested = false;
    bool supported = false;
    bool active = false;
    uint32_t micromapCount = 0;
    uint32_t meshCount = 0;
    uint32_t triangleArrayCount = 0;
    uint32_t indexedTriangleCount = 0;
    uint32_t packedMicroTriangleCount = 0;
    VkDeviceSize micromapBytes = 0;
    VkDeviceSize buildInputBytes = 0;
    VkDeviceSize buildScratchBytes = 0;
    float buildMs = 0.0f;
    std::string fallbackReason;
};

struct RayTracingBlasGeometryStats {
    uint32_t geometryCount = 0;
    uint32_t opaqueGeometryCount = 0;
    uint32_t alphaTestedGeometryCount = 0;
    uint32_t blendedGeometryCount = 0;
    uint32_t opacityMicromapGeometryCount = 0;
};

struct RayTracingMotionInstanceStats {
    bool active = false;
    uint32_t instanceCount = 0;
    uint32_t movingInstanceCount = 0;
    uint32_t staticInstanceCount = 0;
    uint32_t tlasRefitCount = 0;
    float maxTransformDelta = 0.0f;
};

class RayTracingScene final : private NonCopyable {
public:
    RayTracingScene(
        const VulkanContext& context,
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        const GpuScene& scene,
        RayTracingSceneBuildOptions options = {});
    ~RayTracingScene();

    [[nodiscard]] VkAccelerationStructureKHR tlas() const { return tlas_.handle(); }
    [[nodiscard]] uint32_t blasCount() const { return static_cast<uint32_t>(blases_.size()); }
    [[nodiscard]] uint32_t instanceCount() const { return instanceCount_; }
    [[nodiscard]] VkDeviceSize accelerationStructureBytes() const { return accelerationStructureBytes_; }
    [[nodiscard]] const OpacityMicromapBuildStats& opacityMicromapStats() const { return opacityMicromapStats_; }
    [[nodiscard]] const RayTracingBlasGeometryStats& blasGeometryStats() const { return blasGeometryStats_; }
    [[nodiscard]] const RayTracingMotionInstanceStats& motionInstanceStats() const { return motionInstanceStats_; }
    [[nodiscard]] const Buffer& geometryTriangleOffsetsBuffer() const { return geometryTriangleOffsetsBuffer_; }
    [[nodiscard]] const Buffer& meshGeometryRangesBuffer() const { return meshGeometryRangesBuffer_; }
    [[nodiscard]] float lastTlasRefitMs() const { return lastTlasRefitMs_; }
    [[nodiscard]] bool motionBlurActive() const { return motionBlurActive_; }
    [[nodiscard]] bool refitTransforms(
        const VulkanContext& context,
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        const GpuScene& scene);

private:
    void build(
        const VulkanContext& context,
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        const GpuScene& scene,
        RayTracingSceneBuildOptions options);
    void destroyOpacityMicromaps();

    std::vector<AccelerationStructure> blases_;
    AccelerationStructure tlas_;
    Buffer instanceBuffer_;
    Buffer tlasRefitScratch_;
    Buffer geometryTriangleOffsetsBuffer_;
    Buffer meshGeometryRangesBuffer_;
    std::vector<VkMicromapEXT> opacityMicromaps_;
    std::vector<Buffer> opacityMicromapBuffers_;
    VkDevice opacityMicromapDevice_ = VK_NULL_HANDLE;
    uint32_t instanceCount_ = 0;
    VkDeviceSize accelerationStructureBytes_ = 0;
    VkDeviceSize tlasUpdateScratchSize_ = 0;
    bool motionBlurActive_ = false;
    OpacityMicromapBuildStats opacityMicromapStats_{};
    RayTracingBlasGeometryStats blasGeometryStats_{};
    RayTracingMotionInstanceStats motionInstanceStats_{};
    float lastTlasRefitMs_ = 0.0f;
};

} // namespace rtv
