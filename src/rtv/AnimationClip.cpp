#include "rtv/AnimationClip.h"

#include "rtv/NativeBinaryIO.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

namespace rtv {

namespace {

constexpr uint32_t kRtanimMetadataJsonChunk = 100u;

bool readFileBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    return file.good() || size == 0;
}

bool chunkRangeInside(const NativeChunkRecord& chunk, size_t fileSize) {
    return chunk.offset <= fileSize && chunk.size <= fileSize - chunk.offset;
}

AnimationTrackPath trackPathFromString(const std::string& path) {
    if (path == "translation") return AnimationTrackPath::Translation;
    if (path == "rotation") return AnimationTrackPath::Rotation;
    if (path == "scale") return AnimationTrackPath::Scale;
    if (path == "weights") return AnimationTrackPath::Weights;
    if (path == "meshVertexPositions") return AnimationTrackPath::MeshVertexPositions;
    if (path == "cameraYfov") return AnimationTrackPath::CameraYfov;
    if (path == "cameraAspectRatio") return AnimationTrackPath::CameraAspectRatio;
    if (path == "cameraOrthoXmag") return AnimationTrackPath::CameraOrthoXmag;
    if (path == "cameraOrthoYmag") return AnimationTrackPath::CameraOrthoYmag;
    if (path == "cameraNearFar") return AnimationTrackPath::CameraNearFar;
    if (path == "lightColor") return AnimationTrackPath::LightColor;
    if (path == "lightIntensity") return AnimationTrackPath::LightIntensity;
    if (path == "lightRadius") return AnimationTrackPath::LightRadius;
    if (path == "lightConeAngles") return AnimationTrackPath::LightConeAngles;
    return AnimationTrackPath::Unknown;
}

size_t expectedComponentCount(AnimationTrackPath path) {
    switch (path) {
    case AnimationTrackPath::Translation:
    case AnimationTrackPath::Scale:
        return 3;
    case AnimationTrackPath::Rotation:
        return 4;
    case AnimationTrackPath::CameraNearFar:
    case AnimationTrackPath::LightConeAngles:
        return 2;
    case AnimationTrackPath::LightColor:
        return 3;
    case AnimationTrackPath::CameraYfov:
    case AnimationTrackPath::CameraAspectRatio:
    case AnimationTrackPath::CameraOrthoXmag:
    case AnimationTrackPath::CameraOrthoYmag:
    case AnimationTrackPath::LightIntensity:
    case AnimationTrackPath::LightRadius:
        return 1;
    case AnimationTrackPath::Weights:
    case AnimationTrackPath::MeshVertexPositions:
    case AnimationTrackPath::Unknown:
        return 0;
    }
    return 0;
}

std::vector<float> floatVectorFromJsonArray(const nlohmann::json& json) {
    std::vector<float> result;
    if (!json.is_array()) {
        return result;
    }
    result.reserve(json.size());
    for (const nlohmann::json& item : json) {
        if (item.is_number()) {
            result.push_back(item.get<float>());
        }
    }
    return result;
}

std::vector<std::vector<float>> floatVectorArrayFromJson(const nlohmann::json& json) {
    std::vector<std::vector<float>> result;
    if (!json.is_array()) {
        return result;
    }
    result.reserve(json.size());
    for (const nlohmann::json& item : json) {
        result.push_back(floatVectorFromJsonArray(item));
    }
    return result;
}

const nlohmann::json* animationPayloadFromRtanimRoot(const nlohmann::json& root) {
    if (root.contains("animation") && root["animation"].is_object()) {
        return &root["animation"];
    }
    if (root.contains("channels") && root["channels"].is_array()) {
        return &root;
    }
    return nullptr;
}

void addWarning(std::vector<std::string>* warnings, std::string message) {
    if (warnings != nullptr) {
        warnings->push_back(std::move(message));
    }
}

bool validateTrack(const AnimationClip::Track& track) {
    if (track.node < 0 || track.path == AnimationTrackPath::Unknown || track.times.empty() || track.values.empty()) {
        return false;
    }
    if (track.values.size() < track.times.size()) {
        return false;
    }
    const size_t fixedComponents = expectedComponentCount(track.path);
    if (fixedComponents > 0) {
        for (size_t i = 0; i < track.times.size(); ++i) {
            if (track.values[i].size() != fixedComponents) {
                return false;
            }
        }
    } else if (track.path == AnimationTrackPath::Weights) {
        const size_t weightCount = track.values.front().size();
        if (weightCount == 0) {
            return false;
        }
        for (size_t i = 0; i < track.times.size(); ++i) {
            if (track.values[i].size() != weightCount) {
                return false;
            }
        }
    } else if (track.path == AnimationTrackPath::MeshVertexPositions) {
        const size_t componentCount = track.values.front().size();
        if (componentCount == 0 || componentCount % 3u != 0u) {
            return false;
        }
        for (size_t i = 0; i < track.times.size(); ++i) {
            if (track.values[i].size() != componentCount) {
                return false;
            }
        }
    }
    const bool cubic = track.interpolation == "CUBICSPLINE";
    if (cubic && (track.inTangents.size() < track.times.size() || track.outTangents.size() < track.times.size())) {
        return false;
    }
    return true;
}

double wrappedSampleTime(double timeSeconds, double start, double duration, bool loop) {
    if (!loop || duration <= 0.0) {
        return timeSeconds;
    }
    double local = std::fmod(timeSeconds - start, duration);
    if (local < 0.0) {
        local += duration;
    }
    return start + local;
}

size_t lowerKeyIndex(const std::vector<float>& times, double timeSeconds) {
    if (times.size() <= 1 || timeSeconds <= static_cast<double>(times.front())) {
        return 0;
    }
    if (timeSeconds >= static_cast<double>(times.back())) {
        return times.size() - 1;
    }
    const auto it = std::upper_bound(times.begin(), times.end(), static_cast<float>(timeSeconds));
    return static_cast<size_t>(std::distance(times.begin(), it) - 1);
}

std::vector<float> sampleVectorTrack(const AnimationClip::Track& track, double timeSeconds) {
    const size_t key0 = lowerKeyIndex(track.times, timeSeconds);
    if (key0 + 1 >= track.times.size() || track.interpolation == "STEP") {
        return track.values[key0];
    }

    const size_t key1 = key0 + 1;
    const double t0 = static_cast<double>(track.times[key0]);
    const double t1 = static_cast<double>(track.times[key1]);
    const double dt = std::max(t1 - t0, std::numeric_limits<double>::epsilon());
    const float u = static_cast<float>(std::clamp((timeSeconds - t0) / dt, 0.0, 1.0));
    const size_t components = track.values[key0].size();
    std::vector<float> result(components, 0.0f);

    if (track.interpolation == "CUBICSPLINE") {
        const float u2 = u * u;
        const float u3 = u2 * u;
        const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
        const float h10 = u3 - 2.0f * u2 + u;
        const float h01 = -2.0f * u3 + 3.0f * u2;
        const float h11 = u3 - u2;
        for (size_t c = 0; c < components; ++c) {
            const float p0 = track.values[key0][c];
            const float p1 = track.values[key1][c];
            const float m0 = key0 < track.outTangents.size() && c < track.outTangents[key0].size()
                ? static_cast<float>(dt) * track.outTangents[key0][c]
                : 0.0f;
            const float m1 = key1 < track.inTangents.size() && c < track.inTangents[key1].size()
                ? static_cast<float>(dt) * track.inTangents[key1][c]
                : 0.0f;
            result[c] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
        }
        return result;
    }

    for (size_t c = 0; c < components; ++c) {
        result[c] = track.values[key0][c] + (track.values[key1][c] - track.values[key0][c]) * u;
    }
    return result;
}

glm::vec3 vec3FromTrackValue(const std::vector<float>& value, glm::vec3 fallback) {
    if (value.size() < 3) {
        return fallback;
    }
    return glm::vec3{value[0], value[1], value[2]};
}

glm::vec2 vec2FromTrackValue(const std::vector<float>& value, glm::vec2 fallback) {
    if (value.size() < 2) {
        return fallback;
    }
    return glm::vec2{value[0], value[1]};
}

float scalarFromTrackValue(const std::vector<float>& value, float fallback) {
    return value.empty() ? fallback : value.front();
}

std::vector<glm::vec3> vec3ArrayFromTrackValue(const std::vector<float>& value) {
    std::vector<glm::vec3> result;
    if (value.size() < 3u) {
        return result;
    }
    result.reserve(value.size() / 3u);
    for (size_t i = 0; i + 2u < value.size(); i += 3u) {
        result.push_back(glm::vec3{value[i], value[i + 1u], value[i + 2u]});
    }
    return result;
}

glm::quat quatFromTrackValue(const std::vector<float>& value) {
    if (value.size() < 4) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }
    const glm::quat q{value[3], value[0], value[1], value[2]};
    const float len2 = glm::dot(q, q);
    return len2 > 1.0e-10f ? glm::normalize(q) : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
}

