#pragma once

#include "rtv/AnimationClip.h"
#include "rtv/AnimationController.h"
#include "rtv/AssetManager.h"
#include "rtv/GpuUploadTicket.h"
#include "rtv/NativeBinaryIO.h"
#include "rtv/NativeTextureFormatPolicy.h"
#include "rtv/RuntimeSkeleton.h"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtv {

struct NativeRuntimeLoadOptions {
    NativeTextureFormatSupport textureFormatSupport = nativeTextureOfflineFallbackFormatSupport();
    bool rejectUnsupportedTextureFormats = true;
    bool validatePayloadHashes = true;
    bool retainLoadedPayloadsInReport = true;
    uint64_t eagerCpuLoadWarningBytes = 128ull * 1024ull * 1024ull;
    uint64_t eagerCpuLoadHardLimitBytes = 2ull * 1024ull * 1024ull * 1024ull;
    bool allowLargeEagerCpuLoad = false;
    std::vector<std::filesystem::path> looseFileAllowList;
};

struct NativeRuntimeTextureBinding {
    uint32_t slot = 0;
    std::string slotName;
    std::string textureGuid;
    uint32_t cookedTextureIndex = UINT32_MAX;
    TextureAssetHandle textureHandle{};
    std::string nativeSource;
    std::filesystem::path nativePath;
    bool resolved = false;
    bool resident = false;
    bool fallback = false;
    bool missing = false;
    std::string repairAction;
};

struct NativeRuntimeSkeletalMeshBinding {
    std::string meshGuid;
    std::string skeletonGuid;
    uint32_t jointRemapCount = 0;
    uint32_t jointRemapChunk = 0;
    uint32_t skinningDataChunk = 0;
    uint32_t bindMetadataChunk = 0;
    uint32_t flags = 0;
    bool hasJointRemap = false;
    bool hasSkinningData = false;
    bool hasBindMetadata = false;
};

struct NativeRuntimeAnimationClipBinding {
    std::string source;
    std::string clipGuid;
    std::filesystem::path clipPath;
    std::filesystem::path resolvedNativePath;
    bool resolved = false;
};

struct NativeRuntimeAnimationMetadataBridge {
    bool available = false;
    std::string schema;
    std::string sourceFormat;
    std::string name;
    double durationSeconds = 0.0;
    uint32_t channelCount = 0;
    uint32_t decodedChannelCount = 0;
    uint32_t decodedKeyframeCount = 0;
    uint32_t trackCount = 0;
    uint32_t eventCount = 0;
    uint32_t rootMotionCandidateCount = 0;
    std::string runtimeSupport;
};

struct NativeRuntimeLoadedAsset {
    bool ok = false;
    NativeAssetKind kind = NativeAssetKind::Unknown;
    std::string guid;
    std::filesystem::path path;
    MeshAsset mesh;
    MaterialAsset material;
    TextureAsset texture;
    RuntimeSkeleton skeleton;
    AnimationClip animationClip;
    NativeRuntimeAnimationMetadataBridge animationMetadataBridge;
    AnimationController animationController;
    NativeRuntimeSkeletalMeshBinding skeletalMeshBinding;
    MeshAssetHandle meshHandle{};
    MaterialAssetHandle materialHandle{};
    TextureAssetHandle textureHandle{};
    VkFormat texturePayloadFormat = VK_FORMAT_UNDEFINED;
    NativeTextureRole textureRole = NativeTextureRole::Unknown;
    bool texturePayloadFormatSupported = true;
    bool textureVariantCandidate = false;
    bool textureVariantSelected = false;
    uint32_t textureVariantCandidateCount = 0;
    std::string textureVariantSelectionReason;
    std::string textureFormatSupportPlatform;
    std::string texturePayloadVariantPlanJson;
    std::string textureSourceContainerKind;
    bool textureSourceContainerPreserved = false;
    bool textureSourceContainerTranscoded = false;
    bool texturePackageVariantSidecarEligible = false;
    bool texturePackageVariantSidecarImplemented = false;
    std::string texturePackageVariantSidecarPolicy;
    std::vector<std::string> texturePackageVariantMissingRequirements;
    std::vector<std::string> dependencyGuids;
    std::vector<NativeRuntimeTextureBinding> materialTextureBindings;
    std::vector<NativeRuntimeAnimationClipBinding> animationClipBindings;
    std::vector<std::string> warnings;
    std::vector<NativeBinaryError> errors;
};

struct NativeRuntimeRendererUploadPlan {
    bool available = false;
    bool assetManagerBacked = false;
    bool packageBacked = false;
    uint32_t textureCount = 0;
    uint32_t textureResidentCount = 0;
    uint32_t materialCount = 0;
    uint32_t meshCount = 0;
    uint64_t textureUploadBytes = 0;
    uint64_t vertexUploadBytes = 0;
    uint64_t indexUploadBytes = 0;
};

