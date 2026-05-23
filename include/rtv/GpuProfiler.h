#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <array>
#include <cstdint>

namespace rtv {

struct GpuFrameTimings {
    float pathTraceMs = 0.0f;
    float restirSpatialMs = 0.0f;
    float fogIntegrateMs = 0.0f;
    float atmosphereMs = 0.0f;
    float denoiserMs = 0.0f;
    float historyCopyMs = 0.0f;
    float taaMs = 0.0f;
    float autoExposureMs = 0.0f;
    float toneMapMs = 0.0f;
    float selectionOutlineMs = 0.0f;
    float fullscreenMs = 0.0f;
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
        RestirSpatialStart = 2,
        RestirSpatialEnd = 3,
        FogIntegrateStart = 4,
        FogIntegrateEnd = 5,
        AtmosphereStart = 6,
        AtmosphereEnd = 7,
        DenoiserStart = 8,
        DenoiserEnd = 9,
        HistoryCopyStart = 10,
        HistoryCopyEnd = 11,
        TaaStart = 12,
        TaaEnd = 13,
        AutoExposureStart = 14,
        AutoExposureEnd = 15,
        ToneMapStart = 16,
        ToneMapEnd = 17,
        SelectionOutlineStart = 18,
        SelectionOutlineEnd = 19,
        FullscreenStart = 20,
        FullscreenEnd = 21,
        Count = 22,
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
