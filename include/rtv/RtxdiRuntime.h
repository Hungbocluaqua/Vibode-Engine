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

class RtxdiRuntime {
public:
    explicit RtxdiRuntime(const RtxdiRuntimeConfig& config);

    void recreate(const RtxdiRuntimeConfig& config);
    void beginFrame(uint32_t frameIndex);

    [[nodiscard]] const RtxdiRuntimeConfig& config() const { return config_; }
    [[nodiscard]] RtxdiMemoryRequirements memoryRequirements() const;

    [[nodiscard]] rtxdi::ImportanceSamplingContext& context() { return *context_; }
    [[nodiscard]] const rtxdi::ImportanceSamplingContext& context() const { return *context_; }

private:
    void applyQualityPreset();

    RtxdiRuntimeConfig config_{};
    std::unique_ptr<rtxdi::ImportanceSamplingContext> context_;
};

} // namespace rtv