glm::quat sampleRotationTrack(const AnimationClip::Track& track, double timeSeconds) {
    const size_t key0 = lowerKeyIndex(track.times, timeSeconds);
    if (key0 + 1 >= track.times.size() || track.interpolation == "STEP" || track.interpolation == "CUBICSPLINE") {
        return quatFromTrackValue(sampleVectorTrack(track, timeSeconds));
    }

    const size_t key1 = key0 + 1;
    const double t0 = static_cast<double>(track.times[key0]);
    const double t1 = static_cast<double>(track.times[key1]);
    const double dt = std::max(t1 - t0, std::numeric_limits<double>::epsilon());
    const float u = static_cast<float>(std::clamp((timeSeconds - t0) / dt, 0.0, 1.0));
    return glm::normalize(glm::slerp(quatFromTrackValue(track.values[key0]), quatFromTrackValue(track.values[key1]), u));
}

} // namespace

AnimationClip AnimationClip::fromRtanimJson(const nlohmann::json& root, std::vector<std::string>* warnings) {
    AnimationClip clip;
    const nlohmann::json* animation = animationPayloadFromRtanimRoot(root);
    if (animation == nullptr) {
        addWarning(warnings, "Animation clip load failed: .rtanim JSON does not contain an animation payload.");
        return clip;
    }

    clip.name_ = animation->value("name", std::string{});
    if (animation->contains("clip") && (*animation)["clip"].is_object()) {
        const nlohmann::json& clipJson = (*animation)["clip"];
        clip.startTime_ = clipJson.value("startTime", 0.0);
        clip.endTime_ = clipJson.value("endTime", 0.0);
        clip.duration_ = clipJson.value("duration", std::max(0.0, clip.endTime_ - clip.startTime_));
    }
    clip.rootMotionCandidateCount_ = animation->value("rootMotionCandidateCount", 0u);
    if (animation->contains("rootMotionCandidates") && (*animation)["rootMotionCandidates"].is_array()) {
        for (const nlohmann::json& item : (*animation)["rootMotionCandidates"]) {
            if (!item.is_object()) {
                continue;
            }
            AnimationClip::RootMotionCandidate candidate;
            candidate.node = item.value("node", -1);
            candidate.nodeName = item.value("nodeName", std::string{});
            candidate.path = trackPathFromString(item.value("path", std::string{}));
            candidate.reason = item.value("candidateReason", std::string{});
            if (candidate.node >= 0 && (candidate.path == AnimationTrackPath::Translation || candidate.path == AnimationTrackPath::Rotation)) {
                clip.rootMotionCandidates_.push_back(std::move(candidate));
            }
        }
        clip.rootMotionCandidateCount_ = clip.rootMotionCandidates_.size();
    }

    if (animation->contains("events") && (*animation)["events"].is_array()) {
        for (const nlohmann::json& item : (*animation)["events"]) {
            if (!item.is_object()) {
                continue;
            }
            AnimationClip::Event event;
            event.timeSeconds = item.value("timeSeconds", item.value("time", 0.0));
            event.name = item.value("name", std::string{});
            if (item.contains("payload")) {
                event.payloadJson = item["payload"].dump();
            }
            if (event.name.empty()) {
                addWarning(warnings, "Animation clip skipped an event without a name.");
                continue;
            }
            clip.events_.push_back(std::move(event));
        }
        std::sort(clip.events_.begin(), clip.events_.end(), [](const AnimationClip::Event& a, const AnimationClip::Event& b) {
            return a.timeSeconds < b.timeSeconds;
        });
    }

    if (!animation->contains("channels") || !(*animation)["channels"].is_array()) {
        addWarning(warnings, "Animation clip load skipped: animation payload has no decoded channels.");
        return clip;
    }

    for (const nlohmann::json& channel : (*animation)["channels"]) {
        if (!channel.is_object() || !channel.contains("decodedTrack") || !channel["decodedTrack"].is_object()) {
            continue;
        }
        const nlohmann::json& decodedTrack = channel["decodedTrack"];
        if (!decodedTrack.value("decoded", false)) {
            continue;
        }
        const nlohmann::json target = channel.contains("target") && channel["target"].is_object()
            ? channel["target"]
            : nlohmann::json::object();
        Track track;
        track.node = target.value("node", -1);
        track.nodeName = target.value("nodeName", std::string{});
        track.path = trackPathFromString(target.value("path", decodedTrack.value("targetPath", std::string{})));
        track.interpolation = decodedTrack.value("interpolation", std::string("LINEAR"));
        track.times = floatVectorFromJsonArray(decodedTrack.value("times", nlohmann::json::array()));
        track.values = floatVectorArrayFromJson(decodedTrack.value("values", nlohmann::json::array()));
        track.inTangents = floatVectorArrayFromJson(decodedTrack.value("inTangents", nlohmann::json::array()));
        track.outTangents = floatVectorArrayFromJson(decodedTrack.value("outTangents", nlohmann::json::array()));
        if (!validateTrack(track)) {
            addWarning(warnings, "Animation clip load skipped an invalid decoded track.");
            continue;
        }
        clip.tracks_.push_back(std::move(track));
    }

    if (clip.duration_ <= 0.0 && !clip.tracks_.empty()) {
        double start = std::numeric_limits<double>::max();
        double end = 0.0;
        for (const Track& track : clip.tracks_) {
            start = std::min(start, static_cast<double>(track.times.front()));
            end = std::max(end, static_cast<double>(track.times.back()));
        }
        clip.startTime_ = start == std::numeric_limits<double>::max() ? 0.0 : start;
        clip.endTime_ = end;
        clip.duration_ = std::max(0.0, clip.endTime_ - clip.startTime_);
    }
    return clip;
}

