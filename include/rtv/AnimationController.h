#pragma once

#include <filesystem>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace rtv {

enum class AnimationControllerParameterType {
    Bool,
    Int,
    Float,
    Trigger,
    Unknown,
};

struct AnimationControllerParameterValue {
    AnimationControllerParameterType type = AnimationControllerParameterType::Unknown;
    bool boolValue = false;
    int intValue = 0;
    float floatValue = 0.0f;
    bool triggerValue = false;
};

[[nodiscard]] AnimationControllerParameterType animationControllerParameterTypeFromName(std::string_view type);
[[nodiscard]] const char* animationControllerParameterTypeName(AnimationControllerParameterType type);

struct AnimationControllerEvaluation {
    std::string previousState;
    std::string state;
    std::string transitionTo;
    bool transitioned = false;
    std::vector<std::string> consumedTriggers;
    std::vector<std::string> routedEventNames;
    std::vector<std::string> routedEventPayloads;
    std::string selectedClipGuid;
    std::filesystem::path selectedClipPath;
    std::string selectedBlendTreeChild;
    float selectedBlendTreeParameterValue = 0.0f;
    bool blendTreeActive = false;
    std::string blendFromClipGuid;
    std::filesystem::path blendFromClipPath;
    std::string blendFromChild;
    std::string blendToClipGuid;
    std::filesystem::path blendToClipPath;
    std::string blendToChild;
    float blendAlpha = 0.0f;
    std::string blendEventPolicy = "primary_clip_only";
    std::string blendRootMotionPolicy = "primary_clip_candidates";
};

class AnimationController {
public:
    struct Parameter {
        std::string name;
        AnimationControllerParameterType type = AnimationControllerParameterType::Unknown;
        AnimationControllerParameterValue defaultValue;
    };

    struct Condition {
        std::string parameter;
        std::string op;
        AnimationControllerParameterValue value;
    };

    struct Event {
        std::string name;
        std::string payloadJson;
    };

    struct BlendTreeChild {
        std::string name;
        std::string clipGuid;
        std::filesystem::path clipPath;
        float threshold = 0.0f;
    };

    struct BlendTree {
        std::string type = "1d";
        std::string parameter;
        std::vector<BlendTreeChild> children;
    };

    struct AvatarMask {
        std::string name;
        std::vector<std::string> includedJoints;
        std::vector<std::string> excludedJoints;
    };

    struct Layer {
        std::string name;
        std::string clipGuid;
        std::filesystem::path clipPath;
        float weight = 1.0f;
        bool additive = false;
        std::string mask;
    };

    struct Transition {
        std::string to;
        float exitTimeSeconds = 0.0f;
        std::vector<Condition> conditions;
        std::vector<Event> events;
    };

    struct State {
        std::string name;
        std::string clipGuid;
        std::filesystem::path clipPath;
        float speed = 1.0f;
        bool loop = true;
        bool defaultState = false;
        std::vector<Transition> transitions;
        std::vector<Event> events;
        BlendTree blendTree;
        bool hasBlendTree = false;
    };

    [[nodiscard]] static AnimationController fromJson(const nlohmann::json& root, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static AnimationController loadJson(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static AnimationController loadNativeBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static AnimationController loadNative(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static AnimationController load(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);

    [[nodiscard]] bool valid() const { return !states_.empty() && !initialState_.empty(); }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::string& initialState() const { return initialState_; }
    [[nodiscard]] const std::vector<Parameter>& parameters() const { return parameters_; }
    [[nodiscard]] const std::vector<State>& states() const { return states_; }
    [[nodiscard]] const std::vector<Layer>& layers() const { return layers_; }
    [[nodiscard]] const std::vector<AvatarMask>& avatarMasks() const { return avatarMasks_; }
    [[nodiscard]] std::unordered_map<std::string, AnimationControllerParameterValue> defaultParameters() const;
    [[nodiscard]] const State* state(std::string_view name) const;
    [[nodiscard]] AnimationControllerEvaluation evaluate(
        std::string_view currentState,
        float timeInStateSeconds,
        const std::unordered_map<std::string, AnimationControllerParameterValue>& parameters) const;

private:
    std::string name_;
    std::string initialState_;
    std::vector<Parameter> parameters_;
    std::vector<State> states_;
    std::vector<Layer> layers_;
    std::vector<AvatarMask> avatarMasks_;
};

[[nodiscard]] nlohmann::json animationControllerInspectionJson(
    const AnimationController& controller,
    const std::vector<std::string>& warnings = {});
[[nodiscard]] nlohmann::json animationControllerTransparentJson(const AnimationController& controller);
[[nodiscard]] int inspectAnimationControllerCommand(const std::filesystem::path& path, const std::filesystem::path& jsonOut = {});
[[nodiscard]] int exportAnimationControllerCommand(
    const std::filesystem::path& path,
    const std::filesystem::path& outputPath = {},
    const std::filesystem::path& jsonOut = {});
[[nodiscard]] int mutateAnimationControllerCommand(
    const std::filesystem::path& path,
    const std::filesystem::path& mutationPath = {},
    const std::filesystem::path& outputPath = {},
    const std::filesystem::path& jsonOut = {},
    bool dryRun = false,
    bool forceOverwrite = false);

} // namespace rtv
