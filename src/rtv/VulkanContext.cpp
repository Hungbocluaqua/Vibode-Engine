#include "rtv/VulkanContext.h"

#include "rtv/Check.h"

#include <Volk/volk.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace rtv {

namespace {

constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";

const std::vector<const char*> requiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

const std::vector<const char*> optionalRayTracingDeviceExtensions = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_SPIRV_1_4_EXTENSION_NAME,
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
};

constexpr const char* kVkNvInvocationReorderExtension = VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME;
constexpr const char* kVkNvRayTracingMotionBlurExtension = VK_NV_RAY_TRACING_MOTION_BLUR_EXTENSION_NAME;
constexpr uint32_t kNvidiaVendorId = 0x10DEu;

const char* serReorderingHintName(VkRayTracingInvocationReorderModeNV hint) {
    switch (hint) {
        case VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_NV: return "reorder";
        case VK_RAY_TRACING_INVOCATION_REORDER_MODE_NONE_NV: return "none";
        default: return "unknown";
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "Vulkan validation: " << callbackData->pMessage << '\n';
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debugMessengerCreateInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    return info;
}

} // namespace

VulkanContext::VulkanContext(GLFWwindow* window) {
    headless_ = false;
    checkVk(volkInitialize(), "volkInitialize");
    createInstance(window);
    volkLoadInstance(instance_);
    createDebugMessenger();
    createSurface(window);
    pickPhysicalDevice();
    bindlessCapabilities_ = queryBindlessCapabilities(physicalDevice_);
    rayTracingInfo_ = queryRayTracingDeviceInfo(physicalDevice_);
    opacityMicromapInfo_ = queryOpacityMicromapDeviceInfo(physicalDevice_);
    serInfo_ = querySerDeviceInfo(physicalDevice_);
    rayTracingMotionBlurInfo_ = queryRayTracingMotionBlurDeviceInfo(physicalDevice_);
    createDevice();
    volkLoadDevice(device_);

    std::cout << "Vulkan device: " << physicalDeviceProperties_.deviceName << '\n';
    std::cout << "Descriptor indexing: "
              << (bindlessCapabilities_.descriptorIndexing && bindlessCapabilities_.runtimeDescriptorArray ? "available" : "limited")
              << " (max sampled update-after-bind " << bindlessCapabilities_.maxSampledImages << ")\n";
    if (rayTracingInfo_.capabilities.supported) {
        std::cout << "Hardware ray tracing: available (max recursion depth "
                  << rayTracingInfo_.rayTracingPipelineProperties.maxRayRecursionDepth
                  << ", group handle size "
                  << rayTracingInfo_.rayTracingPipelineProperties.shaderGroupHandleSize
                  << ", group base alignment "
                  << rayTracingInfo_.rayTracingPipelineProperties.shaderGroupBaseAlignment << ")\n";
    } else {
        std::cout << "Hardware ray tracing: unavailable";
        if (!rayTracingInfo_.capabilities.missing.empty()) {
            std::cout << " (missing ";
            for (size_t i = 0; i < rayTracingInfo_.capabilities.missing.size(); ++i) {
                if (i > 0) {
                    std::cout << ", ";
                }
                std::cout << rayTracingInfo_.capabilities.missing[i];
            }
            std::cout << ")";
        }
        std::cout << '\n';
    }
    std::cout << "VMA memory budget: " << (supportsMemoryBudget_ ? "available" : "unavailable") << '\n';
    if (opacityMicromapInfo_.supported) {
        std::cout << "Opacity micromaps: available (2-state subdivision "
                  << opacityMicromapInfo_.maxOpacity2StateSubdivisionLevel
                  << ", 4-state subdivision "
                  << opacityMicromapInfo_.maxOpacity4StateSubdivisionLevel << ")\n";
    } else {
        std::cout << "Opacity micromaps: fallback alpha any-hit path ("
                  << opacityMicromapInfo_.disabledReason << ")\n";
    }
    if (serInfo_.supported) {
        std::cout << "Shader execution reordering: available (hint "
                  << serReorderingHintName(serInfo_.reorderingHint)
                  << ", max invocation reorder depth "
                  << (serInfo_.maxInvocationReorderDepthReported ? std::to_string(serInfo_.maxRayTracingInvocationReorderDepth) : "not reported")
                  << ")\n";
    } else {
        std::cout << "Shader execution reordering: unavailable ("
                  << serInfo_.disabledReason << ")\n";
    }
    if (rayTracingMotionBlurInfo_.supported) {
        std::cout << "Ray tracing motion blur: available"
                  << (rayTracingMotionBlurInfo_.rayTracingMotionBlurPipelineTraceRaysIndirect ? " (indirect trace supported)" : "")
                  << "\n";
    } else {
        std::cout << "Ray tracing motion blur: unavailable ("
                  << rayTracingMotionBlurInfo_.disabledReason << ")\n";
    }
}

