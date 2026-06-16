#include "rtv/SceneDocument.h"

#include "rtv/SceneRenderSettingsSync.h"
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

glm::mat3 rotationFromMatrix(const glm::mat4& matrix) {
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
    return rotation;
}

glm::vec3 eulerFromMatrix(const glm::mat4& matrix) {
    const glm::mat3 rotation = rotationFromMatrix(matrix);
    return glm::eulerAngles(glm::quat_cast(rotation));
}

bool directionValid(glm::vec3 direction) {
    return glm::dot(direction, direction) > 1.0e-8f;
}

glm::mat3 cameraLookRotation(glm::vec3 forward) {
    forward = glm::normalize(forward);
    const glm::vec3 zAxis = -forward;
    glm::vec3 xAxis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), zAxis);
    if (!directionValid(xAxis)) {
        xAxis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), zAxis);
    }
    xAxis = glm::normalize(xAxis);
    const glm::vec3 yAxis = glm::normalize(glm::cross(zAxis, xAxis));
    return glm::mat3(xAxis, yAxis, zAxis);
}

glm::vec3 localEulerFromWorldRotation(const SceneRegistry& registry, const Entity& entity, const glm::mat3& worldRotation) {
    glm::mat3 localRotation = worldRotation;
    if (entity.parent.valid()) {
        if (const Entity* parent = registry.entity(entity.parent)) {
            const glm::mat3 parentWorldRotation = rotationFromMatrix(entityWorldMatrix(registry, *parent));
            localRotation = glm::transpose(parentWorldRotation) * worldRotation;
        }
    }
    return glm::eulerAngles(glm::normalize(glm::quat_cast(localRotation)));
}

Camera cameraFromSceneNode(const SceneNodeAsset& node) {
    Camera camera;
    camera.projection = node.cameraProjection;
    camera.verticalFovRadians = node.cameraYfov;
    camera.aspectRatio = node.cameraAspectRatio;
    camera.orthographicXmag = node.cameraOrthoXmag;
    camera.orthographicYmag = node.cameraOrthoYmag;
    camera.nearPlane = node.cameraNear;
    camera.farPlane = node.cameraFar;
    return camera;
}

nlohmann::json vec3Json(glm::vec3 value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

nlohmann::json matrixJson(const glm::mat4& matrix) {
    nlohmann::json values = nlohmann::json::array();
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            values.push_back(matrix[col][row]);
        }
    }
    return values;
}

glm::mat4 matrixFromJson(const nlohmann::json& json, const glm::mat4& fallback = glm::mat4{1.0f}) {
    if (!json.is_array() || json.size() < 16) {
        return fallback;
    }
    glm::mat4 result{1.0f};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            result[col][row] = json[static_cast<size_t>(col * 4 + row)].get<float>();
        }
    }
    return result;
}

std::vector<float> floatVectorFromJson(const nlohmann::json& value) {
    std::vector<float> result;
    if (!value.is_array()) {
        return result;
    }
    result.reserve(value.size());
    for (const nlohmann::json& item : value) {
        if (item.is_number()) {
            result.push_back(item.get<float>());
        }
    }
    return result;
}

