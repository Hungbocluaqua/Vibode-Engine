#include "rtv/SceneOperations.h"

#include "rtv/SunController.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <glm/gtc/quaternion.hpp>

namespace rtv {

namespace {

glm::vec3 translationFromMatrix(const glm::mat4& matrix) {
    return glm::vec3(matrix[3]);
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

class SceneDocumentSnapshotCommand final : public ICommand {
public:
    SceneDocumentSnapshotCommand(
        SceneDocument& document,
        SceneDocument before,
        SceneDocument after,
        SceneUpdateKind updateKind,
        std::string label)
        : document_(document),
          before_(std::move(before)),
          after_(std::move(after)),
          updateKind_(updateKind),
          label_(std::move(label)) {}

    void undo() override {
        document_ = before_;
        document_.markDirty(updateKind_);
    }

    void redo() override {
        document_ = after_;
        document_.markDirty(updateKind_);
    }

    [[nodiscard]] const std::string& label() const override { return label_; }

private:
    SceneDocument& document_;
    SceneDocument before_;
    SceneDocument after_;
    SceneUpdateKind updateKind_ = SceneUpdateKind::TopologyChanged;
    std::string label_;
};

} // namespace

SceneOperations::SceneOperations(SceneDocument& document, SceneEventBus* eventBus)
    : document_(document), eventBus_(eventBus) {}

void SceneOperations::pushDocumentSnapshot(SceneDocument before, SceneUpdateKind updateKind, std::string label) {
    if (undoStack_ == nullptr) {
        return;
    }
    undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
        document_, std::move(before), document_, updateKind, std::move(label)));
}

EntityId SceneOperations::createEntity(const std::string& name, EntityId parent) {
    const SceneDocument before = document_;
    EntityId id = document_.registry().createEntity(name);
    if (Entity* entity = document_.registry().entity(id)) {
        entity->parent = parent;
        if (Entity* parentEntity = document_.registry().entity(parent)) {
            parentEntity->children.push_back(id);
        }
    }
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::EntityCreated, id, parent, SceneUpdateKind::TopologyChanged});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TopologyChanged, "Create Entity"));
    }
    return id;
}

EntityId SceneOperations::duplicateEntity(EntityId id) {
    const Entity* source = document_.registry().entity(id);
    if (source == nullptr) {
        return {};
    }
    const SceneDocument before = document_;
    EntityId duplicate = duplicateEntityRecursive(*source, source->parent);
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::EntityDuplicated, duplicate, id, SceneUpdateKind::TopologyChanged});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TopologyChanged, "Duplicate Entity"));
    }
    return duplicate;
}

bool SceneOperations::deleteEntity(EntityId id) {
    const SceneDocument before = document_;
    if (document_.activeCamera() == id) {
        document_.setActiveCamera({});
    }
    if (!document_.registry().destroyEntity(id)) {
        return false;
    }
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::EntityDeleted, id, {}, SceneUpdateKind::TopologyChanged});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TopologyChanged, "Delete Entity"));
    }
    return true;
}

bool SceneOperations::reparentEntity(EntityId child, EntityId newParent) {
    const SceneDocument before = document_;
    Entity* childEntity = document_.registry().entity(child);
    if (childEntity == nullptr || child == newParent) {
        return false;
    }
    for (EntityId cursor = newParent; cursor.valid();) {
        if (cursor == child) {
            return false;
        }
        const Entity* parent = document_.registry().entity(cursor);
        if (parent == nullptr) {
            break;
        }
        cursor = parent->parent;
    }
    if (Entity* oldParent = document_.registry().entity(childEntity->parent)) {
        oldParent->children.erase(
            std::remove(oldParent->children.begin(), oldParent->children.end(), child),
            oldParent->children.end());
    }
    childEntity->parent = newParent;
    if (Entity* parent = document_.registry().entity(newParent)) {
        parent->children.push_back(child);
    }
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::EntityReparented, child, newParent, SceneUpdateKind::TopologyChanged});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TopologyChanged, "Reparent Entity"));
    }
    return true;
}

bool SceneOperations::setVisibility(EntityId id, bool visible) {
    const SceneDocument before = document_;
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || entity->visible == visible) {
        return false;
    }
    entity->visible = visible;
    if (entity->meshRenderer.has_value()) {
        entity->meshRenderer->visible = visible;
    }
    document_.markDirty(SceneUpdateKind::VisibilityOnly);
    publish({SceneEventType::VisibilityChanged, id, {}, SceneUpdateKind::VisibilityOnly});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::VisibilityOnly, "Set Visibility"));
    }
    return true;
}

