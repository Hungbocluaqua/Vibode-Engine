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

    struct TimestampResult {
        uint64_t timestamp = 0;
        uint64_t available = 0;
    };
    std::array<TimestampResult, Count> timestamps{};
    const VkResult result = vkGetQueryPoolResults(
        device_,
        queryPool_,
        0,
        Count,
        sizeof(TimestampResult) * timestamps.size(),
        timestamps.data(),
        sizeof(TimestampResult),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

    if (result != VK_SUCCESS && result != VK_NOT_READY) {
        checkVk(result, "vkGetQueryPoolResults(gpu profiler)");
    }
    bool anyTimestampAvailable = false;
    for (const TimestampResult& timestamp : timestamps) {
        anyTimestampAvailable = anyTimestampAvailable || timestamp.available != 0;
    }
    if (!anyTimestampAvailable) {
        return;
    }

    const auto elapsedMs = [&](Query begin, Query end) {
        if (timestamps[begin].available == 0 || timestamps[end].available == 0 ||
            timestamps[end].timestamp <= timestamps[begin].timestamp) {
            return 0.0f;
        }
        const double ns = static_cast<double>(timestamps[end].timestamp - timestamps[begin].timestamp) * static_cast<double>(timestampPeriod_);
        return static_cast<float>(ns / 1.0e6);
    };

    const auto smooth = [](float previous, float current) {
        return previous <= 0.0f ? current : previous * 0.8f + current * 0.2f;
    };
    timings_.pathTraceMs = smooth(timings_.pathTraceMs, elapsedMs(PathTraceStart, PathTraceEnd));
    timings_.restirHistoryClearMs = smooth(timings_.restirHistoryClearMs, elapsedMs(RestirHistoryClearStart, RestirHistoryClearEnd));
    timings_.restirGiClearMs = smooth(timings_.restirGiClearMs, elapsedMs(RestirGiClearStart, RestirGiClearEnd));
    timings_.restirSpatialMs = smooth(timings_.restirSpatialMs, elapsedMs(RestirSpatialStart, RestirSpatialEnd));
    timings_.restirSpatialCopyMs = smooth(timings_.restirSpatialCopyMs, elapsedMs(RestirSpatialCopyStart, RestirSpatialCopyEnd));
    timings_.restirGiSpatialMs = smooth(timings_.restirGiSpatialMs, elapsedMs(RestirGiSpatialStart, RestirGiSpatialEnd));
    timings_.restirGiFinalMs = smooth(timings_.restirGiFinalMs, elapsedMs(RestirGiFinalStart, RestirGiFinalEnd));
    timings_.fogIntegrateMs = smooth(timings_.fogIntegrateMs, elapsedMs(FogIntegrateStart, FogIntegrateEnd));
    timings_.atmosphereMs = smooth(timings_.atmosphereMs, elapsedMs(AtmosphereStart, AtmosphereEnd));
    timings_.atmosphereTransmittanceMs = smooth(timings_.atmosphereTransmittanceMs, elapsedMs(AtmosphereTransmittanceStart, AtmosphereTransmittanceEnd));
    timings_.atmosphereMultiScatterMs = smooth(timings_.atmosphereMultiScatterMs, elapsedMs(AtmosphereMultiScatterStart, AtmosphereMultiScatterEnd));
    timings_.atmosphereSkyViewMs = smooth(timings_.atmosphereSkyViewMs, elapsedMs(AtmosphereSkyViewStart, AtmosphereSkyViewEnd));
    timings_.atmosphereSkyReprojectMs = smooth(timings_.atmosphereSkyReprojectMs, elapsedMs(AtmosphereSkyReprojectStart, AtmosphereSkyReprojectEnd));
    timings_.atmosphereSkyCdfMs = smooth(timings_.atmosphereSkyCdfMs, elapsedMs(AtmosphereSkyCdfStart, AtmosphereSkyCdfEnd));
    timings_.atmosphereAerialPerspectiveMs = smooth(timings_.atmosphereAerialPerspectiveMs, elapsedMs(AtmosphereAerialPerspectiveStart, AtmosphereAerialPerspectiveEnd));
    timings_.denoiserMs = smooth(timings_.denoiserMs, elapsedMs(DenoiserStart, DenoiserEnd));
    timings_.momentUpdateMs = smooth(timings_.momentUpdateMs, elapsedMs(MomentUpdateStart, MomentUpdateEnd));
    timings_.historyCopyMs = smooth(timings_.historyCopyMs, elapsedMs(HistoryCopyStart, HistoryCopyEnd));
    timings_.skipDenoiserCopyMs = smooth(timings_.skipDenoiserCopyMs, elapsedMs(SkipDenoiserCopyStart, SkipDenoiserCopyEnd));
    timings_.taaMs = smooth(timings_.taaMs, elapsedMs(TaaStart, TaaEnd));
    timings_.taaHistoryCopyMs = smooth(timings_.taaHistoryCopyMs, elapsedMs(TaaHistoryCopyStart, TaaHistoryCopyEnd));
    timings_.autoExposureHistogramClearMs = smooth(timings_.autoExposureHistogramClearMs, elapsedMs(AutoExposureHistogramClearStart, AutoExposureHistogramClearEnd));
    timings_.autoExposureHistogramMs = smooth(timings_.autoExposureHistogramMs, elapsedMs(AutoExposureHistogramStart, AutoExposureHistogramEnd));
    timings_.autoExposureReduceMs = smooth(timings_.autoExposureReduceMs, elapsedMs(AutoExposureReduceStart, AutoExposureReduceEnd));
    timings_.autoExposureMs = timings_.autoExposureHistogramClearMs + timings_.autoExposureHistogramMs + timings_.autoExposureReduceMs;
    timings_.toneMapMs = smooth(timings_.toneMapMs, elapsedMs(ToneMapStart, ToneMapEnd));
    timings_.selectionOutlineMs = smooth(timings_.selectionOutlineMs, elapsedMs(SelectionOutlineStart, SelectionOutlineEnd));
    timings_.fullscreenMs = smooth(timings_.fullscreenMs, elapsedMs(FullscreenStart, FullscreenEnd));
    timings_.editorPresentationMs = smooth(timings_.editorPresentationMs, elapsedMs(EditorPresentationStart, EditorPresentationEnd));
    timings_.wavefrontTraceMs = smooth(timings_.wavefrontTraceMs, elapsedMs(WavefrontTraceStart, WavefrontTraceEnd));
    timings_.wavefrontSecondaryTraceMs = smooth(timings_.wavefrontSecondaryTraceMs, elapsedMs(WavefrontSecondaryTraceStart, WavefrontSecondaryTraceEnd));
    timings_.wavefrontSortedTraceMs = smooth(timings_.wavefrontSortedTraceMs, elapsedMs(WavefrontSortedTraceStart, WavefrontSortedTraceEnd));
    timings_.wavefrontShadowTraceMs = smooth(timings_.wavefrontShadowTraceMs, elapsedMs(WavefrontShadowTraceStart, WavefrontShadowTraceEnd));
    timings_.wavefrontShadeMs = smooth(timings_.wavefrontShadeMs, elapsedMs(WavefrontShadeStart, WavefrontShadeEnd));
    timings_.wavefrontSecondaryShadeMs = smooth(timings_.wavefrontSecondaryShadeMs, elapsedMs(WavefrontSecondaryShadeStart, WavefrontSecondaryShadeEnd));
    timings_.wavefrontSortedShadeMs = smooth(timings_.wavefrontSortedShadeMs, elapsedMs(WavefrontSortedShadeStart, WavefrontSortedShadeEnd));
    timings_.wavefrontCompactMs = smooth(timings_.wavefrontCompactMs, elapsedMs(WavefrontCompactStart, WavefrontCompactEnd));
    timings_.wavefrontSortMs = smooth(timings_.wavefrontSortMs, elapsedMs(WavefrontSortStart, WavefrontSortEnd));
    timings_.queueWaitMs = smooth(timings_.queueWaitMs, elapsedMs(AsyncProducerEnd, AsyncComputeStart));
    timings_.rayTracingLaneMs = timings_.pathTraceMs +
        timings_.wavefrontTraceMs +
        timings_.wavefrontSecondaryTraceMs +
        timings_.wavefrontSortedTraceMs +
        timings_.wavefrontShadowTraceMs;
    timings_.computeLaneMs =
        timings_.restirHistoryClearMs +
        timings_.restirGiClearMs +
        timings_.restirSpatialMs +
        timings_.restirSpatialCopyMs +
        timings_.restirGiSpatialMs +
        timings_.restirGiFinalMs +
        timings_.fogIntegrateMs +
        timings_.atmosphereMs +
        timings_.denoiserMs +
        timings_.momentUpdateMs +
        timings_.historyCopyMs +
        timings_.skipDenoiserCopyMs +
        timings_.taaMs +
        timings_.taaHistoryCopyMs +
        timings_.autoExposureMs +
        timings_.toneMapMs +
        timings_.selectionOutlineMs +
        timings_.wavefrontShadeMs +
        timings_.wavefrontSecondaryShadeMs +
        timings_.wavefrontSortedShadeMs +
        timings_.wavefrontCompactMs +
        timings_.wavefrontSortMs;
    timings_.graphicsLaneMs = timings_.fullscreenMs + timings_.editorPresentationMs;
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
