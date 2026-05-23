#include "rtv/GpuProfiler.h"

#include "rtv/Check.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#ifndef VK_QUERY_PIPELINE_STATISTIC_RAY_INVOCATIONS_BIT_KHR
#define VK_QUERY_PIPELINE_STATISTIC_RAY_INVOCATIONS_BIT_KHR 0x00000001ull
#endif
#ifndef VK_QUERY_PIPELINE_STATISTIC_RAY_TRIANGLES_HIT_BIT_KHR
#define VK_QUERY_PIPELINE_STATISTIC_RAY_TRIANGLES_HIT_BIT_KHR 0x00000002ull
#endif
#ifndef VK_QUERY_PIPELINE_STATISTIC_RAY_AABBS_HIT_BIT_KHR
#define VK_QUERY_PIPELINE_STATISTIC_RAY_AABBS_HIT_BIT_KHR 0x00000004ull
#endif

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
        statsQueryPool_ = other.statsQueryPool_;
        timestampPeriod_ = other.timestampPeriod_;
        submitted_ = other.submitted_;
        statsSubmitted_ = other.statsSubmitted_;
        timings_ = other.timings_;
        pipelineStats_ = other.pipelineStats_;
        smoothedPipelineStats_ = other.smoothedPipelineStats_;
        other.device_ = VK_NULL_HANDLE;
        other.queryPool_ = VK_NULL_HANDLE;
        other.statsQueryPool_ = VK_NULL_HANDLE;
        other.submitted_ = false;
        other.statsSubmitted_ = false;
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

void GpuProfiler::createPipelineStatsQuery(VkDevice device, bool rayTracingAvailable) {
    if (statsQueryPool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, statsQueryPool_, nullptr);
    }
    device_ = device;

    VkQueryPipelineStatisticFlags statsFlags =
        VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
    if (rayTracingAvailable) {
        statsFlags |=
            VK_QUERY_PIPELINE_STATISTIC_RAY_INVOCATIONS_BIT_KHR |
            VK_QUERY_PIPELINE_STATISTIC_RAY_TRIANGLES_HIT_BIT_KHR |
            VK_QUERY_PIPELINE_STATISTIC_RAY_AABBS_HIT_BIT_KHR;
    }

    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    info.queryCount = 1;
    info.pipelineStatistics = statsFlags;
    checkVk(vkCreateQueryPool(device_, &info, nullptr, &statsQueryPool_),
            "vkCreateQueryPool(gpu pipeline stats)");
}

void GpuProfiler::destroy() {
    if (device_ != VK_NULL_HANDLE && queryPool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, queryPool_, nullptr);
    }
    if (device_ != VK_NULL_HANDLE && statsQueryPool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, statsQueryPool_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    queryPool_ = VK_NULL_HANDLE;
    statsQueryPool_ = VK_NULL_HANDLE;
    submitted_ = false;
    statsSubmitted_ = false;
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
    timings_.restirSpatialMs = smooth(timings_.restirSpatialMs, elapsedMs(RestirSpatialStart, RestirSpatialEnd));
    timings_.fogIntegrateMs = smooth(timings_.fogIntegrateMs, elapsedMs(FogIntegrateStart, FogIntegrateEnd));
    timings_.atmosphereMs = smooth(timings_.atmosphereMs, elapsedMs(AtmosphereStart, AtmosphereEnd));
    timings_.denoiserMs = smooth(timings_.denoiserMs, elapsedMs(DenoiserStart, DenoiserEnd));
    timings_.historyCopyMs = smooth(timings_.historyCopyMs, elapsedMs(HistoryCopyStart, HistoryCopyEnd));
    timings_.taaMs = smooth(timings_.taaMs, elapsedMs(TaaStart, TaaEnd));
    timings_.autoExposureMs = smooth(timings_.autoExposureMs, elapsedMs(AutoExposureStart, AutoExposureEnd));
    timings_.toneMapMs = smooth(timings_.toneMapMs, elapsedMs(ToneMapStart, ToneMapEnd));
    timings_.selectionOutlineMs = smooth(timings_.selectionOutlineMs, elapsedMs(SelectionOutlineStart, SelectionOutlineEnd));
    timings_.fullscreenMs = smooth(timings_.fullscreenMs, elapsedMs(FullscreenStart, FullscreenEnd));
    submitted_ = false;

    if (!statsSubmitted_ || statsQueryPool_ == VK_NULL_HANDLE) {
        return;
    }

    uint64_t statsResults[4] = {};
    const VkResult statsResult = vkGetQueryPoolResults(
        device_,
        statsQueryPool_,
        0,
        1,
        sizeof(statsResults),
        statsResults,
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);

    if (statsResult == VK_NOT_READY) {
        return;
    }
    checkVk(statsResult, "vkGetQueryPoolResults(pipeline stats)");

    pipelineStats_.rayInvocations = statsResults[0];
    pipelineStats_.triangleHits = statsResults[1];
    pipelineStats_.aabbHits = statsResults[2];
    pipelineStats_.valid = true;

    const auto smoothUint = [](uint64_t prev, uint64_t curr) {
        return prev == 0 ? curr : static_cast<uint64_t>(static_cast<double>(prev) * 0.8 + static_cast<double>(curr) * 0.2);
    };
    smoothedPipelineStats_.rayInvocations = smoothUint(smoothedPipelineStats_.rayInvocations, pipelineStats_.rayInvocations);
    smoothedPipelineStats_.triangleHits = smoothUint(smoothedPipelineStats_.triangleHits, pipelineStats_.triangleHits);
    smoothedPipelineStats_.aabbHits = smoothUint(smoothedPipelineStats_.aabbHits, pipelineStats_.aabbHits);
    smoothedPipelineStats_.valid = true;
    statsSubmitted_ = false;
}

void GpuProfiler::resetForFrame(VkCommandBuffer commandBuffer) {
    if (queryPool_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commandBuffer, queryPool_, 0, Count);
    }
    if (statsQueryPool_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commandBuffer, statsQueryPool_, 0, 1);
    }
}

void GpuProfiler::write(VkCommandBuffer commandBuffer, Query query, VkPipelineStageFlagBits2 stage) const {
    if (queryPool_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(commandBuffer, stage, queryPool_, query);
    }
}

void GpuProfiler::beginPipelineStats(VkCommandBuffer commandBuffer) const {
    if (statsQueryPool_ != VK_NULL_HANDLE) {
        vkCmdBeginQuery(commandBuffer, statsQueryPool_, 0, 0);
    }
}

void GpuProfiler::endPipelineStats(VkCommandBuffer commandBuffer) const {
    if (statsQueryPool_ != VK_NULL_HANDLE) {
        vkCmdEndQuery(commandBuffer, statsQueryPool_, 0);
    }
}

} // namespace rtv
