#include "rtv/EditorTimeline.h"

#include <algorithm>

namespace rtv {

namespace {
nlohmann::json vec3Json(glm::vec3 value) {
    return nlohmann::json::array({value.x, value.y, value.z});
}

glm::vec3 vec3FromJson(const nlohmann::json& json, glm::vec3 fallback) {
    if (!json.is_array() || json.size() < 3) {
        return fallback;
    }
    return glm::vec3(json[0].get<float>(), json[1].get<float>(), json[2].get<float>());
}
} // namespace

void EditorTimeline::clear() {
    keyframes_.clear();
    playing = false;
    currentFrame = 0;
    startFrame = 0;
    endFrame = 120;
}

void EditorTimeline::stop() {
    playing = false;
    currentFrame = startFrame;
}

void EditorTimeline::addTransformKey(EntityId entity, uint64_t entityUuid, Transform transform) {
    keyframes_.push_back(TimelineTransformKeyframe{
        .entity = entity,
        .entityUuid = entityUuid,
        .frame = currentFrame,
        .transform = transform,
    });
    std::sort(keyframes_.begin(), keyframes_.end(), [](const TimelineTransformKeyframe& a, const TimelineTransformKeyframe& b) {
        if (a.frame != b.frame) {
            return a.frame < b.frame;
        }
        return a.entityUuid < b.entityUuid;
    });
}

bool EditorTimeline::advance(float deltaSeconds) {
    if (!playing) {
        return false;
    }
    const int before = currentFrame;
    currentFrame += std::max(1, static_cast<int>(deltaSeconds * 30.0f));
    if (currentFrame > endFrame) {
        currentFrame = startFrame;
    }
    return currentFrame != before;
}

bool EditorTimeline::sampleTransform(uint64_t entityUuid, int frame, Transform& outTransform) const {
    const TimelineTransformKeyframe* previous = nullptr;
    const TimelineTransformKeyframe* next = nullptr;
    for (const TimelineTransformKeyframe& key : keyframes_) {
        if (key.entityUuid != entityUuid) {
            continue;
        }
        if (key.frame <= frame && (previous == nullptr || key.frame > previous->frame)) {
            previous = &key;
        }
        if (key.frame >= frame && (next == nullptr || key.frame < next->frame)) {
            next = &key;
        }
    }
    if (previous == nullptr && next == nullptr) {
        return false;
    }
    if (previous == nullptr) {
        outTransform = next->transform;
        return true;
    }
    if (next == nullptr || previous->frame == next->frame) {
        outTransform = previous->transform;
        return true;
    }
    const float t = static_cast<float>(frame - previous->frame) / static_cast<float>(next->frame - previous->frame);
    outTransform = previous->transform;
    outTransform.position = glm::mix(previous->transform.position, next->transform.position, t);
    outTransform.rotationEuler = glm::mix(previous->transform.rotationEuler, next->transform.rotationEuler, t);
    outTransform.scale = glm::mix(previous->transform.scale, next->transform.scale, t);
    outTransform.dirty = true;
    return true;
}

std::vector<uint64_t> EditorTimeline::animatedEntityUuids() const {
    std::vector<uint64_t> uuids;
    for (const TimelineTransformKeyframe& key : keyframes_) {
        if (key.entityUuid == 0) {
            continue;
        }
        if (std::find(uuids.begin(), uuids.end(), key.entityUuid) == uuids.end()) {
            uuids.push_back(key.entityUuid);
        }
    }
    return uuids;
}

nlohmann::json EditorTimeline::serialize() const {
    nlohmann::json json;
    json["version"] = 1;
    json["playing"] = playing;
    json["currentFrame"] = currentFrame;
    json["startFrame"] = startFrame;
    json["endFrame"] = endFrame;
    json["transformKeyframes"] = nlohmann::json::array();
    for (const TimelineTransformKeyframe& key : keyframes_) {
        json["transformKeyframes"].push_back({
            {"entityUuid", key.entityUuid},
            {"frame", key.frame},
            {"position", vec3Json(key.transform.position)},
            {"rotationEuler", vec3Json(key.transform.rotationEuler)},
            {"scale", vec3Json(key.transform.scale)},
        });
    }
    return json;
}

void EditorTimeline::deserialize(const nlohmann::json& json) {
    clear();
    if (!json.is_object()) {
        return;
    }
    playing = json.value("playing", false);
    currentFrame = json.value("currentFrame", 0);
    startFrame = json.value("startFrame", 0);
    endFrame = json.value("endFrame", 120);
    if (endFrame < startFrame) {
        endFrame = startFrame;
    }
    if (!json.contains("transformKeyframes") || !json["transformKeyframes"].is_array()) {
        return;
    }
    for (const nlohmann::json& item : json["transformKeyframes"]) {
        Transform transform;
        transform.position = vec3FromJson(item.value("position", nlohmann::json::array()), transform.position);
        transform.rotationEuler = vec3FromJson(item.value("rotationEuler", nlohmann::json::array()), transform.rotationEuler);
        transform.scale = vec3FromJson(item.value("scale", nlohmann::json::array()), transform.scale);
        keyframes_.push_back(TimelineTransformKeyframe{
            .entity = {},
            .entityUuid = item.value("entityUuid", uint64_t{0}),
            .frame = item.value("frame", 0),
            .transform = transform,
        });
    }
}

} // namespace rtv
