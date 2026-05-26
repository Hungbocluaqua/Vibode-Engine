#include "rtv/SunController.h"

#include "rtv/Entity.h"
#include "rtv/RendererSettings.h"
#include "rtv/SceneDocument.h"

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {

constexpr float defaultSunAngularRadius = 0.00465f;
constexpr float maxSunAngularRadius = 0.08f;
constexpr float defaultSunIlluminanceLux = 100000.0f;
constexpr float legacyIntensityToLux = 45000.0f;
constexpr float defaultSunVisualDistance = 8.0f;
constexpr float defaultSunElevation = 0.97f;
constexpr float defaultSunAzimuth = 3.14159265358979323846f;

bool nearlyZero(glm::vec3 value, float epsilon = 1.0e-4f) {
    return std::abs(value.x) <= epsilon && std::abs(value.y) <= epsilon && std::abs(value.z) <= epsilon;
}

float safeSunAngularRadius(float radius) {
    return std::isfinite(radius) && radius > 0.0f
        ? std::clamp(radius, 0.0001f, maxSunAngularRadius)
        : defaultSunAngularRadius;
}

glm::mat4 entityWorldMatrix(const SceneRegistry& registry, const Entity& entity) {
    const Entity* current = &entity;
    glm::mat4 result(1.0f);
    constexpr int maxDepth = 512;
    for (int depth = 0; depth < maxDepth && current != nullptr; ++depth) {
        result = current->transform.localMatrix() * result;
        if (!current->parent.valid()) {
            break;
        }
        const Entity* parent = registry.entity(current->parent);
        if (parent == nullptr || parent == current) {
            break;
        }
        current = parent;
    }
    return result;
}

Transform transformFromLocalDirection(Transform base, glm::vec3 direction) {
    const float len2 = glm::dot(direction, direction);
    if (len2 <= 1.0e-6f) {
        return base;
    }
    direction *= glm::inversesqrt(len2);
    float elevation = 0.0f;
    float azimuth = 0.0f;
    SunController::anglesFromDirection(direction, elevation, azimuth);
    const float currentDistance = glm::length(base.position);
    const float visualDistance = std::isfinite(currentDistance) && currentDistance > 0.1f
        ? currentDistance
        : defaultSunVisualDistance;
    base.position = direction * visualDistance;
    base.rotationEuler = glm::vec3(-elevation, azimuth, 0.0f);
    base.dirty = true;
    return base;
}

} // namespace

EntityId SunController::primarySunEntity(const SceneDocument& document) {
    const EntityId explicitSun = document.primarySun();
    if (explicitSun.valid()) {
        if (const Entity* entity = document.registry().entity(explicitSun);
            entity != nullptr && entity->sun.has_value()) {
            return explicitSun;
        }
    }
    for (const Entity* entity : document.registry().entities()) {
        if (entity != nullptr && entity->sun.has_value()) {
            return entity->id;
        }
    }
    return {};
}

EntityId SunController::ensurePrimarySun(SceneDocument& document) {
    if (EntityId existing = primarySunEntity(document); existing.valid()) {
        setPrimarySun(document, existing);
        (void)repairPrimarySunTransform(document);
        return existing;
    }
    EntityId sunId = document.registry().createEntity("Sun");
    document.registry().clearDirty();
    document.registry().markDirty(SceneUpdateKind::LightOnly);
    if (Entity* sun = document.registry().entity(sunId)) {
        sun->sun = Sun{};
        sun->transform = transformFromWorldAngles(document.registry(), *sun, sun->transform, defaultSunElevation, defaultSunAzimuth);
    }
    document.setPrimarySun(sunId);
    document.markDirty(SceneUpdateKind::LightOnly);
    return sunId;
}

void SunController::setPrimarySun(SceneDocument& document, EntityId id) {
    document.setPrimarySun(id);
    enforceSinglePrimarySun(document);
}

void SunController::enforceSinglePrimarySun(SceneDocument& document) {
    bool found = false;
    EntityId primary = document.primarySun();
    for (Entity* entity : document.registry().entities()) {
        if (entity == nullptr || !entity->sun.has_value()) {
            continue;
        }
        if (!found && (!primary.valid() || entity->id == primary)) {
            found = true;
            primary = entity->id;
            continue;
        }
        entity->sun.reset();
    }
    if (primary.valid()) {
        document.setPrimarySun(primary);
    }
}

