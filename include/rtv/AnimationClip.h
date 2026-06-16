#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace rtv {

enum class AnimationTrackPath {
    Translation,
    Rotation,
    Scale,
    Weights,
    MeshVertexPositions,
    CameraYfov,
    CameraAspectRatio,
    CameraOrthoXmag,
    CameraOrthoYmag,
    CameraNearFar,
    LightColor,
    LightIntensity,
    LightRadius,
    LightConeAngles,
    Unknown,
};

struct AnimationNodeSample {
    bool hasTranslation = false;
    glm::vec3 translation{0.0f};
    bool hasRotation = false;
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    bool hasScale = false;
    glm::vec3 scale{1.0f};
    bool hasMorphWeights = false;
    std::vector<float> morphWeights;
    bool hasMeshVertexPositions = false;
    std::vector<glm::vec3> meshVertexPositions;
    bool hasCameraYfov = false;
    float cameraYfov = 0.0f;
    bool hasCameraAspectRatio = false;
    float cameraAspectRatio = 0.0f;
    bool hasCameraOrthoXmag = false;
    float cameraOrthoXmag = 0.0f;
    bool hasCameraOrthoYmag = false;
    float cameraOrthoYmag = 0.0f;
    bool hasCameraNearFar = false;
    glm::vec2 cameraNearFar{0.01f, 1000.0f};
    bool hasLightColor = false;
    glm::vec3 lightColor{1.0f};
    bool hasLightIntensity = false;
    float lightIntensity = 1.0f;
    bool hasLightRadius = false;
    float lightRadius = 1.0f;
    bool hasLightConeAngles = false;
    glm::vec2 lightConeAngles{0.35f, 0.70f};
};

struct AnimationSample {
    double timeSeconds = 0.0;
    std::unordered_map<int32_t, AnimationNodeSample> nodes;
};

class AnimationClip {
public:
    struct RootMotionCandidate {
        int32_t node = -1;
        std::string nodeName;
        AnimationTrackPath path = AnimationTrackPath::Unknown;
        std::string reason;
    };

    struct Track {
        int32_t node = -1;
        std::string nodeName;
        AnimationTrackPath path = AnimationTrackPath::Unknown;
        std::string interpolation = "LINEAR";
        std::vector<float> times;
        std::vector<std::vector<float>> values;
        std::vector<std::vector<float>> inTangents;
        std::vector<std::vector<float>> outTangents;
    };

    struct Event {
        double timeSeconds = 0.0;
        std::string name;
        std::string payloadJson;
    };

    [[nodiscard]] static AnimationClip fromRtanimJson(const nlohmann::json& root, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static AnimationClip loadRtanimJson(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static AnimationClip loadRtanimNativeBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static AnimationClip loadRtanimNative(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static AnimationClip loadRtanim(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);

    [[nodiscard]] bool valid() const { return !tracks_.empty(); }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] double startTime() const { return startTime_; }
    [[nodiscard]] double endTime() const { return endTime_; }
    [[nodiscard]] double duration() const { return duration_; }
    [[nodiscard]] size_t trackCount() const { return tracks_.size(); }
    [[nodiscard]] size_t rootMotionCandidateCount() const { return rootMotionCandidateCount_; }
    [[nodiscard]] const std::vector<Track>& tracks() const { return tracks_; }
    [[nodiscard]] const std::vector<RootMotionCandidate>& rootMotionCandidates() const { return rootMotionCandidates_; }
    [[nodiscard]] const std::vector<Event>& events() const { return events_; }

    [[nodiscard]] AnimationSample sample(double timeSeconds, bool loop = true) const;

private:
    std::string name_;
    double startTime_ = 0.0;
    double endTime_ = 0.0;
    double duration_ = 0.0;
    size_t rootMotionCandidateCount_ = 0;
    std::vector<Track> tracks_;
    std::vector<RootMotionCandidate> rootMotionCandidates_;
    std::vector<Event> events_;
};

} // namespace rtv
