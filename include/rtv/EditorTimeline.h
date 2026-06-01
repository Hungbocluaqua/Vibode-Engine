#pragma once

#include "rtv/EntityId.h"
#include "rtv/SceneComponents.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <vector>

namespace rtv {

struct TimelineTransformKeyframe {
    uint64_t id = 0;
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
    int frameRate = 30;

    void clear();
    void stop();
    void addTransformKey(EntityId entity, uint64_t entityUuid, Transform transform);
    [[nodiscard]] bool updateTransformKey(std::size_t index, int frame, Transform transform);
    [[nodiscard]] bool updateTransformKeyFrame(uint64_t keyId, int frame);
    [[nodiscard]] bool removeTransformKey(std::size_t index);
    [[nodiscard]] bool removeTransformKeyById(uint64_t keyId);
    bool advance(float deltaSeconds);
    [[nodiscard]] bool sampleTransform(uint64_t entityUuid, int frame, Transform& outTransform) const;
    [[nodiscard]] std::vector<uint64_t> animatedEntityUuids() const;
    [[nodiscard]] const std::vector<TimelineTransformKeyframe>& keyframes() const { return keyframes_; }
    [[nodiscard]] nlohmann::json serialize() const;
    void deserialize(const nlohmann::json& json);

private:
    std::vector<TimelineTransformKeyframe> keyframes_;
    uint64_t nextKeyId_ = 1;
};

} // namespace rtv
