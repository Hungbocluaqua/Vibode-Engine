#include "rtv/NativeAssetRuntimeLoader.h"

#include "rtv/NativeAssetStore.h"
#include "rtv/NativeTextureFormatPolicy.h"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace rtv {
namespace {

enum NativeRuntimeChunkType : uint32_t {
    ChunkPayloadHeader = 1,
    ChunkMeshVertices = 10,
    ChunkMeshIndices = 11,
    ChunkMeshPrimitives = 12,
    ChunkMeshMaterialSlots = 13,
    ChunkMeshLocalBvhNodes = 14,
    ChunkMeshLocalBvhTriangles = 15,
    ChunkMaterialTextureSlots = 20,
    ChunkMaterialTextureTransforms = 21,
    ChunkTextureMipTable = 30,
    ChunkTexturePayload = 31,
    ChunkMetadataJson = 100,
};

constexpr char kNativeTexturePayloadVariantPlanDebugKey[] = {'n', 'a', 't', 'i', 'v', 'e', 'T', 'e', 'x', 't', 'u', 'r', 'e', 'P', 'a', 'y', 'l', 'o', 'a', 'd', 'V', 'a', 'r', 'i', 'a', 'n', 't', 'P', 'l', 'a', 'n', '\0'};

struct RtmeshPrimitiveRecord {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t materialSlot = UINT32_MAX;
    uint32_t alphaMode = 0;
    uint32_t flags = 0;
    float alphaCutoff = 0.5f;
};

struct RtmaterialTextureSlotRecord {
    uint32_t slot = 0;
    std::array<uint8_t, 16> textureGuid{};
    uint32_t textureIndex = UINT32_MAX;
    uint32_t flags = 0;
};

TextureTransformAsset textureTransformAsset(const RtmaterialTextureTransformRecord& record) {
    TextureTransformAsset transform;
    transform.offset = glm::vec2{record.offset[0], record.offset[1]};
    transform.scale = glm::vec2{record.scale[0], record.scale[1]};
    transform.rotation = record.rotation;
    transform.enabled = record.enabled;
    transform.texCoord = record.texCoord;
    return transform;
}

void assignMaterialTextureTransform(MaterialAsset& material, uint32_t slot, const RtmaterialTextureTransformRecord& record) {
    const TextureTransformAsset transform = textureTransformAsset(record);
    switch (slot) {
    case 0u: material.baseColorTextureTransform = transform; break;
    case 1u: material.normalTextureTransform = transform; break;
    case 2u: material.metallicRoughnessTextureTransform = transform; break;
    case 3u: material.emissiveTextureTransform = transform; break;
    case 4u: material.occlusionTextureTransform = transform; break;
    case 5u: material.sheenColorTextureTransform = transform; break;
    case 6u: material.sheenRoughnessTextureTransform = transform; break;
    case 7u: material.iridescenceTextureTransform = transform; break;
    case 8u: material.iridescenceThicknessTextureTransform = transform; break;
    case 9u: material.clearcoatTextureTransform = transform; break;
    case 10u: material.clearcoatRoughnessTextureTransform = transform; break;
    case 11u: material.clearcoatNormalTextureTransform = transform; break;
    case 12u: material.transmissionTextureTransform = transform; break;
    case 13u: material.volumeThicknessTextureTransform = transform; break;
    case 14u: material.specularTextureTransform = transform; break;
    case 15u: material.specularColorTextureTransform = transform; break;
    case 16u: material.anisotropyTextureTransform = transform; break;
    default: break;
    }
}

struct RttextureMipRecord {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

bool nativeTextureBlockCompressedFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

NativeBinaryError makeLoaderError(
    NativeBinaryErrorCode code,
    const std::filesystem::path& path,
    std::string table,
    uint64_t offset,
    uint64_t expectedSize,
    std::string message) {
    NativeBinaryError error;
    error.code = code;
    error.path = path;
    error.table = std::move(table);
    error.offset = offset;
    error.expectedSize = expectedSize;
    error.message = std::move(message);
    return error;
}

bool readFileBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes, NativeBinaryError* error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (error) *error = makeLoaderError(NativeBinaryErrorCode::IoError, path, "file", 0, 0, "Could not open native runtime asset file");
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        if (error) *error = makeLoaderError(NativeBinaryErrorCode::IoError, path, "file", 0, 0, "Could not determine native runtime asset file size");
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!file.good() && size != 0) {
        if (error) *error = makeLoaderError(NativeBinaryErrorCode::IoError, path, "file", 0, static_cast<uint64_t>(size), "Could not read complete native runtime asset file");
        return false;
    }
    return true;
}

bool rangeInside(uint64_t offset, uint64_t size, uint64_t fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
}

const NativeChunkRecord* findChunk(const NativeAssetInspection& inspection, uint32_t type) {
    const auto it = std::find_if(inspection.chunks.begin(), inspection.chunks.end(), [&](const NativeChunkRecord& chunk) {
        return chunk.type == type;
    });
    return it == inspection.chunks.end() ? nullptr : &*it;
}

std::string nativeDebugDirectoryString(const NativeAssetInspection& inspection, uint32_t offset, uint32_t size) {
    if (size == 0 || offset >= inspection.debugDirectory.size()) {
        return {};
    }
    const uint64_t available = std::min<uint64_t>(size, inspection.debugDirectory.size() - offset);
    return std::string(
        reinterpret_cast<const char*>(inspection.debugDirectory.data() + offset),
        reinterpret_cast<const char*>(inspection.debugDirectory.data() + offset + available));
}

std::string nativeDebugRecordValue(const NativeAssetInspection& inspection, const char* key) {
    for (const NativeDebugRecord& record : inspection.debugRecords) {
        if (nativeDebugDirectoryString(inspection, record.keyOffset, record.keySize) == key) {
            return nativeDebugDirectoryString(inspection, record.valueOffset, record.valueSize);
        }
    }
    return {};
}

uint32_t nativeDebugRecordUint32(const NativeAssetInspection& inspection, const char* key, uint32_t fallback) {
    const std::string value = nativeDebugRecordValue(inspection, key);
    if (value.empty()) {
        return fallback;
    }
    try {
        return static_cast<uint32_t>(std::stoul(value));
    } catch (...) {
        return fallback;
    }
}

template <typename T>
bool readPodChunk(const std::vector<std::byte>& bytes, const NativeChunkRecord* chunk, T& out) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (chunk == nullptr || chunk->size < sizeof(T) || !rangeInside(chunk->offset, sizeof(T), bytes.size())) {
        return false;
    }
    std::memcpy(&out, bytes.data() + chunk->offset, sizeof(T));
    return true;
}

template <typename T>
bool readVectorChunk(const std::vector<std::byte>& bytes, const NativeChunkRecord* chunk, std::vector<T>& out, size_t expectedCount = SIZE_MAX) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (chunk == nullptr || chunk->size % sizeof(T) != 0 || !rangeInside(chunk->offset, chunk->size, bytes.size())) {
        return false;
    }
    const size_t count = static_cast<size_t>(chunk->size / sizeof(T));
    if (expectedCount != SIZE_MAX && count != expectedCount) {
        return false;
    }
    out.resize(count);
    if (!out.empty()) {
        std::memcpy(out.data(), bytes.data() + chunk->offset, static_cast<size_t>(chunk->size));
    }
    return true;
}

bool readByteChunk(const std::vector<std::byte>& bytes, const NativeChunkRecord* chunk, std::vector<uint8_t>& out) {
    if (chunk == nullptr || !rangeInside(chunk->offset, chunk->size, bytes.size())) {
        return false;
    }
    out.resize(static_cast<size_t>(chunk->size));
    if (!out.empty()) {
        std::memcpy(out.data(), bytes.data() + chunk->offset, out.size());
    }
    return true;
}

bool readJsonChunk(const std::vector<std::byte>& bytes, const NativeChunkRecord* chunk, nlohmann::json& out) {
    if (chunk == nullptr || !rangeInside(chunk->offset, chunk->size, bytes.size())) {
        return false;
    }
    const auto* first = reinterpret_cast<const char*>(bytes.data() + chunk->offset);
    const auto* last = first + chunk->size;
    try {
        out = nlohmann::json::parse(first, last);
        return out.is_object();
    } catch (const std::exception&) {
        out = nlohmann::json::object();
        return false;
    }
}

glm::vec3 jsonVec3Or(const nlohmann::json& value, glm::vec3 fallback) {
    if (!value.is_array() || value.size() < 3) {
        return fallback;
    }
    return glm::vec3{
        value[0].get<float>(),
        value[1].get<float>(),
        value[2].get<float>(),
    };
}

void applyMaterialSemanticMetadata(MaterialAsset& material, const nlohmann::json& metadata) {
    if (!metadata.is_object()) {
        return;
    }
    material.nestedPriority = metadata.value("nestedPriority", material.nestedPriority);
    if (const auto it = metadata.find("ior"); it != metadata.end() && it->is_object()) {
        material.hasIor = it->value("present", material.hasIor != 0u) ? 1u : 0u;
        material.iorFactor = it->value("factor", material.iorFactor);
    }
    if (const auto it = metadata.find("clearcoat"); it != metadata.end() && it->is_object()) {
        material.hasClearcoat = it->value("present", material.hasClearcoat != 0u) ? 1u : 0u;
        material.clearcoatFactor = it->value("factor", material.clearcoatFactor);
        material.clearcoatRoughnessFactor = it->value("roughnessFactor", material.clearcoatRoughnessFactor);
    }
    if (const auto it = metadata.find("transmission"); it != metadata.end() && it->is_object()) {
        material.hasTransmission = it->value("present", material.hasTransmission != 0u) ? 1u : 0u;
        material.transmissionFactor = it->value("factor", material.transmissionFactor);
    }
    if (const auto it = metadata.find("volume"); it != metadata.end() && it->is_object()) {
        material.hasVolume = it->value("present", material.hasVolume != 0u) ? 1u : 0u;
        material.volumeThicknessFactor = it->value("thicknessFactor", material.volumeThicknessFactor);
        material.volumeAttenuationDistance = it->value("attenuationDistance", material.volumeAttenuationDistance);
        if (const auto color = it->find("attenuationColor"); color != it->end()) {
            material.volumeAttenuationColor = jsonVec3Or(*color, material.volumeAttenuationColor);
        }
    }
    if (const auto it = metadata.find("dispersion"); it != metadata.end() && it->is_object()) {
        material.hasDispersion = it->value("present", material.hasDispersion != 0u) ? 1u : 0u;
        material.dispersionFactor = it->value("factor", material.dispersionFactor);
    }
    if (const auto it = metadata.find("specular"); it != metadata.end() && it->is_object()) {
        material.hasSpecular = it->value("present", material.hasSpecular != 0u) ? 1u : 0u;
        material.specularFactor = it->value("factor", material.specularFactor);
        if (const auto color = it->find("colorFactor"); color != it->end()) {
            material.specularColorFactor = jsonVec3Or(*color, material.specularColorFactor);
        }
    }
    if (const auto it = metadata.find("sheen"); it != metadata.end() && it->is_object()) {
        material.hasSheen = it->value("present", material.hasSheen != 0u) ? 1u : 0u;
        material.sheenRoughnessFactor = it->value("roughnessFactor", material.sheenRoughnessFactor);
        if (const auto color = it->find("colorFactor"); color != it->end()) {
            material.sheenColorFactor = jsonVec3Or(*color, material.sheenColorFactor);
        }
    }
    if (const auto it = metadata.find("iridescence"); it != metadata.end() && it->is_object()) {
        material.hasIridescence = it->value("present", material.hasIridescence != 0u) ? 1u : 0u;
        material.iridescenceFactor = it->value("factor", material.iridescenceFactor);
        material.iridescenceIor = it->value("ior", material.iridescenceIor);
        material.iridescenceThicknessMinimum = it->value("thicknessMinimum", material.iridescenceThicknessMinimum);
        material.iridescenceThicknessMaximum = it->value("thicknessMaximum", material.iridescenceThicknessMaximum);
    }
    if (const auto it = metadata.find("anisotropy"); it != metadata.end() && it->is_object()) {
        material.hasAnisotropy = it->value("present", material.hasAnisotropy != 0u) ? 1u : 0u;
        material.anisotropyStrength = it->value("strength", material.anisotropyStrength);
        material.anisotropyRotation = it->value("rotation", material.anisotropyRotation);
    }
    if (const auto it = metadata.find("emissiveStrength"); it != metadata.end() && it->is_object()) {
        material.hasEmissiveStrength = it->value("present", material.hasEmissiveStrength != 0u) ? 1u : 0u;
        material.emissiveStrength = it->value("factor", material.emissiveStrength);
    }
    if (const auto it = metadata.find("conductor"); it != metadata.end() && it->is_object()) {
        material.useConductorOptics = it->value("enabled", material.useConductorOptics != 0u) ? 1u : 0u;
        if (const auto eta = it->find("eta"); eta != it->end()) {
            material.conductorEta = jsonVec3Or(*eta, material.conductorEta);
        }
        if (const auto k = it->find("k"); k != it->end()) {
            material.conductorK = jsonVec3Or(*k, material.conductorK);
        }
    }
}

uint32_t jsonArraySizeOr(const nlohmann::json& object, const char* key, uint32_t fallback = 0) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_array()) {
        return fallback;
    }
    return static_cast<uint32_t>(object[key].size());
}

