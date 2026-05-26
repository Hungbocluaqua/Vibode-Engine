#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/BindlessResources.h"

#include <Volk/volk.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace rtv {

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    std::optional<uint32_t> compute;

    [[nodiscard]] bool complete() const {
        return graphics.has_value() && present.has_value();
    }
    [[nodiscard]] bool hasDedicatedCompute() const {
        return compute.has_value() && compute.value() != graphics.value_or(UINT32_MAX);
    }
    [[nodiscard]] bool hasAsyncCompute() const {
        return compute.has_value();
    }
};

struct RayTracingCapabilities {
    bool accelerationStructure = false;
    bool rayTracingPipeline = false;
    bool bufferDeviceAddress = false;
    bool deferredHostOperations = false;
    bool spirv14 = false;
    bool shaderFloatControls = false;
    bool supported = false;
    std::vector<std::string> missing;
};

struct RayTracingDeviceInfo {
    RayTracingCapabilities capabilities;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingPipelineProperties{};
};

class VulkanContext final : private NonCopyable {
public:
    explicit VulkanContext(GLFWwindow* window);
    ~VulkanContext();

    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
    [[nodiscard]] VkQueue graphicsQueue() const { return graphicsQueue_; }
    [[nodiscard]] VkQueue presentQueue() const { return presentQueue_; }
    [[nodiscard]] VkQueue computeQueue() const { return computeQueue_; }
    [[nodiscard]] bool hasAsyncComputeQueue() const { return queueFamilies_.hasDedicatedCompute(); }
    [[nodiscard]] bool canAsyncCompute() const { return queueFamilies_.hasAsyncCompute(); }
    [[nodiscard]] const QueueFamilyIndices& queueFamilies() const { return queueFamilies_; }
    [[nodiscard]] VkPhysicalDeviceProperties physicalDeviceProperties() const { return physicalDeviceProperties_; }
    [[nodiscard]] const BindlessCapabilities& bindlessCapabilities() const { return bindlessCapabilities_; }
    [[nodiscard]] const RayTracingDeviceInfo& rayTracingInfo() const { return rayTracingInfo_; }
    [[nodiscard]] bool supportsHardwareRayTracing() const { return rayTracingInfo_.capabilities.supported; }
    [[nodiscard]] bool supportsBufferDeviceAddress() const { return rayTracingInfo_.capabilities.bufferDeviceAddress; }
    [[nodiscard]] VkSemaphore timelineSemaphore() const { return timelineSemaphore_; }
    [[nodiscard]] bool supportsTimelineSemaphore() const { return timelineSemaphore_ != VK_NULL_HANDLE; }
    [[nodiscard]] bool supportsSER() const { return supportsSER_; }
    [[nodiscard]] bool supportsSamplerAnisotropy() const { return samplerAnisotropy_; }
    [[nodiscard]] float maxSamplerAnisotropy() const { return maxSamplerAnisotropy_; }

private:
    void createInstance(GLFWwindow* window);
    void createDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    void createDevice();

    [[nodiscard]] bool validationRequested() const;
    [[nodiscard]] bool validationAvailable() const;
    [[nodiscard]] std::vector<const char*> requiredInstanceExtensions(GLFWwindow* window) const;
    [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice physicalDevice) const;
    [[nodiscard]] bool deviceSupportsRequiredExtensions(VkPhysicalDevice physicalDevice) const;
    [[nodiscard]] bool deviceSupportsRequiredFeatures(VkPhysicalDevice physicalDevice) const;
    [[nodiscard]] RayTracingDeviceInfo queryRayTracingDeviceInfo(VkPhysicalDevice physicalDevice) const;
    [[nodiscard]] int scorePhysicalDevice(VkPhysicalDevice physicalDevice) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalDeviceProperties_{};
    BindlessCapabilities bindlessCapabilities_{};
    RayTracingDeviceInfo rayTracingInfo_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    VkSemaphore timelineSemaphore_ = VK_NULL_HANDLE;
    bool supportsSER_ = false;
    bool samplerAnisotropy_ = false;
    float maxSamplerAnisotropy_ = 1.0f;
    QueueFamilyIndices queueFamilies_{};
};

} // namespace rtv
