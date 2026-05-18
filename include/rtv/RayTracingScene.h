#pragma once

#include "rtv/AccelerationStructure.h"
#include "rtv/Buffer.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <vector>

namespace rtv {

class BufferUploader;
class GpuScene;
class ResourceAllocator;
class VulkanContext;

class RayTracingScene final : private NonCopyable {
public:
    RayTracingScene(
        const VulkanContext& context,
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        const GpuScene& scene);

    [[nodiscard]] VkAccelerationStructureKHR tlas() const { return tlas_.handle(); }
    [[nodiscard]] uint32_t blasCount() const { return static_cast<uint32_t>(blases_.size()); }
    [[nodiscard]] uint32_t instanceCount() const { return instanceCount_; }
    [[nodiscard]] VkDeviceSize accelerationStructureBytes() const { return accelerationStructureBytes_; }

private:
    void build(const VulkanContext& context, ResourceAllocator& allocator, BufferUploader& uploader, const GpuScene& scene);

    std::vector<AccelerationStructure> blases_;
    AccelerationStructure tlas_;
    Buffer instanceBuffer_;
    uint32_t instanceCount_ = 0;
    VkDeviceSize accelerationStructureBytes_ = 0;
};

} // namespace rtv