constexpr char kAnimationKey[] = {'a', 'n', 'i', 'm', 'a', 't', 'i', 'o', 'n', '\0'};
constexpr char kSchemaKey[] = {'s', 'c', 'h', 'e', 'm', 'a', '\0'};
constexpr char kSourceFormatKey[] = {'s', 'o', 'u', 'r', 'c', 'e', 'F', 'o', 'r', 'm', 'a', 't', '\0'};
constexpr char kNameKey[] = {'n', 'a', 'm', 'e', '\0'};
constexpr char kDurationSecondsKey[] = {'d', 'u', 'r', 'a', 't', 'i', 'o', 'n', 'S', 'e', 'c', 'o', 'n', 'd', 's', '\0'};
constexpr char kClipKey[] = {'c', 'l', 'i', 'p', '\0'};
constexpr char kDurationKey[] = {'d', 'u', 'r', 'a', 't', 'i', 'o', 'n', '\0'};
constexpr char kChannelCountKey[] = {'c', 'h', 'a', 'n', 'n', 'e', 'l', 'C', 'o', 'u', 'n', 't', '\0'};
constexpr char kChannelsKey[] = {'c', 'h', 'a', 'n', 'n', 'e', 'l', 's', '\0'};
constexpr char kDecodedChannelCountKey[] = {'d', 'e', 'c', 'o', 'd', 'e', 'd', 'C', 'h', 'a', 'n', 'n', 'e', 'l', 'C', 'o', 'u', 'n', 't', '\0'};
constexpr char kDecodedKeyframeCountKey[] = {'d', 'e', 'c', 'o', 'd', 'e', 'd', 'K', 'e', 'y', 'f', 'r', 'a', 'm', 'e', 'C', 'o', 'u', 'n', 't', '\0'};
constexpr char kTrackCountKey[] = {'t', 'r', 'a', 'c', 'k', 'C', 'o', 'u', 'n', 't', '\0'};
constexpr char kTracksKey[] = {'t', 'r', 'a', 'c', 'k', 's', '\0'};
constexpr char kEventCountKey[] = {'e', 'v', 'e', 'n', 't', 'C', 'o', 'u', 'n', 't', '\0'};
constexpr char kEventsKey[] = {'e', 'v', 'e', 'n', 't', 's', '\0'};
constexpr char kRootMotionCandidateCountKey[] = {'r', 'o', 'o', 't', 'M', 'o', 't', 'i', 'o', 'n', 'C', 'a', 'n', 'd', 'i', 'd', 'a', 't', 'e', 'C', 'o', 'u', 'n', 't', '\0'};
constexpr char kRootMotionCandidatesKey[] = {'r', 'o', 'o', 't', 'M', 'o', 't', 'i', 'o', 'n', 'C', 'a', 'n', 'd', 'i', 'd', 'a', 't', 'e', 's', '\0'};
constexpr char kRuntimeSupportKey[] = {'r', 'u', 'n', 't', 'i', 'm', 'e', 'S', 'u', 'p', 'p', 'o', 'r', 't', '\0'};
constexpr char kRuntimeStructureKey[] = {'r', 'u', 'n', 't', 'i', 'm', 'e', 'S', 't', 'r', 'u', 'c', 't', 'u', 'r', 'e', '\0'};
constexpr char kPlaybackImplementedKey[] = {'p', 'l', 'a', 'y', 'b', 'a', 'c', 'k', 'I', 'm', 'p', 'l', 'e', 'm', 'e', 'n', 't', 'e', 'd', '\0'};
constexpr char kAnimationMetadataBridgeValue[] = {'A', 'n', 'i', 'm', 'a', 't', 'i', 'o', 'n', 'M', 'e', 't', 'a', 'd', 'a', 't', 'a', 'B', 'r', 'i', 'd', 'g', 'e', '\0'};
constexpr char kMetadataBridgePlaybackPending[] = {'m', 'e', 't', 'a', 'd', 'a', 't', 'a', '_', 'b', 'r', 'i', 'd', 'g', 'e', '_', 'o', 'n', 'l', 'y', '_', 'r', 'u', 'n', 't', 'i', 'm', 'e', '_', 'p', 'l', 'a', 'y', 'b', 'a', 'c', 'k', '_', 'p', 'e', 'n', 'd', 'i', 'n', 'g', '\0'};

bool loadAnimationMetadataBridge(
    const NativeAssetInspection& inspection,
    const std::vector<std::byte>& bytes,
    NativeRuntimeAnimationMetadataBridge& out) {
    nlohmann::json metadata;
    if (!readJsonChunk(bytes, findChunk(inspection, ChunkMetadataJson), metadata)) {
        return false;
    }
    const nlohmann::json& animation = metadata.contains(kAnimationKey) && metadata[kAnimationKey].is_object()
        ? metadata[kAnimationKey]
        : metadata;
    out.available = true;
    out.schema = metadata.value(kSchemaKey, animation.value(kSchemaKey, std::string{}));
    out.sourceFormat = animation.value(kSourceFormatKey, metadata.value(kSourceFormatKey, std::string{}));
    out.name = animation.value(kNameKey, metadata.value(kNameKey, std::string{}));
    out.durationSeconds = animation.value(kDurationSecondsKey, metadata.value(kDurationSecondsKey, 0.0));
    if (out.durationSeconds <= 0.0 && animation.contains(kClipKey) && animation[kClipKey].is_object()) {
        out.durationSeconds = animation[kClipKey].value(kDurationKey, 0.0);
    }
    out.channelCount = animation.value(kChannelCountKey, jsonArraySizeOr(animation, kChannelsKey));
    out.decodedChannelCount = animation.value(kDecodedChannelCountKey, 0u);
    out.decodedKeyframeCount = animation.value(kDecodedKeyframeCountKey, 0u);
    out.trackCount = animation.value(kTrackCountKey, jsonArraySizeOr(animation, kTracksKey, jsonArraySizeOr(animation, kChannelsKey)));
    out.eventCount = animation.value(kEventCountKey, jsonArraySizeOr(animation, kEventsKey));
    out.rootMotionCandidateCount = animation.value(kRootMotionCandidateCountKey, jsonArraySizeOr(animation, kRootMotionCandidatesKey));
    out.runtimeSupport = animation.value(kRuntimeSupportKey, metadata.value(kRuntimeSupportKey, std::string(kMetadataBridgePlaybackPending)));
    return true;
}

TextureSamplerDesc unpackSampler(uint32_t packed) {
    TextureSamplerDesc sampler;
    sampler.minFilter = static_cast<TextureFilter>(packed & 0xfu);
    sampler.magFilter = static_cast<TextureFilter>((packed >> 4u) & 0xfu);
    sampler.wrapS = static_cast<TextureWrap>((packed >> 8u) & 0xfu);
    sampler.wrapT = static_cast<TextureWrap>((packed >> 12u) & 0xfu);
    return sampler;
}

void assignTextureSlot(MaterialAsset& material, uint32_t slot, TextureAssetHandle handle) {
    switch (static_cast<RtmaterialTextureSlot>(slot)) {
    case RtmaterialTextureSlot::BaseColor: material.baseColorTexture = handle; break;
    case RtmaterialTextureSlot::Normal: material.normalTexture = handle; break;
    case RtmaterialTextureSlot::MetallicRoughness: material.metallicRoughnessTexture = handle; break;
    case RtmaterialTextureSlot::Occlusion: material.occlusionTexture = handle; break;
    case RtmaterialTextureSlot::Emissive: material.emissiveTexture = handle; break;
    case RtmaterialTextureSlot::Transmission: material.transmissionTexture = handle; break;
    case RtmaterialTextureSlot::Clearcoat: material.clearcoatTexture = handle; break;
    case RtmaterialTextureSlot::ClearcoatRoughness: material.clearcoatRoughnessTexture = handle; break;
    case RtmaterialTextureSlot::ClearcoatNormal: material.clearcoatNormalTexture = handle; break;
    case RtmaterialTextureSlot::VolumeThickness: material.volumeThicknessTexture = handle; break;
    case RtmaterialTextureSlot::SheenColor: material.sheenColorTexture = handle; break;
    case RtmaterialTextureSlot::SheenRoughness: material.sheenRoughnessTexture = handle; break;
    case RtmaterialTextureSlot::Specular: material.specularTexture = handle; break;
    case RtmaterialTextureSlot::SpecularColor: material.specularColorTexture = handle; break;
    case RtmaterialTextureSlot::Iridescence: material.iridescenceTexture = handle; break;
    case RtmaterialTextureSlot::IridescenceThickness: material.iridescenceThicknessTexture = handle; break;
    case RtmaterialTextureSlot::Anisotropy: material.anisotropyTexture = handle; break;
    case RtmaterialTextureSlot::Opacity: material.opacityTexture = handle; break;
    case RtmaterialTextureSlot::Height: material.heightTexture = handle; break;
    }
}

std::string materialTextureSlotName(uint32_t slot) {
    if (slot == static_cast<uint32_t>(RtmaterialTextureSlot::ClearcoatNormal)) {
        return std::string({'c', 'l', 'e', 'a', 'r', 'c', 'o', 'a', 't', 'N', 'o', 'r', 'm', 'a', 'l'});
    }
    if (slot == static_cast<uint32_t>(RtmaterialTextureSlot::VolumeThickness)) {
        return std::string({'v', 'o', 'l', 'u', 'm', 'e', 'T', 'h', 'i', 'c', 'k', 'n', 'e', 's', 's'});
    }
    if (slot == static_cast<uint32_t>(RtmaterialTextureSlot::IridescenceThickness)) {
        return std::string({'i', 'r', 'i', 'd', 'e', 's', 'c', 'e', 'n', 'c', 'e', 'T', 'h', 'i', 'c', 'k', 'n', 'e', 's', 's'});
    }
    if (slot == static_cast<uint32_t>(RtmaterialTextureSlot::Anisotropy)) {
        return std::string({'a', 'n', 'i', 's', 'o', 't', 'r', 'o', 'p', 'y'});
    }
    if (slot == static_cast<uint32_t>(RtmaterialTextureSlot::Opacity)) {
        return std::string({'o', 'p', 'a', 'c', 'i', 't', 'y'});
    }
    if (slot == static_cast<uint32_t>(RtmaterialTextureSlot::Height)) {
        return std::string({'h', 'e', 'i', 'g', 'h', 't'});
    }
    switch (static_cast<RtmaterialTextureSlot>(slot)) {
    case RtmaterialTextureSlot::BaseColor: return "baseColor";
    case RtmaterialTextureSlot::Normal: return "normal";
    case RtmaterialTextureSlot::MetallicRoughness: return "metallicRoughness";
    case RtmaterialTextureSlot::Occlusion: return "occlusion";
    case RtmaterialTextureSlot::Emissive: return "emissive";
    case RtmaterialTextureSlot::Transmission: return "transmission";
    case RtmaterialTextureSlot::Clearcoat: return "clearcoat";
    case RtmaterialTextureSlot::ClearcoatRoughness: return "clearcoatRoughness";
    case RtmaterialTextureSlot::SheenColor: return "sheenColor";
    case RtmaterialTextureSlot::SheenRoughness: return "sheenRoughness";
    case RtmaterialTextureSlot::Specular: return "specular";
    case RtmaterialTextureSlot::SpecularColor: return "specularColor";
    case RtmaterialTextureSlot::Iridescence: return "iridescence";
    }
    return "unknown";
}

std::filesystem::path nativeObjectRuntimePath(const NativeAssetStoreObject& object) {
    if (object.source == NativeAssetStoreSource::Package) {
        return object.packageObjectPath.empty() ? object.packagePath : object.packagePath / object.packageObjectPath;
    }
    return object.path;
}

std::string nativeRuntimePathKey(const std::filesystem::path& path) {
    std::filesystem::path normalized = path.lexically_normal();
    std::string key = normalized.generic_string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return key;
}

void applyNativeObjectTextureMetadata(TextureAsset& texture, const std::string& guid, const NativeAssetStoreObject* object) {
    texture.nativeGuid = guid;
    if (object != nullptr) {
        texture.nativeSource = nativeAssetStoreSourceName(object->source);
        texture.nativePath = nativeObjectRuntimePath(*object);
    }
}

void applyNativeObjectMaterialMetadata(MaterialAsset& material, const std::string& guid, const NativeAssetStoreObject* object) {
    material.nativeGuid = guid;
    if (object != nullptr) {
        material.nativeSource = nativeAssetStoreSourceName(object->source);
        material.nativePath = nativeObjectRuntimePath(*object);
    }
}

void applyNativeObjectMeshMetadata(MeshAsset& mesh, const std::string& guid, const NativeAssetStoreObject* object) {
    mesh.nativeGuid = guid;
    if (object != nullptr) {
        mesh.nativeSource = nativeAssetStoreSourceName(object->source);
        mesh.nativePath = nativeObjectRuntimePath(*object);
    }
}

nlohmann::json nativeTextureFormatSupportJson(const NativeTextureFormatSupport& support) {
    return {
        {"platformName", support.platformName},
        {"queriedFromVulkan", support.queriedFromVulkan},
        {"bc1SrgbSampled", support.bc1SrgbSampled},
        {"bc1UnormSampled", support.bc1UnormSampled},
        {"bc3SrgbSampled", support.bc3SrgbSampled},
        {"bc3UnormSampled", support.bc3UnormSampled},
        {"bc7SrgbSampled", support.bc7SrgbSampled},
        {"bc7UnormSampled", support.bc7UnormSampled},
        {"bc5UnormSampled", support.bc5UnormSampled},
        {"bc4UnormSampled", support.bc4UnormSampled},
        {"bc6hUfloatSampled", support.bc6hUfloatSampled},
        {"bc6hSfloatSampled", support.bc6hSfloatSampled},
        {"rgba8SrgbSampled", support.rgba8SrgbSampled},
        {"rgba8UnormSampled", support.rgba8UnormSampled},
        {"rgba16fSampled", support.rgba16fSampled},
    };
}

int nativeTextureVariantFormatRank(VkFormat format) {
    switch (format) {
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
        return 325;
    case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
        return 400;
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
        return 350;
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
        return 300;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        return 375;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 200;
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
        return 100;
    default:
        return 0;
    }
}

bool betterNativeTextureVariant(const NativeRuntimeLoadedAsset& candidate, const NativeRuntimeLoadedAsset& current) {
    if (candidate.texturePayloadFormatSupported != current.texturePayloadFormatSupported) {
        return candidate.texturePayloadFormatSupported;
    }
    const int candidateRank = nativeTextureVariantFormatRank(candidate.texturePayloadFormat);
    const int currentRank = nativeTextureVariantFormatRank(current.texturePayloadFormat);
    if (candidateRank != currentRank) {
        return candidateRank > currentRank;
    }
    return candidate.path.generic_string() < current.path.generic_string();
}

