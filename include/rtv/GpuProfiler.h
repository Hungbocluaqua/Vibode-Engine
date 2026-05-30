#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <array>
#include <cstdint>

namespace rtv {

struct GpuFrameTimings {
    float pathTraceMs = 0.0f;
    float restirHistoryClearMs = 0.0f;
    float restirGiClearMs = 0.0f;
    float restirSpatialMs = 0.0f;
    float restirSpatialCopyMs = 0.0f;
    float restirGiSpatialMs = 0.0f;
    float restirGiFinalMs = 0.0f;
    float fogIntegrateMs = 0.0f;
    float atmosphereMs = 0.0f;
    float atmosphereTransmittanceMs = 0.0f;
    float atmosphereMultiScatterMs = 0.0f;
    float atmosphereSkyViewMs = 0.0f;
    float atmosphereSkyReprojectMs = 0.0f;
    float atmosphereSkyCdfMs = 0.0f;
    float atmosphereAerialPerspectiveMs = 0.0f;
    float denoiserMs = 0.0f;
    float momentUpdateMs = 0.0f;
    float historyCopyMs = 0.0f;
    float skipDenoiserCopyMs = 0.0f;
    float taaMs = 0.0f;
    float taaHistoryCopyMs = 0.0f;
    float autoExposureMs = 0.0f;
    float autoExposureHistogramClearMs = 0.0f;
    float autoExposureHistogramMs = 0.0f;
    float autoExposureReduceMs = 0.0f;
    float toneMapMs = 0.0f;
    float selectionOutlineMs = 0.0f;
    float fullscreenMs = 0.0f;
    float editorPresentationMs = 0.0f;
    float wavefrontTraceMs = 0.0f;
    float wavefrontSecondaryTraceMs = 0.0f;
    float wavefrontSortedTraceMs = 0.0f;
    float wavefrontShadowTraceMs = 0.0f;
    float wavefrontShadeMs = 0.0f;
    float wavefrontSecondaryShadeMs = 0.0f;
    float wavefrontSortedShadeMs = 0.0f;
    float wavefrontCompactMs = 0.0f;
    float wavefrontSortMs = 0.0f;
    float graphicsLaneMs = 0.0f;
    float rayTracingLaneMs = 0.0f;
    float computeLaneMs = 0.0f;
    float queueWaitMs = 0.0f;