AnimationClip AnimationClip::loadRtanimJson(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    std::ifstream file(path);
    if (!file.is_open()) {
        addWarning(warnings, "Animation clip load failed: could not open " + path.string());
        return {};
    }
    try {
        nlohmann::json root;
        file >> root;
        return fromRtanimJson(root, warnings);
    } catch (const std::exception& ex) {
        addWarning(warnings, std::string("Animation clip load failed: ") + ex.what());
    }
    return {};
}

AnimationClip AnimationClip::loadRtanimNativeBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, std::vector<std::string>* warnings) {
    NativeAssetReader reader;
    const NativeAssetInspection inspection = reader.inspectBytes(pathHint, bytes, true);
    if (!inspection.ok) {
        if (!inspection.errors.empty()) {
            addWarning(warnings, "Animation clip native load failed: " + inspection.errors.front().message);
        } else {
            addWarning(warnings, "Animation clip native load failed: invalid native asset " + pathHint.string());
        }
        return {};
    }
    if (static_cast<NativeAssetKind>(inspection.header.assetKind) != NativeAssetKind::Animation) {
        addWarning(warnings, "Animation clip native load failed: asset is not .rtanim " + pathHint.string());
        return {};
    }

    const auto chunkIt = std::find_if(inspection.chunks.begin(), inspection.chunks.end(), [](const NativeChunkRecord& chunk) {
        return chunk.type == kRtanimMetadataJsonChunk;
    });
    if (chunkIt == inspection.chunks.end() || !chunkRangeInside(*chunkIt, bytes.size())) {
        addWarning(warnings, "Animation clip native load failed: missing metadata JSON chunk in " + pathHint.string());
        return {};
    }

    std::string metadata;
    metadata.resize(static_cast<size_t>(chunkIt->size));
    if (!metadata.empty()) {
        std::memcpy(metadata.data(), bytes.data() + chunkIt->offset, metadata.size());
    }
    try {
        return fromRtanimJson(nlohmann::json::parse(metadata), warnings);
    } catch (const std::exception& ex) {
        addWarning(warnings, std::string("Animation clip native load failed: ") + ex.what());
    }
    return {};
}