bool SceneOperations::setLocked(EntityId id, bool locked) {
    const SceneDocument before = document_;
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked == locked) {
        return false;
    }
    entity->locked = locked;
    document_.markDirty(SceneUpdateKind::None);
    publish({SceneEventType::LockChanged, id, {}, SceneUpdateKind::None});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::None, locked ? "Lock Entity" : "Unlock Entity"));
    }
    return true;
}

bool SceneOperations::renameEntity(EntityId id, const std::string& name) {
    const SceneDocument before = document_;
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || entity->name == name) {
        return false;
    }
    if (!document_.registry().renameEntity(id, name)) {
        return false;
    }
    document_.markDirty(SceneUpdateKind::None);
    publish({SceneEventType::ComponentAdded, id, {}, SceneUpdateKind::None});
    pushDocumentSnapshot(before, SceneUpdateKind::None, "Rename Entity");
    return true;
}

bool SceneOperations::setTransform(EntityId id, const Transform& transform) {
    const SceneDocument before = document_;
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked) {
        return false;
    }
    entity->transform = transform;
    entity->transform.dirty = true;
    document_.markDirty(SceneUpdateKind::TransformOnly);
    publish({SceneEventType::TransformChanged, id, {}, SceneUpdateKind::TransformOnly});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TransformOnly, "Set Transform"));
    }
    return true;
}

SceneUpdateKind transformUpdateKind(const SceneDocument& document, const Entity& entity) {
    const bool hasMesh = entity.meshRenderer.has_value();
    const bool hasLight = entity.light.has_value();
    const bool hasSun = entity.sun.has_value();
    const bool hasActiveCamera = entity.camera.has_value() && document.activeCamera() == entity.id;
    if (((hasLight || hasSun) && hasMesh) || (hasActiveCamera && hasMesh) || (hasActiveCamera && (hasLight || hasSun))) {
        return SceneUpdateKind::TopologyChanged;
    }
    if (hasActiveCamera) {
        return SceneUpdateKind::CameraOnly;
    }
    if (hasLight || hasSun) {
        return SceneUpdateKind::LightOnly;
    }
    return SceneUpdateKind::TransformOnly;
}

void SceneOperations::setTransformGizmoDrag(EntityId id, const Transform& oldTransform, const Transform& newTransform) {
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked) {
        return;
    }
    entity->transform = oldTransform;
    const SceneDocument before = document_;
    entity->transform = newTransform;
    entity->transform.dirty = true;
    const SceneUpdateKind updateKind = transformUpdateKind(document_, *entity);
    document_.markDirty(updateKind);
    publish({SceneEventType::TransformChanged, id, {}, updateKind});
    if (undoStack_ != nullptr) {
        const SceneDocument after = document_;
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, after, updateKind, "Move Entity"));
    }
}

bool SceneOperations::addLightComponent(EntityId id, Light light) {
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || entity->light.has_value()) {
        return false;
    }
    const SceneDocument before = document_;
    document_.registry().addLight(id, light);
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::ComponentAdded, id, {}, SceneUpdateKind::TopologyChanged});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TopologyChanged, "Add Light Component"));
    }
    return true;
}

bool SceneOperations::addSunComponent(EntityId id, Sun sun) {
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || entity->sun.has_value()) {
        return false;
    }
    const SceneDocument before = document_;
    entity->sun = sun;
    if (glm::dot(entity->transform.position, entity->transform.position) <= 1.0e-6f &&
        glm::dot(entity->transform.rotationEuler, entity->transform.rotationEuler) <= 1.0e-6f) {
        entity->transform = SunController::transformFromWorldAngles(
            document_.registry(),
            *entity,
            entity->transform,
            document_.renderSettings().sunElevation,
            document_.renderSettings().sunAzimuth);
    }
    document_.setPrimarySun(id);
    for (Entity* other : document_.registry().entities()) {
        if (other != nullptr && other->id != id) {
            other->sun.reset();
        }
    }
    document_.markDirty(SceneUpdateKind::LightOnly);
    publish({SceneEventType::ComponentAdded, id, {}, SceneUpdateKind::LightOnly});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::LightOnly, "Add Sun Component"));
    }
    return true;
}

bool SceneOperations::addCameraComponent(EntityId id, Camera camera) {
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || entity->camera.has_value()) {
        return false;
    }
    const SceneDocument before = document_;
    document_.registry().addCamera(id, camera);
    if (camera.active) {
        document_.setActiveCamera(id);
    }
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::ComponentAdded, id, {}, SceneUpdateKind::TopologyChanged});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TopologyChanged, "Add Camera Component"));
    }
    return true;
}