void resolveNativeMaterialTextureBindings(
    NativeRuntimeLoadedAsset& asset,
    AssetManager* manager,
    const std::unordered_map<std::string, TextureAssetHandle>& textureHandlesByGuid,
    const std::unordered_map<std::string, NativeAssetStoreObject>& objectsByGuid) {
    if (asset.kind != NativeAssetKind::Material) {
        return;
    }
    for (NativeRuntimeTextureBinding& binding : asset.materialTextureBindings) {
        const auto objectIt = objectsByGuid.find(binding.textureGuid);
        if (objectIt != objectsByGuid.end()) {
            binding.nativeSource = nativeAssetStoreSourceName(objectIt->second.source);
            binding.nativePath = nativeObjectRuntimePath(objectIt->second);
        }

        const auto handleIt = textureHandlesByGuid.find(binding.textureGuid);
        if (handleIt == textureHandlesByGuid.end()) {
            assignTextureSlot(asset.material, binding.slot, TextureAssetHandle{});
            binding.missing = true;
            binding.fallback = true;
            binding.repairAction = "reimport_or_recook_source_asset";
            asset.warnings.push_back("Native material texture slot '" + binding.slotName + "' references missing texture GUID " + binding.textureGuid + "; using fallback material texture until the source asset is repaired.");
            continue;
        }

        binding.textureHandle = handleIt->second;
        binding.resolved = binding.textureHandle.valid();
        binding.resident = binding.resolved;
        if (manager != nullptr) {
            if (TextureAsset* texture = manager->texture(binding.textureHandle)) {
                texture->resident = true;
                binding.fallback = texture->fallback;
            }
        }
        assignTextureSlot(asset.material, binding.slot, binding.textureHandle);
    }
}

void resolveNativeMeshMaterialSlots(
    NativeRuntimeLoadedAsset& asset,
    AssetManager* manager,
    const std::unordered_map<std::string, MaterialAssetHandle>& materialHandlesByGuid,
    const std::unordered_map<std::string, NativeAssetStoreObject>& objectsByGuid) {
    if (asset.kind != NativeAssetKind::Mesh || manager == nullptr) {
        return;
    }
    for (MeshPrimitiveAsset& primitive : asset.mesh.primitives) {
        if (!primitive.material.valid()) {
            updatePrimitiveAlphaClassification(primitive, nullptr);
            continue;
        }
        const uint32_t materialSlot = primitive.material.index;
        if (materialSlot >= asset.dependencyGuids.size()) {
            primitive.material = MaterialAssetHandle{};
            updatePrimitiveAlphaClassification(primitive, nullptr);
            asset.warnings.push_back("Native mesh primitive material slot " + std::to_string(materialSlot) + " has no matching material dependency GUID; using default material.");
            continue;
        }

        const std::string& materialGuid = asset.dependencyGuids[materialSlot];
        const auto handleIt = materialHandlesByGuid.find(materialGuid);
        if (handleIt == materialHandlesByGuid.end() || !handleIt->second.valid()) {
            primitive.material = MaterialAssetHandle{};
            updatePrimitiveAlphaClassification(primitive, nullptr);
            const auto objectIt = objectsByGuid.find(materialGuid);
            const std::string source = objectIt != objectsByGuid.end() ? nativeObjectRuntimePath(objectIt->second).generic_string() : std::string{};
            asset.warnings.push_back("Native mesh primitive references missing material GUID " + materialGuid + (source.empty() ? std::string{} : " at " + source) + "; using default material.");
            continue;
        }

        primitive.material = handleIt->second;
        updatePrimitiveAlphaClassification(primitive, manager->material(primitive.material));
    }
}

NativeRuntimeSceneAssetPlan makeSceneAssetPlan(const NativeRuntimeLoadReport& report, const SceneAsset& scene) {
    NativeRuntimeSceneAssetPlan plan;
    plan.available = !scene.meshes.empty() && !scene.nodes.empty();
    plan.assetManagerBacked = report.rendererUploadPlan.assetManagerBacked;
    plan.packageBacked = report.rendererUploadPlan.packageBacked;
    plan.textureCount = static_cast<uint32_t>(scene.textures.size());
    plan.materialCount = static_cast<uint32_t>(scene.materials.size());
    plan.meshCount = static_cast<uint32_t>(scene.meshes.size());
    plan.skinCount = static_cast<uint32_t>(scene.skins.size());
    plan.nodeCount = static_cast<uint32_t>(scene.nodes.size());
    plan.rootNodeCount = static_cast<uint32_t>(scene.rootNodes.size());
    for (const SceneNodeAsset& node : scene.nodes) {
        if (node.skinIndex >= 0) {
            ++plan.skinnedNodeCount;
        }
    }
    glm::vec3 boundsMin{std::numeric_limits<float>::max()};
    glm::vec3 boundsMax{-std::numeric_limits<float>::max()};
    for (const NativeRuntimeLoadedAsset& asset : report.assets) {
        if (!asset.ok || asset.kind != NativeAssetKind::Mesh) {
            continue;
        }
        for (const MeshVertex& vertex : asset.mesh.vertices) {
            boundsMin = glm::min(boundsMin, vertex.position);
            boundsMax = glm::max(boundsMax, vertex.position);
            plan.boundsAvailable = true;
        }
    }
    if (plan.boundsAvailable) {
        plan.boundsMin = {boundsMin.x, boundsMin.y, boundsMin.z};
        plan.boundsMax = {boundsMax.x, boundsMax.y, boundsMax.z};
    }
    plan.rendererPlaceable = plan.available && plan.assetManagerBacked && plan.rootNodeCount > 0 && plan.missingMeshHandleCount == 0;
    return plan;
}

void appendDirectStoreUploadTicket(
    NativeRuntimeLoadReport& report,
    const NativeRuntimeLoadedAsset& asset,
    const NativeAssetStoreObject& object,
    std::string resourceKind,
    std::string label,
    uint64_t uploadBytes) {
    if (uploadBytes == 0) {
        return;
    }
    NativeRuntimeDirectStoreUploadPlan& plan = report.directStoreUploadPlan;
    plan.available = true;
    plan.executable = false;
    plan.assetManagerBypass = true;
    plan.policy = "native-store-upload-ticket-plan-only";
    plan.unavailableReason = "Renderer-owned NativeAssetStore handles, Vulkan resource allocation, timeline fences, and direct resource retirement are not wired yet.";
    if (plan.readinessRequirements.empty()) {
        plan.readinessRequirements = {
            "renderer-owned native-store handles",
            "Vulkan resource allocation from native-store payloads",
            "timeline semaphore/fence submission for upload tickets",
            "per-resource direct-store retirement on package unload",
        };
        plan.missingRequirements = plan.readinessRequirements;
    }
    if (object.source == NativeAssetStoreSource::Package) {
        plan.packageBacked = true;
    } else if (object.source == NativeAssetStoreSource::LooseFile) {
        plan.looseBacked = true;
    }

    NativeRuntimeDirectStoreUploadTicketPlan ticket;
    ticket.guid = asset.guid;
    ticket.kind = asset.kind;
    ticket.source = nativeAssetStoreSourceName(object.source);
    ticket.nativePath = nativeObjectRuntimePath(object);
    ticket.packageObjectPath = object.packageObjectPath;
    ticket.generation = object.generation;
    ticket.resourceKind = std::move(resourceKind);
    ticket.label = std::move(label);
    ticket.uploadBytes = uploadBytes;
    ticket.uploadReady = false;
    ticket.unavailableReason = plan.unavailableReason;
    plan.tickets.push_back(std::move(ticket));
}

void appendDirectStoreUploadPlanForAsset(
    NativeRuntimeLoadReport& report,
    const NativeRuntimeLoadedAsset& asset,
    const NativeAssetStoreObject* object) {
    if (object == nullptr || !asset.ok) {
        return;
    }
    if (asset.kind == NativeAssetKind::Texture) {
        const uint64_t bytes = static_cast<uint64_t>(asset.texture.rgba8.size());
        appendDirectStoreUploadTicket(report, asset, *object, "image", "NativeStore texture image upload", bytes);
        if (bytes > 0) {
            ++report.directStoreUploadPlan.textureTicketCount;
            report.directStoreUploadPlan.textureUploadBytes += bytes;
        }
    } else if (asset.kind == NativeAssetKind::Mesh) {
        const uint64_t vertexBytes = static_cast<uint64_t>(asset.mesh.vertices.size() * sizeof(MeshVertex));
        appendDirectStoreUploadTicket(report, asset, *object, "buffer", "NativeStore mesh vertex buffer upload", vertexBytes);
        if (vertexBytes > 0) {
            ++report.directStoreUploadPlan.meshBufferTicketCount;
            report.directStoreUploadPlan.vertexUploadBytes += vertexBytes;
        }
        const uint64_t indexBytes = static_cast<uint64_t>(asset.mesh.indices.size() * sizeof(uint32_t));
        appendDirectStoreUploadTicket(report, asset, *object, "buffer", "NativeStore mesh index buffer upload", indexBytes);
        if (indexBytes > 0) {
            ++report.directStoreUploadPlan.meshBufferTicketCount;
            report.directStoreUploadPlan.indexUploadBytes += indexBytes;
        }
    }
}

GpuUploadResourceKind gpuUploadKindFromDirectStoreTicket(const NativeRuntimeDirectStoreUploadTicketPlan& ticket) {
    return ticket.resourceKind == "image" ? GpuUploadResourceKind::Image : GpuUploadResourceKind::Buffer;
}

void buildDirectStoreUploadTicketQueueSimulation(NativeRuntimeDirectStoreUploadPlan& plan) {
    if (!plan.available || plan.tickets.empty()) {
        return;
    }
    GpuUploadTicketQueue queue;
    constexpr uint64_t kFrameBudgetBytes = 8ull * 1024ull * 1024ull;
    constexpr uint32_t kMaxFrames = 16u;
    for (const NativeRuntimeDirectStoreUploadTicketPlan& ticket : plan.tickets) {
        if (ticket.uploadBytes == 0) {
            continue;
        }
        const uint64_t ticketId = queue.create(GpuUploadTicketDesc{
            .kind = gpuUploadKindFromDirectStoreTicket(ticket),
            .label = ticket.label + " " + ticket.guid,
            .totalBytes = ticket.uploadBytes,
            .chunkBytes = kFrameBudgetBytes,
        });
        (void)ticketId;
    }
    uint64_t submittedBytes = 0;
    uint32_t frames = 0;
    for (; frames < kMaxFrames; ++frames) {
        const GpuUploadSubmitResult submit = queue.submitFrame(GpuUploadFrameBudget{.maxBytes = kFrameBudgetBytes, .maxSubmissions = 1});
        submittedBytes += submit.submittedBytes;
        const uint64_t completedTimeline = queue.nextTimelineValue() == 0 ? 0 : queue.nextTimelineValue() - 1;
        const bool completed = queue.completeTimeline(completedTimeline);
        (void)completed;
        const std::vector<GpuUploadTicketSnapshot> snapshots = queue.snapshots(false);
        const bool complete = std::all_of(snapshots.begin(), snapshots.end(), [](const GpuUploadTicketSnapshot& snapshot) {
            return snapshot.state == GpuUploadTicketState::Complete || snapshot.state == GpuUploadTicketState::Cancelled;
        });
        if (submit.submittedChunks == 0 && complete) {
            ++frames;
            break;
        }
        if (complete) {
            ++frames;
            break;
        }
    }
    uint64_t completedBytes = 0;
    plan.uploadTicketQueueSnapshots = queue.snapshots(true);
    for (const GpuUploadTicketSnapshot& snapshot : plan.uploadTicketQueueSnapshots) {
        completedBytes += snapshot.completedBytes;
    }
    plan.uploadTicketQueueSimulationAvailable = !plan.uploadTicketQueueSnapshots.empty();
    plan.uploadTicketQueueSimulationExecutable = false;
    plan.uploadTicketQueueFrameBudgetBytes = kFrameBudgetBytes;
    plan.uploadTicketQueueFrameCount = frames;
    plan.uploadTicketQueueSubmittedBytes = submittedBytes;
    plan.uploadTicketQueueCompletedBytes = completedBytes;
    plan.uploadTicketQueueNextTimelineValue = queue.nextTimelineValue();
    plan.uploadTicketQueuePolicy = "diagnostic-ticket-queue-simulation-only; no Vulkan resources, command buffers, or renderer-owned native-store handles are allocated.";
}

void finalizeLegacyCpuLoadPolicy(NativeRuntimeLoadReport& report) {
    NativeRuntimeLegacyCpuLoadPolicyReport& policy = report.legacyCpuLoadPolicy;
    const auto saturatedAdd = [](uint64_t a, uint64_t b) {
        if (b > std::numeric_limits<uint64_t>::max() - a) {
            return std::numeric_limits<uint64_t>::max();
        }
        return a + b;
    };

    policy.available = true;
    policy.assetManagerBacked = report.rendererUploadPlan.assetManagerBacked;
    policy.packageBacked = report.rendererUploadPlan.packageBacked;
    policy.looseBacked = report.directStoreUploadPlan.looseBacked || (!policy.packageBacked && !report.sourceRoot.empty());
    policy.warningThresholdBytes = report.options.eagerCpuLoadWarningBytes;
    policy.hardLimitBytes = report.options.eagerCpuLoadHardLimitBytes;
    policy.allowLargeEagerCpuLoad = report.options.allowLargeEagerCpuLoad;
    policy.policy = "legacy-eager-cpu-load-warning-and-hard-limit";
    policy.recommendedAction = "Use the progressive native streaming path and DirectStorage-backed upload tickets for large package payloads.";

    uint64_t estimated = 0;
    estimated = saturatedAdd(estimated, report.rendererUploadPlan.textureUploadBytes);
    estimated = saturatedAdd(estimated, report.rendererUploadPlan.vertexUploadBytes);
    estimated = saturatedAdd(estimated, report.rendererUploadPlan.indexUploadBytes);
    if (estimated == 0) {
        const uint64_t vertexBytes = report.totalVertexCount > std::numeric_limits<uint64_t>::max() / sizeof(MeshVertex)
            ? std::numeric_limits<uint64_t>::max()
            : report.totalVertexCount * sizeof(MeshVertex);
        const uint64_t indexBytes = report.totalIndexCount > std::numeric_limits<uint64_t>::max() / sizeof(uint32_t)
            ? std::numeric_limits<uint64_t>::max()
            : report.totalIndexCount * sizeof(uint32_t);
        estimated = saturatedAdd(report.totalTextureBytes, saturatedAdd(vertexBytes, indexBytes));
    }
    policy.estimatedEagerCpuBytes = estimated;
    policy.largeEagerLoadWarning = policy.assetManagerBacked && policy.warningThresholdBytes > 0 && estimated >= policy.warningThresholdBytes;
    policy.hardLimitExceeded = policy.assetManagerBacked && !policy.allowLargeEagerCpuLoad && policy.hardLimitBytes > 0 && estimated >= policy.hardLimitBytes;
    policy.streamingRecommended = policy.largeEagerLoadWarning || policy.hardLimitExceeded;

    if (policy.largeEagerLoadWarning) {
        report.warnings.push_back(
            "Legacy eager CPU native runtime load decoded " + std::to_string(estimated) +
            " bytes into AssetManager; large production payloads should use progressive native streaming and DirectStorage upload tickets.");
    }
    if (policy.hardLimitExceeded) {
        report.errors.push_back(makeLoaderError(
            NativeBinaryErrorCode::UnsupportedPlatformFeature,
            report.sourceRoot,
            "legacy_cpu_load_policy",
            0,
            estimated,
            "Legacy eager CPU native runtime load exceeds the hard limit of " + std::to_string(policy.hardLimitBytes) +
                " bytes; use progressive native streaming or explicitly allow large eager CPU loads for diagnostics."));
        report.ok = false;
    }
}

