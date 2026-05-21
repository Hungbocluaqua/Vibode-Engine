#include "rtv/AtmosphereModel.h"

#include <glm/exponential.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {

constexpr float kPi = 3.14159265358979323846f;

} // namespace

AtmosphereModel::AtmosphereModel(AtmosphereParams params)
    : params_(params) {}

void AtmosphereModel::setParams(AtmosphereParams params) {
    params_ = params;
}

AtmosphereUniform AtmosphereModel::uniform() const {
    AtmosphereUniform out{};
    out.planetRadiusAtmosphereRadius = glm::vec4(params_.planetRadius, params_.atmosphereRadius, 0.0f, 0.0f);
    out.rayleighScattering = glm::vec4(params_.rayleighScattering, 0.0f);
    out.mieScatteringAnisotropy = glm::vec4(params_.mieScattering, params_.miePhaseAnisotropy);
    out.absorptionGroundAlbedo = glm::vec4(params_.ozoneAbsorption, params_.groundAlbedo);
    out.scaleHeights = glm::vec4(params_.rayleighScaleHeight, params_.mieScaleHeight, params_.ozoneLayerCenter, params_.ozoneLayerWidth);
    out.sunIlluminance = glm::vec4(params_.sunIlluminance, 0.0f, 0.0f, 0.0f);
    return out;
}

float AtmosphereModel::rayleighPhase(float cosTheta) const {
    return 3.0f * (1.0f + cosTheta * cosTheta) / (16.0f * kPi);
}

float AtmosphereModel::miePhase(float cosTheta) const {
    const float g = std::clamp(params_.miePhaseAnisotropy, -0.95f, 0.95f);
    const float g2 = g * g;
    const float denom = std::max(std::pow(1.0f + g2 - 2.0f * g * cosTheta, 1.5f), 1.0e-4f);
    return (1.0f - g2) / (4.0f * kPi * denom);
}

bool AtmosphereModel::raySphereIntersection(glm::vec3 origin, glm::vec3 direction, float radius, float& tNear, float& tFar) const {
    const glm::dvec3 o(origin);
    const glm::dvec3 d(glm::normalize(direction));
    const double r = static_cast<double>(radius);
    const double a = glm::dot(d, d);
    const double b = 2.0 * glm::dot(o, d);
    const double c = glm::dot(o, o) - r * r;
    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0) {
        return false;
    }
    const double root = std::sqrt(discriminant);
    const double invDenom = 0.5 / a;
    const double t0 = (-b - root) * invDenom;
    const double t1 = (-b + root) * invDenom;
    tNear = std::max(static_cast<float>(t0), 0.0f);
    tFar = static_cast<float>(t1);
    return tFar >= 0.0f && tFar >= tNear;
}

glm::vec3 AtmosphereModel::computeTransmittance(glm::vec3 worldPos, glm::vec3 direction, int sampleCount) const {
    float tNear = 0.0f;
    float tFar = 0.0f;
    if (!raySphereIntersection(worldPos, direction, params_.atmosphereRadius, tNear, tFar)) {
        return glm::vec3(1.0f);
    }

    const int samples = std::max(sampleCount, 1);
    const glm::vec3 dir = glm::normalize(direction);
    const float segmentLength = (tFar - tNear) / static_cast<float>(samples);
    glm::vec3 opticalDepth(0.0f);
    for (int i = 0; i < samples; ++i) {
        const float t = tNear + (static_cast<float>(i) + 0.5f) * segmentLength;
        const glm::vec3 samplePos = worldPos + dir * t;
        const float height = std::max(glm::length(samplePos) - params_.planetRadius, 0.0f);
        opticalDepth += extinctionAtHeight(height) * segmentLength;
    }
    return glm::exp(-opticalDepth);
}

glm::vec3 AtmosphereModel::extinctionAtHeight(float heightMeters) const {
    const float rayleighDensity = std::exp(-heightMeters / std::max(params_.rayleighScaleHeight, 1.0f));
    const float mieDensity = std::exp(-heightMeters / std::max(params_.mieScaleHeight, 1.0f));
    const float ozoneDistance = std::abs(heightMeters - params_.ozoneLayerCenter);
    const float ozoneDensity = std::max(1.0f - ozoneDistance / std::max(params_.ozoneLayerWidth, 1.0f), 0.0f);
    return params_.rayleighScattering * rayleighDensity +
        params_.mieScattering * mieDensity +
        params_.ozoneAbsorption * ozoneDensity;
}

} // namespace rtv