VulkanContext::VulkanContext(bool headless) {
    headless_ = headless;
}

std::unique_ptr<VulkanContext> VulkanContext::createHeadless() {
    auto context = std::unique_ptr<VulkanContext>(new VulkanContext(true));
    checkVk(volkInitialize(), "volkInitialize");
    context->createInstance(nullptr);
    volkLoadInstance(context->instance_);
    context->createDebugMessenger();
    context->pickPhysicalDeviceHeadless();
    context->bindlessCapabilities_ = queryBindlessCapabilities(context->physicalDevice_);
    context->rayTracingInfo_ = context->queryRayTracingDeviceInfo(context->physicalDevice_);
    context->opacityMicromapInfo_ = context->queryOpacityMicromapDeviceInfo(context->physicalDevice_);
    context->serInfo_ = context->querySerDeviceInfo(context->physicalDevice_);
    context->rayTracingMotionBlurInfo_ = context->queryRayTracingMotionBlurDeviceInfo(context->physicalDevice_);
    context->createDevice();
    volkLoadDevice(context->device_);

    std::cout << "Headless Vulkan device: " << context->physicalDeviceProperties_.deviceName << '\n';
    std::cout << "Descriptor indexing: "
              << (context->bindlessCapabilities_.descriptorIndexing && context->bindlessCapabilities_.runtimeDescriptorArray ? "available" : "limited")
              << " (max sampled update-after-bind " << context->bindlessCapabilities_.maxSampledImages << ")\n";
    if (context->rayTracingInfo_.capabilities.supported) {
        std::cout << "Hardware ray tracing: available (max recursion depth "
                  << context->rayTracingInfo_.rayTracingPipelineProperties.maxRayRecursionDepth << ")\n";
    } else {
        std::cout << "Hardware ray tracing: unavailable\n";
    }
    std::cout << "VMA memory budget: " << (context->supportsMemoryBudget_ ? "available" : "unavailable") << '\n';
    if (context->opacityMicromapInfo_.supported) {
        std::cout << "Opacity micromaps: available (2-state subdivision "
                  << context->opacityMicromapInfo_.maxOpacity2StateSubdivisionLevel
                  << ", 4-state subdivision "
                  << context->opacityMicromapInfo_.maxOpacity4StateSubdivisionLevel << ")\n";
    } else {
        std::cout << "Opacity micromaps: fallback alpha any-hit path ("
                  << context->opacityMicromapInfo_.disabledReason << ")\n";
    }
    if (context->serInfo_.supported) {
        std::cout << "Shader execution reordering: available (hint "
                  << serReorderingHintName(context->serInfo_.reorderingHint)
                  << ", max invocation reorder depth "
                  << (context->serInfo_.maxInvocationReorderDepthReported ? std::to_string(context->serInfo_.maxRayTracingInvocationReorderDepth) : "not reported")
                  << ")\n";
    } else {
        std::cout << "Shader execution reordering: unavailable ("
                  << context->serInfo_.disabledReason << ")\n";
    }
    if (context->rayTracingMotionBlurInfo_.supported) {
        std::cout << "Ray tracing motion blur: available"
                  << (context->rayTracingMotionBlurInfo_.rayTracingMotionBlurPipelineTraceRaysIndirect ? " (indirect trace supported)" : "")
                  << "\n";
    } else {
        std::cout << "Ray tracing motion blur: unavailable ("
                  << context->rayTracingMotionBlurInfo_.disabledReason << ")\n";
    }

    return context;
}

