#include "rtv/SceneDocument.h"

#include "rtv/SunController.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace rtv {

namespace {

glm::vec3 translationFromMatrix(const glm::mat4& matrix) {
    return glm::vec3(matrix[3]);
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

glm::vec3 scaleFromMatrix(const glm::mat4& matrix) {
    return {
        glm::length(glm::vec3(matrix[0])),
        glm::length(glm::vec3(matrix[1])),
        glm::length(glm::vec3(matrix[2])),
    };
}

glm::vec3 eulerFromMatrix(const glm::mat4& matrix) {
    glm::vec3 scale = scaleFromMatrix(matrix);
    glm::mat3 rotation{matrix};
    if (scale.x > 0.0f) {
        rotation[0] /= scale.x;
    }
    if (scale.y > 0.0f) {
        rotation[1] /= scale.y;
    }
    if (scale.z > 0.0f) {
        rotation[2] /= scale.z;
    }
    return glm::eulerAngles(glm::quat_cast(rotation));
}

nlohmann::json vec3Json(glm::vec3 value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

glm::vec3 vec3FromJson(const nlohmann::json& json, glm::vec3 fallback = glm::vec3{0.0f}) {
    if (!json.is_array() || json.size() < 3) {
        return fallback;
    }
    return {
        json[0].get<float>(),
        json[1].get<float>(),
        json[2].get<float>(),
    };
}

std::string generateSceneGuid() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    const uint64_t now = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const uint64_t a = rng() ^ now;
    const uint64_t b = rng();
    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<uint32_t>(a >> 32) << '-'
        << std::setw(4) << static_cast<uint16_t>(a >> 16) << '-'
        << std::setw(4) << static_cast<uint16_t>(a) << '-'
        << std::setw(4) << static_cast<uint16_t>(b >> 48) << '-'
        << std::setw(12) << (b & 0x0000FFFFFFFFFFFFull);
    return out.str();
}

bool needsMigrationBackup(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    try {
        nlohmann::json json;
        in >> json;
        if (!json.contains("rtlevel")) {
            return true;
        }
        const nlohmann::json& header = json["rtlevel"];
        return header.value("formatVersion", 0) < 3;
    } catch (...) {
        return true;
    }
}

void createMigrationBackupIfNeeded(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !needsMigrationBackup(path)) {
        return;
    }
    const std::filesystem::path backup = path.string() + ".bak";
    std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, ec);
}

} // namespace

void SceneDocument::setEnvironment(Environment environment) {
    environment_ = std::move(environment);
    markDirty(SceneUpdateKind::EnvironmentOnly);
}

void SceneDocument::setRenderSettings(RenderSettings settings) {
    renderSettings_ = settings;
    markDirty(SceneUpdateKind::RendererSettingsOnly);
}

void SceneDocument::setWorldSettings(WorldSettings settings) {
    worldSettings_ = settings;
    markDirty(SceneUpdateKind::EnvironmentOnly);
}

void SceneDocument::setActiveCamera(EntityId id) {
    if (activeCamera_ == id) {
        return;
    }
    activeCamera_ = id;
    for (Entity* entity : registry_.entities()) {
        if (entity->camera.has_value()) {
            entity->camera->active = entity->id == id;
        }
    }
    markDirty(SceneUpdateKind::CameraOnly);
}

void SceneDocument::setPrimarySun(EntityId id) {
    if (primarySun_ == id) {
        return;
    }
    primarySun_ = id;
    worldSettings_.primarySun = id;
    markDirty(SceneUpdateKind::LightOnly);
}

EntityId SceneDocument::primarySun() const {
    const Entity* entity = registry_.entity(primarySun_);
    return entity != nullptr && entity->sun.has_value() ? primarySun_ : EntityId{};
}

void SceneDocument::setSourceGltfPath(std::optional<std::filesystem::path> path) {
    sourceGltfPath_ = std::move(path);
}

void SceneDocument::setSourceHdrPath(std::optional<std::filesystem::path> path) {
    sourceHdrPath_ = std::move(path);
    if (sourceHdrPath_.has_value()) {
        environment_.hdrPath = *sourceHdrPath_;
    }
}

void SceneDocument::markDirty(SceneUpdateKind kind) {
    dirty_ = true;
    pendingUpdate_ = combine(pendingUpdate_, kind);
    lastChangeReason_ = sceneUpdateKindName(kind);
    if (dirtyReasons_.empty() || dirtyReasons_.back() != lastChangeReason_) {
        dirtyReasons_.push_back(lastChangeReason_);
    }
}

void SceneDocument::clearDirty() {
    dirty_ = false;
    pendingUpdate_ = SceneUpdateKind::None;
    dirtyReasons_.clear();
    registry_.clearDirty();
}

void SceneDocument::setBookmarksJson(const nlohmann::json& json) {
    bookmarksJson_ = json;
}

void SceneDocument::clearBookmarksJson() {
    bookmarksJson_.reset();
}

void SceneDocument::setTimelineJson(const nlohmann::json& json) {
    timelineJson_ = json;
}

void SceneDocument::clearTimelineJson() {
    timelineJson_.reset();
}

void SceneDocument::addPrefabInstance(PrefabInstance instance) {
    prefabInstances_.push_back(std::move(instance));
    markDirty(SceneUpdateKind::TopologyChanged);
}

SceneUpdateKind SceneDocument::pendingUpdate() const {
    return combine(pendingUpdate_, registry_.pendingUpdate());
}

