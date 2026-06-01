#include "rtv/EditorTimeline.h"

#include <algorithm>
#include <cstddef>

namespace rtv {

namespace {
void sortKeyframes(std::vector<TimelineTransformKeyframe>& keyframes) {
    std::sort(keyframes.begin(), keyframes.end(), [](const TimelineTransformKeyframe& a, const TimelineTransformKeyframe& b) {
        if (a.frame != b.frame) {
            return a.frame < b.frame;
        }
        return a.entityUuid < b.entityUuid;
    });
}

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
    nextKeyId_ = 1;
    playing = false;
    currentFrame = 0;
    startFrame = 0;
    endFrame = 120;
    frameRate = 30;
}

void EditorTimeline::stop() {
    playing = false;
    currentFrame = startFrame;
}

void EditorTimeline::addTransformKey(EntityId entity, uint64_t entityUuid, Transform transform) {
    keyframes_.push_back(TimelineTransformKeyframe{
        .id = nextKeyId_++,
        .entity = entity,
        .entityUuid = entityUuid,
        .frame = currentFrame,
        .transform = transform,
    });
    sortKeyframes(keyframes_);
}

bool EditorTimeline::updateTransformKey(std::size_t index, int frame, Transform transform) {
    if (index >= keyframes_.size()) {
        return false;
    }
    keyframes_[index].frame = std::clamp(frame, startFrame, endFrame);
    keyframes_[index].transform = transform;
    keyframes_[index].transform.dirty = true;
    sortKeyframes(keyframes_);
    return true;
}

bool EditorTimeline::updateTransformKeyFrame(uint64_t keyId, int frame) {
    auto it = std::find_if(keyframes_.begin(), keyframes_.end(), [&](const TimelineTransformKeyframe& key) {
        return key.id == keyId;
    });
    if (it == keyframes_.end()) {
        return false;
    }
    it->frame = std::clamp(frame, startFrame, endFrame);
    it->transform.dirty = true;
    sortKeyframes(keyframes_);
    return true;
}

bool EditorTimeline::removeTransformKey(std::size_t index) {
    if (index >= keyframes_.size()) {
        return false;
    }
    keyframes_.erase(keyframes_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool EditorTimeline::removeTransformKeyById(uint64_t keyId) {
    auto it = std::find_if(keyframes_.begin(), keyframes_.end(), [&](const TimelineTransformKeyframe& key) {
        return key.id == keyId;
    });
    if (it == keyframes_.end()) {
        return false;
    }
    keyframes_.erase(it);
    return true;
}

bool EditorTimeline::advance(float deltaSeconds) {
    if (!playing) {
        return false;
    }
    const int before = currentFrame;
    currentFrame += std::max(1, static_cast<int>(deltaSeconds * static_cast<float>(std::clamp(frameRate, 1, 240))));
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
    json["frameRate"] = frameRate;
    json["transformKeyframes"] = nlohmann::json::array();
    for (const TimelineTransformKeyframe& key : keyframes_) {
        json["transformKeyframes"].push_back({
            {"id", key.id},
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
    frameRate = std::clamp(json.value("frameRate", 30), 1, 240);
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
        const uint64_t keyId = item.contains("id") ? item.value("id", uint64_t{0}) : nextKeyId_++;
        keyframes_.push_back(TimelineTransformKeyframe{
            .id = keyId,
            .entity = {},
            .entityUuid = item.value("entityUuid", uint64_t{0}),
            .frame = item.value("frame", 0),
            .transform = transform,
        });
    }
    for (const TimelineTransformKeyframe& key : keyframes_) {
        nextKeyId_ = std::max(nextKeyId_, key.id + 1u);
    }
    sortKeyframes(keyframes_);
}

} // namespace rtv