VulkanContext::~VulkanContext() {
    if (device_ != VK_NULL_HANDLE) {
        std::cerr << "Device idle wait: VulkanContext teardown\n";
        vkDeviceWaitIdle(device_);
        if (timelineSemaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, timelineSemaphore_, nullptr);
        }
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (debugMessenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

void VulkanContext::createInstance(GLFWwindow* window) {
    if (validationRequested() && !validationAvailable()) {
        throw std::runtime_error("Vulkan validation layer VK_LAYER_KHRONOS_validation is not installed");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Ray Tracing Engine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "RayTracingEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    const auto extensions = requiredInstanceExtensions(window);
    const auto debugInfo = debugMessengerCreateInfo();

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (validationRequested()) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &validationLayer;
        createInfo.pNext = &debugInfo;
    }

    checkVk(vkCreateInstance(&createInfo, nullptr, &instance_), "vkCreateInstance");
}

void VulkanContext::createDebugMessenger() {
    if (!validationRequested()) {
        return;
    }

    const auto createInfo = debugMessengerCreateInfo();
    checkVk(vkCreateDebugUtilsMessengerEXT(instance_, &createInfo, nullptr, &debugMessenger_), "vkCreateDebugUtilsMessengerEXT");
}

void VulkanContext::createSurface(GLFWwindow* window) {
    checkVk(glfwCreateWindowSurface(instance_, window, nullptr, &surface_), "glfwCreateWindowSurface");
}

void VulkanContext::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "vkEnumeratePhysicalDevices(count)");
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan physical devices found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");

    int bestScore = std::numeric_limits<int>::min();
    for (VkPhysicalDevice candidate : devices) {
        const int score = scorePhysicalDevice(candidate);
        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = candidate;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE || bestScore < 0) {
        throw std::runtime_error("No suitable Vulkan 1.3 device with swapchain, dynamic rendering, and synchronization2 support was found");
    }

    vkGetPhysicalDeviceProperties(physicalDevice_, &physicalDeviceProperties_);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &features);
    samplerAnisotropy_ = features.samplerAnisotropy == VK_TRUE;
    maxSamplerAnisotropy_ = samplerAnisotropy_ ? physicalDeviceProperties_.limits.maxSamplerAnisotropy : 1.0f;
    queueFamilies_ = findQueueFamilies(physicalDevice_);
}

