#pragma once

#include "rtv/SceneDocument.h"
#include "rtv/SceneEventBus.h"
#include "rtv/UndoStack.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

enum class EditorAlignDistributeAxis : uint32_t;
enum class EditorAlignDistributeMode : uint32_t;
struct EditorAlignDistributeEntityBounds;
struct EditorEntityTransformChange;

class SceneOperations {
public:
    explicit SceneOperations(SceneDocument& document, SceneEventBus* eventBus = nullptr);
    void setUndoStack(UndoStack* undoStack) { undoStack_ = undoStack; }
    void pushDocumentSnapshot(SceneDocument before, SceneUpdateKind updateKind, std::string label);

    [[nodiscard]] EntityId createEntity(
        const std::string& name,
        EntityId parent = {},
        SceneUpdateKind updateKind = SceneUpdateKind::TopologyChanged);
    [[nodiscard]] EntityId duplicateEntity(EntityId id);
    bool deleteEntity(EntityId id);
    bool deleteEntities(const std::vector<EntityId>& ids);
    bool reparentEntity(EntityId child, EntityId newParent);
    bool setVisibility(EntityId id, bool visible);
    bool setLocked(EntityId id, bool locked);
    bool renameEntity(EntityId id, const std::string& name);
    bool setTransform(EntityId id, const Transform& transform);
    void setTransformGizmoDrag(EntityId id, const Transform& oldTransform, const Transform& newTransform);
    bool setTransformGizmoDragBatch(const std::vector<EditorEntityTransformChange>& changes);
    bool alignDistributeEntities(
        const std::vector<EntityId>& ids,
        const std::vector<EditorAlignDistributeEntityBounds>& bounds,
        EditorAlignDistributeAxis axis,
        EditorAlignDistributeMode mode);
    bool addLightComponent(EntityId id, Light light = {});
    bool addSunComponent(EntityId id, Sun sun = {});
    bool addCameraComponent(EntityId id, Camera camera = {});
    bool addMeshRendererComponent(EntityId id, MeshRenderer renderer = {});
    bool addLevelInstanceComponent(EntityId id, LevelInstance instance = {});
    bool removeLightComponent(EntityId id);
    bool removeSunComponent(EntityId id);
    bool removeCameraComponent(EntityId id);
    bool removeMeshRendererComponent(EntityId id);
    bool removeLevelInstanceComponent(EntityId id);
    bool setMeshRenderer(EntityId id, const MeshRenderer& oldRenderer, const MeshRenderer& newRenderer, SceneUpdateKind updateKind);
    bool setLevelInstance(EntityId id, const LevelInstance& oldInstance, const LevelInstance& newInstance);
    bool setLevelInstanceLoaded(EntityId id, bool loaded);
    bool setLevelInstanceEditable(EntityId id, bool editable);
    bool breakLevelInstanceLink(EntityId id);
    bool ensurePrimarySun();
    [[nodiscard]] EntityId mergeSceneAsset(const SceneAsset& scene, const std::string& rootName = "Merged Scene");
    [[nodiscard]] EntityId mergeLevelInstanceAsset(
        const SceneAsset& scene,
        SceneSublevelRecord sublevel,
        const std::string& rootName = "Level Instance");
    [[nodiscard]] PrefabInstance placePrefab(
        const PrefabAsset& prefab,
        const PrefabRuntimeBindings* bindings = nullptr,
        EntityId parent = {});
    bool setLight(EntityId id, const Light& oldLight, const Light& newLight);
    bool setSun(EntityId id, const Sun& oldSun, const Sun& newSun);
    bool setCamera(EntityId id, const Camera& oldCamera, const Camera& newCamera, EntityId oldActiveCamera, EntityId newActiveCamera);
    void commitSunDrag(SceneDocument before, SceneUpdateKind updateKind);

private:
    [[nodiscard]] EntityId duplicateEntityRecursive(const Entity& source, EntityId parent);
    void publish(SceneEvent event) const;

    SceneDocument& document_;
    SceneEventBus* eventBus_ = nullptr;
    UndoStack* undoStack_ = nullptr;
};

} // namespace rtv
