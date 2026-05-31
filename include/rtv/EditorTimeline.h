#pragma once

#include "rtv/EntityId.h"
#include "rtv/SceneComponents.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace rtv {

struct TimelineTransformKeyframe {
    EntityId entity{};
    uint64_t entityUuid = 0;
    int frame = 0;
    Transform transform{};
};

class EditorTimeline final {
public:
    bool playing = false;
    int currentFrame = 0;
    int startFrame = 0;
    int endFrame = 120;

    void clear();
    void stop();
    void addTransformKey(EntityId entity, uint64_t entityUuid, Transform transform);
    bool advance(float deltaSeconds);
    [[nodiscard]] bool sampleTransform(uint64_t entityUuid, int frame, Transform& outTransform) const;
    [[nodiscard]] std::vector<uint64_t> animatedEntityUuids() const;
    [[nodiscard]] const std::vector<TimelineTransformKeyframe>& keyframes() const { return keyframes_; }
    [[nodiscard]] nlohmann::json serialize() const;
    void deserialize(const nlohmann::json& json);

private:
    std::vector<TimelineTransformKeyframe> keyframes_;
};

} // namespace rtv