    [[nodiscard]] float totalMs() const {
        return pathTraceMs +
            restirHistoryClearMs +
            restirGiClearMs +
            restirSpatialMs +
            restirSpatialCopyMs +
            restirGiSpatialMs +
            restirGiFinalMs +
            fogIntegrateMs +
            atmosphereMs +
            denoiserMs +
            momentUpdateMs +
            historyCopyMs +
            skipDenoiserCopyMs +
            taaMs +
            taaHistoryCopyMs +
            autoExposureMs +
            toneMapMs +
            selectionOutlineMs +
            fullscreenMs +
            editorPresentationMs +
            wavefrontTraceMs +
            wavefrontSecondaryTraceMs +
            wavefrontSortedTraceMs +
            wavefrontShadowTraceMs +
            wavefrontShadeMs +
            wavefrontSecondaryShadeMs +
            wavefrontSortedShadeMs +
            wavefrontCompactMs +
            wavefrontSortMs;
    }
};

struct GpuPipelineStatistics {
    uint64_t rayInvocations = 0;
    uint64_t triangleHits = 0;
    uint64_t aabbHits = 0;
    bool valid = false;
};

class GpuProfiler final : private NonCopyable {
public:
    enum Query : uint32_t {
        PathTraceStart = 0,
        PathTraceEnd = 1,
        RestirHistoryClearStart = 2,
        RestirHistoryClearEnd = 3,
        RestirGiClearStart = 4,
        RestirGiClearEnd = 5,
        RestirSpatialStart = 6,
        RestirSpatialEnd = 7,
        RestirSpatialCopyStart = 8,
        RestirSpatialCopyEnd = 9,
        RestirGiSpatialStart = 10,
        RestirGiSpatialEnd = 11,
        RestirGiFinalStart = 12,
        RestirGiFinalEnd = 13,
        FogIntegrateStart = 14,
        FogIntegrateEnd = 15,
        AtmosphereStart = 16,
        AtmosphereEnd = 17,
        AtmosphereTransmittanceStart = 18,
        AtmosphereTransmittanceEnd = 19,
        AtmosphereMultiScatterStart = 20,
        AtmosphereMultiScatterEnd = 21,
        AtmosphereSkyViewStart = 22,
        AtmosphereSkyViewEnd = 23,
        AtmosphereSkyReprojectStart = 24,
        AtmosphereSkyReprojectEnd = 25,
        AtmosphereSkyCdfStart = 26,
        AtmosphereSkyCdfEnd = 27,
        AtmosphereAerialPerspectiveStart = 28,
        AtmosphereAerialPerspectiveEnd = 29,
        DenoiserStart = 30,
        DenoiserEnd = 31,
        HistoryCopyStart = 32,
        HistoryCopyEnd = 33,
        SkipDenoiserCopyStart = 34,
        SkipDenoiserCopyEnd = 35,
        TaaStart = 36,
        TaaEnd = 37,
        TaaHistoryCopyStart = 38,
        TaaHistoryCopyEnd = 39,
        AutoExposureHistogramClearStart = 40,
        AutoExposureHistogramClearEnd = 41,
        AutoExposureHistogramStart = 42,
        AutoExposureHistogramEnd = 43,
        AutoExposureReduceStart = 44,
        AutoExposureReduceEnd = 45,
        ToneMapStart = 46,
        ToneMapEnd = 47,
        SelectionOutlineStart = 48,
        SelectionOutlineEnd = 49,
        FullscreenStart = 50,
        FullscreenEnd = 51,
        EditorPresentationStart = 52,
        EditorPresentationEnd = 53,
        MomentUpdateStart = 54,
        MomentUpdateEnd = 55,
        AsyncProducerEnd = 56,
        AsyncComputeStart = 57,
        AsyncComputeEnd = 58,
        WavefrontShadowTraceStart = 59,
        WavefrontShadowTraceEnd = 60,
        WavefrontCompactStart = 61,
        WavefrontCompactEnd = 62,
        WavefrontSortStart = 63,
        WavefrontSortEnd = 64,
        WavefrontShadeStart = 65,
        WavefrontShadeEnd = 66,
        WavefrontSortedShadeStart = 67,
        WavefrontSortedShadeEnd = 68,
        WavefrontSecondaryShadeStart = 69,
        WavefrontSecondaryShadeEnd = 70,
        WavefrontTraceStart = 71,
        WavefrontTraceEnd = 72,
        WavefrontSecondaryTraceStart = 73,
        WavefrontSecondaryTraceEnd = 74,
        WavefrontSortedTraceStart = 75,
        WavefrontSortedTraceEnd = 76,
        Count = 77,
    };

    GpuProfiler() = default;
    GpuProfiler(VkDevice device, VkPhysicalDevice physicalDevice);
    ~GpuProfiler();

    GpuProfiler(GpuProfiler&& other) noexcept;
    GpuProfiler& operator=(GpuProfiler&& other) noexcept;

    void create(VkDevice device, VkPhysicalDevice physicalDevice);
    void createPipelineStatsQuery(VkDevice device, bool rayTracingAvailable);
    void destroy();

    void collectCompletedFrame();
    void resetForFrame(VkCommandBuffer commandBuffer);
    void write(VkCommandBuffer commandBuffer, Query query, VkPipelineStageFlagBits2 stage) const;
    void beginPipelineStats(VkCommandBuffer commandBuffer) const;
    void endPipelineStats(VkCommandBuffer commandBuffer) const;
    void markSubmitted() { submitted_ = true; }
    void markStatsSubmitted() { statsSubmitted_ = true; }

    [[nodiscard]] const GpuFrameTimings& timings() const { return timings_; }
    [[nodiscard]] const GpuPipelineStatistics& pipelineStats() const { return pipelineStats_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    VkQueryPool statsQueryPool_ = VK_NULL_HANDLE;
    float timestampPeriod_ = 1.0f;
    bool submitted_ = false;
    bool statsSubmitted_ = false;
    GpuFrameTimings timings_{};
    GpuPipelineStatistics pipelineStats_{};
    GpuPipelineStatistics smoothedPipelineStats_{};
};

} // namespace rtv
