#include "rtv/BindlessResources.h"

#include "rtv/Buffer.h"
#include "rtv/Check.h"
#include "rtv/Image.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
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

bool supportsFullBindlessTextures(const BindlessCapabilities& caps) {
    return caps.descriptorIndexing &&
        caps.runtimeDescriptorArray &&
        caps.partiallyBound &&
        caps.updateAfterBind &&
        caps.maxSampledImages > 0;
}

uint32_t fullBindlessTextureCapacityOrThrow(const BindlessCapabilities& caps) {
    if (!supportsFullBindlessTextures(caps)) {
        throw std::runtime_error(
            "Full bindless textures require descriptor indexing, runtime descriptor arrays, "
            "partially-bound sampled image descriptors, and sampled-image update-after-bind support");
    }
    return maxMaterialTextureSlots(caps);
}

BindlessTextureHeap::~BindlessTextureHeap() {
    destroy();
}

void BindlessTextureHeap::init(VkDevice device, const BindlessCapabilities& caps, uint32_t capacity) {
    destroy();
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("Bindless texture heap requires a valid Vulkan device");
    }
    if (!supportsFullBindlessTextures(caps)) {
        (void)fullBindlessTextureCapacityOrThrow(caps);
    }
    capacity_ = std::max(1u, capacity);
    device_ = device;

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = capacity_;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_), "vkCreateDescriptorPool(bindless texture heap)");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = capacity_;
    binding.stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorBindingFlags bindingFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
    bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlagsInfo.bindingCount = 1;
    bindingFlagsInfo.pBindingFlags = &bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &bindingFlagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    checkVk(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout_), "vkCreateDescriptorSetLayout(bindless texture heap)");

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout_;
    checkVk(vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet_), "vkAllocateDescriptorSets(bindless texture heap)");
}

void BindlessTextureHeap::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        }
        if (layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
        }
    }
    device_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    capacity_ = 0;
    descriptorCount_ = 0;
    patchCount_ = 0;
}

void BindlessTextureHeap::updateAll(const std::vector<VkDescriptorImageInfo>& descriptors) {
    if (descriptorSet_ == VK_NULL_HANDLE || descriptors.empty()) {
        return;
    }
    if (descriptors.size() > capacity_) {
        throw std::runtime_error("Bindless texture heap descriptor update exceeds heap capacity");
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = static_cast<uint32_t>(descriptors.size());
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = descriptors.data();
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    descriptorCount_ = static_cast<uint32_t>(descriptors.size());
    ++patchCount_;
}

void BindlessTextureHeap::patch(uint32_t slot, const VkDescriptorImageInfo& descriptor) {
    if (descriptorSet_ == VK_NULL_HANDLE || slot >= capacity_) {
        return;
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descriptor;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    descriptorCount_ = std::max(descriptorCount_, slot + 1u);
    ++patchCount_;
}

BindlessTextureHeapStats BindlessTextureHeap::stats() const {
    return {
        .capacity = capacity_,
        .descriptorCount = descriptorCount_,
        .patchCount = patchCount_,
        .initialized = initialized(),
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
        registrations_ = std::move(other.registrations_);
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
    if (registrations_.size() > slotCount_) {
        registrations_.resize(slotCount_);
    }
}

void BindlessTextureTable::setRegistrationInfo(std::vector<BindlessTextureRegistrationInfo> registrations) {
    registrations_ = std::move(registrations);
}

void BindlessTextureTable::clear() {
    images_.clear();
    descriptors_.clear();
    registrations_.clear();
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

BindlessTextureImageInfo BindlessTextureTable::imageInfo(uint32_t index) const {
    if (index >= images_.size() || images_[index] == nullptr) {
        return {};
    }
    const Image& image = *images_[index];
    return {
        .width = image.width(),
        .height = image.height(),
        .mipLevels = image.mipLevels(),
        .format = image.format(),
    };
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
