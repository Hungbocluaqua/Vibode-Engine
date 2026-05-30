#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace rtv {

struct VarianceEstimator {
    struct Input {
        float luminanceVariance = 0.0f;
        float depthDisocclusion = 0.0f;
        float normalDisocclusion = 0.0f;
        float motionPixels = 0.0f;
        bool historyValid = true;
    };

    struct Result {
        float confidence = 0.0f;
        float reactive = 0.0f;
        float blendFactor = 1.0f;
        float perPixelVariance = 0.0f;
    };

    void update(uint64_t frameIndex);
    [[nodiscard]] Result evaluate(const Input& input) const;
    [[nodiscard]] float reactiveWeight(float localContrast, float luminance) const;
    [[nodiscard]] glm::vec3 clampHistoryYCoCg(glm::vec3 history, glm::vec3 minColor, glm::vec3 maxColor, float sigmaLuminance) const;
    void reset();

    [[nodiscard]] uint64_t frameIndex() const { return frameIndex_; }
    [[nodiscard]] float convergenceRate() const { return convergenceRate_; }

private:
    uint64_t frameIndex_ = 0;
    float convergenceRate_ = 0.0f;
};

} // namespace rtv
