#include "rtv/Buffer.h"

#include "rtv/Check.h"
#include "rtv/ResourceAllocator.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace rtv {

namespace {

VmaMemoryUsage memoryUsage(BufferMemory memory) {
    switch (memory) {
    case BufferMemory::GpuOnly:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case BufferMemory::Upload:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    case BufferMemory::Readback:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }
    return VMA_MEMORY_USAGE_AUTO;
}

VmaAllocationCreateFlags allocationFlags(const BufferDesc& desc) {
    VmaAllocationCreateFlags flags = 0;
    if (desc.persistentMapped) {
        flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    if (desc.memory == BufferMemory::Upload) {
        flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }
    if (desc.memory == BufferMemory::Readback) {
        flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }
    return flags;
}

} // namespace

Buffer::Buffer(ResourceAllocator& allocator, const BufferDesc& desc) {
    create(allocator, desc);
}

Buffer::~Buffer() {
    destroy();
}

Buffer::Buffer(Buffer&& other) noexcept {
    *this = std::move(other);
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        allocator_ = other.allocator_;
        desc_ = other.desc_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;
        mappedData_ = other.mappedData_;
        other.allocator_ = nullptr;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.mappedData_ = nullptr;
        other.desc_ = {};
    }
    return *this;
}

void Buffer::create(ResourceAllocator& allocator, const BufferDesc& desc) {
    if (desc.size == 0) {
        throw std::runtime_error("Buffer size must be greater than zero");
    }

    destroy();
    allocator_ = &allocator;
    desc_ = desc;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = desc.usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = memoryUsage(desc.memory);
    allocationInfo.flags = allocationFlags(desc);

    VmaAllocationInfo createdInfo{};
    checkVk(vmaCreateBuffer(allocator.handle(), &bufferInfo, &allocationInfo, &buffer_, &allocation_, &createdInfo), "vmaCreateBuffer");
    mappedData_ = createdInfo.pMappedData;

    allocator.setDebugName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(buffer_), desc.debugName);
}

void Buffer::destroy() {
    if (buffer_ != VK_NULL_HANDLE && allocator_ != nullptr) {
        vmaDestroyBuffer(allocator_->handle(), buffer_, allocation_);
    }
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    mappedData_ = nullptr;
    allocator_ = nullptr;
    desc_ = {};
}

void Buffer::resize(VkDeviceSize newSize) {
    if (allocator_ == nullptr) {
        throw std::runtime_error("Cannot resize an uninitialized buffer");
    }
    BufferDesc next = desc_;
    next.size = std::max<VkDeviceSize>(newSize, 1);
    ResourceAllocator& allocator = *allocator_;
    create(allocator, next);
}

VkDescriptorBufferInfo Buffer::descriptorInfo(VkDeviceSize offset, VkDeviceSize range) const {
    VkDescriptorBufferInfo info{};
    info.buffer = buffer_;
    info.offset = offset;
    info.range = range;
    return info;
}

bool Buffer::supportsDeviceAddress() const {
    return buffer_ != VK_NULL_HANDLE &&
        allocator_ != nullptr &&
        allocator_->supportsDeviceAddress() &&
        (desc_.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
}

VkDeviceAddress Buffer::deviceAddress() const {
    if (!supportsDeviceAddress()) {
        throw std::runtime_error("Buffer was not created with shader device address support");
    }
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer_;
    return vkGetBufferDeviceAddress(allocator_->device(), &info);
}

void Buffer::write(const void* data, VkDeviceSize byteSize, VkDeviceSize offset) {
    if (mappedData_ == nullptr) {
        throw std::runtime_error("Buffer is not persistently mapped");
    }
    if (offset + byteSize > desc_.size) {
        throw std::runtime_error("Mapped buffer write exceeds buffer size");
    }
    std::memcpy(static_cast<std::byte*>(mappedData_) + offset, data, static_cast<size_t>(byteSize));
}

void Buffer::flush(VkDeviceSize byteSize, VkDeviceSize offset) const {
    if (allocation_ != VK_NULL_HANDLE && allocator_ != nullptr) {
        checkVk(vmaFlushAllocation(allocator_->handle(), allocation_, offset, byteSize), "vmaFlushAllocation");
    }
}

void Buffer::invalidate(VkDeviceSize byteSize, VkDeviceSize offset) const {
    if (allocation_ != VK_NULL_HANDLE && allocator_ != nullptr) {
        checkVk(vmaInvalidateAllocation(allocator_->handle(), allocation_, offset, byteSize), "vmaInvalidateAllocation");
    }
}

VkDeviceSize Buffer::alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment == 0) {
        return value;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace rtv
