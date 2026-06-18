#include "rtv/AnimationStreamingManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace rtv {

void AnimationStreamingManager::registerSkeleton(AssetGuid skeletonGuid, const std::string& name,
                                                   uint32_t boneCount, uint64_t metadataBytes,
                                                   uint64_t bindPoseBytes) {
    SkeletonStreamingRecord skel;
    skel.skeletonGuid = skeletonGuid;
    skel.skeletonName = name;
    skel.boneCount = boneCount;
    skel.metadataBytes = metadataBytes;
    skel.bindPoseBytes = bindPoseBytes;
    skel.state = AnimationStreamingState::SkeletonMetadataLoaded;
    skel.bindPoseAvailable = false;
    skeletons_.push_back(std::move(skel));
}

void AnimationStreamingManager::signalSkeletonState(AssetGuid skeletonGuid, AnimationStreamingState state) {
    for (SkeletonStreamingRecord& skel : skeletons_) {
        if (skel.skeletonGuid == skeletonGuid) {
            skel.state = std::max(skel.state, state);
            if (state >= AnimationStreamingState::BindPoseLoaded) {
                skel.bindPoseAvailable = true;
            }
            return;
        }
    }
}

void AnimationStreamingManager::registerClip(AssetGuid skeletonGuid, AssetGuid clipGuid,
                                               const std::string& clipName, float durationSeconds,
                                               uint32_t keyframeCount, uint64_t totalBytes) {
    for (SkeletonStreamingRecord& skel : skeletons_) {
        if (skel.skeletonGuid == skeletonGuid) {
            AnimationClipStreamingRecord clip;
            clip.clipGuid = clipGuid;
            clip.clipName = clipName;
            clip.durationSeconds = durationSeconds;
            clip.keyframeCount = keyframeCount;
            clip.totalBytes = totalBytes;
            clip.state = AnimationStreamingState::ClipMetadataLoaded;
            skel.clips.push_back(std::move(clip));
            return;
        }
    }
}

void AnimationStreamingManager::signalClipState(AssetGuid clipGuid, AnimationStreamingState state,
                                                  uint32_t streamedKeyframeCount, uint64_t streamedBytes) {
    for (SkeletonStreamingRecord& skel : skeletons_) {
        for (AnimationClipStreamingRecord& clip : skel.clips) {
            if (clip.clipGuid == clipGuid) {
                clip.state = std::max(clip.state, state);
                clip.streamedKeyframeCount = streamedKeyframeCount;
                clip.streamedBytes = streamedBytes;
                // A clip is playable when at least 10% of keyframes are loaded
                // or the state is ClipFullyResident.
                clip.playable = (state >= AnimationStreamingState::ClipPartiallyStreamed &&
                                 streamedKeyframeCount >= std::max(1u, clip.keyframeCount / 10u)) ||
                                state >= AnimationStreamingState::ClipFullyResident;
                return;
            }
        }
    }
}

bool AnimationStreamingManager::isClipPlayable(AssetGuid clipGuid) const {
    const AnimationClipStreamingRecord* clip = findClip(clipGuid);
    return clip != nullptr && clip->playable;
}

bool AnimationStreamingManager::canRenderSkinned(AssetGuid skeletonGuid) const {
    const SkeletonStreamingRecord* skel = findSkeleton(skeletonGuid);
    return skel != nullptr && skel->bindPoseAvailable;
}

const SkeletonStreamingRecord* AnimationStreamingManager::findSkeleton(AssetGuid skeletonGuid) const {
    for (const SkeletonStreamingRecord& skel : skeletons_) {
        if (skel.skeletonGuid == skeletonGuid) {
            return &skel;
        }
    }
    return nullptr;
}

const AnimationClipStreamingRecord* AnimationStreamingManager::findClip(AssetGuid clipGuid) const {
    for (const SkeletonStreamingRecord& skel : skeletons_) {
        for (const AnimationClipStreamingRecord& clip : skel.clips) {
            if (clip.clipGuid == clipGuid) {
                return &clip;
            }
        }
    }
    return nullptr;
}

void AnimationStreamingManager::failSkeleton(AssetGuid skeletonGuid, const std::string& reason) {
    for (SkeletonStreamingRecord& skel : skeletons_) {
        if (skel.skeletonGuid == skeletonGuid) {
            skel.state = AnimationStreamingState::Failed;
            skel.failureReason = reason;
        }
    }
}

void AnimationStreamingManager::failClip(AssetGuid clipGuid, const std::string& reason) {
    for (SkeletonStreamingRecord& skel : skeletons_) {
        for (AnimationClipStreamingRecord& clip : skel.clips) {
            if (clip.clipGuid == clipGuid) {
                clip.state = AnimationStreamingState::Failed;
                clip.failureReason = reason;
            }
        }
    }
}

const char* animationStreamingStateName(AnimationStreamingState state) {
    switch (state) {
    case AnimationStreamingState::None: return "none";
    case AnimationStreamingState::SkeletonMetadataLoaded: return "skeleton_metadata_loaded";
    case AnimationStreamingState::BindPoseLoaded: return "bind_pose_loaded";
    case AnimationStreamingState::ClipMetadataLoaded: return "clip_metadata_loaded";
    case AnimationStreamingState::ClipPartiallyStreamed: return "clip_partially_streamed";
    case AnimationStreamingState::ClipFullyResident: return "clip_fully_resident";
    case AnimationStreamingState::Failed: return "failed";
    }
    return "unknown";
}

nlohmann::json animationStreamingToJson(const std::vector<SkeletonStreamingRecord>& skeletons) {
    nlohmann::json result = nlohmann::json::array();
    for (const SkeletonStreamingRecord& skel : skeletons) {
        nlohmann::json clips = nlohmann::json::array();
        for (const AnimationClipStreamingRecord& clip : skel.clips) {
            clips.push_back({
                {"clip_guid", clip.clipGuid},
                {"clip_name", clip.clipName},
                {"state", animationStreamingStateName(clip.state)},
                {"duration_seconds", clip.durationSeconds},
                {"keyframe_count", clip.keyframeCount},
                {"streamed_keyframes", clip.streamedKeyframeCount},
                {"total_bytes", clip.totalBytes},
                {"streamed_bytes", clip.streamedBytes},
                {"playable", clip.playable},
                {"failure_reason", clip.failureReason},
            });
        }
        result.push_back({
            {"skeleton_guid", skel.skeletonGuid},
            {"skeleton_name", skel.skeletonName},
            {"state", animationStreamingStateName(skel.state)},
            {"bone_count", skel.boneCount},
            {"bind_pose_available", skel.bindPoseAvailable},
            {"failure_reason", skel.failureReason},
            {"clips", clips},
        });
    }
    return result;
}

} // namespace rtv
