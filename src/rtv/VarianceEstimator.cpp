#include "rtv/VarianceEstimator.h"

#include <algorithm>
#include <cmath>

namespace rtv {

void VarianceEstimator::update(uint64_t frameIndex) {
    constexpr float alpha = 0.125f;
    convergenceRate_ = 1.0f - std::pow(1.0f - alpha, static_cast<float>(frameIndex - frameIndex_ + 1));
    frameIndex_ = frameIndex;
}

VarianceEstimator::Result VarianceEstimator::evaluate(const Input& input) const {
    Result result;
    result.blendFactor = 1.0f;

    if (!input.historyValid) {
        return result;
    }

    const float varianceScale = 2.0f;
    const float depthScale = 8.0f;
    const float normalScale = 4.0f;

    float lumConf = std::exp(-input.luminanceVariance * varianceScale);
    float depthConf = std::exp(-input.depthDisocclusion * depthScale);
    float normalConf = std::exp(-input.normalDisocclusion * normalScale);

    result.confidence = lumConf * depthConf * normalConf;
    result.perPixelVariance = input.luminanceVariance;

    result.reactive = reactiveWeight(input.luminanceVariance, input.luminanceVariance);

    if (result.confidence < 0.02f) {
        result.blendFactor = 0.0f;
    } else if (result.confidence < 0.1f) {
        result.blendFactor = result.confidence / 0.1f;
    }

    return result;
}

float VarianceEstimator::reactiveWeight(float localContrast, float luminance) const {
    const float contrastWeight = std::clamp(localContrast, 0.0f, 1.0f);
    const float luminanceFactor = luminance > 0.01f ? (1.0f / std::sqrt(luminance + 0.01f)) : 1.0f;
    return contrastWeight * luminanceFactor;
}

glm::vec3 VarianceEstimator::clampHistoryYCoCg(glm::vec3 history, glm::vec3 minColor, glm::vec3 maxColor, float sigmaLuminance) const {
    const float co = history.x * 0.5f + history.z * 0.5f;
    const float cg = history.x * 0.25f + history.y * 0.5f - history.z * 0.25f;
    const float y = history.x * 0.25f - history.y * 0.5f + history.z * 0.25f + history.x;

    const float coMin = minColor.x * 0.5f + minColor.z * 0.5f;
    const float coMax = maxColor.x * 0.5f + maxColor.z * 0.5f;
    const float cgMin = minColor.x * 0.25f + minColor.y * 0.5f - minColor.z * 0.25f;
    const float cgMax = maxColor.x * 0.25f + maxColor.y * 0.5f - maxColor.z * 0.25f;
    const float yMin = minColor.x * 0.25f - minColor.y * 0.5f + minColor.z * 0.25f + minColor.x;
    const float yMax = maxColor.x * 0.25f - maxColor.y * 0.5f + maxColor.z * 0.25f + maxColor.x;

    const float lumSigmaCo = sigmaLuminance * 0.5f;
    const float lumSigmaCg = sigmaLuminance * 0.25f;
    const float lumSigmaY = sigmaLuminance * 0.25f;

    const float clampedY = std::clamp(y, yMin - lumSigmaY, yMax + lumSigmaY);
    const float clampedCo = std::clamp(co, coMin - lumSigmaCo, coMax + lumSigmaCo);
    const float clampedCg = std::clamp(cg, cgMin - lumSigmaCg, cgMax + lumSigmaCg);

    return glm::vec3(
        clampedY + clampedCo + clampedCg,
        clampedY - clampedCo,
        clampedY + clampedCo - clampedCg
    );
}

void VarianceEstimator::reset() {
    frameIndex_ = 0;
    convergenceRate_ = 0.0f;
}

} // namespace rtv
