#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rtv {

inline constexpr uint32_t kNativeAssetMagicRtmesh = 0x48535452u; // RTSH
inline constexpr uint32_t kNativeAssetMagicRtmaterial = 0x544d5452u; // RTMT
inline constexpr uint32_t kNativeAssetMagicRttexture = 0x54585452u; // RTXT
inline constexpr uint32_t kNativeAssetMagicRtskeleton = 0x4b535452u; // RTSK
inline constexpr uint32_t kNativeAssetMagicRtanim = 0x414e5452u; // RTNA
inline constexpr uint32_t kNativeAssetMagicRtanimController = 0x43415452u; // RTAC
inline constexpr uint32_t kNativeAssetMagicRtskeletalMesh = 0x4d535452u; // RTSM
inline constexpr uint32_t kNativeAssetMagicRtpkg = 0x474b5052u; // RPKG
inline constexpr uint32_t kNativeAssetEndianMarker = 0x04030201u;
inline constexpr uint32_t kNativeAssetHeaderVersion = 1u;
inline constexpr uint32_t kNativeAssetReadableVersionMin = 1u;
inline constexpr uint32_t kNativeAssetReadableVersionMax = 1u;
inline constexpr uint64_t kNativeAssetAlignment = 16u;

enum class NativeAssetKind : uint32_t {
    Unknown = 0,
    Mesh = 1,
    Material = 2,
    Texture = 3,
    Skeleton = 4,
    Animation = 5,
    AnimationController = 6,
    SkeletalMesh = 7,
    Package = 100,
};

enum class NativeChunkCompression : uint32_t {
    None = 0,
    ReservedZstd = 1,
    ReservedGDeflate = 2,
};

enum class NativeDependencyFlags : uint32_t {
    Required = 1u << 0u,
    Optional = 1u << 1u,
    Runtime = 1u << 2u,
    EditorOnly = 1u << 3u,
};

enum class NativeBinaryErrorCode : uint32_t {
    None = 0,
    UnsupportedVersion,
    CorruptHeader,
    CorruptTable,
    MissingDependency,
    HashMismatch,
    MigrationRequired,
    MigrationFailed,
    UnsupportedPlatformFeature,
    IoError,
};

enum class RtmaterialTextureSlot : uint32_t {
    BaseColor = 0,
    Normal = 1,
    MetallicRoughness = 2,
    Occlusion = 3,
    Emissive = 4,
    Transmission = 5,
    Clearcoat = 6,
    ClearcoatRoughness = 7,
    SheenColor = 8,
    SheenRoughness = 9,
    Specular = 10,
    SpecularColor = 11,
    Iridescence = 12,
    ClearcoatNormal = 13,
    VolumeThickness = 14,
    IridescenceThickness = 15,
    Anisotropy = 16,
    Opacity = 17,
    Height = 18,
};

struct alignas(16) NativeAssetHeader {
    uint32_t magic = 0;
    uint32_t headerVersion = kNativeAssetHeaderVersion;
    uint32_t assetKind = static_cast<uint32_t>(NativeAssetKind::Unknown);
    uint32_t endianMarker = kNativeAssetEndianMarker;
    uint32_t headerSize = sizeof(NativeAssetHeader);
    uint32_t contentVersion = 1;
    uint32_t minimumReaderVersion = kNativeAssetReadableVersionMin;
    uint32_t flags = 0;
    std::array<uint8_t, 16> assetGuid{};
    std::array<uint8_t, 32> sourceHash{};
    std::array<uint8_t, 32> importSettingsHash{};
    std::array<uint8_t, 32> payloadHash{};
    uint64_t objectTableOffset = 0;
    uint32_t objectTableCount = 0;
    uint32_t objectTableStride = 0;
    uint64_t chunkTableOffset = 0;
    uint32_t chunkTableCount = 0;
    uint32_t chunkTableStride = 0;
    uint64_t dependencyTableOffset = 0;
    uint32_t dependencyTableCount = 0;
    uint32_t dependencyTableStride = 0;
    uint64_t debugDirectoryOffset = 0;
    uint64_t debugDirectorySize = 0;
    uint64_t migrationTableOffset = 0;
    uint32_t migrationTableCount = 0;
    uint32_t migrationTableStride = 0;
    uint64_t fileSize = 0;
    std::array<uint8_t, 24> reserved{};
};

struct alignas(16) NativeObjectRecord {
    std::array<uint8_t, 16> objectGuid{};
    uint32_t assetKind = 0;
    uint32_t contentVersion = 1;
    uint32_t firstChunk = 0;
    uint32_t chunkCount = 0;
    uint32_t firstDependency = 0;
    uint32_t dependencyCount = 0;
    uint32_t debugNameOffset = 0;
    uint32_t debugNameSize = 0;
    uint32_t flags = 0;
    uint32_t reserved0 = 0;
    std::array<float, 6> boundsMinMax{};
    std::array<uint8_t, 48> reserved{};
};

