#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <array>
#include <cstdint>

namespace rtv {

struct GpuFrameTimings {
    float pathTraceMs = 0.0f;
    float denoiserMs = 0.0f;
    float historyCopyMs = 0.0f;
    float taaMs = 0.0f;
    float autoExposureMs = 0.0f;
    float toneMapMs = 0.0f;
    float selectionOutlineMs = 0.0f;
    float fullscreenMs = 0.0f;
};

class GpuProfiler final : private NonCopyable {
public:
    enum Query : uint32_t {
        PathTraceStart = 0,
        PathTraceEnd = 1,
        DenoiserStart = 2,
        DenoiserEnd = 3,
        HistoryCopyStart = 4,
        HistoryCopyEnd = 5,
        TaaStart = 6,
        TaaEnd = 7,
        AutoExposureStart = 8,
        AutoExposureEnd = 9,
        ToneMapStart = 10,
        ToneMapEnd = 11,
        SelectionOutlineStart = 12,
        SelectionOutlineEnd = 13,
        FullscreenStart = 14,
        FullscreenEnd = 15,
        Count = 16,
    };

    GpuProfiler() = default;
    GpuProfiler(VkDevice device, VkPhysicalDevice physicalDevice);
    ~GpuProfiler();

    GpuProfiler(GpuProfiler&& other) noexcept;
    GpuProfiler& operator=(GpuProfiler&& other) noexcept;

    void create(VkDevice device, VkPhysicalDevice physicalDevice);
    void destroy();

    void collectCompletedFrame();
    void resetForFrame(VkCommandBuffer commandBuffer);
    void write(VkCommandBuffer commandBuffer, Query query, VkPipelineStageFlagBits2 stage) const;
    void markSubmitted() { submitted_ = true; }

    [[nodiscard]] const GpuFrameTimings& timings() const { return timings_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    float timestampPeriod_ = 1.0f;
    bool submitted_ = false;
    GpuFrameTimings timings_{};
};

} // namespace rtv
