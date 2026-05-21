#include "rtv/AccelerationStructure.h"

#include "rtv/Check.h"
#include "rtv/ResourceAllocator.h"

#include <stdexcept>
#include <utility>

namespace rtv {

AccelerationStructure::AccelerationStructure(VkDevice device, ResourceAllocator& allocator, const AccelerationStructureDesc& desc) {
    create(device, allocator, desc);
}

AccelerationStructure::~AccelerationStructure() {
    destroy();
}

AccelerationStructure::AccelerationStructure(AccelerationStructure&& other) noexcept {
    *this = std::move(other);
}

AccelerationStructure& AccelerationStructure::operator=(AccelerationStructure&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        handle_ = other.handle_;
        buffer_ = std::move(other.buffer_);
        deviceAddress_ = other.deviceAddress_;
        size_ = other.size_;
        type_ = other.type_;
        allowUpdate_ = other.allowUpdate_;
        other.device_ = VK_NULL_HANDLE;
        other.handle_ = VK_NULL_HANDLE;
        other.deviceAddress_ = 0;
        other.size_ = 0;
        other.allowUpdate_ = false;
    }
    return *this;
}

void AccelerationStructure::create(VkDevice device, ResourceAllocator& allocator, const AccelerationStructureDesc& desc) {
    if (desc.size == 0) {
        throw std::runtime_error("Acceleration structure size must be greater than zero");
    }
    destroy();
    device_ = device;
    type_ = desc.type;
    size_ = desc.size;
    allowUpdate_ = desc.allowUpdate;

    buffer_.create(allocator, BufferDesc{
        .size = desc.size,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = desc.debugName,
    });

    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = buffer_.handle();
    createInfo.size = desc.size;
    createInfo.type = desc.type;
    checkVk(vkCreateAccelerationStructureKHR(device_, &createInfo, nullptr, &handle_), "vkCreateAccelerationStructureKHR");

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = handle_;
    deviceAddress_ = vkGetAccelerationStructureDeviceAddressKHR(device_, &addressInfo);

    allocator.setDebugName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<uint64_t>(handle_), desc.debugName);
}

void AccelerationStructure::destroy() {
    if (handle_ != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device_, handle_, nullptr);
    }
    handle_ = VK_NULL_HANDLE;
    buffer_.destroy();
    deviceAddress_ = 0;
    size_ = 0;
    device_ = VK_NULL_HANDLE;
    allowUpdate_ = false;
}

} // namespace rtv