struct alignas(16) NativeChunkRecord {
    uint32_t type = 0;
    uint32_t compression = static_cast<uint32_t>(NativeChunkCompression::None);
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t uncompressedSize = 0;
    std::array<uint8_t, 32> payloadHash{};
    uint32_t flags = 0;
    uint32_t reserved0 = 0;
    uint64_t reserved1 = 0;
};

struct alignas(16) NativeDependencyRecord {
    std::array<uint8_t, 16> dependencyGuid{};
    uint32_t assetKind = 0;
    uint32_t flags = static_cast<uint32_t>(NativeDependencyFlags::Required);
    uint32_t debugNameOffset = 0;
    uint32_t debugNameSize = 0;
    std::array<uint8_t, 16> reserved{};
};

struct alignas(16) NativeDebugRecord {
    uint32_t type = 0;
    uint32_t keyOffset = 0;
    uint32_t keySize = 0;
    uint32_t valueOffset = 0;
    uint32_t valueSize = 0;
    uint32_t flags = 0;
    uint64_t reserved = 0;
};

struct alignas(16) NativeMigrationRecord {
    uint32_t fromVersion = 0;
    uint32_t toVersion = 0;
    uint64_t migrationDataOffset = 0;
    uint64_t migrationDataSize = 0;
    uint32_t flags = 0;
    uint32_t reserved0 = 0;
    uint64_t reserved1 = 0;
};

inline constexpr uint32_t kRtmeshPayloadVersion = 1u;
inline constexpr uint32_t kRtmaterialPayloadVersion = 1u;
inline constexpr uint32_t kRttexturePayloadVersion = 1u;
inline constexpr uint32_t kRtskeletonPayloadVersion = 1u;
inline constexpr uint32_t kRtanimPayloadVersion = 1u;
inline constexpr uint32_t kRtanimControllerPayloadVersion = 1u;
inline constexpr uint32_t kRtskeletalMeshPayloadVersion = 1u;
inline constexpr uint32_t kRtmaterialTextureTransformCount = 17u;

struct alignas(16) RtmeshPayloadHeader {
    uint32_t version = kRtmeshPayloadVersion;
    uint32_t vertexStride = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t primitiveRangeCount = 0;
    uint32_t materialSlotCount = 0;
    uint32_t morphTargetCount = 0;
    uint32_t flags = 0;
    uint32_t vertexChunk = 0;
    uint32_t indexChunk = 0;
    uint32_t primitiveRangeChunk = 0;
    uint32_t materialSlotChunk = 0;
    uint32_t morphTargetChunk = 0;
    uint32_t reserved0 = 0;
    std::array<float, 6> boundsMinMax{};
};

struct alignas(16) RtmaterialPayloadHeader {
    uint32_t version = kRtmaterialPayloadVersion;
    uint32_t flags = 0;
    uint32_t textureSlotCount = 0;
    uint32_t textureSlotChunk = 0;
    std::array<float, 4> baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> emissiveFactor{};
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float alphaCutoff = 0.5f;
    float occlusionStrength = 1.0f;
    uint32_t alphaMode = 0;
    uint32_t shaderCompatibilityMask = 0;
    uint64_t reserved = 0;
};

struct alignas(16) RtmaterialTextureTransformRecord {
    std::array<float, 2> offset{0.0f, 0.0f};
    std::array<float, 2> scale{1.0f, 1.0f};
    float rotation = 0.0f;
    uint32_t enabled = 0;
    uint32_t texCoord = 0;
    uint32_t reserved = 0;
};

enum class NativeTextureColorSpace : uint32_t {
    Linear = 0,
    Srgb = 1,
    SourceDefined = 2,
    HdrLinear = 3,
};

enum class NativeTextureRole : uint32_t {
    Unknown = 0,
    BaseColor = 1,
    Normal = 2,
    MetallicRoughness = 3,
    Metallic = 4,
    Roughness = 5,
    Occlusion = 6,
    Emissive = 7,
    Opacity = 8,
    Height = 9,
    EnvironmentHdr = 10,
    Data = 11,
    Specular = 12,
    SpecularColor = 13,
    Transmission = 14,
    Clearcoat = 15,
    ClearcoatRoughness = 16,
    ClearcoatNormal = 17,
    Sheen = 18,
    SheenColor = 19,
    SheenRoughness = 20,
    Iridescence = 21,
    IridescenceThickness = 22,
    Anisotropy = 23,
    Thickness = 24,
};

