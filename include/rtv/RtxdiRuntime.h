#pragma once

#include "rtv/RtxdiSettings.h"

#include <Rtxdi/ImportanceSamplingContext.h>

#include <cstdint>
#include <memory>

namespace rtv {

struct RtxdiRuntimeConfig {
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    RtxdiQualityPreset qualityPreset = RtxdiQualityPreset::Medium;
    bool checkerboard = false;
};

struct RtxdiMemoryRequirements {
    uint64_t diReservoirBytes = 0;
    uint64_t giReservoirBytes = 0;
    uint64_t ptReservoirBytes = 0;
    uint64_t totalReservoirBytes = 0;
};

struct RtxdiRuntimeParameters {
    uint32_t diLocalLightSamples = 0;
    uint32_t diBrdfSamples = 0;
    uint32_t diInfiniteLightSamples = 0;
    uint32_t diEnvironmentSamples = 0;
    uint32_t diReservoirBlockRowPitch = 0;
    uint32_t diReservoirArrayPitch = 0;
    uint32_t giReservoirBlockRowPitch = 0;
    uint32_t giReservoirArrayPitch = 0;
    uint32_t ptReservoirBlockRowPitch = 0;
    uint32_t ptReservoirArrayPitch = 0;
    uint32_t diTemporalHistoryLength = 0;
    uint32_t diSpatialSamples = 0;
    float diSpatialRadius = 0.0f;
    float diDepthThreshold = 0.0f;
    float diNormalThreshold = 0.0f;
    uint32_t giTemporalHistoryLength = 0;
    uint32_t giMaxReservoirAge = 0;
    uint32_t giSpatialSamples = 0;
    float giSpatialRadius = 0.0f;
    float giDepthThreshold = 0.0f;
    float giNormalThreshold = 0.0f;
    uint32_t ptTemporalHistoryLength = 0;
    uint32_t ptMaxReservoirAge = 0;
    uint32_t ptSpatialSamples = 0;
    uint32_t ptMaxBounceDepth = 0;
    uint32_t ptMaxRcVertexLength = 0;
    uint32_t activeCheckerboardField = 0;
};

class RtxdiRuntime {
public:
    explicit RtxdiRuntime(const RtxdiRuntimeConfig& config);

    void recreate(const RtxdiRuntimeConfig& config);
    void beginFrame(uint32_t frameIndex);

    [[nodiscard]] const RtxdiRuntimeConfig& config() const { return config_; }
    [[nodiscard]] RtxdiMemoryRequirements memoryRequirements() const;
    [[nodiscard]] RtxdiRuntimeParameters runtimeParameters() const;

    [[nodiscard]] rtxdi::ImportanceSamplingContext& context() { return *context_; }
    [[nodiscard]] const rtxdi::ImportanceSamplingContext& context() const { return *context_; }

private:
    void applyQualityPreset();

    RtxdiRuntimeConfig config_{};
    std::unique_ptr<rtxdi::ImportanceSamplingContext> context_;
};

} // namespace rtv