void SceneDocument::importSceneAsset(const SceneAsset& scene) {
    registry_ = SceneRegistry{};
    activeCamera_ = {};
    prefabInstances_.clear();
    sceneTextures_ = scene.textures;
    sceneMaterials_ = scene.materials;
    sceneMeshes_ = scene.meshes;

    std::vector<EntityId> nodeEntities(scene.nodes.size());
    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNodeAsset& node = scene.nodes[i];
        EntityId id = registry_.createEntity(node.name.empty() ? "Node " + std::to_string(i) : node.name);
        nodeEntities[i] = id;
        Entity* entity = registry_.entity(id);
        if (entity == nullptr) {
            continue;
        }
        entity->transform.position = translationFromMatrix(node.transform);
        entity->transform.rotationEuler = eulerFromMatrix(node.transform);
        entity->transform.scale = scaleFromMatrix(node.transform);
        entity->transform.dirty = true;

        if (node.mesh.valid()) {
            MeshRenderer renderer;
            renderer.mesh = node.mesh;
            renderer.materialSlots.clear();
            entity->meshRenderer = renderer;
        }
        if (node.hasCamera) {
            Camera camera;
            camera.verticalFovRadians = node.cameraYfov;
            camera.nearPlane = node.cameraNear;
            camera.farPlane = node.cameraFar;
            camera.active = !activeCamera_.valid();
            entity->camera = camera;
            if (camera.active) {
                activeCamera_ = id;
            }
        }
    }

    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        Entity* entity = registry_.entity(nodeEntities[i]);
        if (entity == nullptr) {
            continue;
        }
        const SceneNodeAsset& node = scene.nodes[i];
        if (node.parent >= 0 && static_cast<uint32_t>(node.parent) < nodeEntities.size()) {
            entity->parent = nodeEntities[static_cast<uint32_t>(node.parent)];
        }
        entity->children.clear();
        for (uint32_t child : node.children) {
            if (child < nodeEntities.size() && nodeEntities[child].valid()) {
                entity->children.push_back(nodeEntities[child]);
            }
        }
    }

    for (uint32_t i = 0; i < scene.lights.size(); ++i) {
        const SceneLightAsset& source = scene.lights[i];
        EntityId id = source.nodeIndex >= 0 && static_cast<uint32_t>(source.nodeIndex) < nodeEntities.size()
            ? nodeEntities[static_cast<uint32_t>(source.nodeIndex)]
            : registry_.createEntity("Light " + std::to_string(i));
        Entity* entity = registry_.entity(id);
        if (entity == nullptr) {
            continue;
        }
        entity->transform.position = translationFromMatrix(source.transform);
        entity->transform.rotationEuler = eulerFromMatrix(source.transform);
        entity->transform.scale = scaleFromMatrix(source.transform);
        Light light;
        light.type = static_cast<LightType>(std::min(source.type, 3u));
        light.color = source.color;
        light.intensity = source.intensity;
        light.sizeOrRadius = source.sizeOrRadius;
        light.enabled = source.enabled;
        entity->light = light;
    }

    (void)SunController::migrateLegacyDirectionalSun(*this);
    (void)SunController::repairPrimarySunTransform(*this);

    std::unordered_map<std::string, EntityId> entitiesByName;
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        if (const Entity* e = registry_.entity(nodeEntities[i]); e != nullptr && !e->name.empty()) {
            entitiesByName[e->name] = nodeEntities[i];
        }
    }
    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        Entity* entity = registry_.entity(nodeEntities[i]);
        if (entity == nullptr) continue;
        const std::string& name = entity->name;
        constexpr size_t targetSuffixLen = 7;
        if (name.size() > targetSuffixLen && name.compare(name.size() - targetSuffixLen, targetSuffixLen, ".Target") == 0) {
            std::string baseName = name.substr(0, name.size() - targetSuffixLen);
            auto it = entitiesByName.find(baseName);
            if (it != entitiesByName.end()) {
                Entity* refEntity = registry_.entity(it->second);
                if (refEntity == nullptr) continue;
                glm::vec3 forward = glm::normalize(entity->transform.position - refEntity->transform.position);
                if (refEntity->camera.has_value()) {
                    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
                    glm::vec3 up = glm::cross(forward, right);
                    glm::mat3 rot;
                    rot[0] = right;
                    rot[1] = up;
                    rot[2] = -forward;
                    refEntity->transform.rotationEuler = glm::eulerAngles(glm::quat_cast(rot));
                } else if (refEntity->sun.has_value()) {
                    float elevation = 0.0f;
                    float azimuth = 0.0f;
                    SunController::anglesFromDirection(-forward, elevation, azimuth);
                    refEntity->transform = SunController::transformFromWorldAngles(
                        registry_, *refEntity, refEntity->transform, elevation, azimuth);
                }
            }
        }
    }

    clearDirty();
    markDirty(SceneUpdateKind::TopologyChanged);
}

SceneAsset SceneDocument::toSceneAsset() const {
    SceneAsset scene;
    scene.name = sourceGltfPath_.has_value() ? sourceGltfPath_->filename().string() : "SceneDocument";
    if (sourceGltfPath_.has_value()) {
        scene.sourcePath = *sourceGltfPath_;
    }
    scene.textures = sceneTextures_;
    scene.materials = sceneMaterials_;
    scene.meshes = sceneMeshes_;

    std::vector<const Entity*> entities = registry_.entities();
    std::unordered_map<uint64_t, uint32_t> nodeIndexForEntity;
    nodeIndexForEntity.reserve(entities.size());
    scene.nodes.reserve(entities.size());
    for (const Entity* entity : entities) {
        SceneNodeAsset node;
        node.name = entity->name;
        node.transform = entity->transform.localMatrix();
        if (entity->meshRenderer.has_value()) {
            node.mesh = entity->meshRenderer->mesh;
            node.visible = entity->meshRenderer->visible;
            node.castShadow = entity->meshRenderer->castShadow;
            node.visibleToCamera = entity->meshRenderer->visibleToCamera;
            if (node.mesh.valid()) {
                scene.meshes.push_back(node.mesh);
            }
            for (const MaterialSlot& slot : entity->meshRenderer->materialSlots) {
                MaterialAssetHandle material = slot.resolvedMaterial();
                if (material.valid()) {
                    scene.materials.push_back(material);
                }
            }
        }
        nodeIndexForEntity.emplace(entity->uuid, static_cast<uint32_t>(scene.nodes.size()));
        scene.nodes.push_back(node);
    }

    for (size_t i = 0; i < entities.size(); ++i) {
        const Entity* entity = entities[i];
        SceneNodeAsset& node = scene.nodes[i];
        if (entity->parent.valid()) {
            const Entity* parentEntity = registry_.entity(entity->parent);
            const uint64_t key = parentEntity != nullptr ? parentEntity->uuid : 0u;
            const auto it = nodeIndexForEntity.find(key);
            if (it != nodeIndexForEntity.end()) {
                node.parent = static_cast<int32_t>(it->second);
            }
        }
        for (EntityId child : entity->children) {
            const Entity* childEntity = registry_.entity(child);
            const uint64_t key = childEntity != nullptr ? childEntity->uuid : 0u;
            const auto it = nodeIndexForEntity.find(key);
            if (it != nodeIndexForEntity.end()) {
                node.children.push_back(it->second);
            }
        }
        if (node.parent < 0) {
            scene.rootNodes.push_back(static_cast<uint32_t>(i));
        }
        if (entity->light.has_value()) {
            SceneLightAsset light;
            light.type = static_cast<uint32_t>(entity->light->type);
            light.transform = entityWorldMatrix(registry_, *entity);
            light.color = entity->light->color;
            light.intensity = entity->light->intensity;
            light.sizeOrRadius = entity->light->sizeOrRadius;
            light.enabled = entity->light->enabled;
            scene.lights.push_back(light);
        }
    }

    std::sort(scene.meshes.begin(), scene.meshes.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index < b.index; });
    scene.meshes.erase(std::unique(scene.meshes.begin(), scene.meshes.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index == b.index; }), scene.meshes.end());
    std::sort(scene.materials.begin(), scene.materials.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index < b.index; });
    scene.materials.erase(std::unique(scene.materials.begin(), scene.materials.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index == b.index; }), scene.materials.end());
    return scene;
}

