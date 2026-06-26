#pragma once

#include "rtv/FreeListAllocator.h"

#include <Volk/volk.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rtv {

class Image;
class Buffer;

struct BindlessCapabilities {
    bool descriptorIndexing = false;
    bool runtimeDescriptorArray = false;
    bool partiallyBound = false;
    bool updateAfterBind = false;
    uint32_t maxSampledImages = 0;
};

[[nodiscard]] BindlessCapabilities queryBindlessCapabilities(VkPhysicalDevice physicalDevice);

[[nodiscard]] bool supportsFullBindlessTextures(const BindlessCapabilities& caps);
[[nodiscard]] uint32_t fullBindlessTextureCapacityOrThrow(const BindlessCapabilities& caps);

[[nodiscard]] constexpr uint32_t maxMaterialTextureSlots(const BindlessCapabilities& caps) {
    constexpr uint32_t kDefaultLimit = 128;
    constexpr uint32_t kReasonableCap = 65536;
    if (!caps.descriptorIndexing || caps.maxSampledImages == 0) {
        return kDefaultLimit;
    }
    return caps.maxSampledImages > kReasonableCap ? kReasonableCap : caps.maxSampledImages;
}

struct BindlessTextureHeapStats {
    uint32_t capacity = 0;
    uint32_t descriptorCount = 0;
    uint32_t patchCount = 0;
    bool initialized = false;
};

class BindlessTextureHeap {
public:
    BindlessTextureHeap() = default;
    ~BindlessTextureHeap();

    BindlessTextureHeap(const BindlessTextureHeap&) = delete;
    BindlessTextureHeap& operator=(const BindlessTextureHeap&) = delete;

    void init(VkDevice device, const BindlessCapabilities& caps, uint32_t capacity);
    void destroy();
    void updateAll(const std::vector<VkDescriptorImageInfo>& descriptors);
    void patch(uint32_t slot, const VkDescriptorImageInfo& descriptor);

    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_; }
    [[nodiscard]] VkDescriptorSet descriptorSet() const { return descriptorSet_; }
    [[nodiscard]] uint32_t capacity() const { return capacity_; }
    [[nodiscard]] uint32_t patchCount() const { return patchCount_; }
    [[nodiscard]] bool initialized() const { return descriptorSet_ != VK_NULL_HANDLE; }
    [[nodiscard]] BindlessTextureHeapStats stats() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    uint32_t capacity_ = 0;
    uint32_t descriptorCount_ = 0;
    uint32_t patchCount_ = 0;
};

struct TextureHandle { uint32_t index = UINT32_MAX; [[nodiscard]] bool valid() const { return index != UINT32_MAX; } };
struct MaterialHandle { uint32_t index = UINT32_MAX; [[nodiscard]] bool valid() const { return index != UINT32_MAX; } };
struct MeshHandle { uint32_t index = UINT32_MAX; [[nodiscard]] bool valid() const { return index != UINT32_MAX; } };

struct BindlessMaterialReference {
    uint32_t baseColorTexture = UINT32_MAX;
    uint32_t normalTexture = UINT32_MAX;
    uint32_t metallicRoughnessTexture = UINT32_MAX;
    uint32_t emissiveTexture = UINT32_MAX;
};

struct BindlessTextureRegistrationInfo {
    uint32_t slot = UINT32_MAX;
    std::string guid;
    std::string nativeSource;
    std::filesystem::path nativePath;
    bool resident = false;
    bool fallback = false;
    bool missing = false;
};

struct BindlessTextureImageInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

class BindlessTextureTable {
public:
    BindlessTextureTable() = default;
    ~BindlessTextureTable();

    BindlessTextureTable(const BindlessTextureTable&) = delete;
    BindlessTextureTable& operator=(const BindlessTextureTable&) = delete;
    BindlessTextureTable(BindlessTextureTable&&) noexcept;
    BindlessTextureTable& operator=(BindlessTextureTable&&) noexcept;

    void setImages(std::vector<std::unique_ptr<Image>> images, uint32_t slotCount);
    void setRegistrationInfo(std::vector<BindlessTextureRegistrationInfo> registrations);
    void clear();

    [[nodiscard]] const std::vector<VkDescriptorImageInfo>& descriptors() const { return descriptors_; }
    [[nodiscard]] uint32_t residentCount() const { return static_cast<uint32_t>(images_.size()); }
    [[nodiscard]] uint32_t slotCount() const { return slotCount_; }
    [[nodiscard]] VkImageView imageView(uint32_t index) const;
    [[nodiscard]] BindlessTextureImageInfo imageInfo(uint32_t index) const;

    [[nodiscard]] const FreeListAllocator<uint32_t>& allocator() const { return allocator_; }
    [[nodiscard]] float fragmentation() const { return allocator_.fragmentationRatio(); }
    [[nodiscard]] uint32_t allocatedCount() const { return allocator_.allocatedCount(); }
    [[nodiscard]] const std::vector<BindlessTextureRegistrationInfo>& registrations() const { return registrations_; }

private:
    void rebuildDescriptors();
    void rebuildAllocator();

    std::vector<std::unique_ptr<Image>> images_;
    std::vector<VkDescriptorImageInfo> descriptors_;
    std::vector<BindlessTextureRegistrationInfo> registrations_;
    uint32_t slotCount_ = 0;
    FreeListAllocator<uint32_t> allocator_{0};
};

class BindlessMaterialTable {
public:
    BindlessMaterialTable() = default;

    BindlessMaterialTable(const BindlessMaterialTable&) = delete;
    BindlessMaterialTable& operator=(const BindlessMaterialTable&) = delete;

    void init(uint32_t maxMaterials, Buffer& gpuBuffer);
    MaterialHandle registerMaterial(const void* materialData);
    void updateMaterial(MaterialHandle handle, const void* materialData);
    void removeMaterial(MaterialHandle handle);
    void clear();

    [[nodiscard]] Buffer* buffer() const { return gpuBuffer_; }
    [[nodiscard]] uint32_t count() const { return allocator_.allocatedCount(); }
    [[nodiscard]] uint32_t capacity() const { return allocator_.capacity(); }
    [[nodiscard]] float fragmentation() const { return allocator_.fragmentationRatio(); }

    [[nodiscard]] const FreeListAllocator<uint32_t>& allocator() const { return allocator_; }

private:
    static constexpr uint32_t kMaterialStride = 32;
    Buffer* gpuBuffer_ = nullptr;
    FreeListAllocator<uint32_t> allocator_{0};
};

class BindlessMeshTable {
public:
    BindlessMeshTable() = default;

    BindlessMeshTable(const BindlessMeshTable&) = delete;
    BindlessMeshTable& operator=(const BindlessMeshTable&) = delete;

    void init(uint32_t maxMeshes, Buffer& gpuBuffer);
    MeshHandle registerMesh(const void* meshData, uint32_t stride);
    void updateMesh(MeshHandle handle, const void* meshData, uint32_t stride);
    void removeMesh(MeshHandle handle);
    void clear();

    [[nodiscard]] Buffer* buffer() const { return gpuBuffer_; }
    [[nodiscard]] uint32_t count() const { return allocator_.allocatedCount(); }
    [[nodiscard]] uint32_t capacity() const { return allocator_.capacity(); }
    [[nodiscard]] float fragmentation() const { return allocator_.fragmentationRatio(); }

    [[nodiscard]] const FreeListAllocator<uint32_t>& allocator() const { return allocator_; }

private:
    static constexpr uint32_t kMeshStride = 64;
    Buffer* gpuBuffer_ = nullptr;
    FreeListAllocator<uint32_t> allocator_{0};
};

} // namespace rtv