void appendControllerClipBinding(
    NativeRuntimeLoadedAsset& controllerAsset,
    const std::unordered_map<std::string, const NativeRuntimeLoadedAsset*>& animationsByGuid,
    std::string source,
    const std::string& clipGuid,
    const std::filesystem::path& clipPath) {
    if (clipGuid.empty() && clipPath.empty()) {
        return;
    }
    NativeRuntimeAnimationClipBinding binding;
    binding.source = std::move(source);
    binding.clipGuid = clipGuid;
    binding.clipPath = clipPath;
    const auto animationIt = animationsByGuid.find(clipGuid);
    if (animationIt != animationsByGuid.end() && animationIt->second != nullptr) {
        binding.resolved = true;
        binding.resolvedNativePath = animationIt->second->path;
    }
    controllerAsset.animationClipBindings.push_back(std::move(binding));
}

void resolveAnimationControllerClipBindings(NativeRuntimeLoadReport& report) {
    std::unordered_map<std::string, const NativeRuntimeLoadedAsset*> animationsByGuid;
    for (const NativeRuntimeLoadedAsset& asset : report.assets) {
        if (asset.ok && asset.kind == NativeAssetKind::Animation && !asset.guid.empty()) {
            animationsByGuid[asset.guid] = &asset;
        }
    }
    if (animationsByGuid.empty()) {
        return;
    }

    for (NativeRuntimeLoadedAsset& asset : report.assets) {
        if (!asset.ok || asset.kind != NativeAssetKind::AnimationController) {
            continue;
        }
        asset.animationClipBindings.clear();
        for (const AnimationController::State& state : asset.animationController.states()) {
            appendControllerClipBinding(asset, animationsByGuid, "state:" + state.name, state.clipGuid, state.clipPath);
            if (state.hasBlendTree) {
                for (const AnimationController::BlendTreeChild& child : state.blendTree.children) {
                    appendControllerClipBinding(
                        asset,
                        animationsByGuid,
                        "state:" + state.name + ".blendTree:" + child.name,
                        child.clipGuid,
                        child.clipPath);
                }
            }
        }
        for (const AnimationController::Layer& layer : asset.animationController.layers()) {
            appendControllerClipBinding(asset, animationsByGuid, "layer:" + layer.name, layer.clipGuid, layer.clipPath);
        }
    }
}

} // namespace

SceneAsset buildNativeRuntimeSceneAsset(const NativeRuntimeLoadReport& report, const std::filesystem::path& sourcePath) {
    SceneAsset scene;
    scene.name = sourcePath.empty() ? std::string("Native Runtime Scene") : sourcePath.stem().string();
    scene.sourcePath = sourcePath;

    std::unordered_set<uint32_t> seenTextures;
    std::unordered_set<uint32_t> seenMaterials;
    std::unordered_set<uint32_t> seenMeshes;
    std::unordered_map<std::string, uint32_t> meshNodeByGuid;
    std::unordered_map<std::string, const RuntimeSkeleton*> skeletonByGuid;
    std::unordered_map<std::string, std::unordered_map<int32_t, uint32_t>> skeletonJointNodeByGuid;

    for (const NativeRuntimeLoadedAsset& asset : report.assets) {
        if (!asset.ok) {
            continue;
        }
        if (asset.kind == NativeAssetKind::Texture && asset.textureHandle.valid() && seenTextures.insert(asset.textureHandle.index).second) {
            scene.textures.push_back(asset.textureHandle);
        } else if (asset.kind == NativeAssetKind::Material && asset.materialHandle.valid() && seenMaterials.insert(asset.materialHandle.index).second) {
            scene.materials.push_back(asset.materialHandle);
        } else if (asset.kind == NativeAssetKind::Mesh && asset.meshHandle.valid() && seenMeshes.insert(asset.meshHandle.index).second) {
            scene.meshes.push_back(asset.meshHandle);
            SceneNodeAsset node;
            node.name = asset.mesh.name.empty() ? asset.path.stem().string() : asset.mesh.name;
            if (node.name.empty()) {
                node.name = "Native Mesh " + std::to_string(scene.nodes.size());
            }
            node.mesh = asset.meshHandle;
            node.sourceNodeIndex = static_cast<int32_t>(scene.nodes.size());
            const uint32_t nodeIndex = static_cast<uint32_t>(scene.nodes.size());
            scene.nodes.push_back(std::move(node));
            scene.rootNodes.push_back(nodeIndex);
            if (!asset.guid.empty()) {
                meshNodeByGuid[asset.guid] = nodeIndex;
            }
        } else if (asset.kind == NativeAssetKind::Skeleton && asset.skeleton.valid() && !asset.guid.empty()) {
            skeletonByGuid[asset.guid] = &asset.skeleton;
        }
    }

    for (const NativeRuntimeLoadedAsset& asset : report.assets) {
        if (!asset.ok || asset.kind != NativeAssetKind::SkeletalMesh || !asset.skeletalMeshBinding.hasJointRemap) {
            continue;
        }
        const auto nodeIt = meshNodeByGuid.find(asset.skeletalMeshBinding.meshGuid);
        const auto skeletonIt = skeletonByGuid.find(asset.skeletalMeshBinding.skeletonGuid);
        if (nodeIt == meshNodeByGuid.end() || skeletonIt == skeletonByGuid.end()) {
            continue;
        }
        const uint32_t meshNodeIndex = nodeIt->second;
        if (meshNodeIndex >= scene.nodes.size() || scene.nodes[meshNodeIndex].skinIndex >= 0) {
            continue;
        }
        const RuntimeSkeleton& skeleton = *skeletonIt->second;
        std::unordered_map<int32_t, uint32_t>& jointNodeBySource = skeletonJointNodeByGuid[asset.skeletalMeshBinding.skeletonGuid];
        if (jointNodeBySource.empty()) {
            for (const RuntimeSkeletonJoint& joint : skeleton.joints()) {
                if (joint.index < 0) {
                    continue;
                }
                SceneNodeAsset jointNode;
                jointNode.name = joint.name.empty() ? "Joint " + std::to_string(joint.index) : joint.name;
                jointNode.sourceNodeIndex = joint.index;
                const uint32_t jointNodeIndex = static_cast<uint32_t>(scene.nodes.size());
                jointNodeBySource[joint.index] = jointNodeIndex;
                scene.nodes.push_back(std::move(jointNode));
            }
            for (const RuntimeSkeletonJoint& joint : skeleton.joints()) {
                if (joint.index < 0) {
                    continue;
                }
                const auto jointNodeIt = jointNodeBySource.find(joint.index);
                if (jointNodeIt == jointNodeBySource.end()) {
                    continue;
                }
                SceneNodeAsset& jointNode = scene.nodes[jointNodeIt->second];
                const auto parentIt = jointNodeBySource.find(joint.parentIndex);
                const uint32_t parentNodeIndex = parentIt != jointNodeBySource.end() ? parentIt->second : meshNodeIndex;
                if (parentNodeIndex < scene.nodes.size() && parentNodeIndex != jointNodeIt->second) {
                    jointNode.parent = static_cast<int32_t>(parentNodeIndex);
                    std::vector<uint32_t>& siblings = scene.nodes[parentNodeIndex].children;
                    if (std::find(siblings.begin(), siblings.end(), jointNodeIt->second) == siblings.end()) {
                        siblings.push_back(jointNodeIt->second);
                    }
                }
            }
        }
        SceneSkinAsset skin;
        skin.name = skeleton.name().empty() ? asset.path.stem().string() : skeleton.name();
        const auto rootIt = jointNodeBySource.find(skeleton.skeletonRoot());
        skin.skeletonRoot = rootIt != jointNodeBySource.end() ? static_cast<int32_t>(rootIt->second) : -1;
        skin.joints.reserve(skeleton.joints().size());
        skin.inverseBindMatrices.reserve(skeleton.joints().size());
        for (const RuntimeSkeletonJoint& joint : skeleton.joints()) {
            if (joint.index < 0) {
                continue;
            }
            const auto jointNodeIt = jointNodeBySource.find(joint.index);
            if (jointNodeIt == jointNodeBySource.end()) {
                continue;
            }
            skin.joints.push_back(jointNodeIt->second);
            skin.inverseBindMatrices.push_back(joint.hasInverseBindMatrix ? glm::make_mat4(joint.inverseBindMatrix.data()) : glm::mat4{1.0f});
        }
        if (skin.skeletonRoot < 0 && !skin.joints.empty()) {
            skin.skeletonRoot = static_cast<int32_t>(skin.joints.front());
        }
        if (skin.joints.empty()) {
            continue;
        }
        scene.nodes[meshNodeIndex].skinIndex = static_cast<int32_t>(scene.skins.size());
        scene.skins.push_back(std::move(skin));
    }

    return scene;
}

NativeRuntimeLoadedAsset NativeAssetRuntimeLoader::loadStandalone(const std::filesystem::path& path, const NativeRuntimeLoadOptions& options) const {
    std::vector<std::byte> bytes;
    NativeBinaryError readError;
    if (!readFileBytes(path, bytes, &readError)) {
        NativeRuntimeLoadedAsset result;
        result.path = path;
        result.errors.push_back(readError);
        return result;
    }
    return loadBytes(path, bytes, options);
}