bool SceneDocument::saveJson(const std::filesystem::path& path) const {
    createMigrationBackupIfNeeded(path);
    if (header_.sceneGuid.empty()) {
        header_.sceneGuid = generateSceneGuid();
    }
    header_.formatVersion = 3;
    header_.engineVersion = "0.1";
    header_.projectRelativePaths = true;

    nlohmann::json root;
    root["version"] = 3;
    root["rtlevel"] = {
        {"formatVersion", header_.formatVersion},
        {"sceneGuid", header_.sceneGuid},
        {"engineVersion", header_.engineVersion},
        {"projectRelativePaths", header_.projectRelativePaths},
        {"legacyVersion", 2},
    };
    root["assetReferences"] = {
        {"source", {
            {"assetGuid", std::string{}},
            {"sourceGltf", sourceGltfPath_.has_value() ? sourceGltfPath_->generic_string() : std::string{}},
        }},
        {"environment", {
            {"assetGuid", std::string{}},
            {"hdrPath", environment_.hdrPath.generic_string()},
        }},
        {"meshes", nlohmann::json::array()},
        {"materials", nlohmann::json::array()},
        {"textures", nlohmann::json::array()},
        {"prefabs", nlohmann::json::array()},
    };
    root["sourceGltf"] = sourceGltfPath_.has_value() ? sourceGltfPath_->string() : "";
    root["sourceHdr"] = sourceHdrPath_.has_value() ? sourceHdrPath_->string() : "";
    root["activeCamera"] = activeCamera_.valid() ? (registry_.entity(activeCamera_) != nullptr ? registry_.entity(activeCamera_)->uuid : 0u) : 0u;
    root["primarySun"] = primarySun().valid() ? (registry_.entity(primarySun()) != nullptr ? registry_.entity(primarySun())->uuid : 0u) : 0u;
    root["environment"] = {
        {"hdrPath", environment_.hdrPath.string()},
        {"intensity", environment_.intensity},
        {"rotation", environment_.rotation},
        {"backgroundIntensity", environment_.backgroundIntensity},
        {"enabled", environment_.enabled},
    };
    auto entityUuid = [&](EntityId id) -> uint64_t {
        const Entity* entity = registry_.entity(id);
        return entity != nullptr ? entity->uuid : 0u;
    };
    root["worldSettings"] = {
        {"activeEnvironment", entityUuid(worldSettings_.activeEnvironment)},
        {"primarySun", entityUuid(worldSettings_.primarySun)},
        {"skyAtmosphere", entityUuid(worldSettings_.skyAtmosphere)},
        {"heightFog", entityUuid(worldSettings_.heightFog)},
        {"postProcessVolume", entityUuid(worldSettings_.postProcessVolume)},
        {"atmosphereEnabled", worldSettings_.atmosphereEnabled},
        {"fogEnabled", worldSettings_.fogEnabled},
        {"postProcessEnabled", worldSettings_.postProcessEnabled},
    };
    root["renderSettings"] = {
        {"renderPreset", static_cast<uint32_t>(renderSettings_.renderPreset)},
        {"pathTracingEnabled", renderSettings_.pathTracingEnabled},
        {"cameraJitterEnabled", renderSettings_.cameraJitterEnabled},
        {"directLightingEnabled", renderSettings_.directLightingEnabled},
        {"maxBounces", renderSettings_.maxBounces},
        {"environmentDirectSamples", renderSettings_.environmentDirectSamples},
        {"toneMapper", static_cast<uint32_t>(renderSettings_.toneMapper)},
        {"exposure", renderSettings_.exposure},
        {"gamma", renderSettings_.gamma},
        {"contrast", renderSettings_.contrast},
        {"saturation", renderSettings_.saturation},
        {"brightness", renderSettings_.brightness},
        {"whitePoint", renderSettings_.whitePoint},
        {"autoExposureEnabled", renderSettings_.autoExposureEnabled},
        {"targetLuminance", renderSettings_.targetLuminance},
        {"minExposure", renderSettings_.minExposure},
        {"maxExposure", renderSettings_.maxExposure},
        {"adaptationSpeed", renderSettings_.adaptationSpeed},
        {"histogramMinLogLuminance", renderSettings_.histogramMinLogLuminance},
        {"histogramMaxLogLuminance", renderSettings_.histogramMaxLogLuminance},
        {"histogramLowPercentile", renderSettings_.histogramLowPercentile},
        {"histogramHighPercentile", renderSettings_.histogramHighPercentile},
        {"histogramTargetPercentile", renderSettings_.histogramTargetPercentile},
        {"skyIntensity", renderSettings_.skyIntensity},
        {"indirectStrength", renderSettings_.indirectStrength},
        {"restirMode", static_cast<uint32_t>(renderSettings_.restirMode)},
        {"restirGiEnabled", renderSettings_.restirGiEnabled},
        {"denoiserEnabled", renderSettings_.denoiserEnabled},
        {"denoiseWhileMoving", renderSettings_.denoiseWhileMoving},
        {"samplesPerPixel", renderSettings_.samplesPerPixel},
        {"limitSamplesPerPixel", renderSettings_.limitSamplesPerPixel},
        {"atrousIterations", renderSettings_.atrousIterations},
        {"denoiserStrength", renderSettings_.denoiserStrength},
        {"denoiserMaxHistoryLength", renderSettings_.denoiserMaxHistoryLength},
        {"momentValidityThreshold", renderSettings_.momentValidityThreshold},
        {"taaEnabled", renderSettings_.taaEnabled},
        {"taaFeedback", renderSettings_.taaFeedback},
        {"taaMotionFeedback", renderSettings_.taaMotionFeedback},
        {"taaReactiveFeedback", renderSettings_.taaReactiveFeedback},
        {"taaSharpeningStrength", renderSettings_.taaSharpeningStrength},
        {"debugView", static_cast<uint32_t>(renderSettings_.debugView)},
        {"accumulate", renderSettings_.accumulate},
        {"accumulationLimit", renderSettings_.accumulationLimit},
        {"resolutionScale", renderSettings_.resolutionScale},
        {"materialTextureAnisotropy", renderSettings_.materialTextureAnisotropy},
        {"specularAaEnabled", renderSettings_.specularAaEnabled},
        {"opacityMicromapsEnabled", renderSettings_.opacityMicromapsEnabled},
        {"shadowRayBias", renderSettings_.shadowRayBias},
        {"shadowDistanceBias", renderSettings_.shadowDistanceBias},
        {"fireflyClamp", renderSettings_.fireflyClamp},
        {"restirGiTemporalMaxAge", renderSettings_.restirGiTemporalMaxAge},
        {"restirGiSpatialRounds", renderSettings_.restirGiSpatialRounds},
        {"restirGiSpatialRadius", renderSettings_.restirGiSpatialRadius},
        {"restirGiDepthThresholdScale", renderSettings_.restirGiDepthThresholdScale},
        {"restirGiSpatialCompatibilityThreshold", renderSettings_.restirGiSpatialCompatibilityThreshold},
        {"restirGiHalfResolution", renderSettings_.restirGiHalfResolution},
        {"restirGiVisibilityRayBudget", renderSettings_.restirGiVisibilityRayBudget},
        {"adaptiveQualityMode", static_cast<uint32_t>(renderSettings_.adaptiveQualityMode)},
        {"adaptiveGpuFrameTargetMs", renderSettings_.adaptiveGpuFrameTargetMs},
        {"usePhysicalCamera", renderSettings_.usePhysicalCamera},
        {"physicalAperture", renderSettings_.physicalAperture},
        {"physicalShutterSeconds", renderSettings_.physicalShutterSeconds},
        {"physicalIso", renderSettings_.physicalIso},
        {"physicalExposureCompensation", renderSettings_.physicalExposureCompensation},
        {"dofApertureRadius", renderSettings_.dofApertureRadius},
        {"dofFocusDistance", renderSettings_.dofFocusDistance},
        {"dofBladeCount", renderSettings_.dofBladeCount},
        {"dofBokehRotation", renderSettings_.dofBokehRotation},
        {"motionBlurEnabled", renderSettings_.motionBlurEnabled},
        {"motionBlurShutterOpen", renderSettings_.motionBlurShutterOpen},
        {"motionBlurShutterClose", renderSettings_.motionBlurShutterClose},
        {"homogeneousVolumeEnabled", renderSettings_.homogeneousVolumeEnabled},
        {"homogeneousVolumeScattering", renderSettings_.homogeneousVolumeScattering},
        {"homogeneousVolumeAbsorption", renderSettings_.homogeneousVolumeAbsorption},
        {"homogeneousVolumeAnisotropy", renderSettings_.homogeneousVolumeAnisotropy},
        {"mneeCausticsEnabled", renderSettings_.mneeCausticsEnabled},
    };

    root["entities"] = nlohmann::json::array();
    const std::vector<const Entity*> entities = registry_.entities();
    for (const Entity* entityPtr : entities) {
        const Entity& entity = *entityPtr;
        nlohmann::json item;
        item["id"] = {{"index", entity.id.index}, {"generation", entity.id.generation}, {"uuid", entity.uuid}};
        item["parent"] = entity.parent.valid() ? (registry_.entity(entity.parent) != nullptr ? registry_.entity(entity.parent)->uuid : 0u) : 0u;
        item["children"] = nlohmann::json::array();
        for (EntityId child : entity.children) {
            const Entity* childEntity = registry_.entity(child);
            if (childEntity != nullptr) {
                item["children"].push_back(childEntity->uuid);
            }
        }
        item["name"] = entity.name;
        item["locked"] = entity.locked;
        item["transform"] = {
            {"position", vec3Json(entity.transform.position)},
            {"rotationEuler", vec3Json(entity.transform.rotationEuler)},
            {"scale", vec3Json(entity.transform.scale)},
        };
        if (entity.meshRenderer.has_value()) {
            nlohmann::json renderer;
            renderer["mesh"] = entity.meshRenderer->mesh.index;
            renderer["meshGuid"] = entity.meshRenderer->meshGuid;
            renderer["visible"] = entity.meshRenderer->visible;
            renderer["castShadow"] = entity.meshRenderer->castShadow;
            renderer["visibleToCamera"] = entity.meshRenderer->visibleToCamera;
            if (!entity.meshRenderer->meshGuid.empty()) {
                root["assetReferences"]["meshes"].push_back({{"assetGuid", entity.meshRenderer->meshGuid}});
            }
            renderer["materialSlots"] = nlohmann::json::array();
            for (size_t slotIndex = 0; slotIndex < entity.meshRenderer->materialSlots.size(); ++slotIndex) {
                const MaterialSlot& slot = entity.meshRenderer->materialSlots[slotIndex];
                nlohmann::json slotJson = {
                    {"name", slot.name},
                    {"material", slot.material.index},
                    {"materialGuid", slot.materialGuid},
                };
                if (!slot.materialGuid.empty()) {
                    root["assetReferences"]["materials"].push_back({{"assetGuid", slot.materialGuid}});
                }
                if (slot.overrideMaterial.has_value()) {
                    slotJson["overrideMaterial"] = slot.overrideMaterial->index;
                    slotJson["overrideMaterialGuid"] = slot.overrideMaterialGuid.value_or(std::string{});
                    if (slot.overrideMaterialGuid.has_value() && !slot.overrideMaterialGuid->empty()) {
                        root["assetReferences"]["materials"].push_back({{"assetGuid", *slot.overrideMaterialGuid}});
                    }
                }
                renderer["materialSlots"].push_back(std::move(slotJson));
            }
            item["meshRenderer"] = std::move(renderer);
        }
        if (entity.light.has_value()) {
            item["light"] = {
                {"type", static_cast<uint32_t>(entity.light->type)},
                {"color", vec3Json(entity.light->color)},
                {"intensity", entity.light->intensity},
                {"sizeOrRadius", entity.light->sizeOrRadius},
                {"innerConeRadians", entity.light->innerConeRadians},
                {"outerConeRadians", entity.light->outerConeRadians},
                {"enabled", entity.light->enabled},
            };
        }
        if (entity.sun.has_value()) {
            item["sun"] = {
                {"enabled", entity.sun->enabled},
                {"illuminanceLux", entity.sun->illuminanceLux},
                {"angularRadiusRadians", entity.sun->angularRadiusRadians},
                {"colorTemperatureKelvin", entity.sun->colorTemperatureKelvin},
            };
        }
        if (entity.camera.has_value()) {
            item["camera"] = {
                {"verticalFovRadians", entity.camera->verticalFovRadians},
                {"nearPlane", entity.camera->nearPlane},
                {"farPlane", entity.camera->farPlane},
                {"active", entity.camera->active},
                {"useRenderSettingsExposure", entity.camera->useRenderSettingsExposure},
            };
        }
        if (entity.environmentLight.has_value()) {
            item["environmentLight"] = {
                {"enabled", entity.environmentLight->enabled},
                {"intensity", entity.environmentLight->intensity},
                {"backgroundIntensity", entity.environmentLight->backgroundIntensity},
                {"rotation", entity.environmentLight->rotation},
            };
        }
        if (entity.skyAtmosphere.has_value()) {
            item["skyAtmosphere"] = {
                {"enabled", entity.skyAtmosphere->enabled},
                {"rayleighScaleHeight", entity.skyAtmosphere->rayleighScaleHeight},
                {"mieScaleHeight", entity.skyAtmosphere->mieScaleHeight},
                {"mieAnisotropy", entity.skyAtmosphere->mieAnisotropy},
                {"groundAlbedo", entity.skyAtmosphere->groundAlbedo},
            };
        }
        if (entity.heightFog.has_value()) {
            item["heightFog"] = {
                {"enabled", entity.heightFog->enabled},
                {"density", entity.heightFog->density},
                {"heightFalloff", entity.heightFog->heightFalloff},
                {"color", vec3Json(entity.heightFog->color)},
            };
        }
        if (entity.volumetricCloud.has_value()) {
            item["volumetricCloud"] = {
                {"enabled", entity.volumetricCloud->enabled},
                {"density", entity.volumetricCloud->density},
                {"coverage", entity.volumetricCloud->coverage},
            };
        }
        if (entity.postProcessVolume.has_value()) {
            item["postProcessVolume"] = {
                {"enabled", entity.postProcessVolume->enabled},
                {"unbound", entity.postProcessVolume->unbound},
                {"priority", entity.postProcessVolume->priority},
                {"exposureCompensation", entity.postProcessVolume->exposureCompensation},
                {"saturation", entity.postProcessVolume->saturation},
                {"contrast", entity.postProcessVolume->contrast},
            };
        }
        if (entity.cameraPostProcess.has_value()) {
            item["cameraPostProcess"] = {
                {"enabled", entity.cameraPostProcess->enabled},
                {"overrideExposure", entity.cameraPostProcess->overrideExposure},
                {"exposureCompensation", entity.cameraPostProcess->exposureCompensation},
                {"overrideDepthOfField", entity.cameraPostProcess->overrideDepthOfField},
                {"dofApertureRadius", entity.cameraPostProcess->dofApertureRadius},
                {"dofFocusDistance", entity.cameraPostProcess->dofFocusDistance},
            };
        }
        root["entities"].push_back(std::move(item));
    }
    if (bookmarksJson_.has_value()) {
        root["bookmarks"] = *bookmarksJson_;
    }
    if (timelineJson_.has_value()) {
        root["timeline"] = *timelineJson_;
    }
    root["prefabInstances"] = nlohmann::json::array();
    for (const PrefabInstance& instance : prefabInstances_) {
        nlohmann::json item;
        item["prefabGuid"] = instance.prefabGuid;
        const Entity* rootEntity = registry_.entity(instance.instanceRoot);
        item["instanceRoot"] = rootEntity != nullptr ? rootEntity->uuid : 0u;
        item["generatedEntityUuids"] = instance.generatedEntityUuids;
        item["overrides"] = nlohmann::json::array();
        for (const PrefabOverride& override : instance.overrides) {
            item["overrides"].push_back({{"path", override.path}, {"value", override.valueJson}});
        }
        root["prefabInstances"].push_back(std::move(item));
        root["assetReferences"]["prefabs"].push_back({{"assetGuid", instance.prefabGuid}});
    }
    root["dirtyReasons"] = dirtyReasons_.empty()
        ? nlohmann::json::array({lastChangeReason_})
        : nlohmann::json(dirtyReasons_);
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const std::filesystem::path tempPath = path.string() + ".tmp";
    std::ofstream out(tempPath, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << std::setw(2) << root << '\n';
    out.close();
    if (!out) {
        return false;
    }
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tempPath, path, ec);
    }
    return !ec;
}