bool SceneOperations::addMeshRendererComponent(EntityId id, MeshRenderer renderer) {
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || entity->meshRenderer.has_value()) {
        return false;
    }
    const SceneDocument before = document_;
    document_.registry().addMeshRenderer(id, std::move(renderer));
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::ComponentAdded, id, {}, SceneUpdateKind::TopologyChanged});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TopologyChanged, "Add Mesh Renderer Component"));
    }
    return true;
}

bool SceneOperations::removeLightComponent(EntityId id) {
    const SceneDocument before = document_;
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || !entity->light.has_value()) {
        return false;
    }
    entity->light.reset();
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::ComponentRemoved, id, {}, SceneUpdateKind::TopologyChanged});
    pushDocumentSnapshot(before, SceneUpdateKind::TopologyChanged, "Remove Light Component");
    return true;
}

bool SceneOperations::removeSunComponent(EntityId id) {
    const SceneDocument before = document_;
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || !entity->sun.has_value()) {
        return false;
    }
    entity->sun.reset();
    if (document_.primarySun() == id) {
        document_.setPrimarySun({});
    }
    document_.markDirty(SceneUpdateKind::LightOnly);
    publish({SceneEventType::ComponentRemoved, id, {}, SceneUpdateKind::LightOnly});
    pushDocumentSnapshot(before, SceneUpdateKind::LightOnly, "Remove Sun Component");
    return true;
}

bool SceneOperations::removeCameraComponent(EntityId id) {
    const SceneDocument before = document_;
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || !entity->camera.has_value()) {
        return false;
    }
    entity->camera.reset();
    if (document_.activeCamera() == id) {
        document_.setActiveCamera({});
    }
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::ComponentRemoved, id, {}, SceneUpdateKind::TopologyChanged});
    pushDocumentSnapshot(before, SceneUpdateKind::TopologyChanged, "Remove Camera Component");
    return true;
}

bool SceneOperations::removeMeshRendererComponent(EntityId id) {
    const SceneDocument before = document_;
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || !entity->meshRenderer.has_value()) {
        return false;
    }
    entity->meshRenderer.reset();
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::ComponentRemoved, id, {}, SceneUpdateKind::TopologyChanged});
    pushDocumentSnapshot(before, SceneUpdateKind::TopologyChanged, "Remove Mesh Renderer Component");
    return true;
}

bool SceneOperations::setMeshRenderer(EntityId id, const MeshRenderer& oldRenderer, const MeshRenderer& newRenderer, SceneUpdateKind updateKind) {
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || !entity->meshRenderer.has_value()) {
        return false;
    }
    entity->meshRenderer = oldRenderer;
    const SceneDocument before = document_;
    entity = document_.registry().entity(id);
    if (entity == nullptr || !entity->meshRenderer.has_value()) {
        return false;
    }
    entity->meshRenderer = newRenderer;
    document_.markDirty(updateKind);
    publish({SceneEventType::ComponentAdded, id, {}, updateKind});
    pushDocumentSnapshot(before, updateKind, "Edit Mesh Renderer");
    return true;
}

bool SceneOperations::ensurePrimarySun() {
    const SceneDocument before = document_;
    const EntityId existing = SunController::primarySunEntity(document_);
    const EntityId sun = SunController::ensurePrimarySun(document_);
    if (!sun.valid()) {
        return false;
    }
    const bool createdOrChanged = !existing.valid() || existing != sun;
    if (createdOrChanged) {
        pushDocumentSnapshot(before, SceneUpdateKind::LightOnly, existing.valid() ? "Set Primary Sun" : "Create Primary Sun");
    }
    return createdOrChanged;
}

