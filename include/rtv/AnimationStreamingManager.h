#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/AssetRegistry.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

// Streaming states for progressive animation loading.
enum class AnimationStreamingState : uint8_t {
    // No animation data loaded.
    None,
    // Skeleton metadata loaded (bone count, names, hierarchy).
    SkeletonMetadataLoaded,
    // Bind pose matrices available for static posing.
    BindPoseLoaded,
    // Animation clip metadata loaded (clip name, duration, keyframe count).
    ClipMetadataLoaded,
    // Animation clip keyframes partially streamed (e.g., first N keyframes).
    ClipPartiallyStreamed,
    // Full animation clip resident and playable.
    ClipFullyResident,
    // Failed to load.
    Failed,
};

// Per-clip streaming record.
struct AnimationClipStreamingRecord {
    AssetGuid clipGuid;
    std::string clipName;
    AnimationStreamingState state = AnimationStreamingState::None;
    float durationSeconds = 0.0f;
    uint32_t keyframeCount = 0;
    uint32_t streamedKeyframeCount = 0;
    uint64_t totalBytes = 0;
    uint64_t streamedBytes = 0;
    bool playable = false;  // True when enough keyframes are resident to begin playback.
    std::string failureReason;
};

// Per-skeleton streaming record.
struct SkeletonStreamingRecord {
    AssetGuid skeletonGuid;
    std::string skeletonName;
    AnimationStreamingState state = AnimationStreamingState::None;
    uint32_t boneCount = 0;
    uint64_t metadataBytes = 0;
    uint64_t bindPoseBytes = 0;
    bool bindPoseAvailable = false;
    std::string failureReason;
    std::vector<AnimationClipStreamingRecord> clips;
};

// Manages progressive animation streaming: skeleton metadata first,
// then bind pose, then animation clips stream independently.
//
// Key behaviors:
// - Static hierarchy appears first; skeleton metadata follows.
// - Bind pose loads before animation clips so the mesh can render
//   in a static pose while clips stream.
// - Animation clips stream independently; each clip can be played
//   as soon as enough keyframes are resident.
// - Skinned mesh GPU buffers render bind pose until animation is ready.
// - Morph targets stream after base mesh (via separate mesh streaming).
class AnimationStreamingManager final : private NonCopyable {
public:
    AnimationStreamingManager() = default;

    // Register a skeleton for progressive streaming.
    void registerSkeleton(AssetGuid skeletonGuid, const std::string& name,
                          uint32_t boneCount, uint64_t metadataBytes,
                          uint64_t bindPoseBytes);

    // Signal streaming progress for a skeleton.
    void signalSkeletonState(AssetGuid skeletonGuid, AnimationStreamingState state);

    // Register an animation clip for a skeleton.
    void registerClip(AssetGuid skeletonGuid, AssetGuid clipGuid,
                      const std::string& clipName, float durationSeconds,
                      uint32_t keyframeCount, uint64_t totalBytes);

    // Signal streaming progress for a clip.
    void signalClipState(AssetGuid clipGuid, AnimationStreamingState state,
                         uint32_t streamedKeyframeCount = 0, uint64_t streamedBytes = 0);

    // Check if a clip is playable (enough keyframes streamed).
    [[nodiscard]] bool isClipPlayable(AssetGuid clipGuid) const;

    // Check if an entity can render skinned (at least bind pose available).
    [[nodiscard]] bool canRenderSkinned(AssetGuid skeletonGuid) const;

    // Query records.
    [[nodiscard]] const SkeletonStreamingRecord* findSkeleton(AssetGuid skeletonGuid) const;
    [[nodiscard]] const AnimationClipStreamingRecord* findClip(AssetGuid clipGuid) const;

    [[nodiscard]] const std::vector<SkeletonStreamingRecord>& skeletons() const { return skeletons_; }

    // Mark failed.
    void failSkeleton(AssetGuid skeletonGuid, const std::string& reason);
    void failClip(AssetGuid clipGuid, const std::string& reason);

private:
    std::vector<SkeletonStreamingRecord> skeletons_;
};

[[nodiscard]] const char* animationStreamingStateName(AnimationStreamingState state);
[[nodiscard]] nlohmann::json animationStreamingToJson(
    const std::vector<SkeletonStreamingRecord>& skeletons);

} // namespace rtv
