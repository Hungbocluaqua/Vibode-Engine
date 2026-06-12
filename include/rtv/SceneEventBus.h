#pragma once

#include "rtv/EntityId.h"
#include "rtv/SceneComponents.h"

#include <functional>
#include <string>
#include <vector>

namespace rtv {

enum class SceneEventType : uint8_t {
    EntityCreated,
    EntityDeleted,
    EntityDuplicated,
    EntityReparented,
    TransformChanged,
    VisibilityChanged,
    LockChanged,
    MaterialChanged,
    ComponentAdded,
    ComponentRemoved,
    AnimationEventFired,
    SelectionChanged,
    SceneLoaded,
    SceneCleared,
};

struct SceneEvent {
    SceneEventType type = SceneEventType::SceneLoaded;
    EntityId entity{};
    EntityId relatedEntity{};
    SceneUpdateKind updateKind = SceneUpdateKind::None;
    std::string animationEventName;
    std::string animationEventPayloadJson;
    double animationEventTimeSeconds = 0.0;
};

class SceneEventBus {
public:
    using Handler = std::function<void(const SceneEvent&)>;

    void subscribe(Handler handler);
    void publish(const SceneEvent& event) const;
    void clear();

private:
    std::vector<Handler> handlers_;
};

} // namespace rtv