bool SceneDocument::loadJson(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }

    nlohmann::json root;
    try {
        in >> root;
    } catch (...) {
        return false;
    }

    renderSettings_ = RenderSettings{};
    worldSettings_ = WorldSettings{};
    header_ = RtLevelHeader{};
    if (root.contains("rtlevel") && root["rtlevel"].is_object()) {
        const nlohmann::json& header = root["rtlevel"];
        header_.formatVersion = header.value("formatVersion", header_.formatVersion);
        header_.sceneGuid = header.value("sceneGuid", std::string{});
        header_.engineVersion = header.value("engineVersion", header_.engineVersion);
        header_.projectRelativePaths = header.value("projectRelativePaths", true);
    }
    if (header_.sceneGuid.empty()) {
        header_.sceneGuid = generateSceneGuid();
    }

    sourceGltfPath_.reset();
    sourceHdrPath_.reset();
    bookmarksJson_.reset();
    timelineJson_.reset();
    if (const std::string source = root.value("sourceGltf", std::string{}); !source.empty()) {
        sourceGltfPath_ = source;
    }
    if (const std::string source = root.value("sourceHdr", std::string{}); !source.empty()) {
        sourceHdrPath_ = source;
        environment_.hdrPath = source;
    }
    if (root.contains("assetReferences") && root["assetReferences"].is_object()) {
        const nlohmann::json& refs = root["assetReferences"];
        if (!sourceGltfPath_.has_value() && refs.contains("source") && refs["source"].is_object()) {
            const std::string source = refs["source"].value("sourceGltf", std::string{});
            if (!source.empty()) sourceGltfPath_ = source;
        }
        if (refs.contains("environment") && refs["environment"].is_object()) {
            const std::string hdr = refs["environment"].value("hdrPath", std::string{});
            if (!hdr.empty()) {
                sourceHdrPath_ = hdr;
                environment_.hdrPath = hdr;
            }
        }
    }
    if (root.contains("bookmarks")) {
        bookmarksJson_ = root["bookmarks"];
    }
    if (root.contains("timeline")) {
        timelineJson_ = root["timeline"];
    }

    if (root.contains("environment")) {
        const nlohmann::json& env = root["environment"];
        environment_.hdrPath = env.value("hdrPath", environment_.hdrPath.string());
        environment_.intensity = env.value("intensity", environment_.intensity);
        environment_.rotation = env.value("rotation", environment_.rotation);
        environment_.backgroundIntensity = env.value("backgroundIntensity", environment_.backgroundIntensity);
        environment_.enabled = env.value("enabled", environment_.enabled);
    }
    if (root.contains("renderSettings")) {
        const nlohmann::json& render = root["renderSettings"];
        renderSettings_.renderPreset = render.contains("renderPreset")
            ? static_cast<RenderPreset>(render.value("renderPreset", static_cast<uint32_t>(renderSettings_.renderPreset)))
            : RenderPreset::Custom;
        renderSettings_.pathTracingEnabled = render.value("pathTracingEnabled", renderSettings_.pathTracingEnabled);
        renderSettings_.cameraJitterEnabled = render.value("cameraJitterEnabled", renderSettings_.cameraJitterEnabled);
        renderSettings_.directLightingEnabled = render.value("directLightingEnabled", renderSettings_.directLightingEnabled);
        renderSettings_.maxBounces = render.value("maxBounces", renderSettings_.maxBounces);
        renderSettings_.environmentDirectSamples = render.value("environmentDirectSamples", renderSettings_.environmentDirectSamples);
        renderSettings_.toneMapper = static_cast<ToneMapper>(render.value("toneMapper", static_cast<uint32_t>(renderSettings_.toneMapper)));
        renderSettings_.exposure = render.value("exposure", renderSettings_.exposure);
        renderSettings_.gamma = render.value("gamma", renderSettings_.gamma);
        renderSettings_.contrast = render.value("contrast", renderSettings_.contrast);
        renderSettings_.saturation = render.value("saturation", renderSettings_.saturation);
        renderSettings_.brightness = render.value("brightness", renderSettings_.brightness);
        renderSettings_.whitePoint = render.value("whitePoint", renderSettings_.whitePoint);
        renderSettings_.autoExposureEnabled = render.value("autoExposureEnabled", renderSettings_.autoExposureEnabled);
        renderSettings_.targetLuminance = render.value("targetLuminance", renderSettings_.targetLuminance);
        renderSettings_.minExposure = render.value("minExposure", renderSettings_.minExposure);
        renderSettings_.maxExposure = render.value("maxExposure", renderSettings_.maxExposure);
        renderSettings_.adaptationSpeed = render.value("adaptationSpeed", renderSettings_.adaptationSpeed);
        renderSettings_.histogramMinLogLuminance = render.value("histogramMinLogLuminance", renderSettings_.histogramMinLogLuminance);
        renderSettings_.histogramMaxLogLuminance = render.value("histogramMaxLogLuminance", renderSettings_.histogramMaxLogLuminance);
        renderSettings_.histogramLowPercentile = render.value("histogramLowPercentile", renderSettings_.histogramLowPercentile);
        renderSettings_.histogramHighPercentile = render.value("histogramHighPercentile", renderSettings_.histogramHighPercentile);
        renderSettings_.histogramTargetPercentile = render.value("histogramTargetPercentile", renderSettings_.histogramTargetPercentile);
        renderSettings_.sunlightEnabled = render.value("sunlightEnabled", renderSettings_.sunlightEnabled);
        renderSettings_.sunIntensity = render.value("sunIntensity", renderSettings_.sunIntensity);
        renderSettings_.skyIntensity = render.value("skyIntensity", renderSettings_.skyIntensity);
        renderSettings_.sunElevation = render.value("sunElevation", renderSettings_.sunElevation);
        renderSettings_.sunAzimuth = render.value("sunAzimuth", renderSettings_.sunAzimuth);
        renderSettings_.sunAngularRadius = render.value("sunAngularRadius", renderSettings_.sunAngularRadius);
        renderSettings_.indirectStrength = render.value("indirectStrength", renderSettings_.indirectStrength);
        renderSettings_.restirMode = static_cast<RestirMode>(render.value("restirMode", static_cast<uint32_t>(renderSettings_.restirMode)));
        renderSettings_.restirGiEnabled = render.value("restirGiEnabled", renderSettings_.restirGiEnabled);
        renderSettings_.denoiserEnabled = render.value("denoiserEnabled", renderSettings_.denoiserEnabled);
        renderSettings_.denoiseWhileMoving = render.value("denoiseWhileMoving", renderSettings_.denoiseWhileMoving);
        renderSettings_.samplesPerPixel = render.value("samplesPerPixel", renderSettings_.samplesPerPixel);
        renderSettings_.limitSamplesPerPixel = render.value("limitSamplesPerPixel", renderSettings_.limitSamplesPerPixel);
        renderSettings_.atrousIterations = render.value("atrousIterations", renderSettings_.atrousIterations);
        renderSettings_.denoiserStrength = render.value("denoiserStrength", renderSettings_.denoiserStrength);
        renderSettings_.denoiserMaxHistoryLength = render.value("denoiserMaxHistoryLength", renderSettings_.denoiserMaxHistoryLength);
        renderSettings_.momentValidityThreshold = render.value("momentValidityThreshold", renderSettings_.momentValidityThreshold);
        renderSettings_.taaEnabled = render.value("taaEnabled", renderSettings_.taaEnabled);
        renderSettings_.taaFeedback = render.value("taaFeedback", renderSettings_.taaFeedback);
        renderSettings_.taaMotionFeedback = render.value("taaMotionFeedback", renderSettings_.taaMotionFeedback);
        renderSettings_.taaReactiveFeedback = render.value("taaReactiveFeedback", renderSettings_.taaReactiveFeedback);
        renderSettings_.taaSharpeningStrength = render.value("taaSharpeningStrength", renderSettings_.taaSharpeningStrength);
        renderSettings_.debugView = static_cast<RendererDebugView>(render.value("debugView", static_cast<uint32_t>(renderSettings_.debugView)));
        renderSettings_.accumulate = render.value("accumulate", renderSettings_.accumulate);
        renderSettings_.accumulationLimit = render.value("accumulationLimit", renderSettings_.accumulationLimit);
        renderSettings_.resolutionScale = render.value("resolutionScale", renderSettings_.resolutionScale);
        renderSettings_.materialTextureAnisotropy = render.value("materialTextureAnisotropy", renderSettings_.materialTextureAnisotropy);
        renderSettings_.specularAaEnabled = render.value("specularAaEnabled", renderSettings_.specularAaEnabled);
        renderSettings_.opacityMicromapsEnabled = render.value("opacityMicromapsEnabled", renderSettings_.opacityMicromapsEnabled);
        renderSettings_.shadowRayBias = render.value("shadowRayBias", renderSettings_.shadowRayBias);
        renderSettings_.shadowDistanceBias = render.value("shadowDistanceBias", renderSettings_.shadowDistanceBias);
        renderSettings_.fireflyClamp = render.value("fireflyClamp", renderSettings_.fireflyClamp);
        renderSettings_.restirGiTemporalMaxAge = render.value("restirGiTemporalMaxAge", renderSettings_.restirGiTemporalMaxAge);
        renderSettings_.restirGiSpatialRounds = render.value("restirGiSpatialRounds", renderSettings_.restirGiSpatialRounds);
        renderSettings_.restirGiSpatialRadius = render.value("restirGiSpatialRadius", renderSettings_.restirGiSpatialRadius);
        renderSettings_.restirGiDepthThresholdScale = render.value("restirGiDepthThresholdScale", renderSettings_.restirGiDepthThresholdScale);
        renderSettings_.restirGiSpatialCompatibilityThreshold = render.value("restirGiSpatialCompatibilityThreshold", renderSettings_.restirGiSpatialCompatibilityThreshold);
        renderSettings_.restirGiHalfResolution = render.value("restirGiHalfResolution", renderSettings_.restirGiHalfResolution);
        renderSettings_.restirGiVisibilityRayBudget = render.value("restirGiVisibilityRayBudget", renderSettings_.restirGiVisibilityRayBudget);
        renderSettings_.adaptiveQualityMode = static_cast<AdaptiveQualityMode>(render.value("adaptiveQualityMode", static_cast<uint32_t>(renderSettings_.adaptiveQualityMode)));
        renderSettings_.adaptiveGpuFrameTargetMs = render.value("adaptiveGpuFrameTargetMs", renderSettings_.adaptiveGpuFrameTargetMs);
        renderSettings_.usePhysicalCamera = render.value("usePhysicalCamera", renderSettings_.usePhysicalCamera);
        renderSettings_.physicalAperture = render.value("physicalAperture", renderSettings_.physicalAperture);
        renderSettings_.physicalShutterSeconds = render.value("physicalShutterSeconds", renderSettings_.physicalShutterSeconds);
        renderSettings_.physicalIso = render.value("physicalIso", renderSettings_.physicalIso);
        renderSettings_.physicalExposureCompensation = render.value("physicalExposureCompensation", renderSettings_.physicalExposureCompensation);
        renderSettings_.dofApertureRadius = render.value("dofApertureRadius", renderSettings_.dofApertureRadius);
        renderSettings_.dofFocusDistance = render.value("dofFocusDistance", renderSettings_.dofFocusDistance);
        renderSettings_.dofBladeCount = render.value("dofBladeCount", renderSettings_.dofBladeCount);
        renderSettings_.dofBokehRotation = render.value("dofBokehRotation", renderSettings_.dofBokehRotation);
        renderSettings_.motionBlurEnabled = render.value("motionBlurEnabled", renderSettings_.motionBlurEnabled);
        renderSettings_.motionBlurShutterOpen = render.value("motionBlurShutterOpen", renderSettings_.motionBlurShutterOpen);
        renderSettings_.motionBlurShutterClose = render.value("motionBlurShutterClose", renderSettings_.motionBlurShutterClose);
        renderSettings_.homogeneousVolumeEnabled = render.value("homogeneousVolumeEnabled", renderSettings_.homogeneousVolumeEnabled);
        renderSettings_.homogeneousVolumeScattering = render.value("homogeneousVolumeScattering", renderSettings_.homogeneousVolumeScattering);
        renderSettings_.homogeneousVolumeAbsorption = render.value("homogeneousVolumeAbsorption", renderSettings_.homogeneousVolumeAbsorption);
        renderSettings_.homogeneousVolumeAnisotropy = render.value("homogeneousVolumeAnisotropy", renderSettings_.homogeneousVolumeAnisotropy);
        renderSettings_.mneeCausticsEnabled = render.value("mneeCausticsEnabled", renderSettings_.mneeCausticsEnabled);
    }

    registry_ = SceneRegistry{};
    prefabInstances_.clear();
    sceneMeshes_.clear();
    sceneMaterials_.clear();
    activeCamera_ = {};
    primarySun_ = {};

    std::unordered_map<uint64_t, EntityId> idMap;
    uint64_t maxUuid = 0;
    std::vector<std::pair<EntityId, uint64_t>> pendingParents;
    for (const nlohmann::json& item : root.value("entities", nlohmann::json::array())) {
        const EntityId id = registry_.createEntity(item.value("name", std::string{"Entity"}));
        Entity* entity = registry_.entity(id);
        if (entity == nullptr) {
            continue;
        }
        const uint64_t stable = item.contains("id")
            ? item["id"].value("uuid", item["id"].value("stable", entity->uuid))
            : entity->uuid;
        entity->uuid = stable;
        maxUuid = std::max(maxUuid, stable);
        idMap.emplace(stable, id);
        pendingParents.push_back({id, item.value("parent", uint64_t{0})});
        entity->locked = item.value("locked", false);

        if (item.contains("transform")) {
            const nlohmann::json& transform = item["transform"];
            entity->transform.position = vec3FromJson(transform.value("position", nlohmann::json::array()), entity->transform.position);
            entity->transform.rotationEuler = vec3FromJson(transform.value("rotationEuler", nlohmann::json::array()), entity->transform.rotationEuler);
            entity->transform.scale = vec3FromJson(transform.value("scale", nlohmann::json::array()), entity->transform.scale);
        }

        if (item.contains("meshRenderer")) {
            const nlohmann::json& source = item["meshRenderer"];
            MeshRenderer renderer;
            renderer.mesh = MeshAssetHandle{source.value("mesh", UINT32_MAX)};
            renderer.meshGuid = source.value("meshGuid", std::string{});
            renderer.visible = source.value("visible", true);
            renderer.castShadow = source.value("castShadow", true);
            renderer.visibleToCamera = source.value("visibleToCamera", true);
            for (const nlohmann::json& slotSource : source.value("materialSlots", nlohmann::json::array())) {
                MaterialSlot slot;
                slot.name = slotSource.value("name", std::string{});
                slot.material = MaterialAssetHandle{slotSource.value("material", UINT32_MAX)};
                slot.materialGuid = slotSource.value("materialGuid", std::string{});
                if (slotSource.contains("overrideMaterial")) {
                    slot.overrideMaterial = MaterialAssetHandle{slotSource.value("overrideMaterial", UINT32_MAX)};
                }
                if (slotSource.contains("overrideMaterialGuid")) {
                    slot.overrideMaterialGuid = slotSource.value("overrideMaterialGuid", std::string{});
                }
                renderer.materialSlots.push_back(slot);
                if (slot.resolvedMaterial().valid()) {
                    sceneMaterials_.push_back(slot.resolvedMaterial());
                }
            }
            if (renderer.mesh.valid()) {
                sceneMeshes_.push_back(renderer.mesh);
            }
            entity->meshRenderer = std::move(renderer);
        }
        if (item.contains("light")) {
            const nlohmann::json& source = item["light"];
            Light light;
            light.type = static_cast<LightType>(source.value("type", static_cast<uint32_t>(LightType::Point)));
            light.color = vec3FromJson(source.value("color", nlohmann::json::array()), light.color);
            light.intensity = source.value("intensity", light.intensity);
            light.sizeOrRadius = source.value("sizeOrRadius", light.sizeOrRadius);
            light.innerConeRadians = source.value("innerConeRadians", light.innerConeRadians);
            light.outerConeRadians = source.value("outerConeRadians", light.outerConeRadians);
            light.enabled = source.value("enabled", true);
            entity->light = light;
        }
        if (item.contains("sun")) {
            const nlohmann::json& source = item["sun"];
            Sun sun;
            sun.enabled = source.value("enabled", sun.enabled);
            sun.illuminanceLux = source.value("illuminanceLux", sun.illuminanceLux);
            sun.angularRadiusRadians = source.value("angularRadiusRadians", sun.angularRadiusRadians);
            sun.colorTemperatureKelvin = source.value("colorTemperatureKelvin", sun.colorTemperatureKelvin);
            entity->sun = sun;
        }
        if (item.contains("camera")) {
            const nlohmann::json& source = item["camera"];
            Camera camera;
            camera.verticalFovRadians = source.value("verticalFovRadians", camera.verticalFovRadians);
            camera.nearPlane = source.value("nearPlane", camera.nearPlane);
            camera.farPlane = source.value("farPlane", camera.farPlane);
            camera.active = source.value("active", false);
            camera.useRenderSettingsExposure = source.value("useRenderSettingsExposure", camera.useRenderSettingsExposure);
            entity->camera = camera;
            if (camera.active) {
                activeCamera_ = id;
            }
        }
        if (item.contains("environmentLight")) {
            const nlohmann::json& source = item["environmentLight"];
            EnvironmentLight component;
            component.enabled = source.value("enabled", component.enabled);
            component.intensity = source.value("intensity", component.intensity);
            component.backgroundIntensity = source.value("backgroundIntensity", component.backgroundIntensity);
            component.rotation = source.value("rotation", component.rotation);
            entity->environmentLight = component;
        }
        if (item.contains("skyAtmosphere")) {
            const nlohmann::json& source = item["skyAtmosphere"];
            SkyAtmosphere component;
            component.enabled = source.value("enabled", component.enabled);
            component.rayleighScaleHeight = source.value("rayleighScaleHeight", component.rayleighScaleHeight);
            component.mieScaleHeight = source.value("mieScaleHeight", component.mieScaleHeight);
            component.mieAnisotropy = source.value("mieAnisotropy", component.mieAnisotropy);
            component.groundAlbedo = source.value("groundAlbedo", component.groundAlbedo);
            entity->skyAtmosphere = component;
        }
        if (item.contains("heightFog")) {
            const nlohmann::json& source = item["heightFog"];
            HeightFog component;
            component.enabled = source.value("enabled", component.enabled);
            component.density = source.value("density", component.density);
            component.heightFalloff = source.value("heightFalloff", component.heightFalloff);
            component.color = vec3FromJson(source.value("color", nlohmann::json::array()), component.color);
            entity->heightFog = component;
        }
        if (item.contains("volumetricCloud")) {
            const nlohmann::json& source = item["volumetricCloud"];
            VolumetricCloud component;
            component.enabled = source.value("enabled", component.enabled);
            component.density = source.value("density", component.density);
            component.coverage = source.value("coverage", component.coverage);
            entity->volumetricCloud = component;
        }
        if (item.contains("postProcessVolume")) {
            const nlohmann::json& source = item["postProcessVolume"];
            PostProcessVolume component;
            component.enabled = source.value("enabled", component.enabled);
            component.unbound = source.value("unbound", component.unbound);
            component.priority = source.value("priority", component.priority);
            component.exposureCompensation = source.value("exposureCompensation", component.exposureCompensation);
            component.saturation = source.value("saturation", component.saturation);
            component.contrast = source.value("contrast", component.contrast);
            entity->postProcessVolume = component;
        }
        if (item.contains("cameraPostProcess")) {
            const nlohmann::json& source = item["cameraPostProcess"];
            CameraPostProcess component;
            component.enabled = source.value("enabled", component.enabled);
            component.overrideExposure = source.value("overrideExposure", component.overrideExposure);
            component.exposureCompensation = source.value("exposureCompensation", component.exposureCompensation);
            component.overrideDepthOfField = source.value("overrideDepthOfField", component.overrideDepthOfField);
            component.dofApertureRadius = source.value("dofApertureRadius", component.dofApertureRadius);
            component.dofFocusDistance = source.value("dofFocusDistance", component.dofFocusDistance);
            entity->cameraPostProcess = component;
        }
    }

    registry_.ensureUuidCounter(maxUuid);

    for (const auto& [child, parentStable] : pendingParents) {
        if (parentStable == 0u) {
            continue;
        }
        const auto it = idMap.find(parentStable);
        Entity* childEntity = registry_.entity(child);
        Entity* parentEntity = it != idMap.end() ? registry_.entity(it->second) : nullptr;
        if (childEntity != nullptr && parentEntity != nullptr) {
            childEntity->parent = parentEntity->id;
            parentEntity->children.push_back(child);
        }
    }

    const uint64_t activeStable = root.value("activeCamera", uint64_t{0});
    if (activeStable != 0u) {
        const auto it = idMap.find(activeStable);
        if (it != idMap.end() && registry_.camera(it->second) != nullptr) {
            setActiveCamera(it->second);
        }
    }
    if (!activeCamera_.valid()) {
        for (Entity* entity : registry_.entities()) {
            if (entity->camera.has_value()) {
                setActiveCamera(entity->id);
                break;
            }
        }
    }
    const uint64_t primarySunStable = root.value("primarySun", uint64_t{0});
    if (primarySunStable != 0u) {
        const auto it = idMap.find(primarySunStable);
        if (it != idMap.end() && registry_.sun(it->second) != nullptr) {
            setPrimarySun(it->second);
        }
    }
    if (!primarySun_.valid()) {
        (void)SunController::migrateLegacyDirectionalSun(*this);
    }
    if (root.contains("worldSettings") && root["worldSettings"].is_object()) {
        const nlohmann::json& world = root["worldSettings"];
        auto resolve = [&](const char* key) -> EntityId {
            const uint64_t uuid = world.value(key, uint64_t{0});
            const auto it = idMap.find(uuid);
            return it != idMap.end() ? it->second : EntityId{};
        };
        worldSettings_.activeEnvironment = resolve("activeEnvironment");
        worldSettings_.primarySun = resolve("primarySun");
        worldSettings_.skyAtmosphere = resolve("skyAtmosphere");
        worldSettings_.heightFog = resolve("heightFog");
        worldSettings_.postProcessVolume = resolve("postProcessVolume");
        worldSettings_.atmosphereEnabled = world.value("atmosphereEnabled", worldSettings_.atmosphereEnabled);
        worldSettings_.fogEnabled = world.value("fogEnabled", worldSettings_.fogEnabled);
        worldSettings_.postProcessEnabled = world.value("postProcessEnabled", worldSettings_.postProcessEnabled);
    }
    SunController::enforceSinglePrimarySun(*this);
    (void)SunController::repairPrimarySunTransform(*this);

    std::sort(sceneMeshes_.begin(), sceneMeshes_.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index < b.index; });
    sceneMeshes_.erase(std::unique(sceneMeshes_.begin(), sceneMeshes_.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index == b.index; }), sceneMeshes_.end());
    std::sort(sceneMaterials_.begin(), sceneMaterials_.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index < b.index; });
    sceneMaterials_.erase(std::unique(sceneMaterials_.begin(), sceneMaterials_.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index == b.index; }), sceneMaterials_.end());
    if (root.contains("bookmarks") && root["bookmarks"].is_array()) {
        bookmarksJson_ = root["bookmarks"];
    } else {
        bookmarksJson_.reset();
    }
    if (root.contains("timeline") && root["timeline"].is_object()) {
        timelineJson_ = root["timeline"];
    } else {
        timelineJson_.reset();
    }
    dirtyReasons_.clear();
    if (root.contains("dirtyReasons") && root["dirtyReasons"].is_array()) {
        for (const nlohmann::json& reason : root["dirtyReasons"]) {
            if (reason.is_string()) {
                dirtyReasons_.push_back(reason.get<std::string>());
            }
        }
        if (!dirtyReasons_.empty()) {
            lastChangeReason_ = dirtyReasons_.back();
        }
    }
    if (root.contains("prefabInstances") && root["prefabInstances"].is_array()) {
        for (const nlohmann::json& source : root["prefabInstances"]) {
            PrefabInstance instance;
            instance.prefabGuid = source.value("prefabGuid", std::string{});
            const uint64_t rootUuid = source.value("instanceRoot", uint64_t{0});
            const auto rootIt = idMap.find(rootUuid);
            if (rootIt != idMap.end()) {
                instance.instanceRoot = rootIt->second;
            }
            if (source.contains("generatedEntityUuids") && source["generatedEntityUuids"].is_array()) {
                for (const nlohmann::json& uuid : source["generatedEntityUuids"]) {
                    instance.generatedEntityUuids.push_back(uuid.get<uint64_t>());
                }
            }
            if (source.contains("overrides") && source["overrides"].is_array()) {
                for (const nlohmann::json& overrideJson : source["overrides"]) {
                    instance.overrides.push_back(PrefabOverride{
                        overrideJson.value("path", std::string{}),
                        overrideJson.value("value", std::string{}),
                    });
                }
            }
            if (!instance.prefabGuid.empty()) {
                prefabInstances_.push_back(std::move(instance));
            }
        }
    }
    markDirty(SceneUpdateKind::TopologyChanged);
    return true;
}

SceneUpdateKind SceneDocument::combine(SceneUpdateKind current, SceneUpdateKind next) {
    if (next == SceneUpdateKind::None) {
        return current;
    }
    if (current == SceneUpdateKind::None) {
        return next;
    }
    if (current == next) {
        return current;
    }
    if (current == SceneUpdateKind::RendererSettingsOnly) {
        return next;
    }
    if (next == SceneUpdateKind::RendererSettingsOnly) {
        return current;
    }
    if (current == SceneUpdateKind::TopologyChanged || next == SceneUpdateKind::TopologyChanged) {
        return SceneUpdateKind::TopologyChanged;
    }
    return SceneUpdateKind::TopologyChanged;
}

} // namespace rtv
