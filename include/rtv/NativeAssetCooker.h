#pragma once

#include "rtv/AssetRegistry.h"
#include "rtv/MeshAsset.h"
#include "rtv/NativeAssetFormat.h"
#include "rtv/NativeTextureFormatPolicy.h"
#include "rtv/TextureAsset.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

class AnimationController;

struct NativeAssetCookInput {
    AssetGuid guid;
    std::filesystem::path outputPath;
    std::filesystem::path sourcePath;
    std::string displayName;
    std::string sourceHash;
    std::string importSettingsHash;
};

struct NativeTexturePayloadVariantPlanEntry {
    std::string name;
    std::string vkFormat;
    std::string compressionFamily;
    bool emitted = false;
    bool selectedPlatformTarget = false;
    bool runtimeUsable = false;
    std::string source;
    std::string pendingReason;
};

struct NativeAssetCookResult {
    bool success = false;
    std::filesystem::path path;
    std::string payloadHash;
    uint64_t payloadBytes = 0;
    std::string emittedVkFormat;
    std::string platformSelectedVkFormat;
    std::string texturePayloadVariant;
    std::string sourceContainerKind;
    std::string nativePayloadSource;
    bool platformSelectedFormatRealized = false;
    bool platformCompressedPayloadEmission = false;
    bool sourceContainerPreserved = false;
    bool sourceContainerTranscoded = false;
    std::vector<NativeTexturePayloadVariantPlanEntry> texturePayloadVariantPlan;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

class NativeAssetCooker {
public:
    NativeAssetCooker();
    explicit NativeAssetCooker(NativeTextureFormatSupport formatSupport);

    [[nodiscard]] const NativeTextureFormatSupport& textureFormatSupport() const { return textureFormatSupport_; }

    [[nodiscard]] NativeAssetCookResult cookMesh(
        const NativeAssetCookInput& input,
        const MeshAsset& mesh,
        const std::vector<AssetGuid>& materialGuids,
        bool buildLocalBvh = true) const;

    [[nodiscard]] NativeAssetCookResult cookMaterial(
        const NativeAssetCookInput& input,
        const MaterialAsset& material,
        const std::vector<AssetGuid>& textureGuids) const;

    [[nodiscard]] NativeAssetCookResult cookTexture(
        const NativeAssetCookInput& input,
        const TextureAsset& texture,
        std::string_view role) const;

    [[nodiscard]] NativeAssetCookResult cookAnimationController(
        const NativeAssetCookInput& input,
        const AnimationController& controller) const;

    [[nodiscard]] NativeAssetCookResult cookSkeletalMeshBinding(
        const NativeAssetCookInput& input,
        const AssetGuid& meshGuid,
        const AssetGuid& skeletonGuid,
        const std::vector<uint32_t>& jointRemap,
        const nlohmann::json& bindMetadata) const;

    [[nodiscard]] NativeAssetCookResult cookMetadataPayload(
        const NativeAssetCookInput& input,
        NativeAssetKind kind,
        const nlohmann::json& metadata) const;

private:
    NativeTextureFormatSupport textureFormatSupport_;
};

[[nodiscard]] nlohmann::json nativeCookRuntimePayloadJson(
    const NativeAssetCookResult& result,
    NativeAssetKind kind,
    const AssetGuid& guid,
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& sourcePath,
    const std::string& sourceHash,
    const std::string& importSettingsHash);

} // namespace rtv