struct NativeRuntimeDirectStoreUploadTicketPlan {
    std::string guid;
    NativeAssetKind kind = NativeAssetKind::Unknown;
    std::string source;
    std::filesystem::path nativePath;
    std::string packageObjectPath;
    uint64_t generation = 0;
    std::string resourceKind;
    std::string label;
    uint64_t uploadBytes = 0;
    bool uploadReady = false;
    std::string unavailableReason;
};

struct NativeRuntimeDirectStoreUploadPlan {
    bool available = false;
    bool executable = false;
    bool assetManagerBypass = true;
    bool packageBacked = false;
    bool looseBacked = false;
    uint32_t textureTicketCount = 0;
    uint32_t meshBufferTicketCount = 0;
    uint64_t textureUploadBytes = 0;
    uint64_t vertexUploadBytes = 0;
    uint64_t indexUploadBytes = 0;
    std::string policy;
    std::string unavailableReason;
    std::vector<std::string> readinessRequirements;
    std::vector<std::string> missingRequirements;
    std::vector<NativeRuntimeDirectStoreUploadTicketPlan> tickets;
    bool uploadTicketQueueSimulationAvailable = false;
    bool uploadTicketQueueSimulationExecutable = false;
    uint64_t uploadTicketQueueFrameBudgetBytes = 0;
    uint32_t uploadTicketQueueFrameCount = 0;
    uint64_t uploadTicketQueueSubmittedBytes = 0;
    uint64_t uploadTicketQueueCompletedBytes = 0;
    uint64_t uploadTicketQueueNextTimelineValue = 0;
    std::string uploadTicketQueuePolicy;
    std::vector<GpuUploadTicketSnapshot> uploadTicketQueueSnapshots;
};

struct NativeRuntimeSceneAssetPlan {
    bool available = false;
    bool assetManagerBacked = false;
    bool packageBacked = false;
    bool rendererPlaceable = false;
    uint32_t textureCount = 0;
    uint32_t materialCount = 0;
    uint32_t meshCount = 0;
    uint32_t skinCount = 0;
    uint32_t skinnedNodeCount = 0;
    uint32_t nodeCount = 0;
    uint32_t rootNodeCount = 0;
    uint32_t missingMeshHandleCount = 0;
    bool boundsAvailable = false;
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
};

struct NativeRuntimeLegacyCpuLoadPolicyReport {
    bool available = false;
    bool assetManagerBacked = false;
    bool packageBacked = false;
    bool looseBacked = false;
    uint64_t estimatedEagerCpuBytes = 0;
    uint64_t warningThresholdBytes = 0;
    uint64_t hardLimitBytes = 0;
    bool allowLargeEagerCpuLoad = false;
    bool largeEagerLoadWarning = false;
    bool hardLimitExceeded = false;
    bool streamingRecommended = false;
    std::string policy;
    std::string recommendedAction;
};

struct NativeRuntimeLoadReport {
    bool ok = false;
    std::filesystem::path sourceRoot;
    uint32_t textureCount = 0;
    uint32_t materialCount = 0;
    uint32_t meshCount = 0;
    uint32_t skeletonCount = 0;
    uint32_t animationCount = 0;
    uint32_t animationControllerCount = 0;
    uint32_t skeletalMeshCount = 0;
    uint64_t totalTextureBytes = 0;
    uint64_t totalVertexCount = 0;
    uint64_t totalIndexCount = 0;
    NativeRuntimeRendererUploadPlan rendererUploadPlan;
    NativeRuntimeDirectStoreUploadPlan directStoreUploadPlan;
    NativeRuntimeSceneAssetPlan sceneAssetPlan;
    NativeRuntimeLegacyCpuLoadPolicyReport legacyCpuLoadPolicy;
    SceneAsset sceneAsset;
    NativeRuntimeLoadOptions options;
    std::vector<NativeRuntimeLoadedAsset> assets;
    std::vector<std::string> warnings;
    std::vector<NativeBinaryError> errors;
};

class NativeAssetRuntimeLoader {
public:
    [[nodiscard]] NativeRuntimeLoadedAsset loadStandalone(const std::filesystem::path& path, const NativeRuntimeLoadOptions& options = {}) const;
    [[nodiscard]] NativeRuntimeLoadedAsset loadBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, const NativeRuntimeLoadOptions& options = {}) const;
    [[nodiscard]] NativeRuntimeLoadReport loadLooseRoot(const std::filesystem::path& root, AssetManager* manager = nullptr, const NativeRuntimeLoadOptions& options = {}) const;
};

[[nodiscard]] nlohmann::json nativeRuntimeLoadedAssetToJson(const NativeRuntimeLoadedAsset& asset);
[[nodiscard]] SceneAsset buildNativeRuntimeSceneAsset(const NativeRuntimeLoadReport& report, const std::filesystem::path& sourcePath = {});
[[nodiscard]] nlohmann::json nativeRuntimeLoadReportToJson(const NativeRuntimeLoadReport& report);
[[nodiscard]] int loadNativeRuntimeAssetsCommand(const std::filesystem::path& input, const std::filesystem::path& jsonOut = {}, const NativeTextureFormatSupport& textureFormatSupport = nativeTextureOfflineFallbackFormatSupport());

} // namespace rtv
