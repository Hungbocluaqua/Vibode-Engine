#include "rtv/GpuProfiler.h"

#include "rtv/Check.h"
#include "rtv/NsightMarkers.h"
#include "rtv/NsightPerfMarkers.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
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

namespace {

const char* gpuMarkerBeginLabel(GpuProfiler::Query query) {
    switch (query) {
    case GpuProfiler::PathTraceStart: return "PathTrace";
    case GpuProfiler::RestirHistoryClearStart: return "ReSTIR History Clear";
    case GpuProfiler::RestirGiClearStart: return "ReSTIR GI Clear";
    case GpuProfiler::RestirGiTemporalStart: return "ReSTIR GI Temporal";
    case GpuProfiler::RestirSpatialStart: return "ReSTIR Spatial";
    case GpuProfiler::RestirSpatialCopyStart: return "ReSTIR Spatial Copy";
    case GpuProfiler::RestirGiSpatialStart: return "ReSTIR GI Spatial";
    case GpuProfiler::RestirGiUpsampleStart: return "ReSTIR GI Upsample";
    case GpuProfiler::RestirGiFinalStart: return "ReSTIR GI Final";
    case GpuProfiler::RestirGiCountersReadbackStart: return "ReSTIR GI Counters Readback";
    case GpuProfiler::RestirDiTemporalStart: return "ReSTIR DI Temporal";
    case GpuProfiler::RestirDiSpatialStart: return "ReSTIR DI Spatial";
    case GpuProfiler::RestirDiFinalStart: return "ReSTIR DI Final";
    case GpuProfiler::RestirDiHistoryCopyStart: return "ReSTIR DI History Copy";
    case GpuProfiler::RestirDiCountersReadbackStart: return "ReSTIR DI Counters Readback";
    case GpuProfiler::FogIntegrateStart: return "Fog Integrate";
    case GpuProfiler::AtmosphereStart: return "Atmosphere";
    case GpuProfiler::AtmosphereTransmittanceStart: return "Atmosphere Transmittance";
    case GpuProfiler::AtmosphereMultiScatterStart: return "Atmosphere Multi-Scatter";
    case GpuProfiler::AtmosphereSkyViewStart: return "Atmosphere Sky View";
    case GpuProfiler::AtmosphereSkyReprojectStart: return "Atmosphere Sky Reproject";
    case GpuProfiler::AtmosphereSkyCdfStart: return "Atmosphere Sky CDF";
    case GpuProfiler::AtmosphereAerialPerspectiveStart: return "Atmosphere Aerial Perspective";
    case GpuProfiler::DenoiserStart: return "Denoiser";
    case GpuProfiler::MomentUpdateStart: return "Moment Update";
    case GpuProfiler::HistoryCopyStart: return "History Copy";
    case GpuProfiler::SkipDenoiserCopyStart: return "Skip Denoiser Copy";
    case GpuProfiler::TaaStart: return "TAA/TSR";
    case GpuProfiler::TaaHistoryCopyStart: return "TAA History Copy";
    case GpuProfiler::AutoExposureHistogramClearStart: return "AutoExposure Histogram Clear";
    case GpuProfiler::AutoExposureHistogramStart: return "AutoExposure Histogram";
    case GpuProfiler::AutoExposureReduceStart: return "AutoExposure Reduce";
    case GpuProfiler::ToneMapStart: return "ToneMap";
    case GpuProfiler::SelectionOutlineStart: return "Selection Outline";
    case GpuProfiler::FullscreenStart: return "Fullscreen/Present";
    case GpuProfiler::EditorPresentationStart: return "Editor Presentation";
    case GpuProfiler::WavefrontShadowTraceStart: return "Wavefront Shadow Trace";
    case GpuProfiler::WavefrontCompactStart: return "Wavefront Compact";
    case GpuProfiler::WavefrontSortStart: return "Wavefront Sort";
    case GpuProfiler::WavefrontShadeStart: return "Wavefront Shade";
    case GpuProfiler::WavefrontSortedShadeStart: return "Wavefront Sorted Shade";
    case GpuProfiler::WavefrontSecondaryShadeStart: return "Wavefront Secondary Shade";
    case GpuProfiler::WavefrontTraceStart: return "Wavefront Trace";
    case GpuProfiler::WavefrontSecondaryTraceStart: return "Wavefront Secondary Trace";
    case GpuProfiler::WavefrontSortedTraceStart: return "Wavefront Sorted Trace";
    case GpuProfiler::DynamicBlasUpdateStart: return "BLAS/TLAS Update";
    case GpuProfiler::AsyncComputeStart: return "Async Compute";
    default: return nullptr;
    }
}

bool gpuMarkerEndsLabel(GpuProfiler::Query query) {
    switch (query) {
    case GpuProfiler::PathTraceEnd:
    case GpuProfiler::RestirHistoryClearEnd:
    case GpuProfiler::RestirGiClearEnd:
    case GpuProfiler::RestirGiTemporalEnd:
    case GpuProfiler::RestirSpatialEnd:
    case GpuProfiler::RestirSpatialCopyEnd:
    case GpuProfiler::RestirGiSpatialEnd:
    case GpuProfiler::RestirGiUpsampleEnd:
    case GpuProfiler::RestirGiFinalEnd:
    case GpuProfiler::RestirGiCountersReadbackEnd:
    case GpuProfiler::RestirDiTemporalEnd:
    case GpuProfiler::RestirDiSpatialEnd:
    case GpuProfiler::RestirDiFinalEnd:
    case GpuProfiler::RestirDiHistoryCopyEnd:
    case GpuProfiler::RestirDiCountersReadbackEnd:
    case GpuProfiler::FogIntegrateEnd:
    case GpuProfiler::AtmosphereEnd:
    case GpuProfiler::AtmosphereTransmittanceEnd:
    case GpuProfiler::AtmosphereMultiScatterEnd:
    case GpuProfiler::AtmosphereSkyViewEnd:
    case GpuProfiler::AtmosphereSkyReprojectEnd:
    case GpuProfiler::AtmosphereSkyCdfEnd:
    case GpuProfiler::AtmosphereAerialPerspectiveEnd:
    case GpuProfiler::DenoiserEnd:
    case GpuProfiler::MomentUpdateEnd:
    case GpuProfiler::HistoryCopyEnd:
    case GpuProfiler::SkipDenoiserCopyEnd:
    case GpuProfiler::TaaEnd:
    case GpuProfiler::TaaHistoryCopyEnd:
    case GpuProfiler::AutoExposureHistogramClearEnd:
    case GpuProfiler::AutoExposureHistogramEnd:
    case GpuProfiler::AutoExposureReduceEnd:
    case GpuProfiler::ToneMapEnd:
    case GpuProfiler::SelectionOutlineEnd:
    case GpuProfiler::FullscreenEnd:
    case GpuProfiler::EditorPresentationEnd:
    case GpuProfiler::WavefrontShadowTraceEnd:
    case GpuProfiler::WavefrontCompactEnd:
    case GpuProfiler::WavefrontSortEnd:
    case GpuProfiler::WavefrontShadeEnd:
    case GpuProfiler::WavefrontSortedShadeEnd:
    case GpuProfiler::WavefrontSecondaryShadeEnd:
    case GpuProfiler::WavefrontTraceEnd:
    case GpuProfiler::WavefrontSecondaryTraceEnd:
    case GpuProfiler::WavefrontSortedTraceEnd:
    case GpuProfiler::DynamicBlasUpdateEnd:
    case GpuProfiler::AsyncComputeEnd:
        return true;
    default:
        return false;
    }
}

void beginGpuMarker(VkCommandBuffer commandBuffer, const char* label) {
    if (label == nullptr || vkCmdBeginDebugUtilsLabelEXT == nullptr) {
        return;
    }
    VkDebugUtilsLabelEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    info.pLabelName = label;
    info.color[0] = 0.21f;
    info.color[1] = 0.53f;
    info.color[2] = 0.88f;
    info.color[3] = 1.0f;
    vkCmdBeginDebugUtilsLabelEXT(commandBuffer, &info);
}

void endGpuMarker(VkCommandBuffer commandBuffer) {
    if (vkCmdEndDebugUtilsLabelEXT != nullptr) {
        vkCmdEndDebugUtilsLabelEXT(commandBuffer);
    }
}

} // namespace

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
        gpuMarkersEnabled_ = other.gpuMarkersEnabled_;
        activeQueries_ = other.activeQueries_;
        timings_ = other.timings_;
        pipelineStats_ = other.pipelineStats_;
        smoothedPipelineStats_ = other.smoothedPipelineStats_;
        other.device_ = VK_NULL_HANDLE;
        other.queryPool_ = VK_NULL_HANDLE;
        other.statsQueryPool_ = VK_NULL_HANDLE;
        other.submitted_ = false;
        other.statsSubmitted_ = false;
        other.activeQueries_.fill(false);
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
    activeQueries_.fill(false);
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

    const auto elapsedMs = [&](Query begin, Query end) -> std::optional<float> {
        if (!activeQueries_[begin] || !activeQueries_[end]) {
            return std::nullopt;
        }
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
    const auto updateTiming = [&](float& dst, Query begin, Query end) {
        if (std::optional<float> current = elapsedMs(begin, end)) {
            dst = smooth(dst, *current);
        } else {
            dst = 0.0f;
        }
    };
    updateTiming(timings_.pathTraceMs, PathTraceStart, PathTraceEnd);
    updateTiming(timings_.restirHistoryClearMs, RestirHistoryClearStart, RestirHistoryClearEnd);
    updateTiming(timings_.restirGiClearMs, RestirGiClearStart, RestirGiClearEnd);
    updateTiming(timings_.restirGiTemporalMs, RestirGiTemporalStart, RestirGiTemporalEnd);
    updateTiming(timings_.restirSpatialMs, RestirSpatialStart, RestirSpatialEnd);
    updateTiming(timings_.restirSpatialCopyMs, RestirSpatialCopyStart, RestirSpatialCopyEnd);
    updateTiming(timings_.restirGiSpatialMs, RestirGiSpatialStart, RestirGiSpatialEnd);
    updateTiming(timings_.restirGiUpsampleMs, RestirGiUpsampleStart, RestirGiUpsampleEnd);
    updateTiming(timings_.restirGiFinalMs, RestirGiFinalStart, RestirGiFinalEnd);
    updateTiming(timings_.restirGiCountersReadbackMs, RestirGiCountersReadbackStart, RestirGiCountersReadbackEnd);
    updateTiming(timings_.restirDiTemporalMs, RestirDiTemporalStart, RestirDiTemporalEnd);
    updateTiming(timings_.restirDiSpatialMs, RestirDiSpatialStart, RestirDiSpatialEnd);
    updateTiming(timings_.restirDiFinalMs, RestirDiFinalStart, RestirDiFinalEnd);
    updateTiming(timings_.restirDiHistoryCopyMs, RestirDiHistoryCopyStart, RestirDiHistoryCopyEnd);
    updateTiming(timings_.restirDiCountersReadbackMs, RestirDiCountersReadbackStart, RestirDiCountersReadbackEnd);
    updateTiming(timings_.fogIntegrateMs, FogIntegrateStart, FogIntegrateEnd);
    updateTiming(timings_.atmosphereMs, AtmosphereStart, AtmosphereEnd);
    updateTiming(timings_.atmosphereTransmittanceMs, AtmosphereTransmittanceStart, AtmosphereTransmittanceEnd);
    updateTiming(timings_.atmosphereMultiScatterMs, AtmosphereMultiScatterStart, AtmosphereMultiScatterEnd);
    updateTiming(timings_.atmosphereSkyViewMs, AtmosphereSkyViewStart, AtmosphereSkyViewEnd);
    updateTiming(timings_.atmosphereSkyReprojectMs, AtmosphereSkyReprojectStart, AtmosphereSkyReprojectEnd);
    updateTiming(timings_.atmosphereSkyCdfMs, AtmosphereSkyCdfStart, AtmosphereSkyCdfEnd);
    updateTiming(timings_.atmosphereAerialPerspectiveMs, AtmosphereAerialPerspectiveStart, AtmosphereAerialPerspectiveEnd);
    updateTiming(timings_.denoiserMs, DenoiserStart, DenoiserEnd);
    updateTiming(timings_.momentUpdateMs, MomentUpdateStart, MomentUpdateEnd);
    updateTiming(timings_.historyCopyMs, HistoryCopyStart, HistoryCopyEnd);
    updateTiming(timings_.skipDenoiserCopyMs, SkipDenoiserCopyStart, SkipDenoiserCopyEnd);
    updateTiming(timings_.taaMs, TaaStart, TaaEnd);
    updateTiming(timings_.taaHistoryCopyMs, TaaHistoryCopyStart, TaaHistoryCopyEnd);
    updateTiming(timings_.autoExposureHistogramClearMs, AutoExposureHistogramClearStart, AutoExposureHistogramClearEnd);
    updateTiming(timings_.autoExposureHistogramMs, AutoExposureHistogramStart, AutoExposureHistogramEnd);
    updateTiming(timings_.autoExposureReduceMs, AutoExposureReduceStart, AutoExposureReduceEnd);
    timings_.autoExposureMs = timings_.autoExposureHistogramClearMs + timings_.autoExposureHistogramMs + timings_.autoExposureReduceMs;
    updateTiming(timings_.toneMapMs, ToneMapStart, ToneMapEnd);
    updateTiming(timings_.selectionOutlineMs, SelectionOutlineStart, SelectionOutlineEnd);
    updateTiming(timings_.fullscreenMs, FullscreenStart, FullscreenEnd);
    updateTiming(timings_.editorPresentationMs, EditorPresentationStart, EditorPresentationEnd);
    updateTiming(timings_.dynamicBlasUpdateMs, DynamicBlasUpdateStart, DynamicBlasUpdateEnd);
    updateTiming(timings_.wavefrontTraceMs, WavefrontTraceStart, WavefrontTraceEnd);
    updateTiming(timings_.wavefrontSecondaryTraceMs, WavefrontSecondaryTraceStart, WavefrontSecondaryTraceEnd);
    updateTiming(timings_.wavefrontSortedTraceMs, WavefrontSortedTraceStart, WavefrontSortedTraceEnd);
    updateTiming(timings_.wavefrontShadowTraceMs, WavefrontShadowTraceStart, WavefrontShadowTraceEnd);
    updateTiming(timings_.wavefrontShadeMs, WavefrontShadeStart, WavefrontShadeEnd);
    updateTiming(timings_.wavefrontSecondaryShadeMs, WavefrontSecondaryShadeStart, WavefrontSecondaryShadeEnd);
    updateTiming(timings_.wavefrontSortedShadeMs, WavefrontSortedShadeStart, WavefrontSortedShadeEnd);
    updateTiming(timings_.wavefrontCompactMs, WavefrontCompactStart, WavefrontCompactEnd);
    updateTiming(timings_.wavefrontSortMs, WavefrontSortStart, WavefrontSortEnd);
    updateTiming(timings_.queueWaitMs, AsyncProducerEnd, AsyncComputeStart);
    timings_.rayTracingLaneMs = timings_.pathTraceMs +
        timings_.dynamicBlasUpdateMs +
        timings_.wavefrontTraceMs +
        timings_.wavefrontSecondaryTraceMs +
        timings_.wavefrontSortedTraceMs +
        timings_.wavefrontShadowTraceMs;
    timings_.computeLaneMs =
        timings_.restirHistoryClearMs +
        timings_.restirGiClearMs +
        timings_.restirGiTemporalMs +
        timings_.restirSpatialMs +
        timings_.restirSpatialCopyMs +
        timings_.restirGiSpatialMs +
        timings_.restirGiUpsampleMs +
        timings_.restirGiFinalMs +
        timings_.restirGiCountersReadbackMs +
        timings_.restirDiTemporalMs +
        timings_.restirDiSpatialMs +
        timings_.restirDiFinalMs +
        timings_.restirDiHistoryCopyMs +
        timings_.restirDiCountersReadbackMs +
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
    activeQueries_.fill(false);
    if (statsQueryPool_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commandBuffer, statsQueryPool_, 0, 1);
    }
}

void GpuProfiler::write(VkCommandBuffer commandBuffer, Query query, VkPipelineStageFlagBits2 stage) {
    if (gpuMarkersEnabled_) {
        const char* label = gpuMarkerBeginLabel(query);
        beginGpuMarker(commandBuffer, label);
        beginNsightRange(label);
        (void)beginNsightPerfCommandBufferRange(commandBuffer, label);
    }
    if (queryPool_ != VK_NULL_HANDLE) {
        activeQueries_[query] = true;
        vkCmdWriteTimestamp2(commandBuffer, stage, queryPool_, query);
    }
    if (gpuMarkersEnabled_ && gpuMarkerEndsLabel(query)) {
        endGpuMarker(commandBuffer);
        endNsightRange();
        (void)endNsightPerfCommandBufferRange(commandBuffer);
    }
}

void GpuProfiler::setGpuMarkersEnabled(bool enabled) {
    gpuMarkersEnabled_ = enabled;
    setNsightMarkersEnabled(enabled);
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
