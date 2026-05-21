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

private:
    [[nodiscard]] EntityId duplicateEntityRecursive(const Entity& source, EntityId parent);
    void publish(SceneEvent event) const;

    SceneDocument& document_;
    SceneEventBus* eventBus_ = nullptr;
    UndoStack* undoStack_ = nullptr;
};

} // namespace rtv