NativeRuntimeLoadedAsset NativeAssetRuntimeLoader::loadBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, const NativeRuntimeLoadOptions& options) const {
    NativeRuntimeLoadedAsset result;
    result.path = pathHint;
    NativeAssetReader reader;
    const NativeAssetInspection inspection = reader.inspectBytes(pathHint, bytes, options.validatePayloadHashes);
    if (!inspection.ok) {
        result.errors = inspection.errors;
        return result;
    }
    result.kind = static_cast<NativeAssetKind>(inspection.header.assetKind);
    result.guid = nativeGuidToText(inspection.header.assetGuid);
    for (const NativeDependencyRecord& dependency : inspection.dependencies) {
        result.dependencyGuids.push_back(nativeGuidToText(dependency.dependencyGuid));
    }
    if (result.kind == NativeAssetKind::Mesh) {
        RtmeshPayloadHeader header;
        if (!readPodChunk(bytes, findChunk(inspection, ChunkPayloadHeader), header) || header.vertexStride != sizeof(MeshVertex)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtmesh.header", 0, sizeof(RtmeshPayloadHeader), "Invalid rtmesh payload header"));
            return result;
        }
        result.mesh.name = pathHint.stem().string();
        if (!readVectorChunk(bytes, findChunk(inspection, ChunkMeshVertices), result.mesh.vertices, header.vertexCount) ||
            !readVectorChunk(bytes, findChunk(inspection, ChunkMeshIndices), result.mesh.indices, header.indexCount)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtmesh.buffers", 0, 0, "Invalid rtmesh vertex or index chunk"));
            return result;
        }
        std::vector<RtmeshPrimitiveRecord> primitiveRecords;
        if (!readVectorChunk(bytes, findChunk(inspection, ChunkMeshPrimitives), primitiveRecords, header.primitiveRangeCount)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtmesh.primitives", 0, 0, "Invalid rtmesh primitive chunk"));
            return result;
        }
        const NativeChunkRecord* localBvhNodesChunk = findChunk(inspection, ChunkMeshLocalBvhNodes);
        const NativeChunkRecord* localBvhTrianglesChunk = findChunk(inspection, ChunkMeshLocalBvhTriangles);
        if (localBvhNodesChunk != nullptr &&
            !readVectorChunk(bytes, localBvhNodesChunk, result.mesh.cachedLocalBvhNodes)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtmesh.localBvhNodes", 0, 0, "Invalid rtmesh local BVH node chunk"));
            return result;
        }
        if (localBvhTrianglesChunk != nullptr &&
            !readVectorChunk(bytes, localBvhTrianglesChunk, result.mesh.cachedLocalBvhTriangles)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtmesh.localBvhTriangles", 0, 0, "Invalid rtmesh local BVH triangle chunk"));
            return result;
        }
        result.mesh.primitives.reserve(primitiveRecords.size());
        for (const RtmeshPrimitiveRecord& record : primitiveRecords) {
            MeshPrimitiveAsset primitive;
            primitive.firstIndex = record.firstIndex;
            primitive.indexCount = record.indexCount;
            primitive.firstVertex = record.firstVertex;
            primitive.vertexCount = record.vertexCount;
            primitive.material = record.materialSlot == UINT32_MAX ? MaterialAssetHandle{} : MaterialAssetHandle{record.materialSlot};
            primitive.alphaMode = record.alphaMode;
            primitive.alphaCutoff = record.alphaCutoff;
            primitive.containsAlphaTestedGeometry = (record.flags & 1u) != 0;
            primitive.containsBlendedGeometry = (record.flags & 2u) != 0;
            result.mesh.primitives.push_back(primitive);
        }
    } else if (result.kind == NativeAssetKind::Material) {
        RtmaterialPayloadHeader header;
        if (!readPodChunk(bytes, findChunk(inspection, ChunkPayloadHeader), header)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtmaterial.header", 0, sizeof(RtmaterialPayloadHeader), "Invalid rtmaterial payload header"));
            return result;
        }
        result.material.name = pathHint.stem().string();
        result.material.baseColorFactor = glm::vec4(header.baseColorFactor[0], header.baseColorFactor[1], header.baseColorFactor[2], header.baseColorFactor[3]);
        result.material.emissiveFactor = glm::vec3(header.emissiveFactor[0], header.emissiveFactor[1], header.emissiveFactor[2]);
        result.material.emissiveStrength = header.emissiveFactor[3];
        result.material.metallicFactor = header.metallicFactor;
        result.material.roughnessFactor = header.roughnessFactor;
        result.material.alphaCutoff = header.alphaCutoff;
        result.material.occlusionStrength = header.occlusionStrength;
        result.material.alphaMode = header.alphaMode;
        result.material.shaderCompatibilityMask = header.shaderCompatibilityMask;
        const uint32_t heightScaleBits = static_cast<uint32_t>(header.reserved & 0xffffffffull);
        std::memcpy(&result.material.heightScale, &heightScaleBits, sizeof(result.material.heightScale));
        const uint32_t materialSemanticBits = static_cast<uint32_t>(header.reserved >> 32u);
        result.material.materialWorkflow = (materialSemanticBits >> 0u) & 0xffu;
        result.material.normalMapConvention = (materialSemanticBits >> 8u) & 0xffu;
        result.material.specularTextureAlphaMode = (materialSemanticBits >> 16u) & 0xffu;
        result.material.doubleSided = header.flags & 1u;
        result.material.hasClearcoat = (header.flags >> 1u) & 1u;
        result.material.hasTransmission = (header.flags >> 2u) & 1u;
        result.material.hasVolume = (header.flags >> 3u) & 1u;
        result.material.hasIor = (header.flags >> 4u) & 1u;
        result.material.hasSpecular = (header.flags >> 5u) & 1u;
        result.material.hasSheen = (header.flags >> 6u) & 1u;
        result.material.hasIridescence = (header.flags >> 7u) & 1u;
        result.material.hasAnisotropy = (header.flags >> 8u) & 1u;
        result.material.hasEmissiveStrength = (header.flags >> 9u) & 1u;
        nlohmann::json materialMetadata;
        if (readJsonChunk(bytes, findChunk(inspection, ChunkMetadataJson), materialMetadata)) {
            applyMaterialSemanticMetadata(result.material, materialMetadata);
        }
        std::vector<RtmaterialTextureSlotRecord> slots;
        if (header.textureSlotCount > 0 && !readVectorChunk(bytes, findChunk(inspection, ChunkMaterialTextureSlots), slots, header.textureSlotCount)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtmaterial.textureSlots", 0, 0, "Invalid rtmaterial texture slot chunk"));
            return result;
        }
        for (const RtmaterialTextureSlotRecord& slot : slots) {
            NativeRuntimeTextureBinding binding;
            binding.slot = slot.slot;
            binding.slotName = materialTextureSlotName(slot.slot);
            binding.textureGuid = nativeGuidToText(slot.textureGuid);
            binding.cookedTextureIndex = slot.textureIndex;
            result.materialTextureBindings.push_back(std::move(binding));
        }
        if (const NativeChunkRecord* transformChunk = findChunk(inspection, ChunkMaterialTextureTransforms)) {
            std::vector<RtmaterialTextureTransformRecord> transforms;
            if (!readVectorChunk(bytes, transformChunk, transforms)) {
                result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtmaterial.textureTransforms", 0, 0, "Invalid rtmaterial texture transform chunk"));
                return result;
            }
            const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(transforms.size()), kRtmaterialTextureTransformCount);
            for (uint32_t i = 0; i < count; ++i) {
                assignMaterialTextureTransform(result.material, i, transforms[i]);
            }
        }
    } else if (result.kind == NativeAssetKind::Texture) {
        RttexturePayloadHeader header;
        if (!readPodChunk(bytes, findChunk(inspection, ChunkPayloadHeader), header)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rttexture.header", 0, sizeof(RttexturePayloadHeader), "Invalid rttexture payload header"));
            return result;
        }
        result.texture.name = pathHint.stem().string();
        result.texture.sourcePath = pathHint;
        result.texture.width = header.width;
        result.texture.height = header.height;
        result.texture.mipLevels = static_cast<int>(std::max(1u, header.mipCount));
        result.texture.srgb = header.colorSpace == static_cast<uint32_t>(NativeTextureColorSpace::Srgb);
        result.texture.linearColorSpace = (header.flags & 1u) != 0;
        result.texture.format = static_cast<VkFormat>(header.vkFormat);
        result.texture.isCompressed = header.compression == static_cast<uint32_t>(NativeTextureCompressionPolicy::PreserveSourceContainer) ||
            nativeTextureBlockCompressedFormat(result.texture.format);
        result.texturePayloadFormat = result.texture.format;
        result.textureRole = static_cast<NativeTextureRole>(header.role);
        result.textureFormatSupportPlatform = options.textureFormatSupport.platformName;
        result.texturePayloadVariantPlanJson = nativeDebugRecordValue(inspection, kNativeTexturePayloadVariantPlanDebugKey);
        result.textureSourceContainerKind = nativeDebugRecordValue(inspection, "sourceContainerKind");
        result.textureSourceContainerPreserved = nativeDebugRecordValue(inspection, "sourceContainerPreserved") == "true";
        result.textureSourceContainerTranscoded = nativeDebugRecordValue(inspection, "sourceContainerTranscoded") == "true";
        result.texture.sourceContainerKind = result.textureSourceContainerKind;
        result.texture.sourceContainerPreserved = result.textureSourceContainerPreserved;
        result.texture.sourceContainerTranscoded = result.textureSourceContainerTranscoded;
        result.texture.nativePayloadSource = nativeDebugRecordValue(inspection, "nativePayloadSource");
        result.texture.sourceArrayLayers = nativeDebugRecordUint32(inspection, "sourceArrayLayers", std::max(1u, header.arrayLayers));
        result.texture.sourceDepth = nativeDebugRecordUint32(inspection, "sourceDepth", std::max(1u, header.depth));
        result.texture.sourceFaceCount = nativeDebugRecordUint32(inspection, "sourceFaceCount", 1u);
        result.texture.sourceIsCubemap = nativeDebugRecordValue(inspection, "sourceIsCubemap") == "true";
        const bool ktx2OrBasisContainer = result.textureSourceContainerKind == "ktx2" ||
            result.textureSourceContainerKind == "basisu-ktx2" ||
            result.textureSourceContainerPreserved ||
            result.textureSourceContainerTranscoded;
        if (ktx2OrBasisContainer) {
            result.texturePackageVariantSidecarEligible = true;
            result.texturePackageVariantSidecarImplemented = true;
            result.texturePackageVariantSidecarPolicy = "ktx2-basisu-package-variant-sidecar-cook-emitted";
            result.texturePackageVariantMissingRequirements = {};
        }
        result.texturePayloadFormatSupported = nativeTextureFormatSupportedByPolicy(result.texturePayloadFormat, options.textureFormatSupport);
        if (options.rejectUnsupportedTextureFormats && !result.texturePayloadFormatSupported) {
            result.errors.push_back(makeLoaderError(
                NativeBinaryErrorCode::UnsupportedPlatformFeature,
                pathHint,
                "rttexture.format",
                0,
                0,
                "Native rttexture payload format " + nativeTextureFormatName(result.texturePayloadFormat) +
                    " is not sampled-image supported by texture format profile '" + options.textureFormatSupport.platformName + "'"));
            return result;
        }
        result.texture.compressedFormat = result.texture.isCompressed ? static_cast<VkFormat>(header.vkFormat) : VK_FORMAT_UNDEFINED;
        result.texture.sampler = unpackSampler(header.sampler);
        std::vector<RttextureMipRecord> mipRecords;
        if (!readVectorChunk(bytes, findChunk(inspection, ChunkTextureMipTable), mipRecords, header.mipCount) ||
            !readByteChunk(bytes, findChunk(inspection, ChunkTexturePayload), result.texture.rgba8)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rttexture.payload", 0, 0, "Invalid rttexture mip or payload chunk"));
            return result;
        }
        result.texture.mipData.reserve(mipRecords.size());
        for (const RttextureMipRecord& record : mipRecords) {
            result.texture.mipData.push_back(TextureMipLevel{.offset = record.offset, .size = record.size, .width = record.width, .height = record.height});
        }
    } else if (result.kind == NativeAssetKind::Skeleton) {
        result.skeleton = RuntimeSkeleton::loadNativeBytes(pathHint, bytes, &result.warnings);
        if (!result.skeleton.valid()) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtskeleton.payload", 0, 0, "Invalid rtskeleton runtime payload"));
            return result;
        }
    } else if (result.kind == NativeAssetKind::Animation) {
        result.animationClip = AnimationClip::loadRtanimNativeBytes(pathHint, bytes, &result.warnings);
        if (!result.animationClip.valid()) {
            (void)loadAnimationMetadataBridge(inspection, bytes, result.animationMetadataBridge);
        }
        if (!result.animationClip.valid() && !result.animationMetadataBridge.available) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtanim.payload", 0, 0, "Invalid rtanim runtime payload"));
            return result;
        }
    } else if (result.kind == NativeAssetKind::AnimationController) {
        result.animationController = AnimationController::loadNativeBytes(pathHint, bytes, &result.warnings);
        if (!result.animationController.valid()) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtanimcontroller.payload", 0, 0, "Invalid rtanimcontroller runtime payload"));
            return result;
        }
    } else if (result.kind == NativeAssetKind::SkeletalMesh) {
        RtskeletalMeshPayloadHeader header;
        if (!readPodChunk(bytes, findChunk(inspection, kRtskeletalMeshChunkPayloadHeader), header)) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtskeletalmesh.header", 0, sizeof(RtskeletalMeshPayloadHeader), "Invalid rtskeletalmesh payload header"));
            return result;
        }
        if (header.version != kRtskeletalMeshPayloadVersion) {
            result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::UnsupportedVersion, pathHint, "rtskeletalmesh.header", 0, sizeof(RtskeletalMeshPayloadHeader), "Unsupported rtskeletalmesh payload version"));
            return result;
        }
        result.skeletalMeshBinding.meshGuid = nativeGuidToText(header.meshGuid);
        result.skeletalMeshBinding.skeletonGuid = nativeGuidToText(header.skeletonGuid);
        result.skeletalMeshBinding.jointRemapCount = header.jointRemapCount;
        result.skeletalMeshBinding.jointRemapChunk = header.jointRemapChunk;
        result.skeletalMeshBinding.skinningDataChunk = header.skinningDataChunk;
        result.skeletalMeshBinding.bindMetadataChunk = header.bindMetadataChunk;
        result.skeletalMeshBinding.flags = header.flags;

        if (header.jointRemapCount > 0) {
            std::vector<uint32_t> jointRemap;
            const uint32_t chunkType = header.jointRemapChunk != 0u ? header.jointRemapChunk : kRtskeletalMeshChunkJointRemap;
            if (!readVectorChunk(bytes, findChunk(inspection, chunkType), jointRemap, header.jointRemapCount)) {
                result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtskeletalmesh.jointRemap", 0, 0, "Invalid rtskeletalmesh joint remap chunk"));
                return result;
            }
            result.skeletalMeshBinding.hasJointRemap = true;
        }
        if (header.skinningDataChunk != 0u) {
            const NativeChunkRecord* skinningChunk = findChunk(inspection, header.skinningDataChunk);
            if (skinningChunk == nullptr || !rangeInside(skinningChunk->offset, skinningChunk->size, bytes.size())) {
                result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtskeletalmesh.skinningData", 0, 0, "Invalid rtskeletalmesh skinning data chunk"));
                return result;
            }
            result.skeletalMeshBinding.hasSkinningData = skinningChunk->size > 0;
        }
        if (header.bindMetadataChunk != 0u) {
            const NativeChunkRecord* metadataChunk = findChunk(inspection, header.bindMetadataChunk);
            if (metadataChunk == nullptr || !rangeInside(metadataChunk->offset, metadataChunk->size, bytes.size())) {
                result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::CorruptTable, pathHint, "rtskeletalmesh.bindMetadata", 0, 0, "Invalid rtskeletalmesh bind metadata chunk"));
                return result;
            }
            result.skeletalMeshBinding.hasBindMetadata = metadataChunk->size > 0;
        }
    } else {
        result.errors.push_back(makeLoaderError(NativeBinaryErrorCode::UnsupportedPlatformFeature, pathHint, "assetKind", 0, 0, "Native runtime loader currently supports mesh, material, texture, skeleton, animation, animation controller, and skeletal mesh binding payloads"));
        return result;
    }

    result.ok = true;
    return result;
}

