#pragma once

#include "rtv/Buffer.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

namespace rtv {

class ResourceAllocator;

struct AccelerationStructureDesc {
    VkAccelerationStructureTypeKHR type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkDeviceSize size = 0;
    const char* debugName = nullptr;
};

class AccelerationStructure final : private NonCopyable {
public:
    AccelerationStructure() = default;
    AccelerationStructure(VkDevice device, ResourceAllocator& allocator, const AccelerationStructureDesc& desc);
    ~AccelerationStructure();

    AccelerationStructure(AccelerationStructure&& other) noexcept;
    AccelerationStructure& operator=(AccelerationStructure&& other) noexcept;

    void create(VkDevice device, ResourceAllocator& allocator, const AccelerationStructureDesc& desc);
    void destroy();

    [[nodiscard]] VkAccelerationStructureKHR handle() const { return handle_; }
    [[nodiscard]] VkDeviceAddress deviceAddress() const { return deviceAddress_; }
    [[nodiscard]] VkDeviceSize size() const { return size_; }
    [[nodiscard]] VkAccelerationStructureTypeKHR type() const { return type_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkAccelerationStructureKHR handle_ = VK_NULL_HANDLE;
    Buffer buffer_;
    VkDeviceAddress deviceAddress_ = 0;
    VkDeviceSize size_ = 0;
    VkAccelerationStructureTypeKHR type_ = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
};

} // namespace rtv