EntityId SceneOperations::mergeSceneAsset(const SceneAsset& scene, const std::string& rootName) {
    if (scene.nodes.empty() && scene.lights.empty()) {
        return {};
    }

    const SceneDocument before = document_;
    const bool hadActiveCamera = document_.activeCamera().valid();
    bool assignedMergedCamera = false;

    EntityId importRoot = document_.registry().createEntity(rootName.empty() ? "Merged Scene" : rootName);
    if (!importRoot.valid()) {
        return {};
    }

    std::vector<EntityId> nodeEntities(scene.nodes.size());
    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNodeAsset& node = scene.nodes[i];
        const EntityId id = document_.registry().createEntity(node.name.empty() ? "Merged Node " + std::to_string(i) : node.name);
        nodeEntities[i] = id;

        Entity* entity = document_.registry().entity(id);
        if (entity == nullptr) {
            continue;
        }
        entity->transform.position = translationFromMatrix(node.transform);
        entity->transform.rotationEuler = eulerFromMatrix(node.transform);
        entity->transform.scale = scaleFromMatrix(node.transform);
        entity->transform.dirty = true;
        entity->visible = node.visible;

        if (node.mesh.valid()) {
            MeshRenderer renderer;
            renderer.mesh = node.mesh;
            renderer.visible = node.visible;
            renderer.castShadow = node.castShadow;
            renderer.visibleToCamera = node.visibleToCamera;
            entity->meshRenderer = renderer;
        }
        if (node.hasCamera) {
            Camera camera;
            camera.verticalFovRadians = node.cameraYfov;
            camera.nearPlane = node.cameraNear;
            camera.farPlane = node.cameraFar;
            camera.active = !hadActiveCamera && !assignedMergedCamera;
            entity->camera = camera;
            if (camera.active) {
                document_.setActiveCamera(id);
                assignedMergedCamera = true;
            }
        }
    }

    for (uint32_t i = 0; i < scene.nodes.size(); ++i) {
        Entity* entity = document_.registry().entity(nodeEntities[i]);
        if (entity == nullptr) {
            continue;
        }
        const SceneNodeAsset& node = scene.nodes[i];
        EntityId parentId = importRoot;
        if (node.parent >= 0 && static_cast<uint32_t>(node.parent) < nodeEntities.size()) {
            parentId = nodeEntities[static_cast<uint32_t>(node.parent)];
        }
        entity->parent = parentId;
        if (Entity* parentEntity = document_.registry().entity(parentId)) {
            parentEntity->children.push_back(entity->id);
        }
    }

    for (uint32_t i = 0; i < scene.lights.size(); ++i) {
        const SceneLightAsset& source = scene.lights[i];
        EntityId id = source.nodeIndex >= 0 && static_cast<uint32_t>(source.nodeIndex) < nodeEntities.size()
            ? nodeEntities[static_cast<uint32_t>(source.nodeIndex)]
            : document_.registry().createEntity("Merged Light " + std::to_string(i));
        Entity* entity = document_.registry().entity(id);
        if (entity == nullptr) {
            continue;
        }
        if (source.nodeIndex < 0) {
            entity->parent = importRoot;
            if (Entity* rootEntity = document_.registry().entity(importRoot)) {
                rootEntity->children.push_back(id);
            }
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

    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::EntityCreated, importRoot, {}, SceneUpdateKind::TopologyChanged});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TopologyChanged, "Merge Scene"));
    }
    return importRoot;
}

PrefabInstance SceneOperations::placePrefab(
    const PrefabAsset& prefab,
    const PrefabRuntimeBindings* bindings,
    EntityId parent) {
    PrefabInstance instance;
    if (prefab.guid.empty()) {
        return instance;
    }

    const SceneDocument before = document_;
    instance.prefabGuid = prefab.guid;

    EntityId root = document_.registry().createEntity(prefab.name.empty() ? "Prefab Instance" : prefab.name);
    instance.instanceRoot = root;
    if (Entity* rootEntity = document_.registry().entity(root)) {
        rootEntity->parent = parent;
        if (Entity* parentEntity = document_.registry().entity(parent)) {
            parentEntity->children.push_back(root);
        }
        instance.generatedEntityUuids.push_back(rootEntity->uuid);
    }

    std::vector<EntityId> nodeEntities(prefab.nodes.size());
    for (uint32_t i = 0; i < prefab.nodes.size(); ++i) {
        const PrefabNodeAsset& node = prefab.nodes[i];
        EntityId id = document_.registry().createEntity(node.name.empty() ? "Prefab Node " + std::to_string(i) : node.name);
        nodeEntities[i] = id;
        if (Entity* entity = document_.registry().entity(id)) {
            instance.generatedEntityUuids.push_back(entity->uuid);
        }
    }

    for (uint32_t i = 0; i < prefab.nodes.size(); ++i) {
        Entity* entity = document_.registry().entity(nodeEntities[i]);
        if (entity == nullptr) {
            continue;
        }
        const PrefabNodeAsset& node = prefab.nodes[i];
        EntityId parentId = root;
        if (node.parent >= 0 && static_cast<uint32_t>(node.parent) < nodeEntities.size()) {
            parentId = nodeEntities[static_cast<uint32_t>(node.parent)];
        }
        entity->parent = parentId;
        if (Entity* parentEntity = document_.registry().entity(parentId)) {
            parentEntity->children.push_back(entity->id);
        }
        if (!node.meshGuid.empty()) {
            MeshRenderer renderer;
            renderer.meshGuid = node.meshGuid;
            if (bindings != nullptr) {
                const auto meshIt = bindings->meshes.find(node.meshGuid);
                if (meshIt != bindings->meshes.end()) {
                    renderer.mesh = meshIt->second;
                }
            }
            renderer.materialSlots.reserve(node.materialGuids.size());
            for (size_t slotIndex = 0; slotIndex < node.materialGuids.size(); ++slotIndex) {
                MaterialSlot slot;
                slot.name = "Primitive " + std::to_string(slotIndex);
                slot.materialGuid = node.materialGuids[slotIndex];
                if (bindings != nullptr) {
                    const auto materialIt = bindings->materials.find(slot.materialGuid);
                    if (materialIt != bindings->materials.end()) {
                        slot.material = materialIt->second;
                    }
                }
                renderer.materialSlots.push_back(std::move(slot));
            }
            entity->meshRenderer = std::move(renderer);
        }
    }

    document_.addPrefabInstance(instance);
    document_.markDirty(SceneUpdateKind::TopologyChanged);
    publish({SceneEventType::EntityCreated, root, parent, SceneUpdateKind::TopologyChanged});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::TopologyChanged, "Place Prefab"));
    }
    return instance;
}

