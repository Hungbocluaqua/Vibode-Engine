#include "rtv/BindlessResources.h"

#include "rtv/Image.h"

#include <algorithm>
#include <utility>

namespace rtv {

BindlessCapabilities queryBindlessCapabilities(VkPhysicalDevice physicalDevice) {
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
    indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &indexing;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

    VkPhysicalDeviceDescriptorIndexingProperties indexingProps{};
    indexingProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &indexingProps;
    vkGetPhysicalDeviceProperties2(physicalDevice, &props);

    return {
        .descriptorIndexing = indexing.shaderSampledImageArrayNonUniformIndexing == VK_TRUE,
        .runtimeDescriptorArray = indexing.runtimeDescriptorArray == VK_TRUE,
        .partiallyBound = indexing.descriptorBindingPartiallyBound == VK_TRUE,
        .updateAfterBind = indexing.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE,
        .maxSampledImages = indexingProps.maxDescriptorSetUpdateAfterBindSampledImages,
    };
}

BindlessTextureTable::~BindlessTextureTable() = default;

BindlessTextureTable::BindlessTextureTable(BindlessTextureTable&& other) noexcept {
    *this = std::move(other);
}

BindlessTextureTable& BindlessTextureTable::operator=(BindlessTextureTable&& other) noexcept {
    if (this != &other) {
        images_ = std::move(other.images_);
        descriptors_ = std::move(other.descriptors_);
        slotCount_ = other.slotCount_;
        other.slotCount_ = 0;
    }
    return *this;
}

void BindlessTextureTable::setImages(std::vector<std::unique_ptr<Image>> images, uint32_t slotCount) {
    images_ = std::move(images);
    slotCount_ = std::max(slotCount, 1u);
    rebuildDescriptors();
}

void BindlessTextureTable::clear() {
    images_.clear();
    descriptors_.clear();
    slotCount_ = 0;
}

void BindlessTextureTable::rebuildDescriptors() {
    descriptors_.clear();
    if (images_.empty() || slotCount_ == 0) {
        return;
    }
    descriptors_.reserve(slotCount_);
    for (uint32_t slot = 0; slot < slotCount_; ++slot) {
        const uint32_t imageIndex = slot < images_.size() ? slot : 0u;
        descriptors_.push_back(images_[imageIndex]->sampledDescriptor(VK_NULL_HANDLE));
    }
}

} // namespace rtv
