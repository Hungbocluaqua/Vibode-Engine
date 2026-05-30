#pragma once

#include "rtv/SceneDocument.h"
#include "rtv/SceneEventBus.h"
#include "rtv/UndoStack.h"

#include <string>

namespace rtv {

class SceneOperations {
public:
    explicit SceneOperations(SceneDocument& document, SceneEventBus* eventBus = nullptr);
    void setUndoStack(UndoStack* undoStack) { undoStack_ = undoStack; }

    [[nodiscard]] EntityId createEntity(const std::string& name, EntityId parent = {});
    [[nodiscard]] EntityId duplicateEntity(EntityId id);
    bool deleteEntity(EntityId id);
    bool reparentEntity(EntityId child, EntityId newParent);
    bool setVisibility(EntityId id, bool visible);
    bool setLocked(EntityId id, bool locked);
    bool setTransform(EntityId id, const Transform& transform);
    void setTransformGizmoDrag(EntityId id, const Transform& oldTransform, const Transform& newTransform);
    bool addLightComponent(EntityId id, Light light = {});
    bool addSunComponent(EntityId id, Sun sun = {});
    bool addCameraComponent(EntityId id, Camera camera = {});
    bool addMeshRendererComponent(EntityId id, MeshRenderer renderer = {});
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