bool SunController::migrateLegacyDirectionalSun(SceneDocument& document) {
    if (primarySunEntity(document).valid()) {
        enforceSinglePrimarySun(document);
        return false;
    }

    for (Entity* entity : document.registry().entities()) {
        if (entity == nullptr ||
            !entity->light.has_value() ||
            entity->light->type != LightType::Directional) {
            continue;
        }
        entity->sun = sunFromLegacy(document.renderSettings(), &*entity->light);
        entity->light.reset();
        if (nearlyZero(entity->transform.rotationEuler)) {
            const float positionLen2 = glm::dot(entity->transform.position, entity->transform.position);
            entity->transform = positionLen2 > 1.0e-4f
                ? transformFromLocalDirection(entity->transform, entity->transform.position)
                : transformFromWorldAngles(
                    document.registry(),
                    *entity,
                    entity->transform,
                    document.renderSettings().sunElevation,
                    document.renderSettings().sunAzimuth);
        } else {
            float elevation = 0.0f;
            float azimuth = 0.0f;
            anglesFromWorldTransform(document.registry(), *entity, elevation, azimuth);
            entity->transform = transformFromWorldAngles(document.registry(), *entity, entity->transform, elevation, azimuth);
        }
        document.setPrimarySun(entity->id);
        document.markDirty(SceneUpdateKind::LightOnly);
        return true;
    }
    return false;
}

bool SunController::repairPrimarySunTransform(SceneDocument& document) {
    enforceSinglePrimarySun(document);

    const EntityId sunId = primarySunEntity(document);
    Entity* entity = document.registry().entity(sunId);
    if (entity == nullptr || !entity->sun.has_value()) {
        return false;
    }

    if (nearlyZero(entity->transform.rotationEuler)) {
        const float positionLen2 = glm::dot(entity->transform.position, entity->transform.position);
        if (positionLen2 > 1.0e-4f) {
            entity->transform = transformFromLocalDirection(entity->transform, entity->transform.position);
        } else {
            entity->transform = transformFromWorldAngles(
                document.registry(),
                *entity,
                entity->transform,
                document.renderSettings().sunElevation,
                document.renderSettings().sunAzimuth);
        }
        document.markDirty(SceneUpdateKind::LightOnly);
        return true;
    }

    return false;
}

SunDerivedState SunController::derivedState(const SceneDocument& document) {
    SunDerivedState state;
    const EntityId sunId = primarySunEntity(document);
    const Entity* entity = document.registry().entity(sunId);
    if (entity == nullptr || !entity->sun.has_value()) {
        const RenderSettings& render = document.renderSettings();
        state.enabled = render.sunlightEnabled;
        state.illuminanceLux = std::max(render.sunIntensity, 0.0f) * legacyIntensityToLux;
        state.angularRadiusRadians = safeSunAngularRadius(render.sunAngularRadius);
        state.direction = directionFromAngles(render.sunElevation, render.sunAzimuth);
        state.elevation = render.sunElevation;
        state.azimuth = render.sunAzimuth;
        state.color = colorFromTemperature(state.colorTemperatureKelvin);
        return state;
    }

    state.enabled = entity->sun->enabled;
    state.illuminanceLux = std::max(entity->sun->illuminanceLux, 0.0f);
    state.angularRadiusRadians = safeSunAngularRadius(entity->sun->angularRadiusRadians);
    state.colorTemperatureKelvin = std::clamp(entity->sun->colorTemperatureKelvin, 1000.0f, 40000.0f);
    state.color = colorFromTemperature(state.colorTemperatureKelvin);
    const glm::mat4 world = entityWorldMatrix(document.registry(), *entity);
    glm::vec3 direction = glm::vec3(world[3]);
    float len2 = glm::dot(direction, direction);
    if (len2 <= 1.0e-6f) {
        direction = -(glm::mat3(world) * glm::vec3(0.0f, 0.0f, -1.0f));
        len2 = glm::dot(direction, direction);
    }
    state.direction = len2 > 1.0e-6f ? direction * glm::inversesqrt(len2) : directionFromAngles(defaultSunElevation, defaultSunAzimuth);
    anglesFromDirection(state.direction, state.elevation, state.azimuth);
    return state;
}