enum class NativeTextureCompressionPolicy : uint32_t {
    None = 0,
    PreserveSourceContainer = 1,
    DecodedRgba8 = 2,
    DecodedHdr = 3,
};

struct alignas(16) RttexturePayloadHeader {
    uint32_t version = kRttexturePayloadVersion;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t mipCount = 1;
    uint32_t arrayLayers = 1;
    uint32_t vkFormat = 0;
    uint32_t colorSpace = 0;
    uint32_t role = 0;
    uint32_t sampler = 0;
    uint32_t compression = 0;
    uint32_t mipTableChunk = 0;
    uint32_t payloadChunk = 0;
    uint32_t flags = 0;
    uint64_t reserved = 0;
};

struct alignas(16) RtskeletonPayloadHeader {
    uint32_t version = kRtskeletonPayloadVersion;
    uint32_t jointCount = 0;
    uint32_t hierarchyChunk = 0;
    uint32_t inverseBindMatrixChunk = 0;
    uint32_t bindPoseChunk = 0;
    uint32_t namesChunk = 0;
    uint32_t flags = 0;
    uint32_t reserved0 = 0;
    std::array<uint8_t, 16> reserved{};
};

struct alignas(16) RtanimPayloadHeader {
    uint32_t version = kRtanimPayloadVersion;
    uint32_t trackCount = 0;
    uint32_t keyframeCount = 0;
    uint32_t eventCount = 0;
    float durationSeconds = 0.0f;
    uint32_t trackChunk = 0;
    uint32_t keyframeChunk = 0;
    uint32_t eventChunk = 0;
    uint32_t rootMotionChunk = 0;
    uint32_t flags = 0;
    uint64_t reserved = 0;
};