nlohmann::json transformJson(const Transform& transform) {
    return {
        {"position", vec3Json(transform.position)},
        {"rotationEuler", vec3Json(transform.rotationEuler)},
        {"scale", vec3Json(transform.scale)},
    };
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

Transform transformFromJson(const nlohmann::json& json, Transform fallback = {}) {
    if (!json.is_object()) {
        return fallback;
    }
    Transform transform = fallback;
    transform.position = vec3FromJson(json.value("position", nlohmann::json::array()), transform.position);
    transform.rotationEuler = vec3FromJson(json.value("rotationEuler", nlohmann::json::array()), transform.rotationEuler);
    transform.scale = vec3FromJson(json.value("scale", nlohmann::json::array()), transform.scale);
    return transform;
}

const char* editorPivotModeName(EditorPivotMode mode) {
    switch (mode) {
    case EditorPivotMode::Active: return "Active";
    case EditorPivotMode::SelectionCenter: return "SelectionCenter";
    case EditorPivotMode::BoundsCenter: return "BoundsCenter";
    case EditorPivotMode::Custom: return "Custom";
    }
    return "Active";
}

EditorPivotMode editorPivotModeFromString(const std::string& value) {
    if (value == "SelectionCenter") return EditorPivotMode::SelectionCenter;
    if (value == "BoundsCenter") return EditorPivotMode::BoundsCenter;
    if (value == "Custom") return EditorPivotMode::Custom;
    return EditorPivotMode::Active;
}

nlohmann::json editorPivotJson(const EditorPivotSettings& pivot) {
    return {
        {"mode", editorPivotModeName(pivot.mode)},
        {"customPosition", vec3Json(pivot.customPosition)},
        {"customRotationEuler", vec3Json(pivot.customRotationEuler)},
    };
}

EditorPivotSettings editorPivotFromJson(const nlohmann::json& json) {
    EditorPivotSettings pivot;
    if (!json.is_object()) {
        return pivot;
    }
    pivot.mode = editorPivotModeFromString(json.value("mode", std::string{"Active"}));
    pivot.customPosition = vec3FromJson(json.value("customPosition", nlohmann::json::array()), pivot.customPosition);
    pivot.customRotationEuler = vec3FromJson(json.value("customRotationEuler", nlohmann::json::array()), pivot.customRotationEuler);
    return pivot;
}

nlohmann::json levelInstanceJson(const LevelInstance& instance) {
    return {
        {"sceneGuid", instance.sceneGuid},
        {"scenePath", instance.scenePath.generic_string()},
        {"visible", instance.visible},
        {"loaded", instance.loaded},
        {"editable", instance.editable},
        {"sourceRevision", instance.sourceRevision},
        {"sourceHash", instance.sourceHash},
        {"overridesDirty", instance.overridesDirty},
        {"sourceDirty", instance.sourceDirty},
    };
}

LevelInstance levelInstanceFromJson(const nlohmann::json& json) {
    LevelInstance instance;
    if (!json.is_object()) {
        return instance;
    }
    instance.sceneGuid = json.value("sceneGuid", std::string{});
    instance.scenePath = json.value("scenePath", std::string{});
    instance.visible = json.value("visible", instance.visible);
    instance.loaded = json.value("loaded", instance.loaded);
    instance.editable = json.value("editable", instance.editable);
    instance.sourceRevision = json.value("sourceRevision", std::string{});
    instance.sourceHash = json.value("sourceHash", std::string{});
    instance.overridesDirty = json.value("overridesDirty", instance.overridesDirty);
    instance.sourceDirty = json.value("sourceDirty", instance.sourceDirty);
    return instance;
}

nlohmann::json sublevelJson(const SceneSublevelRecord& sublevel) {
    return {
        {"sceneGuid", sublevel.sceneGuid},
        {"scenePath", sublevel.scenePath.generic_string()},
        {"transform", transformJson(sublevel.transform)},
        {"visible", sublevel.visible},
        {"loaded", sublevel.loaded},
        {"editable", sublevel.editable},
        {"sourceRevision", sublevel.sourceRevision},
        {"sourceHash", sublevel.sourceHash},
        {"overridesDirty", sublevel.overridesDirty},
        {"sourceDirty", sublevel.sourceDirty},
    };
}

SceneSublevelRecord sublevelFromJson(const nlohmann::json& json) {
    SceneSublevelRecord sublevel;
    if (!json.is_object()) {
        return sublevel;
    }
    sublevel.sceneGuid = json.value("sceneGuid", std::string{});
    sublevel.scenePath = json.value("scenePath", std::string{});
    sublevel.transform = transformFromJson(json.value("transform", nlohmann::json::object()), sublevel.transform);
    sublevel.visible = json.value("visible", sublevel.visible);
    sublevel.loaded = json.value("loaded", sublevel.loaded);
    sublevel.editable = json.value("editable", sublevel.editable);
    sublevel.sourceRevision = json.value("sourceRevision", std::string{});
    sublevel.sourceHash = json.value("sourceHash", std::string{});
    sublevel.overridesDirty = json.value("overridesDirty", sublevel.overridesDirty);
    sublevel.sourceDirty = json.value("sourceDirty", sublevel.sourceDirty);
    return sublevel;
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
    markDirty(sceneUpdateKindMask(kind));
}

void SceneDocument::markDirty(SceneUpdateMask mask) {
    dirty_ = true;
    pendingUpdateMask_ |= mask;
    lastChangeReason_ = sceneUpdateMaskName(mask);
    if (dirtyReasons_.empty() || dirtyReasons_.back() != lastChangeReason_) {
        dirtyReasons_.push_back(lastChangeReason_);
    }
}

void SceneDocument::clearDirty() {
    dirty_ = false;
    pendingUpdateMask_ = SceneUpdateMaskNone;
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

void SceneDocument::setEditorPivot(EditorPivotSettings pivot) {
    editorPivot_ = pivot;
}

void SceneDocument::setSublevels(std::vector<SceneSublevelRecord> sublevels) {
    sublevels_ = std::move(sublevels);
    markDirty(SceneUpdateKind::TopologyChanged);
}

void SceneDocument::addSublevel(SceneSublevelRecord sublevel) {
    auto existing = std::find_if(sublevels_.begin(), sublevels_.end(), [&](const SceneSublevelRecord& item) {
        if (!sublevel.sceneGuid.empty() && item.sceneGuid == sublevel.sceneGuid) {
            return true;
        }
        return !sublevel.scenePath.empty() && item.scenePath == sublevel.scenePath;
    });
    if (existing != sublevels_.end()) {
        *existing = std::move(sublevel);
    } else {
        sublevels_.push_back(std::move(sublevel));
    }
    markDirty(SceneUpdateKind::TopologyChanged);
}

bool SceneDocument::removeSublevel(const AssetGuid& sceneGuid) {
    if (sceneGuid.empty()) {
        return false;
    }
    const auto oldSize = sublevels_.size();
    sublevels_.erase(std::remove_if(sublevels_.begin(), sublevels_.end(), [&](const SceneSublevelRecord& item) {
        return item.sceneGuid == sceneGuid;
    }), sublevels_.end());
    if (sublevels_.size() == oldSize) {
        return false;
    }
    markDirty(SceneUpdateKind::TopologyChanged);
    return true;
}

bool SceneDocument::hasSublevelDirtyState() const {
    const bool sublevelRecordDirty = std::any_of(sublevels_.begin(), sublevels_.end(), [](const SceneSublevelRecord& item) {
        return item.overridesDirty || item.sourceDirty;
    });
    if (sublevelRecordDirty) {
        return true;
    }
    const std::vector<const Entity*> entities = registry_.entities();
    return std::any_of(entities.begin(), entities.end(), [](const Entity* entity) {
        return entity != nullptr && entity->levelInstance.has_value() &&
            (entity->levelInstance->overridesDirty || entity->levelInstance->sourceDirty);
    });
}

void SceneDocument::addPrefabInstance(PrefabInstance instance) {
    prefabInstances_.push_back(std::move(instance));
    markDirty(SceneUpdateKind::TopologyChanged);
}

size_t SceneDocument::replaceAssetGuidReferences(const AssetGuid& oldGuid, const AssetGuid& newGuid) {
    if (oldGuid.empty() || newGuid.empty() || oldGuid == newGuid) {
        return 0;
    }

    size_t replacements = 0;
    for (Entity* entity : registry_.entities()) {
        if (entity == nullptr) {
            continue;
        }
        if (entity->meshRenderer.has_value()) {
            MeshRenderer& renderer = *entity->meshRenderer;
            if (renderer.meshGuid == oldGuid) {
                renderer.meshGuid = newGuid;
                ++replacements;
            }
            for (MaterialSlot& slot : renderer.materialSlots) {
                if (slot.materialGuid == oldGuid) {
                    slot.materialGuid = newGuid;
                    ++replacements;
                }
                if (slot.overrideMaterialGuid.has_value() && *slot.overrideMaterialGuid == oldGuid) {
                    slot.overrideMaterialGuid = newGuid;
                    ++replacements;
                }
            }
        }
        if (entity->animationPlayer.has_value()) {
            if (entity->animationPlayer->animationGuid == oldGuid) {
                entity->animationPlayer->animationGuid = newGuid;
                ++replacements;
            }
            if (entity->animationPlayer->controllerGuid == oldGuid) {
                entity->animationPlayer->controllerGuid = newGuid;
                ++replacements;
            }
        }
        if (entity->levelInstance.has_value() && entity->levelInstance->sceneGuid == oldGuid) {
            entity->levelInstance->sceneGuid = newGuid;
            ++replacements;
        }
    }
    for (SceneSublevelRecord& sublevel : sublevels_) {
        if (sublevel.sceneGuid == oldGuid) {
            sublevel.sceneGuid = newGuid;
            ++replacements;
        }
    }
    for (PrefabInstance& instance : prefabInstances_) {
        if (instance.prefabGuid == oldGuid) {
            instance.prefabGuid = newGuid;
            ++replacements;
        }
    }
    if (replacements > 0) {
        markDirty(SceneUpdateKind::TopologyChanged);
    }
    return replacements;
}

SceneUpdateKind SceneDocument::pendingUpdate() const {
    return sceneUpdateKindFromMask(pendingUpdateMask());
}

SceneUpdateMask SceneDocument::pendingUpdateMask() const {
    return pendingUpdateMask_ | registry_.pendingUpdateMask();
}

void SceneDocument::importSceneAsset(const SceneAsset& scene) {
    registry_ = SceneRegistry{};
    activeCamera_ = {};
    sublevels_.clear();
    prefabInstances_.clear();
    sceneTextures_ = scene.textures;
    sceneMaterials_ = scene.materials;
    sceneMeshes_ = scene.meshes;
    sceneSkins_ = scene.skins;

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
        entity->defaultTransform = entity->transform;
        entity->sourceNodeIndex = node.sourceNodeIndex >= 0 ? node.sourceNodeIndex : static_cast<int32_t>(i);

        if (node.mesh.valid()) {
            MeshRenderer renderer;
            renderer.mesh = node.mesh;
            renderer.morphWeights = node.morphWeights;
            renderer.skinIndex = node.skinIndex;
            renderer.materialSlots.clear();
            entity->meshRenderer = renderer;
        }
        if (node.hasCamera) {
            Camera camera = cameraFromSceneNode(node);
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
        if (source.nodeIndex < 0) {
            entity->transform.position = translationFromMatrix(source.transform);
            entity->transform.rotationEuler = eulerFromMatrix(source.transform);
            entity->transform.scale = scaleFromMatrix(source.transform);
            entity->defaultTransform = entity->transform;
        }
        Light light;
        light.type = static_cast<LightType>(std::min(source.type, 3u));
        light.color = source.color;
        light.intensity = source.intensity;
        light.sizeOrRadius = source.sizeOrRadius;
        light.innerConeRadians = source.innerConeRadians;
        light.outerConeRadians = source.outerConeRadians;
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
                const glm::mat4 refWorld = entityWorldMatrix(registry_, *refEntity);
                const glm::mat4 targetWorld = entityWorldMatrix(registry_, *entity);
                glm::vec3 forward = glm::vec3(targetWorld[3]) - glm::vec3(refWorld[3]);
                if (!directionValid(forward)) {
                    continue;
                }
                forward = glm::normalize(forward);
                if (refEntity->camera.has_value()) {
                    glm::vec3 currentForward = rotationFromMatrix(refWorld) * glm::vec3(0.0f, 0.0f, -1.0f);
                    if (directionValid(currentForward)) {
                        currentForward = glm::normalize(currentForward);
                        if (glm::dot(currentForward, forward) > 0.999f) {
                            continue;
                        }
                    }
                    refEntity->transform.rotationEuler = localEulerFromWorldRotation(
                        registry_,
                        *refEntity,
                        cameraLookRotation(forward));
                    refEntity->defaultTransform = refEntity->transform;
                } else if (refEntity->sun.has_value()) {
                    float elevation = 0.0f;
                    float azimuth = 0.0f;
                    SunController::anglesFromDirection(-forward, elevation, azimuth);
                    refEntity->sun->elevation = elevation;
                    refEntity->sun->azimuth = azimuth;
                }
            }
        }
    }

    for (Entity* entity : registry_.entities()) {
        if (entity != nullptr) {
            entity->defaultTransform = entity->transform;
        }
    }

    clearDirty();
    markDirty(SceneUpdateKind::TopologyChanged);
}