void VulkanContext::createDevice() {
    std::map<uint32_t, uint32_t> queueCounts;
    auto requireQueue = [&](uint32_t family, uint32_t queueIndex) {
        queueCounts[family] = std::max(queueCounts[family], queueIndex + 1u);
    };
    requireQueue(queueFamilies_.graphics.value(), 0);
    requireQueue(queueFamilies_.present.value(), 0);
    if (queueFamilies_.compute.has_value()) {
        requireQueue(queueFamilies_.compute.value(), queueFamilies_.computeQueueIndex);
    }

    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    std::vector<std::vector<float>> queuePriorities;
    queueInfos.reserve(queueCounts.size());
    queuePriorities.reserve(queueCounts.size());
    for (const auto& [family, count] : queueCounts) {
        queuePriorities.push_back(std::vector<float>(count, 1.0f));
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = count;
        queueInfo.pQueuePriorities = queuePriorities.back().data();
        queueInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineSupport{};
    timelineSupport.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    VkPhysicalDeviceFeatures2 supportedFeatures{};
    supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supportedFeatures.pNext = &timelineSupport;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &supportedFeatures);
    timelineSemaphoreSupported_ = timelineSupport.timelineSemaphore == VK_TRUE;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipeline{};
    rayTracingPipeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rayTracingPipeline.rayTracingPipeline = rayTracingInfo_.capabilities.supported ? VK_TRUE : VK_FALSE;
    rayTracingPipeline.rayTracingPipelineTraceRaysIndirect = rayTracingInfo_.capabilities.traceRaysIndirect ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{};
    accelerationStructure.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelerationStructure.accelerationStructure = rayTracingInfo_.capabilities.supported ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddress{};
    bufferDeviceAddress.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    bufferDeviceAddress.bufferDeviceAddress = rayTracingInfo_.capabilities.bufferDeviceAddress ? VK_TRUE : VK_FALSE;

    void* featureTail = &features13;
    if (rayTracingInfo_.capabilities.supported) {
        rayTracingPipeline.pNext = featureTail;
        accelerationStructure.pNext = &rayTracingPipeline;
        bufferDeviceAddress.pNext = &accelerationStructure;
        featureTail = &bufferDeviceAddress;
    } else if (rayTracingInfo_.capabilities.bufferDeviceAddress) {
        bufferDeviceAddress.pNext = featureTail;
        featureTail = &bufferDeviceAddress;
    }

    VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing{};
    descriptorIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    descriptorIndexing.pNext = featureTail;
    if (bindlessCapabilities_.descriptorIndexing) {
        descriptorIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    }
    if (bindlessCapabilities_.runtimeDescriptorArray) {
        descriptorIndexing.runtimeDescriptorArray = VK_TRUE;
    }
    if (bindlessCapabilities_.partiallyBound) {
        descriptorIndexing.descriptorBindingPartiallyBound = VK_TRUE;
    }
    if (bindlessCapabilities_.updateAfterBind) {
        descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    }
    featureTail = &descriptorIndexing;

    VkPhysicalDeviceOpacityMicromapFeaturesEXT opacityMicromapFeatures{};
    opacityMicromapFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT;
    if (opacityMicromapInfo_.supported) {
        opacityMicromapFeatures.micromap = VK_TRUE;
        opacityMicromapFeatures.pNext = featureTail;
        featureTail = &opacityMicromapFeatures;
    }

    VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV serFeatures{};
    serFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV;
    if (serInfo_.supported) {
        serFeatures.rayTracingInvocationReorder = VK_TRUE;
        serFeatures.pNext = featureTail;
        featureTail = &serFeatures;
    }

    VkPhysicalDeviceRayTracingMotionBlurFeaturesNV motionBlurFeatures{};
    motionBlurFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV;
    if (rayTracingMotionBlurInfo_.supported) {
        motionBlurFeatures.rayTracingMotionBlur = VK_TRUE;
        motionBlurFeatures.rayTracingMotionBlurPipelineTraceRaysIndirect =
            rayTracingMotionBlurInfo_.rayTracingMotionBlurPipelineTraceRaysIndirect ? VK_TRUE : VK_FALSE;
        motionBlurFeatures.pNext = featureTail;
        featureTail = &motionBlurFeatures;
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemaphore{};
    timelineSemaphore.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineSemaphore.timelineSemaphore = VK_TRUE;
    if (timelineSemaphoreSupported_) {
        timelineSemaphore.pNext = featureTail;
        featureTail = &timelineSemaphore;
    }

    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR rtMaintenance1Features{};
    rtMaintenance1Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR;
    rtMaintenance1Features.rayTracingMaintenance1 = VK_FALSE;
    if (rayTracingInfo_.capabilities.supported) {
        rtMaintenance1Features.pNext = featureTail;
        featureTail = &rtMaintenance1Features;
    }

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features.shaderFloat64 = VK_TRUE;
    features2.features.pipelineStatisticsQuery = VK_TRUE;
    features2.features.samplerAnisotropy = samplerAnisotropy_ ? VK_TRUE : VK_FALSE;
    features2.pNext = featureTail;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features2;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = queueInfos.data();
    std::vector<const char*> enabledExtensions;
    if (!headless_) {
        enabledExtensions = requiredDeviceExtensions;
    }
    supportsMemoryBudget_ = deviceSupportsExtension(physicalDevice_, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (supportsMemoryBudget_) {
        enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }
    if (rayTracingInfo_.capabilities.supported) {
        enabledExtensions.insert(enabledExtensions.end(), optionalRayTracingDeviceExtensions.begin(), optionalRayTracingDeviceExtensions.end());
        if (opacityMicromapInfo_.supported) {
            enabledExtensions.push_back(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
        }
        if (serInfo_.supported) {
            enabledExtensions.push_back(kVkNvInvocationReorderExtension);
        }
        if (rayTracingMotionBlurInfo_.supported) {
            enabledExtensions.push_back(kVkNvRayTracingMotionBlurExtension);
        }
    } else if (rayTracingInfo_.capabilities.bufferDeviceAddress) {
        enabledExtensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();

    checkVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamilies_.graphics.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, queueFamilies_.present.value(), 0, &presentQueue_);
    if (queueFamilies_.compute.has_value()) {
        vkGetDeviceQueue(device_, queueFamilies_.compute.value(), queueFamilies_.computeQueueIndex, &computeQueue_);
    }
    if (timelineSemaphoreSupported_) {
        VkSemaphoreTypeCreateInfo timelineCreateInfo{};
        timelineCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineCreateInfo.initialValue = 0;
        VkSemaphoreCreateInfo semCreateInfo{};
        semCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semCreateInfo.pNext = &timelineCreateInfo;
        checkVk(vkCreateSemaphore(device_, &semCreateInfo, nullptr, &timelineSemaphore_),
                "vkCreateSemaphore(timeline)");
    } else {
        std::cout << "Timeline semaphore: unavailable; async compute will use single-queue fallback\n";
    }
}

bool VulkanContext::validationRequested() const {
#if defined(NDEBUG)
    return false;
#else
    return true;
#endif
}

bool VulkanContext::validationAvailable() const {
    uint32_t layerCount = 0;
    checkVk(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "vkEnumerateInstanceLayerProperties(count)");
    std::vector<VkLayerProperties> layers(layerCount);
    checkVk(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "vkEnumerateInstanceLayerProperties");

    return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, validationLayer) == 0;
    });
}

