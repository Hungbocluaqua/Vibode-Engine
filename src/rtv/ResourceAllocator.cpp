#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include "rtv/ResourceAllocator.h"

#include "rtv/Check.h"
#include "rtv/VulkanContext.h"

namespace rtv {

ResourceAllocator::ResourceAllocator(const VulkanContext& context) {
    device_ = context.device();
    physicalDevice_ = context.physicalDevice();
    supportsDeviceAddress_ = context.supportsBufferDeviceAddress();
    supportsSamplerAnisotropy_ = context.supportsSamplerAnisotropy();
    maxSamplerAnisotropy_ = context.maxSamplerAnisotropy();

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo createInfo{};
    createInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    createInfo.physicalDevice = context.physicalDevice();
    createInfo.device = context.device();
    createInfo.instance = context.instance();
    createInfo.pVulkanFunctions = &functions;
    if (supportsDeviceAddress_) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    checkVk(vmaCreateAllocator(&createInfo, &allocator_), "vmaCreateAllocator");
}

ResourceAllocator::~ResourceAllocator() {
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
    }
}

void ResourceAllocator::setDebugName(VkObjectType objectType, uint64_t objectHandle, const char* name) const {
    if (objectHandle == 0 || name == nullptr || vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }

    VkDebugUtilsObjectNameInfoEXT nameInfo{};
    nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    nameInfo.objectType = objectType;
    nameInfo.objectHandle = objectHandle;
    nameInfo.pObjectName = name;
    (void)vkSetDebugUtilsObjectNameEXT(device_, &nameInfo);
}

} // namespace rtv