struct alignas(16) RtanimControllerPayloadHeader {
    uint32_t version = kRtanimControllerPayloadVersion;
    uint32_t parameterCount = 0;
    uint32_t stateCount = 0;
    uint32_t transitionCount = 0;
    uint32_t layerCount = 0;
    uint32_t parameterChunk = 0;
    uint32_t stateChunk = 0;
    uint32_t transitionChunk = 0;
    uint32_t blendTreeChunk = 0;
    uint32_t eventRouteChunk = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

inline constexpr uint32_t kRtanimControllerChunkPayloadHeader = 1u;
inline constexpr uint32_t kRtanimControllerChunkStringTable = 40u;
inline constexpr uint32_t kRtanimControllerChunkParameters = 41u;
inline constexpr uint32_t kRtanimControllerChunkStates = 42u;
inline constexpr uint32_t kRtanimControllerChunkTransitions = 43u;
inline constexpr uint32_t kRtanimControllerChunkConditions = 44u;
inline constexpr uint32_t kRtanimControllerChunkEvents = 45u;
inline constexpr uint32_t kRtanimControllerChunkBlendTrees = 46u;
inline constexpr uint32_t kRtanimControllerChunkBlendTreeChildren = 47u;
inline constexpr uint32_t kRtanimControllerChunkLayers = 48u;
inline constexpr uint32_t kRtanimControllerChunkAvatarMasks = 49u;
inline constexpr uint32_t kRtanimControllerChunkAvatarMaskJoints = 50u;
inline constexpr uint32_t kRtanimControllerChunkMetadataJson = 100u;

inline constexpr uint32_t kRtskeletalMeshChunkPayloadHeader = 1u;
inline constexpr uint32_t kRtskeletalMeshChunkJointRemap = 60u;
inline constexpr uint32_t kRtskeletalMeshChunkSkinningData = 61u;
inline constexpr uint32_t kRtskeletalMeshChunkBindMetadataJson = 100u;

struct alignas(16) RtanimControllerParameterRecord {
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    uint32_t type = 0;
    uint32_t boolValue = 0;
    int32_t intValue = 0;
    float floatValue = 0.0f;
    uint32_t triggerValue = 0;
    uint32_t flags = 0;
};

struct alignas(16) RtanimControllerStateRecord {
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    std::array<uint8_t, 16> clipGuid{};
    uint32_t clipPathOffset = 0;
    uint32_t clipPathSize = 0;
    float speed = 1.0f;
    uint32_t loop = 1;
    uint32_t defaultState = 0;
    uint32_t firstTransition = 0;
    uint32_t transitionCount = 0;
    uint32_t firstEvent = 0;
    uint32_t eventCount = 0;
    uint32_t blendTreeIndex = UINT32_MAX;
    uint32_t flags = 0;
};

struct alignas(16) RtanimControllerTransitionRecord {
    uint32_t toOffset = 0;
    uint32_t toSize = 0;
    float exitTimeSeconds = 0.0f;
    uint32_t firstCondition = 0;
    uint32_t conditionCount = 0;
    uint32_t firstEvent = 0;
    uint32_t eventCount = 0;
    uint32_t flags = 0;
};

struct alignas(16) RtanimControllerConditionRecord {
    uint32_t parameterOffset = 0;
    uint32_t parameterSize = 0;
    uint32_t opOffset = 0;
    uint32_t opSize = 0;
    uint32_t type = 0;
    uint32_t boolValue = 0;
    int32_t intValue = 0;
    float floatValue = 0.0f;
    uint32_t triggerValue = 0;
    uint32_t flags = 0;
    uint64_t reserved = 0;
};

struct alignas(16) RtanimControllerEventRecord {
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    uint32_t payloadOffset = 0;
    uint32_t payloadSize = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
    uint64_t reserved1 = 0;
};

struct alignas(16) RtanimControllerBlendTreeRecord {
    uint32_t stateIndex = 0;
    uint32_t typeOffset = 0;
    uint32_t typeSize = 0;
    uint32_t parameterOffset = 0;
    uint32_t parameterSize = 0;
    uint32_t firstChild = 0;
    uint32_t childCount = 0;
    uint32_t flags = 0;
};

struct alignas(16) RtanimControllerBlendTreeChildRecord {
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    std::array<uint8_t, 16> clipGuid{};
    uint32_t clipPathOffset = 0;
    uint32_t clipPathSize = 0;
    float threshold = 0.0f;
    uint32_t flags = 0;
    uint64_t reserved = 0;
};

struct alignas(16) RtanimControllerLayerRecord {
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    std::array<uint8_t, 16> clipGuid{};
    uint32_t clipPathOffset = 0;
    uint32_t clipPathSize = 0;
    float weight = 1.0f;
    uint32_t additive = 0;
    uint32_t maskOffset = 0;
    uint32_t maskSize = 0;
    uint64_t reserved = 0;
};

struct alignas(16) RtanimControllerAvatarMaskRecord {
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    uint32_t firstIncludedJoint = 0;
    uint32_t includedJointCount = 0;
    uint32_t firstExcludedJoint = 0;
    uint32_t excludedJointCount = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

struct alignas(16) RtanimControllerAvatarMaskJointRecord {
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

struct alignas(16) RtskeletalMeshPayloadHeader {
    uint32_t version = kRtskeletalMeshPayloadVersion;
    std::array<uint8_t, 16> meshGuid{};
    std::array<uint8_t, 16> skeletonGuid{};
    uint32_t jointRemapCount = 0;
    uint32_t jointRemapChunk = 0;
    uint32_t skinningDataChunk = 0;
    uint32_t bindMetadataChunk = 0;
    uint32_t flags = 0;
    uint32_t reserved0 = 0;
    std::array<uint8_t, 16> reserved{};
};

static_assert(alignof(NativeAssetHeader) == 16);
static_assert(sizeof(NativeAssetHeader) == 256);
static_assert(sizeof(NativeObjectRecord) == 128);
static_assert(sizeof(NativeChunkRecord) == 80);
static_assert(sizeof(NativeDependencyRecord) == 48);
static_assert(sizeof(NativeDebugRecord) == 32);
static_assert(sizeof(NativeMigrationRecord) == 48);
static_assert(sizeof(RtmeshPayloadHeader) == 80);
static_assert(sizeof(RtmaterialPayloadHeader) == 80);
static_assert(sizeof(RttexturePayloadHeader) == 64);
static_assert(sizeof(RtskeletonPayloadHeader) == 48);
static_assert(sizeof(RtanimPayloadHeader) == 48);
static_assert(sizeof(RtanimControllerPayloadHeader) == 48);
static_assert(sizeof(RtanimControllerParameterRecord) == 32);
static_assert(sizeof(RtanimControllerStateRecord) == 80);
static_assert(sizeof(RtanimControllerTransitionRecord) == 32);
static_assert(sizeof(RtanimControllerConditionRecord) == 48);
static_assert(sizeof(RtanimControllerEventRecord) == 32);
static_assert(sizeof(RtanimControllerBlendTreeRecord) == 32);
static_assert(sizeof(RtanimControllerBlendTreeChildRecord) == 48);
static_assert(sizeof(RtanimControllerLayerRecord) == 64);
static_assert(sizeof(RtanimControllerAvatarMaskRecord) == 32);
static_assert(sizeof(RtanimControllerAvatarMaskJointRecord) == 16);
static_assert(sizeof(RtskeletalMeshPayloadHeader) == 80);
static_assert(offsetof(NativeAssetHeader, objectTableOffset) == 144);
static_assert(offsetof(NativeAssetHeader, fileSize) == 224);

} // namespace rtv
