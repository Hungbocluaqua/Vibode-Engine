#pragma once

#include "rtv/EntityId.h"
#include "rtv/SceneComponents.h"

#include <glm/glm.hpp>

namespace rtv {

class SceneDocument;
class SceneRegistry;
struct Entity;
struct RendererSettings;

struct SunDerivedState {
    bool enabled = true;
    float illuminanceLux = 100000.0f;
    float angularRadiusRadians = 0.00465f;
    float colorTemperatureKelvin = 5778.0f;
    glm::vec3 color{1.0f};
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    float elevation = 0.97f;
    float azimuth = 0.0f;
};

class SunController {
public:
    [[nodiscard]] static EntityId primarySunEntity(const SceneDocument& document);
    [[nodiscard]] static EntityId ensurePrimarySun(SceneDocument& document);
    static void setPrimarySun(SceneDocument& document, EntityId id);
    static void enforceSinglePrimarySun(SceneDocument& document);
    static bool migrateLegacyDirectionalSun(SceneDocument& document);
    static bool repairPrimarySunTransform(SceneDocument& document);

    [[nodiscard]] static SunDerivedState derivedState(const SceneDocument& document);
    static void applyToRendererSettings(const SceneDocument& document, RendererSettings& settings);

    [[nodiscard]] static glm::vec3 directionFromAngles(float elevation, float azimuth);
    static void anglesFromDirection(glm::vec3 direction, float& elevation, float& azimuth);
    static void anglesFromWorldTransform(const SceneRegistry& registry, const Entity& entity, float& elevation, float& azimuth);
    [[nodiscard]] static Transform transformFromWorldAngles(
        const SceneRegistry& registry,
        const Entity& entity,
        Transform base,
        float elevation,
        float azimuth);
    [[nodiscard]] static glm::vec3 colorFromTemperature(float kelvin);
    [[nodiscard]] static Sun sunFromLegacy(const RenderSettings& render, const Light* light);
};

} // namespace rtv
