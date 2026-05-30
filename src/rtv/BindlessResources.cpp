#include "rtv/BindlessResources.h"

#include "rtv/Buffer.h"
#include "rtv/Image.h"

#include <algorithm>
#include <cstring>
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
        allocator_ = std::move(other.allocator_);
        other.slotCount_ = 0;
    }
    return *this;
}

void BindlessTextureTable::setImages(std::vector<std::unique_ptr<Image>> images, uint32_t slotCount) {
    images_ = std::move(images);
    slotCount_ = std::max(slotCount, 1u);
    rebuildAllocator();
    rebuildDescriptors();
}

void BindlessTextureTable::clear() {
    images_.clear();
    descriptors_.clear();
    slotCount_ = 0;
    allocator_.clear();
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

void BindlessTextureTable::rebuildAllocator() {
    allocator_ = FreeListAllocator<uint32_t>(slotCount_);
}

VkImageView BindlessTextureTable::imageView(uint32_t index) const {
    if (index >= images_.size() || images_[index] == nullptr) {
        return VK_NULL_HANDLE;
    }
    return images_[index]->view();
}

void BindlessMaterialTable::init(uint32_t maxMaterials, Buffer& gpuBuffer) {
    gpuBuffer_ = &gpuBuffer;
    allocator_ = FreeListAllocator<uint32_t>(maxMaterials);
}

MaterialHandle BindlessMaterialTable::registerMaterial(const void* materialData) {
    const uint32_t index = allocator_.allocate();
    if (index == UINT32_MAX || gpuBuffer_ == nullptr) {
        return {};
    }
    const size_t offset = static_cast<size_t>(index) * kMaterialStride;
    gpuBuffer_->write(materialData, kMaterialStride, offset);
    return MaterialHandle{index};
}

void BindlessMaterialTable::updateMaterial(MaterialHandle handle, const void* materialData) {
    if (!handle.valid() || gpuBuffer_ == nullptr) {
        return;
    }
    const size_t offset = static_cast<size_t>(handle.index) * kMaterialStride;
    gpuBuffer_->write(materialData, kMaterialStride, offset);
}

void BindlessMaterialTable::removeMaterial(MaterialHandle handle) {
    if (handle.valid()) {
        allocator_.free(handle.index);
    }
}

void BindlessMaterialTable::clear() {
    allocator_.clear();
    gpuBuffer_ = nullptr;
}

void BindlessMeshTable::init(uint32_t maxMeshes, Buffer& gpuBuffer) {
    gpuBuffer_ = &gpuBuffer;
    allocator_ = FreeListAllocator<uint32_t>(maxMeshes);
}

MeshHandle BindlessMeshTable::registerMesh(const void* meshData, uint32_t stride) {
    const uint32_t index = allocator_.allocate();
    if (index == UINT32_MAX || gpuBuffer_ == nullptr) {
        return {};
    }
    const size_t offset = static_cast<size_t>(index) * kMeshStride;
    gpuBuffer_->write(meshData, std::min(stride, kMeshStride), offset);
    return MeshHandle{index};
}

void BindlessMeshTable::updateMesh(MeshHandle handle, const void* meshData, uint32_t stride) {
    if (!handle.valid() || gpuBuffer_ == nullptr) {
        return;
    }
    const size_t offset = static_cast<size_t>(handle.index) * kMeshStride;
    gpuBuffer_->write(meshData, std::min(stride, kMeshStride), offset);
}

void BindlessMeshTable::removeMesh(MeshHandle handle) {
    if (handle.valid()) {
        allocator_.free(handle.index);
    }
}

void BindlessMeshTable::clear() {
    allocator_.clear();
    gpuBuffer_ = nullptr;
}

} // namespace rtv