std::vector<const char*> VulkanContext::requiredInstanceExtensions(GLFWwindow* window) const {
    (void)window;
    std::vector<const char*> extensions;
    if (headless_) {
        if (validationRequested()) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        return extensions;
    }

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
        throw std::runtime_error("GLFW did not report required Vulkan instance extensions");
    }

    extensions.assign(glfwExtensions, glfwExtensions + glfwExtensionCount);
    if (validationRequested()) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return extensions;
}

QueueFamilyIndices VulkanContext::findQueueFamilies(VkPhysicalDevice physicalDevice) const {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, families.data());

    QueueFamilyIndices indices;
    std::optional<uint32_t> dedicatedCompute;
    std::optional<uint32_t> sameFamilyCompute;
    uint32_t sameFamilyComputeQueueIndex = 0;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        const bool hasGraphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool hasCompute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;

        if (hasGraphics && !indices.graphics.has_value()) {
            indices.graphics = i;
        }
        if (hasCompute && !hasGraphics) {
            dedicatedCompute = i;
        }
        if (hasGraphics && hasCompute && families[i].queueCount > 1 && !sameFamilyCompute.has_value()) {
            sameFamilyCompute = i;
            sameFamilyComputeQueueIndex = 1;
        }

        if (!headless_ && surface_ != VK_NULL_HANDLE) {
            VkBool32 presentSupported = VK_FALSE;
            checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface_, &presentSupported), "vkGetPhysicalDeviceSurfaceSupportKHR");
            if (presentSupported == VK_TRUE && !indices.present.has_value()) {
                indices.present = i;
            }
        } else if (headless_) {
            if (hasGraphics && !indices.present.has_value()) {
                indices.present = i;
            }
        }

        // Keep scanning after graphics/present are found so a later dedicated compute
        // family is not missed.
    }

    if (dedicatedCompute.has_value()) {
        indices.compute = *dedicatedCompute;
        indices.computeQueueIndex = 0;
    } else if (sameFamilyCompute.has_value()) {
        indices.compute = *sameFamilyCompute;
        indices.computeQueueIndex = sameFamilyComputeQueueIndex;
    } else if (indices.graphics.has_value()) {
        indices.compute = *indices.graphics;
        indices.computeQueueIndex = 0;
    }

    return indices;
}

bool VulkanContext::deviceSupportsRequiredExtensions(VkPhysicalDevice physicalDevice) const {
    uint32_t extensionCount = 0;
    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties(count)");
    std::vector<VkExtensionProperties> available(extensionCount);
    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, available.data()), "vkEnumerateDeviceExtensionProperties");

    if (headless_) {
        return true;
    }

    std::set<std::string> missing(requiredDeviceExtensions.begin(), requiredDeviceExtensions.end());
    for (const auto& extension : available) {
        missing.erase(extension.extensionName);
    }
    return missing.empty();
}

bool VulkanContext::deviceSupportsRequiredFeatures(VkPhysicalDevice physicalDevice) const {
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features13;

    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    return features13.dynamicRendering == VK_TRUE && features13.synchronization2 == VK_TRUE;
}

bool VulkanContext::deviceSupportsExtension(VkPhysicalDevice physicalDevice, const char* extensionName) const {
    uint32_t extensionCount = 0;
    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr),
            "vkEnumerateDeviceExtensionProperties(count support check)");
    std::vector<VkExtensionProperties> available(extensionCount);
    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, available.data()),
            "vkEnumerateDeviceExtensionProperties(support check)");

    return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, extensionName) == 0;
    });
}