NativeRuntimeLoadReport NativeAssetRuntimeLoader::loadLooseRoot(const std::filesystem::path& root, AssetManager* manager, const NativeRuntimeLoadOptions& options) const {
    NativeRuntimeLoadReport report;
    report.sourceRoot = root;
    report.options = options;
    std::unordered_map<std::string, TextureAssetHandle> textureHandlesByGuid;
    std::unordered_map<std::string, MaterialAssetHandle> materialHandlesByGuid;
    std::unordered_map<std::string, NativeAssetStoreObject> objectsByGuid;
    std::unordered_set<std::string> looseFileAllowSet;
    looseFileAllowSet.reserve(options.looseFileAllowList.size());
    for (const std::filesystem::path& allowedPath : options.looseFileAllowList) {
        if (!allowedPath.empty()) {
            looseFileAllowSet.insert(nativeRuntimePathKey(allowedPath));
        }
    }
    auto looseFileAllowed = [&](const std::filesystem::path& path) {
        return looseFileAllowSet.empty() || looseFileAllowSet.find(nativeRuntimePathKey(path)) != looseFileAllowSet.end();
    };

    auto rememberObjects = [&](const NativeAssetStoreInspection& inspection) {
        for (const NativeAssetStoreObject& object : inspection.objects) {
            objectsByGuid[object.guid] = object;
        }
        for (const std::string& missing : inspection.missingDependencies) {
            report.warnings.push_back("Native asset store missing dependency: " + missing);
        }
    };

    auto addAsset = [&](NativeRuntimeLoadedAsset asset, const NativeAssetStoreObject* sourceObject = nullptr, bool registerWithManager = true) {
        const NativeAssetStoreObject* object = nullptr;
        if (sourceObject != nullptr) {
            object = sourceObject;
        } else if (!asset.guid.empty()) {
            const auto objectIt = objectsByGuid.find(asset.guid);
            if (objectIt != objectsByGuid.end()) {
                object = &objectIt->second;
            }
        }
        if (asset.ok && asset.kind == NativeAssetKind::Texture) {
            applyNativeObjectTextureMetadata(asset.texture, asset.guid, object);
            asset.texture.resident = manager != nullptr && registerWithManager && asset.texturePayloadFormatSupported;
        }
        if (asset.ok && asset.kind == NativeAssetKind::Material) {
            applyNativeObjectMaterialMetadata(asset.material, asset.guid, object);
        }
        if (asset.ok && asset.kind == NativeAssetKind::Mesh) {
            applyNativeObjectMeshMetadata(asset.mesh, asset.guid, object);
        }
        if (asset.ok && asset.kind == NativeAssetKind::Material && manager != nullptr) {
            resolveNativeMaterialTextureBindings(asset, manager, textureHandlesByGuid, objectsByGuid);
        }
        if (asset.ok && asset.kind == NativeAssetKind::Mesh && manager != nullptr) {
            resolveNativeMeshMaterialSlots(asset, manager, materialHandlesByGuid, objectsByGuid);
        }
        const uint64_t texturePayloadBytes = asset.ok && asset.kind == NativeAssetKind::Texture
            ? static_cast<uint64_t>(asset.texture.rgba8.size())
            : 0ull;
        const uint64_t meshVertexCount = asset.ok && asset.kind == NativeAssetKind::Mesh
            ? static_cast<uint64_t>(asset.mesh.vertices.size())
            : 0ull;
        const uint64_t meshIndexCount = asset.ok && asset.kind == NativeAssetKind::Mesh
            ? static_cast<uint64_t>(asset.mesh.indices.size())
            : 0ull;
        const bool keepReportPayload = options.retainLoadedPayloadsInReport;
        TextureAsset textureSummary;
        if (asset.ok && asset.kind == NativeAssetKind::Texture && !keepReportPayload) {
            textureSummary.name = asset.texture.name;
            textureSummary.sourcePath = asset.texture.sourcePath;
            textureSummary.width = asset.texture.width;
            textureSummary.height = asset.texture.height;
            textureSummary.channels = asset.texture.channels;
            textureSummary.sourceArrayLayers = asset.texture.sourceArrayLayers;
            textureSummary.sourceDepth = asset.texture.sourceDepth;
            textureSummary.sourceFaceCount = asset.texture.sourceFaceCount;
            textureSummary.sourceIsCubemap = asset.texture.sourceIsCubemap;
            textureSummary.mipLevels = asset.texture.mipLevels;
            textureSummary.srgb = asset.texture.srgb;
            textureSummary.resident = asset.texture.resident;
            textureSummary.fallback = asset.texture.fallback;
            textureSummary.isCompressed = asset.texture.isCompressed;
            textureSummary.linearColorSpace = asset.texture.linearColorSpace;
            textureSummary.format = asset.texture.format;
            textureSummary.compressedFormat = asset.texture.compressedFormat;
            textureSummary.sampler = asset.texture.sampler;
            textureSummary.nativeGuid = asset.texture.nativeGuid;
            textureSummary.nativeSource = asset.texture.nativeSource;
            textureSummary.nativePath = asset.texture.nativePath;
            textureSummary.sourceContainerKind = asset.texture.sourceContainerKind;
            textureSummary.nativePayloadSource = asset.texture.nativePayloadSource;
            textureSummary.authoredRole = asset.texture.authoredRole;
            textureSummary.sourceContainerPreserved = asset.texture.sourceContainerPreserved;
            textureSummary.sourceContainerTranscoded = asset.texture.sourceContainerTranscoded;
            textureSummary.mipData = asset.texture.mipData;
        }
        MeshAsset meshSummary;
        if (asset.ok && asset.kind == NativeAssetKind::Mesh && !keepReportPayload) {
            meshSummary.name = asset.mesh.name;
            meshSummary.nativeGuid = asset.mesh.nativeGuid;
            meshSummary.nativeSource = asset.mesh.nativeSource;
            meshSummary.nativePath = asset.mesh.nativePath;
        }
        MaterialAsset materialSummary;
        if (asset.ok && asset.kind == NativeAssetKind::Material && !keepReportPayload) {
            materialSummary = asset.material;
        }
        if (!asset.textureVariantCandidate || asset.textureVariantSelected) {
            appendDirectStoreUploadPlanForAsset(report, asset, object);
        }
        if (asset.ok && manager != nullptr && registerWithManager) {
            if (asset.kind == NativeAssetKind::Texture) {
                asset.textureHandle = keepReportPayload
                    ? manager->addTexture(asset.texture)
                    : manager->addTexture(std::move(asset.texture));
                if (!asset.guid.empty()) {
                    textureHandlesByGuid[asset.guid] = asset.textureHandle;
                    if (object != nullptr) {
                        objectsByGuid[asset.guid] = *object;
                    }
                }
            } else if (asset.kind == NativeAssetKind::Material) {
                asset.materialHandle = keepReportPayload
                    ? manager->addMaterial(asset.material)
                    : manager->addMaterial(std::move(asset.material));
                if (!asset.guid.empty()) {
                    materialHandlesByGuid[asset.guid] = asset.materialHandle;
                }
            } else if (asset.kind == NativeAssetKind::Mesh) {
                asset.meshHandle = keepReportPayload
                    ? manager->addMesh(asset.mesh)
                    : manager->addMesh(std::move(asset.mesh));
            }
        }
        if (asset.ok && !keepReportPayload) {
            if (asset.kind == NativeAssetKind::Texture) {
                asset.texture = std::move(textureSummary);
            } else if (asset.kind == NativeAssetKind::Material) {
                asset.material = std::move(materialSummary);
            } else if (asset.kind == NativeAssetKind::Mesh) {
                asset.mesh = std::move(meshSummary);
            }
        }
        if (asset.ok && manager != nullptr) {
            report.rendererUploadPlan.available = true;
            report.rendererUploadPlan.assetManagerBacked = true;
            if (object != nullptr && object->source == NativeAssetStoreSource::Package) {
                report.rendererUploadPlan.packageBacked = true;
            }
            if (asset.kind == NativeAssetKind::Texture && asset.textureHandle.valid()) {
                ++report.rendererUploadPlan.textureCount;
                if (asset.texture.resident) {
                    ++report.rendererUploadPlan.textureResidentCount;
                }
                report.rendererUploadPlan.textureUploadBytes += texturePayloadBytes;
            } else if (asset.kind == NativeAssetKind::Material && asset.materialHandle.valid()) {
                ++report.rendererUploadPlan.materialCount;
            } else if (asset.kind == NativeAssetKind::Mesh && asset.meshHandle.valid()) {
                ++report.rendererUploadPlan.meshCount;
                report.rendererUploadPlan.vertexUploadBytes += sizeof(MeshVertex) * meshVertexCount;
                report.rendererUploadPlan.indexUploadBytes += sizeof(uint32_t) * meshIndexCount;
            }
        }
        if (asset.ok) {
            if (asset.kind == NativeAssetKind::Texture) {
                ++report.textureCount;
                report.totalTextureBytes += texturePayloadBytes;
            } else if (asset.kind == NativeAssetKind::Material) {
                ++report.materialCount;
            } else if (asset.kind == NativeAssetKind::Mesh) {
                ++report.meshCount;
                report.totalVertexCount += meshVertexCount;
                report.totalIndexCount += meshIndexCount;
            } else if (asset.kind == NativeAssetKind::Skeleton) {
                ++report.skeletonCount;
            } else if (asset.kind == NativeAssetKind::Animation) {
                ++report.animationCount;
            } else if (asset.kind == NativeAssetKind::AnimationController) {
                ++report.animationControllerCount;
            } else if (asset.kind == NativeAssetKind::SkeletalMesh) {
                ++report.skeletalMeshCount;
            }
        } else {
            report.errors.insert(report.errors.end(), asset.errors.begin(), asset.errors.end());
        }
        report.assets.push_back(std::move(asset));
    };

    const NativeAssetKind rootKind = nativeAssetKindFromExtension(root);
    std::error_code ec;
    auto sortObjectsForRuntimeLoad = [](std::vector<NativeAssetStoreObject>& objects) {
        std::sort(objects.begin(), objects.end(), [](const NativeAssetStoreObject& a, const NativeAssetStoreObject& b) {
            auto rank = [](NativeAssetKind kind) {
                if (kind == NativeAssetKind::Texture) return 0;
                if (kind == NativeAssetKind::Material) return 1;
                if (kind == NativeAssetKind::Mesh) return 2;
                if (kind == NativeAssetKind::Skeleton) return 3;
                if (kind == NativeAssetKind::Animation) return 4;
                if (kind == NativeAssetKind::AnimationController) return 5;
                if (kind == NativeAssetKind::SkeletalMesh) return 6;
                return 7;
            };
            const int ar = rank(a.kind);
            const int br = rank(b.kind);
            if (ar != br) {
                return ar < br;
            }
            return nativeObjectRuntimePath(a).generic_string() < nativeObjectRuntimePath(b).generic_string();
        });
    };

    auto loadStoreObjects = [&](NativeAssetStore& store, const NativeAssetStoreInspection& inspection) {
        rememberObjects(inspection);
        std::vector<NativeAssetStoreObject> objects;
        std::unordered_map<std::string, uint32_t> textureVariantCounts;
        for (const NativeAssetStoreObject& object : inspection.objects) {
            if (object.kind == NativeAssetKind::Mesh || object.kind == NativeAssetKind::Material || object.kind == NativeAssetKind::Texture ||
                object.kind == NativeAssetKind::Skeleton || object.kind == NativeAssetKind::Animation || object.kind == NativeAssetKind::AnimationController ||
                object.kind == NativeAssetKind::SkeletalMesh) {
                if (object.source == NativeAssetStoreSource::LooseFile && !looseFileAllowed(nativeObjectRuntimePath(object))) {
                    continue;
                }
                objects.push_back(object);
                if (object.kind == NativeAssetKind::Texture) {
                    ++textureVariantCounts[object.guid];
                }
            }
        }
        sortObjectsForRuntimeLoad(objects);
        auto loadStoreObject = [&](const NativeAssetStoreObject& object, const NativeRuntimeLoadOptions& loadOptions) {
            NativeBinaryError error;
            const std::vector<std::byte> bytes = store.readObjectBytes(object, &error);
            const std::filesystem::path pathHint = nativeObjectRuntimePath(object);
            if (bytes.empty()) {
                NativeRuntimeLoadedAsset asset;
                asset.path = pathHint;
                asset.kind = object.kind;
                asset.guid = object.guid;
                asset.errors.push_back(error);
                return asset;
            }
            NativeRuntimeLoadedAsset asset = loadBytes(pathHint, bytes, loadOptions);
            asset.guid = object.guid;
            return asset;
        };
        std::unordered_set<std::string> processedTextureVariantGuids;
        for (const NativeAssetStoreObject& object : objects) {
            const uint32_t textureVariantCount = object.kind == NativeAssetKind::Texture ? textureVariantCounts[object.guid] : 0u;
            if (textureVariantCount > 1u) {
                if (!processedTextureVariantGuids.insert(object.guid).second) {
                    continue;
                }
                std::vector<NativeAssetStoreObject> variants;
                for (const NativeAssetStoreObject& candidate : objects) {
                    if (candidate.kind == NativeAssetKind::Texture && candidate.guid == object.guid) {
                        variants.push_back(candidate);
                    }
                }
                std::vector<NativeRuntimeLoadedAsset> candidateAssets;
                candidateAssets.reserve(variants.size());
                NativeRuntimeLoadOptions candidateOptions = options;
                candidateOptions.rejectUnsupportedTextureFormats = false;
                size_t selectedIndex = SIZE_MAX;
                for (const NativeAssetStoreObject& variant : variants) {
                    NativeRuntimeLoadedAsset asset = loadStoreObject(variant, candidateOptions);
                    asset.textureVariantCandidate = true;
                    asset.textureVariantCandidateCount = static_cast<uint32_t>(variants.size());
                    if (asset.ok && asset.kind == NativeAssetKind::Texture && asset.texturePayloadFormatSupported) {
                        if (selectedIndex == SIZE_MAX || betterNativeTextureVariant(asset, candidateAssets[selectedIndex])) {
                            selectedIndex = candidateAssets.size();
                        }
                    }
                    candidateAssets.push_back(std::move(asset));
                }
                if (selectedIndex == SIZE_MAX && options.rejectUnsupportedTextureFormats && !candidateAssets.empty()) {
                    candidateAssets.front().ok = false;
                    candidateAssets.front().errors.push_back(makeLoaderError(
                        NativeBinaryErrorCode::UnsupportedPlatformFeature,
                        candidateAssets.front().path,
                        "rttexture.variant_selection",
                        0,
                        0,
                        "No duplicate native texture variant for GUID " + object.guid + " is sampled-image supported by texture format profile '" + options.textureFormatSupport.platformName + "'"));
                }
                for (size_t i = 0; i < candidateAssets.size(); ++i) {
                    NativeRuntimeLoadedAsset& asset = candidateAssets[i];
                    asset.textureVariantSelected = i == selectedIndex;
                    if (asset.textureVariantSelected) {
                        asset.textureVariantSelectionReason = "selected_by_active_native_texture_format_profile:" + options.textureFormatSupport.platformName;
                    } else if (!asset.texturePayloadFormatSupported) {
                        asset.textureVariantSelectionReason = "not_selected_unsupported_payload_format_for_active_profile:" + options.textureFormatSupport.platformName;
                    } else {
                        asset.textureVariantSelectionReason = "not_selected_lower_ranked_supported_variant_for_active_profile:" + options.textureFormatSupport.platformName;
                    }
                    addAsset(std::move(asset), &variants[i], i == selectedIndex);
                }
                continue;
            }
            NativeRuntimeLoadedAsset asset = loadStoreObject(object, options);
            addAsset(std::move(asset), &object);
        }
    };

    auto finalizeReport = [&]() {
        resolveAnimationControllerClipBindings(report);
        buildDirectStoreUploadTicketQueueSimulation(report.directStoreUploadPlan);
        report.sceneAsset = buildNativeRuntimeSceneAsset(report, root);
        report.sceneAssetPlan = makeSceneAssetPlan(report, report.sceneAsset);
        finalizeLegacyCpuLoadPolicy(report);
    };

    if (std::filesystem::is_regular_file(root, ec) && rootKind == NativeAssetKind::Package) {
        NativeAssetStore store;
        NativeAssetStoreMountReport mount = store.mountPackage(root);
        report.warnings.insert(report.warnings.end(), mount.warnings.begin(), mount.warnings.end());
        if (!mount.ok) {
            report.errors.insert(report.errors.end(), mount.errors.begin(), mount.errors.end());
            return report;
        }
        NativeAssetStoreInspection inspection = store.inspect();
        loadStoreObjects(store, inspection);
        report.ok = report.errors.empty() && (!report.assets.empty());
        finalizeReport();
        return report;
    }

    if (std::filesystem::is_directory(root, ec)) {
        if (!options.validatePayloadHashes) {
            std::vector<NativeAssetStoreObject> objects;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) {
                    report.warnings.push_back("Stopped fast loose native asset scan: " + ec.message());
                    break;
                }
                std::error_code entryError;
                if (!entry.is_regular_file(entryError)) {
                    continue;
                }
                const NativeAssetKind kind = nativeAssetKindFromExtension(entry.path());
                if (kind == NativeAssetKind::Unknown || kind == NativeAssetKind::Package) {
                    continue;
                }
                if (kind != NativeAssetKind::Mesh && kind != NativeAssetKind::Material && kind != NativeAssetKind::Texture &&
                    kind != NativeAssetKind::Skeleton && kind != NativeAssetKind::Animation && kind != NativeAssetKind::AnimationController &&
                    kind != NativeAssetKind::SkeletalMesh) {
                    continue;
                }
                if (!looseFileAllowed(entry.path())) {
                    continue;
                }
                NativeAssetStoreObject object;
                object.kind = kind;
                object.source = NativeAssetStoreSource::LooseFile;
                object.path = entry.path();
                object.size = static_cast<uint64_t>(entry.file_size(entryError));
                object.payloadHashValid = false;
                objects.push_back(std::move(object));
            }
            sortObjectsForRuntimeLoad(objects);
            for (NativeAssetStoreObject& object : objects) {
                NativeRuntimeLoadedAsset asset = loadStandalone(object.path, options);
                if (!asset.guid.empty()) {
                    object.guid = asset.guid;
                    objectsByGuid[asset.guid] = object;
                }
                addAsset(std::move(asset), &object);
            }
            report.ok = report.errors.empty() && (!report.assets.empty());
            finalizeReport();
            return report;
        }
        NativeAssetStore store;
        NativeAssetStoreMountReport mount = store.mountLooseRoot(root);
        report.warnings.insert(report.warnings.end(), mount.warnings.begin(), mount.warnings.end());
        report.errors.insert(report.errors.end(), mount.errors.begin(), mount.errors.end());
        NativeAssetStoreInspection inspection = store.inspect();
        loadStoreObjects(store, inspection);
    } else if (std::filesystem::is_regular_file(root, ec) && nativeAssetKindFromExtension(root) != NativeAssetKind::Unknown && nativeAssetKindFromExtension(root) != NativeAssetKind::Package) {
        NativeRuntimeLoadedAsset asset = loadStandalone(root, options);
        if (asset.ok && (asset.kind == NativeAssetKind::Mesh || asset.kind == NativeAssetKind::Material || asset.kind == NativeAssetKind::Texture ||
                asset.kind == NativeAssetKind::Skeleton || asset.kind == NativeAssetKind::Animation || asset.kind == NativeAssetKind::AnimationController ||
                asset.kind == NativeAssetKind::SkeletalMesh)) {
            NativeAssetStoreObject object;
            object.guid = asset.guid;
            object.kind = asset.kind;
            object.source = NativeAssetStoreSource::LooseFile;
            object.path = root;
            object.size = static_cast<uint64_t>(std::filesystem::file_size(root, ec));
            objectsByGuid[object.guid] = object;
        }
        addAsset(std::move(asset));
    } else {
        report.errors.push_back(makeLoaderError(NativeBinaryErrorCode::IoError, root, "root", 0, 0, "Native runtime input is not a native file or directory"));
        return report;
    }
    report.ok = report.errors.empty() && (!report.assets.empty());
    finalizeReport();
    return report;
}