void SunController::applyToRendererSettings(const SceneDocument& document, RendererSettings& settings) {
    const SunDerivedState sun = derivedState(document);
    settings.sunlightEnabled = sun.enabled;
    settings.sunIlluminanceLux = sun.illuminanceLux;
    settings.sunColorTemperatureKelvin = sun.colorTemperatureKelvin;
    settings.sunColor = sun.color;
    settings.sunDirection = sun.direction;
    settings.sunAngularRadius = sun.angularRadiusRadians;
    settings.sunElevation = sun.elevation;
    settings.sunAzimuth = sun.azimuth;
    settings.sunIntensity = sun.illuminanceLux / legacyIntensityToLux;
}

glm::vec3 SunController::directionFromAngles(float elevation, float azimuth) {
    const float clampedElevation = std::clamp(elevation, -0.20f, 1.45f);
    const float cosElevation = std::cos(clampedElevation);
    return glm::normalize(glm::vec3(
        cosElevation * std::sin(azimuth),
        std::sin(clampedElevation),
        cosElevation * std::cos(azimuth)));
}

void SunController::anglesFromDirection(glm::vec3 direction, float& elevation, float& azimuth) {
    const float len2 = glm::dot(direction, direction);
    if (len2 <= 1.0e-6f) {
        elevation = 0.97f;
        azimuth = 0.0f;
        return;
    }
    direction *= glm::inversesqrt(len2);
    elevation = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
    azimuth = std::atan2(direction.x, direction.z);
}

void SunController::anglesFromWorldTransform(const SceneRegistry& registry, const Entity& entity, float& elevation, float& azimuth) {
    glm::vec3 direction = -(glm::mat3(entityWorldMatrix(registry, entity)) * glm::vec3(0.0f, 0.0f, -1.0f));
    anglesFromDirection(direction, elevation, azimuth);
}

Transform SunController::transformFromWorldAngles(
    const SceneRegistry& registry,
    const Entity& entity,
    Transform base,
    float elevation,
    float azimuth) {
    glm::vec3 worldDirection = directionFromAngles(elevation, azimuth);
    if (entity.parent.valid()) {
        if (const Entity* parent = registry.entity(entity.parent)) {
            const glm::mat3 inverseParent = glm::inverse(glm::mat3(entityWorldMatrix(registry, *parent)));
            worldDirection = glm::normalize(inverseParent * worldDirection);
        }
    }
    return transformFromLocalDirection(base, worldDirection);
}

glm::vec3 SunController::colorFromTemperature(float kelvin) {
    const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    glm::vec3 color;
    color.r = t <= 66.0f ? 1.0f : std::clamp(1.292936186f * std::pow(t - 60.0f, -0.1332047592f), 0.0f, 1.0f);
    color.g = t <= 66.0f
        ? std::clamp(0.3900815788f * std::log(t) - 0.6318414438f, 0.0f, 1.0f)
        : std::clamp(1.129890861f * std::pow(t - 60.0f, -0.0755148492f), 0.0f, 1.0f);
    color.b = t >= 66.0f ? 1.0f : (t <= 19.0f ? 0.0f : std::clamp(0.5432067891f * std::log(t - 10.0f) - 1.1962540891f, 0.0f, 1.0f));
    return color / std::max(std::max(color.r, color.g), std::max(color.b, 1.0e-4f));
}

Sun SunController::sunFromLegacy(const RenderSettings& render, const Light* light) {
    Sun sun;
    sun.enabled = light != nullptr ? light->enabled : render.sunlightEnabled;
    const float legacyIntensity = light != nullptr ? light->intensity : render.sunIntensity;
    sun.illuminanceLux = std::max(legacyIntensity, 0.0f) * legacyIntensityToLux;
    if (sun.illuminanceLux <= 0.0f) {
        sun.illuminanceLux = defaultSunIlluminanceLux;
    }
    sun.angularRadiusRadians = safeSunAngularRadius(light != nullptr ? light->sizeOrRadius : render.sunAngularRadius);
    sun.colorTemperatureKelvin = 5778.0f;
    return sun;
}

} // namespace rtv