RayTracingDeviceInfo VulkanContext::queryRayTracingDeviceInfo(VkPhysicalDevice physicalDevice) const {
    RayTracingDeviceInfo info{};

    uint32_t extensionCount = 0;
    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties(count rt)");
    std::vector<VkExtensionProperties> available(extensionCount);
    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, available.data()), "vkEnumerateDeviceExtensionProperties(rt)");

    auto hasExtension = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
    };

    info.capabilities.accelerationStructure = hasExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    info.capabilities.rayTracingPipeline = hasExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    info.capabilities.bufferDeviceAddress = hasExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    info.capabilities.deferredHostOperations = hasExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    info.capabilities.spirv14 = hasExtension(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    info.capabilities.shaderFloatControls = hasExtension(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
    rtFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeatures.pNext = &rtFeatures;
    VkPhysicalDeviceBufferDeviceAddressFeatures bdaFeatures{};
    bdaFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    bdaFeatures.pNext = &asFeatures;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &bdaFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    if (bdaFeatures.bufferDeviceAddress != VK_TRUE) {
        info.capabilities.bufferDeviceAddress = false;
    }
    if (asFeatures.accelerationStructure != VK_TRUE) {
        info.capabilities.accelerationStructure = false;
    }
    if (rtFeatures.rayTracingPipeline != VK_TRUE) {
        info.capabilities.rayTracingPipeline = false;
    }
    info.capabilities.traceRaysIndirect = rtFeatures.rayTracingPipelineTraceRaysIndirect == VK_TRUE;

    auto require = [&](bool availableFeature, const char* name) {
        if (!availableFeature) {
            info.capabilities.missing.push_back(name);
        }
    };
    require(info.capabilities.accelerationStructure, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    require(info.capabilities.rayTracingPipeline, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    require(info.capabilities.bufferDeviceAddress, "bufferDeviceAddress feature/extension");
    require(info.capabilities.deferredHostOperations, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    require(info.capabilities.spirv14, VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    require(info.capabilities.shaderFloatControls, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    info.capabilities.supported = info.capabilities.missing.empty();

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    asProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    asProps.pNext = &rtProps;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
    info.accelerationStructureProperties = asProps;
    info.rayTracingPipelineProperties = rtProps;

    return info;
}

OpacityMicromapDeviceInfo VulkanContext::queryOpacityMicromapDeviceInfo(VkPhysicalDevice physicalDevice) const {
    OpacityMicromapDeviceInfo info{};
    const VkPhysicalDeviceProperties properties = [&] {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        return props;
    }();

    if (!rayTracingInfo_.capabilities.supported) {
        info.disabledReason = "hardware ray tracing unavailable";
        return info;
    }
    if (properties.vendorID != kNvidiaVendorId) {
        info.disabledReason = "non-NVIDIA device uses alpha any-hit fallback";
        return info;
    }

    info.extensionSupported = deviceSupportsExtension(physicalDevice, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
    if (!info.extensionSupported) {
        info.disabledReason = "VK_EXT_opacity_micromap not exposed; pre-RTX-40 or unsupported driver uses alpha any-hit fallback";
        return info;
    }

    VkPhysicalDeviceOpacityMicromapFeaturesEXT ommFeatures{};
    ommFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &ommFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    info.micromapFeature = ommFeatures.micromap == VK_TRUE;
    info.captureReplay = ommFeatures.micromapCaptureReplay == VK_TRUE;
    info.hostCommands = ommFeatures.micromapHostCommands == VK_TRUE;
    if (!info.micromapFeature) {
        info.disabledReason = "VK_EXT_opacity_micromap exposed but micromap feature is unavailable";
        return info;
    }

    VkPhysicalDeviceOpacityMicromapPropertiesEXT ommProperties{};
    ommProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &ommProperties;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);

    info.maxOpacity2StateSubdivisionLevel = ommProperties.maxOpacity2StateSubdivisionLevel;
    info.maxOpacity4StateSubdivisionLevel = ommProperties.maxOpacity4StateSubdivisionLevel;
    info.supported = true;
    info.disabledReason.clear();
    return info;
}

SerDeviceInfo VulkanContext::querySerDeviceInfo(VkPhysicalDevice physicalDevice) const {
    SerDeviceInfo info{};
    if (!rayTracingInfo_.capabilities.supported) {
        info.disabledReason = "hardware ray tracing unavailable";
        return info;
    }

    info.extensionSupported = deviceSupportsExtension(physicalDevice, kVkNvInvocationReorderExtension);
    if (!info.extensionSupported) {
        info.disabledReason = "VK_NV_ray_tracing_invocation_reorder not exposed";
        return info;
    }

    VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV serFeatures{};
    serFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &serFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    info.invocationReorderFeature = serFeatures.rayTracingInvocationReorder == VK_TRUE;
    if (!info.invocationReorderFeature) {
        info.disabledReason = "VK_NV_ray_tracing_invocation_reorder exposed but feature is unavailable";
        return info;
    }

    VkPhysicalDeviceRayTracingInvocationReorderPropertiesNV serProperties{};
    serProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_PROPERTIES_NV;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &serProperties;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);

    info.reorderingHint = serProperties.rayTracingInvocationReorderReorderingHint;
    info.maxInvocationReorderDepthReported = false;
    info.maxRayTracingInvocationReorderDepth = 0;
    info.supported = true;
    info.disabledReason.clear();
    return info;
}

RayTracingMotionBlurDeviceInfo VulkanContext::queryRayTracingMotionBlurDeviceInfo(VkPhysicalDevice physicalDevice) const {
    RayTracingMotionBlurDeviceInfo info{};
    if (!rayTracingInfo_.capabilities.supported) {
        info.disabledReason = "hardware ray tracing unavailable";
        return info;
    }

    info.extensionSupported = deviceSupportsExtension(physicalDevice, kVkNvRayTracingMotionBlurExtension);
    if (!info.extensionSupported) {
        info.disabledReason = "VK_NV_ray_tracing_motion_blur not exposed";
        return info;
    }

    VkPhysicalDeviceRayTracingMotionBlurFeaturesNV motionFeatures{};
    motionFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &motionFeatures;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    info.rayTracingMotionBlurFeature = motionFeatures.rayTracingMotionBlur == VK_TRUE;
    info.rayTracingMotionBlurPipelineTraceRaysIndirect =
        motionFeatures.rayTracingMotionBlurPipelineTraceRaysIndirect == VK_TRUE;
    if (!info.rayTracingMotionBlurFeature) {
        info.disabledReason = "VK_NV_ray_tracing_motion_blur exposed but feature is unavailable";
        return info;
    }

    info.supported = true;
    info.disabledReason.clear();
    return info;
}

int VulkanContext::scorePhysicalDevice(VkPhysicalDevice physicalDevice) const {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    if (properties.apiVersion < VK_API_VERSION_1_3) {
        return -1;
    }
    if (!findQueueFamilies(physicalDevice).complete()) {
        return -1;
    }
    if (!deviceSupportsRequiredExtensions(physicalDevice)) {
        return -1;
    }
    if (!deviceSupportsRequiredFeatures(physicalDevice)) {
        return -1;
    }

    if (!headless_) {
        uint32_t formatCount = 0;
        checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface_, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
        uint32_t presentModeCount = 0;
        checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface_, &presentModeCount, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR(count)");
        if (formatCount == 0 || presentModeCount == 0) {
            return -1;
        }
    }

    int score = 0;
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    }
    score += static_cast<int>(properties.limits.maxImageDimension2D);
    return score;
}

void VulkanContext::pickPhysicalDeviceHeadless() {
    uint32_t deviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "vkEnumeratePhysicalDevices(count)");
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan physical devices found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");

    int bestScore = std::numeric_limits<int>::min();
    for (VkPhysicalDevice candidate : devices) {
        const int score = scorePhysicalDevice(candidate);
        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = candidate;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE || bestScore < 0) {
        throw std::runtime_error("No suitable Vulkan 1.3 device with dynamic rendering and synchronization2 support was found");
    }

    vkGetPhysicalDeviceProperties(physicalDevice_, &physicalDeviceProperties_);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &features);
    samplerAnisotropy_ = features.samplerAnisotropy == VK_TRUE;
    maxSamplerAnisotropy_ = samplerAnisotropy_ ? physicalDeviceProperties_.limits.maxSamplerAnisotropy : 1.0f;
    queueFamilies_ = findQueueFamilies(physicalDevice_);
}

} // namespace rtv
