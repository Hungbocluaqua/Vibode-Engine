#pragma once

#include <Volk/volk.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace rtv {

class Image;

struct BindlessCapabilities {
    bool descriptorIndexing = false;
    bool runtimeDescriptorArray = false;
    bool partiallyBound = false;
    bool updateAfterBind = false;
    uint32_t maxSampledImages = 0;
};

[[nodiscard]] BindlessCapabilities queryBindlessCapabilities(VkPhysicalDevice physicalDevice);

struct BindlessMaterialReference {
    uint32_t baseColorTexture = UINT32_MAX;
    uint32_t normalTexture = UINT32_MAX;
    uint32_t metallicRoughnessTexture = UINT32_MAX;
    uint32_t emissiveTexture = UINT32_MAX;
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
    void clear();

    [[nodiscard]] const std::vector<VkDescriptorImageInfo>& descriptors() const { return descriptors_; }
    [[nodiscard]] uint32_t residentCount() const { return static_cast<uint32_t>(images_.size()); }
    [[nodiscard]] uint32_t slotCount() const { return slotCount_; }

private:
    void rebuildDescriptors();

    std::vector<std::unique_ptr<Image>> images_;
    std::vector<VkDescriptorImageInfo> descriptors_;
    uint32_t slotCount_ = 0;
};

} // namespace rtv