AnimationClip AnimationClip::loadRtanimNative(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    std::vector<std::byte> bytes;
    if (!readFileBytes(path, bytes)) {
        addWarning(warnings, "Animation clip load failed: could not open native " + path.string());
        return {};
    }
    return loadRtanimNativeBytes(path, bytes, warnings);
}

AnimationClip AnimationClip::loadRtanim(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    if (nativeAssetKindFromExtension(path) == NativeAssetKind::Animation) {
        return loadRtanimNative(path, warnings);
    }
    return loadRtanimJson(path, warnings);
}

AnimationSample AnimationClip::sample(double timeSeconds, bool loop) const {
    AnimationSample sample;
    sample.timeSeconds = wrappedSampleTime(timeSeconds, startTime_, duration_, loop);
    for (const Track& track : tracks_) {
        AnimationNodeSample& nodeSample = sample.nodes[track.node];
        if (track.path == AnimationTrackPath::Translation) {
            nodeSample.translation = vec3FromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.translation);
            nodeSample.hasTranslation = true;
        } else if (track.path == AnimationTrackPath::Rotation) {
            nodeSample.rotation = sampleRotationTrack(track, sample.timeSeconds);
            nodeSample.hasRotation = true;
        } else if (track.path == AnimationTrackPath::Scale) {
            nodeSample.scale = vec3FromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.scale);
            nodeSample.hasScale = true;
        } else if (track.path == AnimationTrackPath::Weights) {
            nodeSample.morphWeights = sampleVectorTrack(track, sample.timeSeconds);
            nodeSample.hasMorphWeights = true;
        } else if (track.path == AnimationTrackPath::MeshVertexPositions) {
            nodeSample.meshVertexPositions = vec3ArrayFromTrackValue(sampleVectorTrack(track, sample.timeSeconds));
            nodeSample.hasMeshVertexPositions = !nodeSample.meshVertexPositions.empty();
        } else if (track.path == AnimationTrackPath::CameraYfov) {
            nodeSample.cameraYfov = scalarFromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.cameraYfov);
            nodeSample.hasCameraYfov = true;
        } else if (track.path == AnimationTrackPath::CameraAspectRatio) {
            nodeSample.cameraAspectRatio = scalarFromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.cameraAspectRatio);
            nodeSample.hasCameraAspectRatio = true;
        } else if (track.path == AnimationTrackPath::CameraOrthoXmag) {
            nodeSample.cameraOrthoXmag = scalarFromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.cameraOrthoXmag);
            nodeSample.hasCameraOrthoXmag = true;
        } else if (track.path == AnimationTrackPath::CameraOrthoYmag) {
            nodeSample.cameraOrthoYmag = scalarFromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.cameraOrthoYmag);
            nodeSample.hasCameraOrthoYmag = true;
        } else if (track.path == AnimationTrackPath::CameraNearFar) {
            nodeSample.cameraNearFar = vec2FromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.cameraNearFar);
            nodeSample.hasCameraNearFar = true;
        } else if (track.path == AnimationTrackPath::LightColor) {
            nodeSample.lightColor = vec3FromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.lightColor);
            nodeSample.hasLightColor = true;
        } else if (track.path == AnimationTrackPath::LightIntensity) {
            nodeSample.lightIntensity = scalarFromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.lightIntensity);
            nodeSample.hasLightIntensity = true;
        } else if (track.path == AnimationTrackPath::LightRadius) {
            nodeSample.lightRadius = scalarFromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.lightRadius);
            nodeSample.hasLightRadius = true;
        } else if (track.path == AnimationTrackPath::LightConeAngles) {
            nodeSample.lightConeAngles = vec2FromTrackValue(sampleVectorTrack(track, sample.timeSeconds), nodeSample.lightConeAngles);
            nodeSample.hasLightConeAngles = true;
        }
    }
    return sample;
}

} // namespace rtv
