#pragma once

#include <Volk/volk.h>
#include <vk_mem_alloc.h>

#include <cstddef>
#include <cstdint>

namespace rtv {

class ResourceAllocator;

enum class BufferMemory {
    GpuOnly,
    Upload,
    Readback,
};

struct BufferDesc {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    BufferMemory memory = BufferMemory::GpuOnly;
    bool persistentMapped = false;
    const char* debugName = nullptr;
};

class Buffer {
public:
    Buffer() = default;
    Buffer(ResourceAllocator& allocator, const BufferDesc& desc);
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    void create(ResourceAllocator& allocator, const BufferDesc& desc);
    void destroy();
    void resize(VkDeviceSize newSize);

    [[nodiscard]] VkBuffer handle() const { return buffer_; }
    [[nodiscard]] VmaAllocation allocation() const { return allocation_; }
    [[nodiscard]] VkDeviceSize size() const { return desc_.size; }
    [[nodiscard]] VkBufferUsageFlags usage() const { return desc_.usage; }
    [[nodiscard]] void* mappedData() const { return mappedData_; }
    [[nodiscard]] bool mapped() const { return mappedData_ != nullptr; }
    [[nodiscard]] VkDescriptorBufferInfo descriptorInfo(VkDeviceSize offset = 0, VkDeviceSize range = VK_WHOLE_SIZE) const;
    [[nodiscard]] bool supportsDeviceAddress() const;
    [[nodiscard]] VkDeviceAddress deviceAddress() const;

    void write(const void* data, VkDeviceSize byteSize, VkDeviceSize offset = 0);
    void flush(VkDeviceSize byteSize = VK_WHOLE_SIZE, VkDeviceSize offset = 0) const;
    void invalidate(VkDeviceSize byteSize = VK_WHOLE_SIZE, VkDeviceSize offset = 0) const;

    [[nodiscard]] static VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment);

private:
    ResourceAllocator* allocator_ = nullptr;
    BufferDesc desc_{};
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    void* mappedData_ = nullptr;
};

} // namespace rtv
