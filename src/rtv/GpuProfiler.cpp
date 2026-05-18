#include "rtv/GpuProfiler.h"

#include "rtv/Check.h"

#include <algorithm>
#include <array>
#include <utility>

namespace rtv {

GpuProfiler::GpuProfiler(VkDevice device, VkPhysicalDevice physicalDevice) {
    create(device, physicalDevice);
}

GpuProfiler::~GpuProfiler() {
    destroy();
}

GpuProfiler::GpuProfiler(GpuProfiler&& other) noexcept {
    *this = std::move(other);
}

GpuProfiler& GpuProfiler::operator=(GpuProfiler&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        queryPool_ = other.queryPool_;
        timestampPeriod_ = other.timestampPeriod_;
        submitted_ = other.submitted_;
        timings_ = other.timings_;
        other.device_ = VK_NULL_HANDLE;
        other.queryPool_ = VK_NULL_HANDLE;
        other.submitted_ = false;
    }
    return *this;
}

void GpuProfiler::create(VkDevice device, VkPhysicalDevice physicalDevice) {
    destroy();
    device_ = device;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    timestampPeriod_ = properties.limits.timestampPeriod;

    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = Count;
    checkVk(vkCreateQueryPool(device_, &info, nullptr, &queryPool_), "vkCreateQueryPool(gpu profiler)");
}

void GpuProfiler::destroy() {
    if (device_ != VK_NULL_HANDLE && queryPool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, queryPool_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    queryPool_ = VK_NULL_HANDLE;
    submitted_ = false;
}

void GpuProfiler::collectCompletedFrame() {
    if (!submitted_ || queryPool_ == VK_NULL_HANDLE) {
        return;
    }

    std::array<uint64_t, Count> timestamps{};
    const VkResult result = vkGetQueryPoolResults(
        device_,
        queryPool_,
        0,
        Count,
        sizeof(uint64_t) * timestamps.size(),
        timestamps.data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);

    if (result == VK_NOT_READY) {
        return;
    }
    checkVk(result, "vkGetQueryPoolResults(gpu profiler)");

    const auto elapsedMs = [&](Query begin, Query end) {
        if (timestamps[end] <= timestamps[begin]) {
            return 0.0f;
        }
        const double ns = static_cast<double>(timestamps[end] - timestamps[begin]) * static_cast<double>(timestampPeriod_);
        return static_cast<float>(ns / 1.0e6);
    };

    const auto smooth = [](float previous, float current) {
        return previous <= 0.0f ? current : previous * 0.8f + current * 0.2f;
    };
    timings_.pathTraceMs = smooth(timings_.pathTraceMs, elapsedMs(PathTraceStart, PathTraceEnd));
    timings_.denoiserMs = smooth(timings_.denoiserMs, elapsedMs(DenoiserStart, DenoiserEnd));
    timings_.fullscreenMs = smooth(timings_.fullscreenMs, elapsedMs(FullscreenStart, FullscreenEnd));
    submitted_ = false;
}

void GpuProfiler::resetForFrame(VkCommandBuffer commandBuffer) {
    if (queryPool_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commandBuffer, queryPool_, 0, Count);
    }
}

void GpuProfiler::write(VkCommandBuffer commandBuffer, Query query, VkPipelineStageFlagBits2 stage) const {
    if (queryPool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(commandBuffer, stage, queryPool_, query);
    }
}

} // namespace rtv
