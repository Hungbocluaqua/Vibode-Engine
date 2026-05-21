#pragma once

#include <glm/glm.hpp>

namespace rtv {

struct AtmosphereParams {
    float planetRadius = 6'360'000.0f;
    float atmosphereRadius = 6'420'000.0f;
    float rayleighScaleHeight = 8'000.0f;
    float mieScaleHeight = 1'200.0f;
    glm::vec3 rayleighScattering{5.802e-6f, 13.558e-6f, 33.100e-6f};
    glm::vec3 mieScattering{3.996e-6f};
    glm::vec3 ozoneAbsorption{0.650e-6f, 1.881e-6f, 0.085e-6f};
    float ozoneLayerCenter = 25'000.0f;
    float ozoneLayerWidth = 15'000.0f;
    float miePhaseAnisotropy = 0.8f;
    float groundAlbedo = 0.3f;
    float sunIlluminance = 120'000.0f;
};

struct AtmosphereUniform {
    glm::vec4 planetRadiusAtmosphereRadius{};
    glm::vec4 rayleighScattering{};
    glm::vec4 mieScatteringAnisotropy{};
    glm::vec4 absorptionGroundAlbedo{};
    glm::vec4 scaleHeights{};
    glm::vec4 sunIlluminance{};
};

class AtmosphereModel {
public:
    explicit AtmosphereModel(AtmosphereParams params = {});

    [[nodiscard]] const AtmosphereParams& params() const { return params_; }
    void setParams(AtmosphereParams params);

    [[nodiscard]] AtmosphereUniform uniform() const;
    [[nodiscard]] float rayleighPhase(float cosTheta) const;
    [[nodiscard]] float miePhase(float cosTheta) const;
    [[nodiscard]] glm::vec3 computeTransmittance(glm::vec3 worldPos, glm::vec3 direction, int sampleCount = 16) const;
    [[nodiscard]] bool raySphereIntersection(glm::vec3 origin, glm::vec3 direction, float radius, float& tNear, float& tFar) const;

private:
    [[nodiscard]] glm::vec3 extinctionAtHeight(float heightMeters) const;

    AtmosphereParams params_{};
};

} // namespace rtv
