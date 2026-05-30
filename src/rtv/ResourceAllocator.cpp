#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include "rtv/ResourceAllocator.h"

#include "rtv/Check.h"
#include "rtv/VulkanContext.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>

namespace rtv {

ResourceAllocator::ResourceAllocator(const VulkanContext& context) {
    device_ = context.device();
    physicalDevice_ = context.physicalDevice();
    supportsDeviceAddress_ = context.supportsBufferDeviceAddress();
    supportsSamplerAnisotropy_ = context.supportsSamplerAnisotropy();
    supportsMemoryBudget_ = context.supportsMemoryBudget();
    maxSamplerAnisotropy_ = context.maxSamplerAnisotropy();
    const QueueFamilyIndices& queues = context.queueFamilies();
    if (queues.graphics.has_value() && queues.compute.has_value() && queues.graphics.value() != queues.compute.value()) {
        graphicsComputeSharingMode_ = VK_SHARING_MODE_CONCURRENT;
        graphicsComputeQueueFamilyCount_ = 2;
        graphicsComputeQueueFamilies_[0] = queues.graphics.value();
        graphicsComputeQueueFamilies_[1] = queues.compute.value();
    }

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
    if (supportsMemoryBudget_) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
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

ResourceAllocator::MemoryBudgetReport ResourceAllocator::memoryBudgetReport() const {
    MemoryBudgetReport report{};
    report.supported = supportsMemoryBudget_;
    if (allocator_ == VK_NULL_HANDLE) {
        report.pressure = "unavailable";
        return report;
    }

    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator_, budgets.data());

    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
    report.heaps.reserve(memoryProperties.memoryHeapCount);

    auto pressureName = [](float ratio) {
        if (ratio >= 0.95f) { return "critical"; }
        if (ratio >= 0.85f) { return "high"; }
        if (ratio >= 0.70f) { return "medium"; }
        return "normal";
    };

    for (uint32_t heapIndex = 0; heapIndex < memoryProperties.memoryHeapCount; ++heapIndex) {
        const VmaBudget& budget = budgets[heapIndex];
        HeapBudget heap{};
        heap.heapIndex = heapIndex;
        heap.usageBytes = static_cast<uint64_t>(budget.usage);
        heap.budgetBytes = static_cast<uint64_t>(budget.budget);
        heap.allocationBytes = static_cast<uint64_t>(budget.statistics.allocationBytes);
        heap.blockBytes = static_cast<uint64_t>(budget.statistics.blockBytes);
        heap.allocationCount = static_cast<uint32_t>(std::min<uint64_t>(
            static_cast<uint64_t>(budget.statistics.allocationCount),
            std::numeric_limits<uint32_t>::max()));
        heap.blockCount = static_cast<uint32_t>(std::min<uint64_t>(
            static_cast<uint64_t>(budget.statistics.blockCount),
            std::numeric_limits<uint32_t>::max()));
        heap.usageRatio = heap.budgetBytes > 0
            ? static_cast<float>(static_cast<double>(heap.usageBytes) / static_cast<double>(heap.budgetBytes))
            : 0.0f;
        heap.pressure = pressureName(heap.usageRatio);

        report.totalUsageBytes += heap.usageBytes;
        report.totalBudgetBytes += heap.budgetBytes;
        report.totalAllocationBytes += heap.allocationBytes;
        report.totalBlockBytes += heap.blockBytes;
        report.allocationCount += heap.allocationCount;
        report.blockCount += heap.blockCount;
        report.maxUsageRatio = std::max(report.maxUsageRatio, heap.usageRatio);

        if (heap.usageRatio >= 0.70f) {
            std::ostringstream warning;
            warning << "memory heap " << heapIndex << " usage is "
                    << static_cast<uint32_t>(heap.usageRatio * 100.0f) << "% of reported budget";
            report.warnings.push_back(warning.str());
        }

        report.heaps.push_back(std::move(heap));
    }

    if (hasPreviousBudgetUsage_) {
        report.usageDeltaBytes = static_cast<int64_t>(report.totalUsageBytes) -
            static_cast<int64_t>(previousBudgetUsageBytes_);
    }
    previousBudgetUsageBytes_ = report.totalUsageBytes;
    hasPreviousBudgetUsage_ = true;
    peakBudgetUsageBytes_ = std::max(peakBudgetUsageBytes_, report.totalUsageBytes);
    report.peakUsageBytes = peakBudgetUsageBytes_;
    report.pressure = pressureName(report.maxUsageRatio);

#if defined(_MSC_VER)
    char* overrideValue = nullptr;
    size_t overrideLength = 0;
    _dupenv_s(&overrideValue, &overrideLength, "RTV_MEMORY_PRESSURE_OVERRIDE");
    const std::string pressureOverride = overrideValue != nullptr ? std::string(overrideValue) : std::string{};
    std::free(overrideValue);
#else
    const char* overrideEnv = std::getenv("RTV_MEMORY_PRESSURE_OVERRIDE");
    const std::string pressureOverride = overrideEnv != nullptr ? std::string(overrideEnv) : std::string{};
#endif
    if (!pressureOverride.empty()) {
        float overrideRatio = report.maxUsageRatio;
        if (pressureOverride == "medium") {
            overrideRatio = 0.71f;
        } else if (pressureOverride == "high") {
            overrideRatio = 0.86f;
        } else if (pressureOverride == "critical") {
            overrideRatio = 0.96f;
        } else if (pressureOverride == "normal") {
            overrideRatio = 0.0f;
        } else {
            try {
                overrideRatio = std::clamp(std::stof(pressureOverride), 0.0f, 1.0f);
            } catch (...) {
                overrideRatio = report.maxUsageRatio;
            }
        }
        report.overrideActive = true;
        report.maxUsageRatio = overrideRatio;
        report.pressure = pressureName(report.maxUsageRatio);
        report.warnings.push_back("RTV_MEMORY_PRESSURE_OVERRIDE active: " + pressureOverride);
    }
    return report;
}

} // namespace rtv