int32_t SceneDocument::appendSceneSkins(const std::vector<SceneSkinAsset>& skins) {
    if (skins.empty()) {
        return static_cast<int32_t>(sceneSkins_.size());
    }
    const int32_t offset = static_cast<int32_t>(sceneSkins_.size());
    sceneSkins_.insert(sceneSkins_.end(), skins.begin(), skins.end());
    return offset;
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
    scene.skins = sceneSkins_;

    std::vector<const Entity*> entities = registry_.entities();
    uint32_t maxEntityIndex = 0;
    for (const Entity* entity : entities) {
        if (entity != nullptr) {
            maxEntityIndex = std::max(maxEntityIndex, entity->id.index);
        }
    }
    std::vector<int32_t> nodeIndexForEntity(maxEntityIndex + 1u, -1);
    scene.nodes.reserve(entities.size());
    for (const Entity* entity : entities) {
        if (entity == nullptr) {
            continue;
        }
        SceneNodeAsset node;
        node.name = entity->name;
        node.transform = entity->transform.localMatrix();
        node.previousTransform = entity->previousAnimationTransformValid
            ? entity->previousAnimationTransform.localMatrix()
            : node.transform;
        node.previousTransformValid = entity->previousAnimationTransformValid;
        if (entity->meshRenderer.has_value()) {
            node.mesh = entity->meshRenderer->mesh;
            node.morphWeights = entity->meshRenderer->morphWeights;
            node.skinIndex = entity->meshRenderer->skinIndex;
            node.visible = entity->meshRenderer->visible;
            node.castShadow = entity->meshRenderer->castShadow;
            node.receiveShadow = entity->meshRenderer->receiveShadow;
            node.visibleToCamera = entity->meshRenderer->visibleToCamera;
            node.renderLayer = entity->meshRenderer->renderLayer;
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
        if (entity->id.index < nodeIndexForEntity.size()) {
            nodeIndexForEntity[entity->id.index] = static_cast<int32_t>(scene.nodes.size());
        }
        scene.nodes.push_back(node);
    }

    for (size_t i = 0; i < entities.size(); ++i) {
        const Entity* entity = entities[i];
        if (entity == nullptr || i >= scene.nodes.size()) {
            continue;
        }
        SceneNodeAsset& node = scene.nodes[i];
        if (entity->parent.valid()) {
            const Entity* parentEntity = registry_.entity(entity->parent);
            if (parentEntity != nullptr &&
                parentEntity->id.index < nodeIndexForEntity.size() &&
                nodeIndexForEntity[parentEntity->id.index] >= 0) {
                node.parent = nodeIndexForEntity[parentEntity->id.index];
            }
        }
        for (EntityId child : entity->children) {
            const Entity* childEntity = registry_.entity(child);
            if (childEntity != nullptr &&
                childEntity->id.index < nodeIndexForEntity.size() &&
                nodeIndexForEntity[childEntity->id.index] >= 0) {
                node.children.push_back(static_cast<uint32_t>(nodeIndexForEntity[childEntity->id.index]));
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
            light.innerConeRadians = entity->light->innerConeRadians;
            light.outerConeRadians = entity->light->outerConeRadians;
            light.enabled = entity->light->enabled;
            light.nodeIndex = static_cast<int32_t>(i);
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
        {"animations", nlohmann::json::array()},
        {"sublevels", nlohmann::json::array()},
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
        {"denoiserBackend", static_cast<uint32_t>(renderSettings_.denoiserBackend)},
        {"denoiseWhileMoving", renderSettings_.denoiseWhileMoving},
        {"samplesPerPixel", renderSettings_.samplesPerPixel},
        {"limitSamplesPerPixel", renderSettings_.limitSamplesPerPixel},
        {"atrousIterations", renderSettings_.atrousIterations},
        {"denoiserStrength", renderSettings_.denoiserStrength},
        {"denoiserMaxHistoryLength", renderSettings_.denoiserMaxHistoryLength},
        {"momentValidityThreshold", renderSettings_.momentValidityThreshold},
        {"taaEnabled", renderSettings_.taaEnabled},
        {"temporalUpscaler", static_cast<uint32_t>(renderSettings_.temporalUpscaler)},
        {"dlssFrameGenerationEnabled", renderSettings_.dlssFrameGenerationEnabled},
        {"dlssRayReconstructionEnabled", renderSettings_.dlssRayReconstructionEnabled},
        {"streamlineReflexEnabled", renderSettings_.streamlineReflexEnabled},
        {"dlssSharpeningStrength", renderSettings_.dlssSharpeningStrength},
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
        {"restirGiFinalStabilizationEnabled", renderSettings_.restirGiFinalStabilizationEnabled},
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

    root["skins"] = nlohmann::json::array();
    for (size_t skinIndex = 0; skinIndex < sceneSkins_.size(); ++skinIndex) {
        const SceneSkinAsset& skin = sceneSkins_[skinIndex];
        nlohmann::json joints = nlohmann::json::array();
        for (uint32_t joint : skin.joints) {
            joints.push_back(joint);
        }
        nlohmann::json inverseBindMatrices = nlohmann::json::array();
        for (const glm::mat4& matrix : skin.inverseBindMatrices) {
            inverseBindMatrices.push_back(matrixJson(matrix));
        }
        root["skins"].push_back({
            {"index", skinIndex},
            {"name", skin.name},
            {"skeletonRoot", skin.skeletonRoot},
            {"joints", joints},
            {"inverseBindMatrices", inverseBindMatrices},
        });
    }

    root["sublevels"] = nlohmann::json::array();
    for (const SceneSublevelRecord& sublevel : sublevels_) {
        root["sublevels"].push_back(sublevelJson(sublevel));
        if (!sublevel.sceneGuid.empty()) {
            root["assetReferences"]["sublevels"].push_back({
                {"assetGuid", sublevel.sceneGuid},
                {"scenePath", sublevel.scenePath.generic_string()},
            });
        }
    }

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
        item["layer"] = entity.layer;
        item["tags"] = entity.tags;
        item["collections"] = entity.collections;
        item["visible"] = entity.visible;
        item["locked"] = entity.locked;
        item["sourceNodeIndex"] = entity.sourceNodeIndex;
        item["transform"] = transformJson(entity.transform);
        item["defaultTransform"] = transformJson(entity.defaultTransform);
        if (entity.meshRenderer.has_value()) {
            nlohmann::json renderer;
            renderer["mesh"] = entity.meshRenderer->mesh.index;
            renderer["meshGuid"] = entity.meshRenderer->meshGuid;
            renderer["visible"] = entity.meshRenderer->visible;
            renderer["castShadow"] = entity.meshRenderer->castShadow;
            renderer["receiveShadow"] = entity.meshRenderer->receiveShadow;
            renderer["visibleToCamera"] = entity.meshRenderer->visibleToCamera;
            renderer["renderLayer"] = entity.meshRenderer->renderLayer;
            if (!entity.meshRenderer->morphWeights.empty()) {
                renderer["morphWeights"] = entity.meshRenderer->morphWeights;
            }
            renderer["skinIndex"] = entity.meshRenderer->skinIndex;
            renderer["activeMaterialVariantIndex"] = entity.meshRenderer->activeMaterialVariantIndex;
            renderer["activeMaterialVariantName"] = entity.meshRenderer->activeMaterialVariantName;
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
        if (entity.animationPlayer.has_value()) {
            nlohmann::json controllerParameters = nlohmann::json::array();
            for (const AnimationControllerParameterOverride& parameter : entity.animationPlayer->controllerParameters) {
                controllerParameters.push_back({
                    {"name", parameter.name},
                    {"type", parameter.type},
                    {"boolValue", parameter.boolValue},
                    {"intValue", parameter.intValue},
                    {"floatValue", parameter.floatValue},
                    {"triggerValue", parameter.triggerValue},
                });
            }
            item["animationPlayer"] = {
                {"animationGuid", entity.animationPlayer->animationGuid},
                {"animationPath", entity.animationPlayer->animationPath.generic_string()},
                {"controllerGuid", entity.animationPlayer->controllerGuid},
                {"controllerPath", entity.animationPlayer->controllerPath.generic_string()},
                {"controllerState", entity.animationPlayer->controllerState},
                {"controllerParameters", controllerParameters},
                {"enabled", entity.animationPlayer->enabled},
                {"playOnStart", entity.animationPlayer->playOnStart},
                {"playing", entity.animationPlayer->playing},
                {"loop", entity.animationPlayer->loop},
                {"applyRootMotion", entity.animationPlayer->applyRootMotion},
                {"applyMorphWeights", entity.animationPlayer->applyMorphWeights},
                {"playbackSpeed", entity.animationPlayer->playbackSpeed},
                {"currentTimeSeconds", entity.animationPlayer->currentTimeSeconds},
            };
            if (!entity.animationPlayer->animationGuid.empty()) {
                root["assetReferences"]["animations"].push_back({{"assetGuid", entity.animationPlayer->animationGuid}});
            }
            if (!entity.animationPlayer->controllerGuid.empty()) {
                root["assetReferences"]["animations"].push_back({{"assetGuid", entity.animationPlayer->controllerGuid}});
            }
        }
        if (entity.levelInstance.has_value()) {
            item["levelInstance"] = levelInstanceJson(*entity.levelInstance);
            if (!entity.levelInstance->sceneGuid.empty()) {
                root["assetReferences"]["sublevels"].push_back({
                    {"assetGuid", entity.levelInstance->sceneGuid},
                    {"scenePath", entity.levelInstance->scenePath.generic_string()},
                });
            }
        }
        if (entity.light.has_value()) {
            item["light"] = {
                {"type", static_cast<uint32_t>(entity.light->type)},
                {"color", vec3Json(entity.light->color)},
                {"intensity", entity.light->intensity},
                {"exposureMultiplier", entity.light->exposureMultiplier},
                {"sizeOrRadius", entity.light->sizeOrRadius},
                {"innerConeRadians", entity.light->innerConeRadians},
                {"outerConeRadians", entity.light->outerConeRadians},
                {"enabled", entity.light->enabled},
                {"useColorTemperature", entity.light->useColorTemperature},
                {"colorTemperatureKelvin", entity.light->colorTemperatureKelvin},
                {"visibleToCamera", entity.light->visibleToCamera},
                {"castSurfaceShadows", entity.light->castSurfaceShadows},
                {"castVolumetricShadows", entity.light->castVolumetricShadows},
                {"iesProfile", entity.light->iesProfile},
                {"materialSource", entity.light->materialSource},
            };
        }
        if (entity.sun.has_value()) {
            item["sun"] = {
                {"enabled", entity.sun->enabled},
                {"elevation", entity.sun->elevation},
                {"azimuth", entity.sun->azimuth},
                {"illuminanceLux", entity.sun->illuminanceLux},
                {"exposureMultiplier", entity.sun->exposureMultiplier},
                {"angularRadiusRadians", entity.sun->angularRadiusRadians},
                {"useColorTemperature", entity.sun->useColorTemperature},
                {"colorTemperatureKelvin", entity.sun->colorTemperatureKelvin},
                {"castSurfaceShadows", entity.sun->castSurfaceShadows},
                {"castVolumetricShadows", entity.sun->castVolumetricShadows},
                {"shadowBounces", entity.sun->shadowBounces},
                {"volumetricShadowBounces", entity.sun->volumetricShadowBounces},
            };
        }
        if (entity.camera.has_value()) {
            item["camera"] = {
                {"projection", entity.camera->projection},
                {"verticalFovRadians", entity.camera->verticalFovRadians},
                {"aspectRatio", entity.camera->aspectRatio},
                {"orthographicXmag", entity.camera->orthographicXmag},
                {"orthographicYmag", entity.camera->orthographicYmag},
                {"nearPlane", entity.camera->nearPlane},
                {"farPlane", entity.camera->farPlane},
                {"active", entity.camera->active},
                {"useRenderSettingsExposure", entity.camera->useRenderSettingsExposure},
            };
        }
        if (entity.environmentLight.has_value()) {
            item["environmentLight"] = {
                {"hdrPath", entity.environmentLight->hdrPath.generic_string()},
                {"enabled", entity.environmentLight->enabled},
                {"intensity", entity.environmentLight->intensity},
                {"backgroundIntensity", entity.environmentLight->backgroundIntensity},
                {"rotation", entity.environmentLight->rotation},
            };
        }
        if (entity.skyAtmosphere.has_value()) {
            item["skyAtmosphere"] = {
                {"enabled", entity.skyAtmosphere->enabled},
                {"skyIntensity", entity.skyAtmosphere->skyIntensity},
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
                {"bloomEnabled", entity.cameraPostProcess->bloomEnabled},
                {"bloomIntensity", entity.cameraPostProcess->bloomIntensity},
                {"colorCorrectionEnabled", entity.cameraPostProcess->colorCorrectionEnabled},
                {"colorCorrectionSaturation", entity.cameraPostProcess->colorCorrectionSaturation},
                {"colorCorrectionContrast", entity.cameraPostProcess->colorCorrectionContrast},
                {"vignettingEnabled", entity.cameraPostProcess->vignettingEnabled},
                {"vignettingIntensity", entity.cameraPostProcess->vignettingIntensity},
                {"filmGrainEnabled", entity.cameraPostProcess->filmGrainEnabled},
                {"filmGrainIntensity", entity.cameraPostProcess->filmGrainIntensity},
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
    root["editorMetadata"] = {
        {"pivot", editorPivotJson(editorPivot_)},
    };
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
    editorPivot_ = EditorPivotSettings{};
    sublevels_.clear();
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
    if (root.contains("editorMetadata") && root["editorMetadata"].is_object()) {
        const nlohmann::json& editorMetadata = root["editorMetadata"];
        if (editorMetadata.contains("pivot")) {
            editorPivot_ = editorPivotFromJson(editorMetadata["pivot"]);
        }
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
        renderSettings_.denoiserBackend = static_cast<DenoiserBackend>(render.value("denoiserBackend", static_cast<uint32_t>(renderSettings_.denoiserBackend)));
        renderSettings_.denoiseWhileMoving = render.value("denoiseWhileMoving", renderSettings_.denoiseWhileMoving);
        renderSettings_.samplesPerPixel = render.value("samplesPerPixel", renderSettings_.samplesPerPixel);
        renderSettings_.limitSamplesPerPixel = render.value("limitSamplesPerPixel", renderSettings_.limitSamplesPerPixel);
        renderSettings_.atrousIterations = render.value("atrousIterations", renderSettings_.atrousIterations);
        renderSettings_.denoiserStrength = render.value("denoiserStrength", renderSettings_.denoiserStrength);
        renderSettings_.denoiserMaxHistoryLength = render.value("denoiserMaxHistoryLength", renderSettings_.denoiserMaxHistoryLength);
        renderSettings_.momentValidityThreshold = render.value("momentValidityThreshold", renderSettings_.momentValidityThreshold);
        renderSettings_.taaEnabled = render.value("taaEnabled", renderSettings_.taaEnabled);
        renderSettings_.temporalUpscaler = static_cast<TemporalUpscaler>(render.value("temporalUpscaler", static_cast<uint32_t>(renderSettings_.temporalUpscaler)));
        renderSettings_.dlssFrameGenerationEnabled = render.value("dlssFrameGenerationEnabled", renderSettings_.dlssFrameGenerationEnabled);
        renderSettings_.dlssRayReconstructionEnabled = render.value("dlssRayReconstructionEnabled", renderSettings_.dlssRayReconstructionEnabled);
        renderSettings_.streamlineReflexEnabled = render.value("streamlineReflexEnabled", renderSettings_.streamlineReflexEnabled);
        renderSettings_.dlssSharpeningStrength = render.value("dlssSharpeningStrength", renderSettings_.dlssSharpeningStrength);
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
        renderSettings_.restirGiFinalStabilizationEnabled = render.value("restirGiFinalStabilizationEnabled", renderSettings_.restirGiFinalStabilizationEnabled);
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
    sublevels_.clear();
    prefabInstances_.clear();
    sceneMeshes_.clear();
    sceneMaterials_.clear();
    sceneSkins_.clear();
    activeCamera_ = {};
    primarySun_ = {};

    if (root.contains("skins") && root["skins"].is_array()) {
        for (const nlohmann::json& item : root["skins"]) {
            if (!item.is_object()) {
                continue;
            }
            SceneSkinAsset skin;
            skin.name = item.value("name", std::string{});
            skin.skeletonRoot = item.value("skeletonRoot", -1);
            if (item.contains("joints") && item["joints"].is_array()) {
                for (const nlohmann::json& joint : item["joints"]) {
                    if (joint.is_number_unsigned() || joint.is_number_integer()) {
                        const int value = joint.get<int>();
                        if (value >= 0) {
                            skin.joints.push_back(static_cast<uint32_t>(value));
                        }
                    }
                }
            }
            if (item.contains("inverseBindMatrices") && item["inverseBindMatrices"].is_array()) {
                for (const nlohmann::json& matrix : item["inverseBindMatrices"]) {
                    skin.inverseBindMatrices.push_back(matrixFromJson(matrix));
                }
            }
            sceneSkins_.push_back(std::move(skin));
        }
    }

    if (root.contains("sublevels") && root["sublevels"].is_array()) {
        for (const nlohmann::json& item : root["sublevels"]) {
            SceneSublevelRecord sublevel = sublevelFromJson(item);
            if (!sublevel.sceneGuid.empty() || !sublevel.scenePath.empty()) {
                sublevels_.push_back(std::move(sublevel));
            }
        }
    }

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
        entity->layer = item.value("layer", std::string{});
        if (item.contains("tags") && item["tags"].is_array()) {
            entity->tags = item["tags"].get<std::vector<std::string>>();
        }
        if (item.contains("collections") && item["collections"].is_array()) {
            entity->collections = item["collections"].get<std::vector<std::string>>();
        }
        entity->visible = item.value("visible", true);
        entity->locked = item.value("locked", false);
        entity->sourceNodeIndex = item.value("sourceNodeIndex", -1);

        entity->transform = transformFromJson(item.value("transform", nlohmann::json::object()), entity->transform);
        entity->defaultTransform = transformFromJson(item.value("defaultTransform", nlohmann::json::object()), entity->transform);

        if (item.contains("meshRenderer")) {
            const nlohmann::json& source = item["meshRenderer"];
            MeshRenderer renderer;
            renderer.mesh = MeshAssetHandle{source.value("mesh", UINT32_MAX)};
            renderer.meshGuid = source.value("meshGuid", std::string{});
            renderer.visible = source.value("visible", true);
            renderer.castShadow = source.value("castShadow", true);
            renderer.receiveShadow = source.value("receiveShadow", true);
            renderer.visibleToCamera = source.value("visibleToCamera", true);
            renderer.renderLayer = source.value("renderLayer", 0);
            renderer.morphWeights = floatVectorFromJson(source.value("morphWeights", nlohmann::json::array()));
            renderer.skinIndex = source.value("skinIndex", -1);
            renderer.activeMaterialVariantIndex = source.value("activeMaterialVariantIndex", UINT32_MAX);
            renderer.activeMaterialVariantName = source.value("activeMaterialVariantName", std::string{});
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
        if (item.contains("animationPlayer")) {
            const nlohmann::json& source = item["animationPlayer"];
            AnimationPlayer player;
            player.animationGuid = source.value("animationGuid", std::string{});
            player.animationPath = source.value("animationPath", std::string{});
            player.controllerGuid = source.value("controllerGuid", std::string{});
            player.controllerPath = source.value("controllerPath", std::string{});
            player.controllerState = source.value("controllerState", std::string{});
            if (source.contains("controllerParameters") && source["controllerParameters"].is_array()) {
                for (const nlohmann::json& parameterJson : source["controllerParameters"]) {
                    if (!parameterJson.is_object()) {
                        continue;
                    }
                    AnimationControllerParameterOverride parameter;
                    parameter.name = parameterJson.value("name", std::string{});
                    parameter.type = parameterJson.value("type", std::string{});
                    parameter.boolValue = parameterJson.value("boolValue", false);
                    parameter.intValue = parameterJson.value("intValue", 0);
                    parameter.floatValue = parameterJson.value("floatValue", 0.0f);
                    parameter.triggerValue = parameterJson.value("triggerValue", false);
                    if (!parameter.name.empty() && !parameter.type.empty()) {
                        player.controllerParameters.push_back(std::move(parameter));
                    }
                }
            }
            player.enabled = source.value("enabled", player.enabled);
            player.playOnStart = source.value("playOnStart", player.playOnStart);
            player.playing = source.value("playing", player.playOnStart);
            player.loop = source.value("loop", player.loop);
            player.applyRootMotion = source.value("applyRootMotion", player.applyRootMotion);
            player.applyMorphWeights = source.value("applyMorphWeights", player.applyMorphWeights);
            player.playbackSpeed = source.value("playbackSpeed", player.playbackSpeed);
            player.currentTimeSeconds = source.value("currentTimeSeconds", player.currentTimeSeconds);
            entity->animationPlayer = std::move(player);
        }
        if (item.contains("levelInstance")) {
            LevelInstance instance = levelInstanceFromJson(item["levelInstance"]);
            if (!instance.sceneGuid.empty() || !instance.scenePath.empty()) {
                entity->levelInstance = std::move(instance);
            }
        }
        if (item.contains("light")) {
            const nlohmann::json& source = item["light"];
            Light light;
            light.type = static_cast<LightType>(source.value("type", static_cast<uint32_t>(LightType::Point)));
            light.color = vec3FromJson(source.value("color", nlohmann::json::array()), light.color);
            light.intensity = source.value("intensity", light.intensity);
            light.exposureMultiplier = source.value("exposureMultiplier", light.exposureMultiplier);
            light.sizeOrRadius = source.value("sizeOrRadius", light.sizeOrRadius);
            light.innerConeRadians = source.value("innerConeRadians", light.innerConeRadians);
            light.outerConeRadians = source.value("outerConeRadians", light.outerConeRadians);
            light.enabled = source.value("enabled", true);
            light.useColorTemperature = source.value("useColorTemperature", light.useColorTemperature);
            light.colorTemperatureKelvin = source.value("colorTemperatureKelvin", light.colorTemperatureKelvin);
            light.visibleToCamera = source.value("visibleToCamera", light.visibleToCamera);
            light.castSurfaceShadows = source.value("castSurfaceShadows", light.castSurfaceShadows);
            light.castVolumetricShadows = source.value("castVolumetricShadows", light.castVolumetricShadows);
            light.iesProfile = source.value("iesProfile", light.iesProfile);
            light.materialSource = source.value("materialSource", light.materialSource);
            entity->light = light;
        }
        if (item.contains("sun")) {
            const nlohmann::json& source = item["sun"];
            Sun sun;
            sun.enabled = source.value("enabled", sun.enabled);
            const bool hasAuthoredAngles = source.contains("elevation") || source.contains("azimuth");
            sun.elevation = source.value("elevation", sun.elevation);
            sun.azimuth = source.value("azimuth", sun.azimuth);
            sun.illuminanceLux = source.value("illuminanceLux", sun.illuminanceLux);
            sun.exposureMultiplier = source.value("exposureMultiplier", sun.exposureMultiplier);
            sun.angularRadiusRadians = source.value("angularRadiusRadians", sun.angularRadiusRadians);
            sun.useColorTemperature = source.value("useColorTemperature", sun.useColorTemperature);
            sun.colorTemperatureKelvin = source.value("colorTemperatureKelvin", sun.colorTemperatureKelvin);
            sun.castSurfaceShadows = source.value("castSurfaceShadows", sun.castSurfaceShadows);
            sun.castVolumetricShadows = source.value("castVolumetricShadows", sun.castVolumetricShadows);
            sun.shadowBounces = source.value("shadowBounces", sun.shadowBounces);
            sun.volumetricShadowBounces = source.value("volumetricShadowBounces", sun.volumetricShadowBounces);
            if (!hasAuthoredAngles) {
                SunController::anglesFromWorldTransform(registry_, *entity, sun.elevation, sun.azimuth);
            }
            entity->sun = sun;
        }
        if (item.contains("camera")) {
            const nlohmann::json& source = item["camera"];
            Camera camera;
            camera.projection = source.value("projection", camera.projection);
            camera.verticalFovRadians = source.value("verticalFovRadians", camera.verticalFovRadians);
            camera.aspectRatio = source.value("aspectRatio", camera.aspectRatio);
            camera.orthographicXmag = source.value("orthographicXmag", camera.orthographicXmag);
            camera.orthographicYmag = source.value("orthographicYmag", camera.orthographicYmag);
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
            component.hdrPath = source.value("hdrPath", component.hdrPath.generic_string());
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
            component.skyIntensity = source.value("skyIntensity", component.skyIntensity);
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
            component.bloomEnabled = source.value("bloomEnabled", component.bloomEnabled);
            component.bloomIntensity = source.value("bloomIntensity", component.bloomIntensity);
            component.colorCorrectionEnabled = source.value("colorCorrectionEnabled", component.colorCorrectionEnabled);
            component.colorCorrectionSaturation = source.value("colorCorrectionSaturation", component.colorCorrectionSaturation);
            component.colorCorrectionContrast = source.value("colorCorrectionContrast", component.colorCorrectionContrast);
            component.vignettingEnabled = source.value("vignettingEnabled", component.vignettingEnabled);
            component.vignettingIntensity = source.value("vignettingIntensity", component.vignettingIntensity);
            component.filmGrainEnabled = source.value("filmGrainEnabled", component.filmGrainEnabled);
            component.filmGrainIntensity = source.value("filmGrainIntensity", component.filmGrainIntensity);
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
    applySceneWorldComponentsToDocumentSettings(*this);

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
    if (root.contains("editorMetadata") && root["editorMetadata"].is_object() && root["editorMetadata"].contains("pivot")) {
        editorPivot_ = editorPivotFromJson(root["editorMetadata"]["pivot"]);
    } else {
        editorPivot_ = EditorPivotSettings{};
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

} // namespace rtv