nlohmann::json gpuUploadChunkSnapshotJson(const GpuUploadChunkSnapshot& chunk) {
    return {
        {"index", chunk.index},
        {"offset", chunk.offset},
        {"bytes", chunk.bytes},
        {"timeline_value", chunk.timelineValue},
        {"state", gpuUploadChunkStateName(chunk.state)},
        {"staging_retained", chunk.stagingRetained},
    };
}

nlohmann::json gpuUploadTicketSnapshotJson(const GpuUploadTicketSnapshot& ticket) {
    nlohmann::json chunks = nlohmann::json::array();
    for (const GpuUploadChunkSnapshot& chunk : ticket.chunks) {
        chunks.push_back(gpuUploadChunkSnapshotJson(chunk));
    }
    return {
        {"id", ticket.id},
        {"kind", gpuUploadResourceKindName(ticket.kind)},
        {"state", gpuUploadTicketStateName(ticket.state)},
        {"label", ticket.label},
        {"total_bytes", ticket.totalBytes},
        {"submitted_bytes", ticket.submittedBytes},
        {"completed_bytes", ticket.completedBytes},
        {"retained_staging_bytes", ticket.retainedStagingBytes},
        {"chunk_count", ticket.chunkCount},
        {"pending_chunks", ticket.pendingChunks},
        {"submitted_chunks", ticket.submittedChunks},
        {"completed_chunks", ticket.completedChunks},
        {"cancellation_requested", ticket.cancellationRequested},
        {"can_cancel", ticket.canCancel},
        {"can_retire", ticket.canRetire},
        {"chunks", chunks},
    };
}

nlohmann::json directStoreUploadTicketQueueSimulationJson(const NativeRuntimeDirectStoreUploadPlan& plan) {
    nlohmann::json tickets = nlohmann::json::array();
    for (const GpuUploadTicketSnapshot& snapshot : plan.uploadTicketQueueSnapshots) {
        tickets.push_back(gpuUploadTicketSnapshotJson(snapshot));
    }
    return {
        {"available", plan.uploadTicketQueueSimulationAvailable},
        {"executable", plan.uploadTicketQueueSimulationExecutable},
        {"frame_budget_bytes", plan.uploadTicketQueueFrameBudgetBytes},
        {"frame_count", plan.uploadTicketQueueFrameCount},
        {"submitted_bytes", plan.uploadTicketQueueSubmittedBytes},
        {"completed_bytes", plan.uploadTicketQueueCompletedBytes},
        {"next_timeline_value", plan.uploadTicketQueueNextTimelineValue},
        {"policy", plan.uploadTicketQueuePolicy},
        {"tickets", tickets},
    };
}

nlohmann::json nativeRuntimeLoadedAssetToJson(const NativeRuntimeLoadedAsset& asset) {
    nlohmann::json errors = nlohmann::json::array();
    for (const NativeBinaryError& error : asset.errors) {
        errors.push_back({{"code", nativeBinaryErrorCodeName(error.code)}, {"path", error.path.generic_string()}, {"table", error.table}, {"message", error.message}});
    }
    nlohmann::json details = nlohmann::json::object();
    if (asset.kind == NativeAssetKind::Texture) {
        details = {
            {"width", asset.texture.width},
            {"height", asset.texture.height},
            {"mipLevels", asset.texture.mipLevels},
            {"role", nativeTextureRoleName(asset.textureRole)},
            {"roleValue", static_cast<uint32_t>(asset.textureRole)},
            {"payloadFormat", nativeTextureFormatName(asset.texturePayloadFormat)},
            {"payloadFormatValue", static_cast<uint32_t>(asset.texturePayloadFormat)},
            {"payloadFormatSupported", asset.texturePayloadFormatSupported},
            {"variantCandidate", asset.textureVariantCandidate},
            {"variantSelected", asset.textureVariantSelected},
            {"variantCandidateCount", asset.textureVariantCandidateCount},
            {"variantSelectionReason", asset.textureVariantSelectionReason},
            {"textureFormatSupportPlatform", asset.textureFormatSupportPlatform},
            {"sourceContainerKind", asset.textureSourceContainerKind},
            {"sourceContainerPreserved", asset.textureSourceContainerPreserved},
            {"sourceContainerTranscoded", asset.textureSourceContainerTranscoded},
            {"packageVariantSidecarEligible", asset.texturePackageVariantSidecarEligible},
            {"packageVariantSidecarImplemented", asset.texturePackageVariantSidecarImplemented},
            {"packageVariantSidecarPolicy", asset.texturePackageVariantSidecarPolicy},
            {"packageVariantMissingRequirements", asset.texturePackageVariantMissingRequirements},
            {"payloadBytes", asset.texture.rgba8.size()},
            {"handle", asset.textureHandle.index},
            {"nativeGuid", asset.texture.nativeGuid},
            {"nativeSource", asset.texture.nativeSource},
            {"nativePath", asset.texture.nativePath.empty() ? std::string{} : asset.texture.nativePath.generic_string()},
            {"resident", asset.texture.resident},
            {"fallback", asset.texture.fallback},
        };
        if (!asset.texturePayloadVariantPlanJson.empty()) {
            try {
                details[std::string(kNativeTexturePayloadVariantPlanDebugKey)] = nlohmann::json::parse(asset.texturePayloadVariantPlanJson);
            } catch (const std::exception&) {
                details[std::string(kNativeTexturePayloadVariantPlanDebugKey)] = asset.texturePayloadVariantPlanJson;
            }
        }
    } else if (asset.kind == NativeAssetKind::Material) {
        nlohmann::json bindings = nlohmann::json::array();
        for (const NativeRuntimeTextureBinding& binding : asset.materialTextureBindings) {
            bindings.push_back({
                {"slot", binding.slot},
                {"slotName", binding.slotName},
                {"textureGuid", binding.textureGuid},
                {"cookedTextureIndex", binding.cookedTextureIndex},
                {"textureHandle", binding.textureHandle.index},
                {"nativeSource", binding.nativeSource},
                {"nativePath", binding.nativePath.empty() ? std::string{} : binding.nativePath.generic_string()},
                {"resolved", binding.resolved},
                {"resident", binding.resident},
                {"fallback", binding.fallback},
                {"missing", binding.missing},
                {"repairAction", binding.repairAction},
            });
        }
        details = {
            {"textureDependencyCount", asset.dependencyGuids.size()},
            {"textureBindingCount", asset.materialTextureBindings.size()},
            {"textureBindings", bindings},
            {"baseColor", {asset.material.baseColorFactor.x, asset.material.baseColorFactor.y, asset.material.baseColorFactor.z, asset.material.baseColorFactor.w}},
            {"handle", asset.materialHandle.index},
            {"nativeGuid", asset.material.nativeGuid},
            {"nativeSource", asset.material.nativeSource},
            {"nativePath", asset.material.nativePath.empty() ? std::string{} : asset.material.nativePath.generic_string()},
        };
    } else if (asset.kind == NativeAssetKind::Mesh) {
        details = {
            {"vertexCount", asset.mesh.vertices.size()},
            {"indexCount", asset.mesh.indices.size()},
            {"primitiveCount", asset.mesh.primitives.size()},
            {"cachedLocalBvhNodeCount", asset.mesh.cachedLocalBvhNodes.size() / 4u},
            {"cachedLocalBvhTriangleCount", asset.mesh.cachedLocalBvhTriangles.size() / 12u},
            {"cachedLocalBvhPayloadBytes", sizeof(glm::vec4) * (asset.mesh.cachedLocalBvhNodes.size() + asset.mesh.cachedLocalBvhTriangles.size())},
            {"handle", asset.meshHandle.index},
            {"nativeGuid", asset.mesh.nativeGuid},
            {"nativeSource", asset.mesh.nativeSource},
            {"nativePath", asset.mesh.nativePath.empty() ? std::string{} : asset.mesh.nativePath.generic_string()},
        };
    } else if (asset.kind == NativeAssetKind::Skeleton) {
        details = {
            {"name", asset.skeleton.name()},
            {"jointCount", asset.skeleton.joints().size()},
            {"skeletonRoot", asset.skeleton.skeletonRoot()},
            {"runtimeStructure", "RuntimeSkeleton"},
        };
    } else if (asset.kind == NativeAssetKind::Animation) {
        if (asset.animationMetadataBridge.available && !asset.animationClip.valid()) {
            details[kNameKey] = asset.animationMetadataBridge.name;
            details[kSchemaKey] = asset.animationMetadataBridge.schema;
            details[kSourceFormatKey] = asset.animationMetadataBridge.sourceFormat;
            details[kChannelCountKey] = asset.animationMetadataBridge.channelCount;
            details[kDecodedChannelCountKey] = asset.animationMetadataBridge.decodedChannelCount;
            details[kDecodedKeyframeCountKey] = asset.animationMetadataBridge.decodedKeyframeCount;
            details[kTrackCountKey] = asset.animationMetadataBridge.trackCount;
            details[kEventCountKey] = asset.animationMetadataBridge.eventCount;
            details[kDurationKey] = asset.animationMetadataBridge.durationSeconds;
            details[kRootMotionCandidateCountKey] = asset.animationMetadataBridge.rootMotionCandidateCount;
            details[kRuntimeSupportKey] = asset.animationMetadataBridge.runtimeSupport;
            details[kPlaybackImplementedKey] = false;
            details[kRuntimeStructureKey] = kAnimationMetadataBridgeValue;
        } else {
        details = {
            {"name", asset.animationClip.name()},
            {"trackCount", asset.animationClip.trackCount()},
            {"eventCount", asset.animationClip.events().size()},
            {"duration", asset.animationClip.duration()},
            {"rootMotionCandidateCount", asset.animationClip.rootMotionCandidateCount()},
            {"runtimeStructure", "AnimationClip"},
        };
        }
    } else if (asset.kind == NativeAssetKind::AnimationController) {
        nlohmann::json clipBindings = nlohmann::json::array();
        uint32_t resolvedClipBindingCount = 0;
        for (const NativeRuntimeAnimationClipBinding& binding : asset.animationClipBindings) {
            if (binding.resolved) {
                ++resolvedClipBindingCount;
            }
            clipBindings.push_back({
                {"source", binding.source},
                {"clipGuid", binding.clipGuid},
                {"clipPath", binding.clipPath.empty() ? std::string{} : binding.clipPath.generic_string()},
                {"resolved", binding.resolved},
                {"resolvedNativePath", binding.resolvedNativePath.empty() ? std::string{} : binding.resolvedNativePath.generic_string()},
            });
        }
        details = {
            {"name", asset.animationController.name()},
            {"initialState", asset.animationController.initialState()},
            {"parameterCount", asset.animationController.parameters().size()},
            {"stateCount", asset.animationController.states().size()},
            {"layerCount", asset.animationController.layers().size()},
            {"avatarMaskCount", asset.animationController.avatarMasks().size()},
            {"clipBindingCount", asset.animationClipBindings.size()},
            {"resolvedClipBindingCount", resolvedClipBindingCount},
            {"clipBindings", clipBindings},
            {"runtimeStructure", "AnimationController"},
        };
    } else if (asset.kind == NativeAssetKind::SkeletalMesh) {
        details = {
            {"meshGuid", asset.skeletalMeshBinding.meshGuid},
            {"skeletonGuid", asset.skeletalMeshBinding.skeletonGuid},
            {"jointRemapCount", asset.skeletalMeshBinding.jointRemapCount},
            {"jointRemapChunk", asset.skeletalMeshBinding.jointRemapChunk},
            {"skinningDataChunk", asset.skeletalMeshBinding.skinningDataChunk},
            {"bindMetadataChunk", asset.skeletalMeshBinding.bindMetadataChunk},
            {"flags", asset.skeletalMeshBinding.flags},
            {"hasJointRemap", asset.skeletalMeshBinding.hasJointRemap},
            {"hasSkinningData", asset.skeletalMeshBinding.hasSkinningData},
            {"hasBindMetadata", asset.skeletalMeshBinding.hasBindMetadata},
            {"runtimeStructure", "NativeSkeletalMeshBinding"},
        };
    }
    return {
        {"ok", asset.ok},
        {"path", asset.path.generic_string()},
        {"guid", asset.guid},
        {"kind", nativeAssetKindName(asset.kind)},
        {"dependencyGuids", asset.dependencyGuids},
        {"details", details},
        {"warnings", asset.warnings},
        {"errors", errors},
    };
}