bool SceneOperations::setLight(EntityId id, const Light& oldLight, const Light& newLight) {
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || !entity->light.has_value()) {
        return false;
    }
    entity->light = oldLight;
    const SceneDocument before = document_;
    entity->light = newLight;
    document_.markDirty(SceneUpdateKind::LightOnly);
    publish({SceneEventType::ComponentAdded, id, {}, SceneUpdateKind::LightOnly});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::LightOnly, "Edit Light"));
    }
    return true;
}

bool SceneOperations::setSun(EntityId id, const Sun& oldSun, const Sun& newSun) {
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || !entity->sun.has_value()) {
        return false;
    }
    entity->sun = oldSun;
    const SceneDocument before = document_;
    entity->sun = newSun;
    document_.setPrimarySun(id);
    document_.markDirty(SceneUpdateKind::LightOnly);
    publish({SceneEventType::ComponentAdded, id, {}, SceneUpdateKind::LightOnly});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::LightOnly, "Edit Sun"));
    }
    return true;
}

bool SceneOperations::setCamera(EntityId id, const Camera& oldCamera, const Camera& newCamera, EntityId oldActiveCamera, EntityId newActiveCamera) {
    Entity* entity = document_.registry().entity(id);
    if (entity == nullptr || entity->locked || !entity->camera.has_value()) {
        return false;
    }
    entity->camera = oldCamera;
    document_.setActiveCamera(oldActiveCamera);
    const SceneDocument before = document_;
    entity = document_.registry().entity(id);
    if (entity == nullptr || !entity->camera.has_value()) {
        return false;
    }
    entity->camera = newCamera;
    document_.setActiveCamera(newActiveCamera);
    document_.markDirty(SceneUpdateKind::CameraOnly);
    publish({SceneEventType::ComponentAdded, id, {}, SceneUpdateKind::CameraOnly});
    if (undoStack_ != nullptr) {
        undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
            document_, before, document_, SceneUpdateKind::CameraOnly, "Edit Camera"));
    }
    return true;
}

void SceneOperations::commitSunDrag(SceneDocument before, SceneUpdateKind updateKind) {
    if (undoStack_ == nullptr) {
        return;
    }
    const SceneDocument after = document_;
    undoStack_->pushCommand(std::make_unique<SceneDocumentSnapshotCommand>(
        document_, std::move(before), after, updateKind, "Rotate Sun"));
}

EntityId SceneOperations::duplicateEntityRecursive(const Entity& source, EntityId parent) {
    const EntityId copyId = document_.registry().createEntity(source.name.empty() ? "Entity Copy" : source.name + " Copy");
    Entity* copy = document_.registry().entity(copyId);
    if (copy == nullptr) {
        return {};
    }
    copy->transform = source.transform;
    copy->transform.dirty = true;
    copy->visible = source.visible;
    copy->locked = source.locked;
    copy->meshRenderer = source.meshRenderer;
    copy->light = source.light;
    copy->sun = source.sun;
    copy->camera = source.camera;
    if (copy->camera.has_value()) {
        copy->camera->active = false;
    }
    copy->parent = parent;
    if (Entity* parentEntity = document_.registry().entity(parent)) {
        parentEntity->children.push_back(copyId);
    }
    for (EntityId childId : source.children) {
        if (const Entity* child = document_.registry().entity(childId)) {
            (void)duplicateEntityRecursive(*child, copyId);
        }
    }
    return copyId;
}

void SceneOperations::publish(SceneEvent event) const {
    if (eventBus_ != nullptr) {
        eventBus_->publish(event);
    }
}

} // namespace rtv