nlohmann::json nativeRuntimeLoadReportToJson(const NativeRuntimeLoadReport& report) {
    nlohmann::json assets = nlohmann::json::array();
    for (const NativeRuntimeLoadedAsset& asset : report.assets) assets.push_back(nativeRuntimeLoadedAssetToJson(asset));
    nlohmann::json errors = nlohmann::json::array();
    for (const NativeBinaryError& error : report.errors) {
        errors.push_back({{"code", nativeBinaryErrorCodeName(error.code)}, {"path", error.path.generic_string()}, {"table", error.table}, {"message", error.message}});
    }
    nlohmann::json sceneNodes = nlohmann::json::array();
    for (const SceneNodeAsset& node : report.sceneAsset.nodes) {
        sceneNodes.push_back({
            {"name", node.name},
            {"sourceNodeIndex", node.sourceNodeIndex},
            {"meshHandle", node.mesh.index},
            {"skinIndex", node.skinIndex},
            {"parent", node.parent},
            {"children", node.children},
            {"visible", node.visible},
            {"castShadow", node.castShadow},
            {"receiveShadow", node.receiveShadow},
            {"visibleToCamera", node.visibleToCamera},
            {"renderLayer", node.renderLayer},
        });
    }
    const nlohmann::json sceneAsset = {
        {"available", report.sceneAssetPlan.available},
        {"asset_manager_backed", report.sceneAssetPlan.assetManagerBacked},
        {"package_backed", report.sceneAssetPlan.packageBacked},
        {"renderer_placeable", report.sceneAssetPlan.rendererPlaceable},
        {"texture_count", report.sceneAssetPlan.textureCount},
        {"material_count", report.sceneAssetPlan.materialCount},
        {"mesh_count", report.sceneAssetPlan.meshCount},
        {"skin_count", report.sceneAssetPlan.skinCount},
        {"skinned_node_count", report.sceneAssetPlan.skinnedNodeCount},
        {"node_count", report.sceneAssetPlan.nodeCount},
        {"root_node_count", report.sceneAssetPlan.rootNodeCount},
        {"missing_mesh_handle_count", report.sceneAssetPlan.missingMeshHandleCount},
        {"bounds_available", report.sceneAssetPlan.boundsAvailable},
        {"bounds_min", report.sceneAssetPlan.boundsMin},
        {"bounds_max", report.sceneAssetPlan.boundsMax},
        {"name", report.sceneAsset.name},
        {"source_path", report.sceneAsset.sourcePath.empty() ? std::string{} : report.sceneAsset.sourcePath.generic_string()},
        {"nodes", sceneNodes},
    };
    nlohmann::json directStoreTickets = nlohmann::json::array();
    for (const NativeRuntimeDirectStoreUploadTicketPlan& ticket : report.directStoreUploadPlan.tickets) {
        directStoreTickets.push_back({
            {"guid", ticket.guid},
            {"kind", nativeAssetKindName(ticket.kind)},
            {"source", ticket.source},
            {"native_path", ticket.nativePath.empty() ? std::string{} : ticket.nativePath.generic_string()},
            {"package_object_path", ticket.packageObjectPath},
            {"generation", ticket.generation},
            {"resource_kind", ticket.resourceKind},
            {"label", ticket.label},
            {"upload_bytes", ticket.uploadBytes},
            {"upload_ready", ticket.uploadReady},
            {"unavailable_reason", ticket.unavailableReason},
        });
    }
    const nlohmann::json directStoreUploadPlan = {
        {"available", report.directStoreUploadPlan.available},
        {"executable", report.directStoreUploadPlan.executable},
        {"asset_manager_bypass", report.directStoreUploadPlan.assetManagerBypass},
        {"package_backed", report.directStoreUploadPlan.packageBacked},
        {"loose_backed", report.directStoreUploadPlan.looseBacked},
        {"texture_ticket_count", report.directStoreUploadPlan.textureTicketCount},
        {"mesh_buffer_ticket_count", report.directStoreUploadPlan.meshBufferTicketCount},
        {"texture_upload_bytes", report.directStoreUploadPlan.textureUploadBytes},
        {"vertex_upload_bytes", report.directStoreUploadPlan.vertexUploadBytes},
        {"index_upload_bytes", report.directStoreUploadPlan.indexUploadBytes},
        {"policy", report.directStoreUploadPlan.policy},
        {"unavailable_reason", report.directStoreUploadPlan.unavailableReason},
        {"readiness_requirements", report.directStoreUploadPlan.readinessRequirements},
        {"missing_requirements", report.directStoreUploadPlan.missingRequirements},
        {"missing_requirement_count", report.directStoreUploadPlan.missingRequirements.size()},
        {"tickets", directStoreTickets},
        {"upload_ticket_queue_simulation", directStoreUploadTicketQueueSimulationJson(report.directStoreUploadPlan)},
    };
    const nlohmann::json legacyCpuLoadPolicy = {
        {"available", report.legacyCpuLoadPolicy.available},
        {"asset_manager_backed", report.legacyCpuLoadPolicy.assetManagerBacked},
        {"package_backed", report.legacyCpuLoadPolicy.packageBacked},
        {"loose_backed", report.legacyCpuLoadPolicy.looseBacked},
        {"estimated_eager_cpu_bytes", report.legacyCpuLoadPolicy.estimatedEagerCpuBytes},
        {"warning_threshold_bytes", report.legacyCpuLoadPolicy.warningThresholdBytes},
        {"hard_limit_bytes", report.legacyCpuLoadPolicy.hardLimitBytes},
        {"allow_large_eager_cpu_load", report.legacyCpuLoadPolicy.allowLargeEagerCpuLoad},
        {"large_eager_load_warning", report.legacyCpuLoadPolicy.largeEagerLoadWarning},
        {"hard_limit_exceeded", report.legacyCpuLoadPolicy.hardLimitExceeded},
        {"streaming_recommended", report.legacyCpuLoadPolicy.streamingRecommended},
        {"policy", report.legacyCpuLoadPolicy.policy},
        {"recommended_action", report.legacyCpuLoadPolicy.recommendedAction},
    };
    return {
        {"ok", report.ok},
        {"source_root", report.sourceRoot.empty() ? std::string{} : report.sourceRoot.generic_string()},
        {"nativeTextureFormatSupport", nativeTextureFormatSupportJson(report.options.textureFormatSupport)},
        {"rejectUnsupportedTextureFormats", report.options.rejectUnsupportedTextureFormats},
        {"validatePayloadHashes", report.options.validatePayloadHashes},
        {"retainLoadedPayloadsInReport", report.options.retainLoadedPayloadsInReport},
        {"eagerCpuLoadWarningBytes", report.options.eagerCpuLoadWarningBytes},
        {"eagerCpuLoadHardLimitBytes", report.options.eagerCpuLoadHardLimitBytes},
        {"allowLargeEagerCpuLoad", report.options.allowLargeEagerCpuLoad},
        {"texture_count", report.textureCount},
        {"material_count", report.materialCount},
        {"mesh_count", report.meshCount},
        {"skeleton_count", report.skeletonCount},
        {"animation_count", report.animationCount},
        {"animation_controller_count", report.animationControllerCount},
        {"skeletal_mesh_count", report.skeletalMeshCount},
        {"total_texture_bytes", report.totalTextureBytes},
        {"total_vertex_count", report.totalVertexCount},
        {"total_index_count", report.totalIndexCount},
        {"legacy_cpu_load_policy", legacyCpuLoadPolicy},
        {"direct_store_upload_plan", directStoreUploadPlan},
        {"scene_asset", sceneAsset},
        {"assets", assets},
        {"warnings", report.warnings},
        {"errors", errors},
    };
}

int loadNativeRuntimeAssetsCommand(const std::filesystem::path& input, const std::filesystem::path& jsonOut, const NativeTextureFormatSupport& textureFormatSupport) {
    NativeAssetRuntimeLoader loader;
    AssetManager manager;
    NativeRuntimeLoadOptions options;
    options.textureFormatSupport = textureFormatSupport;
    const NativeRuntimeLoadReport report = loader.loadLooseRoot(input, &manager, options);
    nlohmann::json json = nativeRuntimeLoadReportToJson(report);
    const AssetLoadStats stats = manager.stats();
    nlohmann::json rendererUploadPlan = nlohmann::json::object();
    rendererUploadPlan[std::string{'a','v','a','i','l','a','b','l','e'}] = report.rendererUploadPlan.available;
    rendererUploadPlan[std::string{'a','s','s','e','t','_','m','a','n','a','g','e','r','_','b','a','c','k','e','d'}] = report.rendererUploadPlan.assetManagerBacked;
    rendererUploadPlan[std::string{'p','a','c','k','a','g','e','_','b','a','c','k','e','d'}] = report.rendererUploadPlan.packageBacked;
    rendererUploadPlan[std::string{'t','e','x','t','u','r','e','_','c','o','u','n','t'}] = report.rendererUploadPlan.textureCount;
    rendererUploadPlan[std::string{'t','e','x','t','u','r','e','_','r','e','s','i','d','e','n','t','_','c','o','u','n','t'}] = report.rendererUploadPlan.textureResidentCount;
    rendererUploadPlan[std::string{'m','a','t','e','r','i','a','l','_','c','o','u','n','t'}] = report.rendererUploadPlan.materialCount;
    rendererUploadPlan[std::string{'m','e','s','h','_','c','o','u','n','t'}] = report.rendererUploadPlan.meshCount;
    rendererUploadPlan[std::string{'t','e','x','t','u','r','e','_','u','p','l','o','a','d','_','b','y','t','e','s'}] = report.rendererUploadPlan.textureUploadBytes;
    rendererUploadPlan[std::string{'v','e','r','t','e','x','_','u','p','l','o','a','d','_','b','y','t','e','s'}] = report.rendererUploadPlan.vertexUploadBytes;
    rendererUploadPlan[std::string{'i','n','d','e','x','_','u','p','l','o','a','d','_','b','y','t','e','s'}] = report.rendererUploadPlan.indexUploadBytes;
    rendererUploadPlan[std::string{'d','i','r','e','c','t','_','s','t','o','r','e','_','u','p','l','o','a','d'}] = false;
    rendererUploadPlan[std::string{'d','i','r','e','c','t','_','s','t','o','r','e','_','u','p','l','o','a','d','_','o','p','e','n'}] = true;
    json[std::string{'r','e','n','d','e','r','e','r','_','u','p','l','o','a','d','_','p','l','a','n'}] = rendererUploadPlan;
    json["asset_manager"] = {{"texture_count", stats.textureCount}, {"material_count", stats.materialCount}, {"mesh_count", stats.meshCount}};
    if (!jsonOut.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(jsonOut.parent_path(), ec);
        std::ofstream out(jsonOut);
        if (!out.is_open()) {
            return 1;
        }
        out << json.dump(2);
    } else {
        std::cout << json.dump(2) << '\n';
    }
    return report.ok ? 0 : 1;
}

} // namespace rtv
