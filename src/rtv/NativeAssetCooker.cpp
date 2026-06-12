#include "rtv/NativeAssetCooker.h"

#include "rtv/AnimationController.h"
#include "rtv/BvhBuilder.h"
#include "rtv/NativeBinaryIO.h"
#include "rtv/NativeTextureFormatPolicy.h"

#include <nlohmann/json.hpp>

#include <ktx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <type_traits>
#include <utility>

namespace rtv {
namespace {

enum NativeCookChunkType : uint32_t {
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

RtmaterialTextureTransformRecord textureTransformRecord(const TextureTransformAsset& transform) {
    RtmaterialTextureTransformRecord record;
    record.offset = {transform.offset.x, transform.offset.y};
    record.scale = {transform.scale.x, transform.scale.y};
    record.rotation = transform.rotation;
    record.enabled = transform.enabled;
    record.texCoord = transform.texCoord;
    return record;
}

std::vector<RtmaterialTextureTransformRecord> materialTextureTransformRecords(const MaterialAsset& material) {
    std::vector<RtmaterialTextureTransformRecord> records;
    records.reserve(kRtmaterialTextureTransformCount);
    auto add = [&](const TextureTransformAsset& transform) {
        records.push_back(textureTransformRecord(transform));
    };
    add(material.baseColorTextureTransform);
    add(material.normalTextureTransform);
    add(material.metallicRoughnessTextureTransform);
    add(material.emissiveTextureTransform);
    add(material.occlusionTextureTransform);
    add(material.sheenColorTextureTransform);
    add(material.sheenRoughnessTextureTransform);
    add(material.iridescenceTextureTransform);
    add(material.iridescenceThicknessTextureTransform);
    add(material.clearcoatTextureTransform);
    add(material.clearcoatRoughnessTextureTransform);
    add(material.clearcoatNormalTextureTransform);
    add(material.transmissionTextureTransform);
    add(material.volumeThicknessTextureTransform);
    add(material.specularTextureTransform);
    add(material.specularColorTextureTransform);
    add(material.anisotropyTextureTransform);
    return records;
}

struct RttextureMipRecord {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct RtskeletalMeshSkinningSummaryRecord {
    uint32_t jointRemapCount = 0;
    uint32_t maxInfluencesPerVertex = 4;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<std::byte> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

template <typename T>
std::vector<std::byte> bytesOfVector(const std::vector<T>& values) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<std::byte> out(sizeof(T) * values.size());
    if (!out.empty()) {
        std::memcpy(out.data(), values.data(), out.size());
    }
    return out;
}

std::vector<std::byte> bytesOfUint8Vector(const std::vector<uint8_t>& values) {
    std::vector<std::byte> out(values.size());
    if (!out.empty()) {
        std::memcpy(out.data(), values.data(), values.size());
    }
    return out;
}

std::vector<std::byte> bytesOfString(std::string_view value) {
    std::vector<std::byte> out(value.size());
    if (!out.empty()) {
        std::memcpy(out.data(), value.data(), value.size());
    }
    return out;
}

std::array<uint8_t, 32> hashFromExistingOrText(const std::string& value) {
    return nativeHashText(value);
}

NativeAssetWriteDesc baseWriteDesc(const NativeAssetCookInput& input, NativeAssetKind kind) {
    NativeAssetWriteDesc desc;
    desc.kind = kind;
    desc.magic = nativeAssetMagicForKind(kind);
    desc.contentVersion = 1;
    desc.assetGuid = nativeGuidFromText(input.guid);
    desc.sourceHash = hashFromExistingOrText(input.sourceHash);
    desc.importSettingsHash = hashFromExistingOrText(input.importSettingsHash);
    desc.debugName = input.displayName;
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "sourcePath", .value = input.sourcePath.generic_string()});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "sourceHash", .value = input.sourceHash});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "importSettingsHash", .value = input.importSettingsHash});
    return desc;
}

NativeAssetCookResult finishWrite(const std::filesystem::path& path, const NativeAssetWriteDesc& desc) {
    NativeAssetCookResult result;
    result.path = path;
    NativeBinaryError error;
    NativeAssetWriter writer;
    if (!writer.write(path, desc, &error)) {
        result.errors.push_back(error.message.empty() ? "Native asset write failed" : error.message);
        return result;
    }
    NativeAssetReader reader;
    const NativeAssetInspection inspection = reader.inspect(path, true);
    if (!inspection.ok) {
        for (const NativeBinaryError& inspectError : inspection.errors) {
            result.errors.push_back(inspectError.message);
        }
        return result;
    }
    result.success = true;
    result.payloadHash = nativeHashHex(inspection.header.payloadHash);
    result.payloadBytes = inspection.header.fileSize;
    return result;
}

uint32_t appendChunk(NativeAssetWriteDesc& desc, uint32_t type, std::vector<std::byte> payload) {
    const uint32_t index = static_cast<uint32_t>(desc.chunks.size());
    desc.chunks.push_back(NativeBinaryChunkInput{.type = type, .payload = std::move(payload)});
    return index;
}

struct NativeStringRef {
    uint32_t offset = 0;
    uint32_t size = 0;
};

NativeStringRef appendNativeString(std::vector<std::byte>& table, std::map<std::string, uint32_t>& offsets, const std::string& value) {
    const auto existing = offsets.find(value);
    if (existing != offsets.end()) {
        return NativeStringRef{.offset = existing->second, .size = static_cast<uint32_t>(value.size())};
    }
    const uint32_t offset = static_cast<uint32_t>(table.size());
    offsets.emplace(value, offset);
    for (char c : value) {
        table.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    table.push_back(std::byte{0});
    return NativeStringRef{.offset = offset, .size = static_cast<uint32_t>(value.size())};
}

uint32_t parameterTypeToNative(AnimationControllerParameterType type) {
    return static_cast<uint32_t>(type);
}

void writeParameterValue(
    AnimationControllerParameterType type,
    const AnimationControllerParameterValue& value,
    uint32_t& boolValue,
    int32_t& intValue,
    float& floatValue,
    uint32_t& triggerValue) {
    switch (type) {
    case AnimationControllerParameterType::Bool:
        boolValue = value.boolValue ? 1u : 0u;
        break;
    case AnimationControllerParameterType::Int:
        intValue = value.intValue;
        break;
    case AnimationControllerParameterType::Float:
        floatValue = value.floatValue;
        break;
    case AnimationControllerParameterType::Trigger:
        triggerValue = value.triggerValue ? 1u : 0u;
        break;
    case AnimationControllerParameterType::Unknown:
        break;
    }
}

std::pair<uint32_t, uint32_t> appendControllerEvents(
    const std::vector<AnimationController::Event>& sourceEvents,
    std::vector<RtanimControllerEventRecord>& eventRecords,
    std::vector<std::byte>& stringTable,
    std::map<std::string, uint32_t>& stringOffsets) {
    const uint32_t first = static_cast<uint32_t>(eventRecords.size());
    for (const AnimationController::Event& event : sourceEvents) {
        const NativeStringRef name = appendNativeString(stringTable, stringOffsets, event.name);
        const NativeStringRef payload = appendNativeString(stringTable, stringOffsets, event.payloadJson);
        RtanimControllerEventRecord record;
        record.nameOffset = name.offset;
        record.nameSize = name.size;
        record.payloadOffset = payload.offset;
        record.payloadSize = payload.size;
        eventRecords.push_back(record);
    }
    return {first, static_cast<uint32_t>(eventRecords.size() - first)};
}

void appendUniqueDependency(NativeAssetWriteDesc& desc, const AssetGuid& guid, NativeAssetKind kind, std::string debugName) {
    if (guid.empty()) {
        return;
    }
    const std::array<uint8_t, 16> nativeGuid = nativeGuidFromText(guid);
    const auto existing = std::find_if(desc.dependencies.begin(), desc.dependencies.end(), [&](const NativeBinaryDependencyInput& dependency) {
        return dependency.guid == nativeGuid && dependency.kind == kind;
    });
    if (existing != desc.dependencies.end()) {
        return;
    }
    desc.dependencies.push_back(NativeBinaryDependencyInput{
        .guid = nativeGuid,
        .kind = kind,
        .flags = static_cast<uint32_t>(NativeDependencyFlags::Required) | static_cast<uint32_t>(NativeDependencyFlags::Runtime),
        .debugName = std::move(debugName),
    });
}

uint32_t textureSlotIndex(TextureAssetHandle handle) {
    return handle.valid() ? handle.index : UINT32_MAX;
}

void addTextureSlot(std::vector<RtmaterialTextureSlotRecord>& slots, uint32_t slot, TextureAssetHandle handle, const std::vector<AssetGuid>& guids) {
    if (!handle.valid() || handle.index >= guids.size()) {
        return;
    }
    RtmaterialTextureSlotRecord record;
    record.slot = slot;
    record.textureIndex = handle.index;
    record.textureGuid = nativeGuidFromText(guids[handle.index]);
    slots.push_back(record);
}

uint32_t samplerPacked(const TextureSamplerDesc& sampler) {
    return static_cast<uint32_t>(sampler.minFilter) |
        (static_cast<uint32_t>(sampler.magFilter) << 4u) |
        (static_cast<uint32_t>(sampler.wrapS) << 8u) |
        (static_cast<uint32_t>(sampler.wrapT) << 12u);
}

NativeTextureColorSpace nativeTextureColorSpaceForTexture(const TextureAsset& texture, NativeTextureRole role) {
    if (role == NativeTextureRole::EnvironmentHdr || texture.linearColorSpace) {
        return NativeTextureColorSpace::HdrLinear;
    }
    return texture.srgb ? NativeTextureColorSpace::Srgb : NativeTextureColorSpace::Linear;
}

NativeTextureCompressionPolicy nativeTextureCompressionPolicyForTexture(const TextureAsset& texture) {
    if (texture.isCompressed) {
        return NativeTextureCompressionPolicy::PreserveSourceContainer;
    }
    if (texture.linearColorSpace) {
        return NativeTextureCompressionPolicy::DecodedHdr;
    }
    return NativeTextureCompressionPolicy::DecodedRgba8;
}

bool nativeTextureBlockCompressedFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

std::string nativeTexturePayloadVariantName(
    const TextureAsset& texture,
    VkFormat emittedVkFormat,
    const NativeTextureFormatSelection& formatSelection,
    NativeTextureCompressionPolicy compressionPolicy) {
    if (emittedVkFormat == formatSelection.selectedFormat) {
        if (texture.sourceContainerTranscoded) {
            return "platform-selected-ktx2-transcoded-payload";
        }
        if (texture.sourceContainerPreserved) {
            return "platform-selected-ktx2-native-payload";
        }
        return nativeTextureBlockCompressedFormat(emittedVkFormat)
            ? "platform-selected-compressed-payload"
            : "platform-selected-uncompressed-payload";
    }
    if (compressionPolicy == NativeTextureCompressionPolicy::PreserveSourceContainer) {
        return "source-or-loader-compressed-payload";
    }
    return "decoded-fallback-payload";
}

constexpr char kVariantEmittedNativePayload[] = {'e', 'm', 'i', 't', 't', 'e', 'd', '-', 'n', 'a', 't', 'i', 'v', 'e', '-', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '\0'};
constexpr char kVariantPlatformSelectedTarget[] = {'p', 'l', 'a', 't', 'f', 'o', 'r', 'm', '-', 's', 'e', 'l', 'e', 'c', 't', 'e', 'd', '-', 't', 'a', 'r', 'g', 'e', 't', '\0'};
constexpr char kVariantSourceCookOutput[] = {'c', 'o', 'o', 'k', '-', 'o', 'u', 't', 'p', 'u', 't', '\0'};
constexpr char kVariantSourceContainerOrLoaderOutput[] = {'s', 'o', 'u', 'r', 'c', 'e', '-', 'c', 'o', 'n', 't', 'a', 'i', 'n', 'e', 'r', '-', 'o', 'r', '-', 'l', 'o', 'a', 'd', 'e', 'r', '-', 'o', 'u', 't', 'p', 'u', 't', '\0'};
constexpr char kVariantSourceContainerPreservedOutput[] = {'s', 'o', 'u', 'r', 'c', 'e', '-', 'c', 'o', 'n', 't', 'a', 'i', 'n', 'e', 'r', '-', 'p', 'r', 'e', 's', 'e', 'r', 'v', 'e', 'd', '-', 'n', 'a', 't', 'i', 'v', 'e', '-', 'p', 'a', 'y', 'l', 'o', 'a', 'd', '\0'};
constexpr char kVariantSourceContainerTranscodedOutput[] = {'s', 'o', 'u', 'r', 'c', 'e', '-', 'c', 'o', 'n', 't', 'a', 'i', 'n', 'e', 'r', '-', 't', 'r', 'a', 'n', 's', 'c', 'o', 'd', 'e', 'd', '-', 'o', 'u', 't', 'p', 'u', 't', '\0'};
constexpr char kVariantSourceSelectedFormat[] = {'s', 'e', 'l', 'e', 'c', 't', 'e', 'd', '-', 'f', 'o', 'r', 'm', 'a', 't', '\0'};
constexpr char kVariantPendingTranscodeEmission[] = {'t', 'r', 'a', 'n', 's', 'c', 'o', 'd', 'e', '-', 'o', 'u', 't', 'p', 'u', 't', '-', 'e', 'm', 'i', 's', 's', 'i', 'o', 'n', '-', 'p', 'e', 'n', 'd', 'i', 'n', 'g', '\0'};
constexpr char kVariantPendingUnsupportedTarget[] = {'p', 'l', 'a', 't', 'f', 'o', 'r', 'm', '-', 't', 'a', 'r', 'g', 'e', 't', '-', 'u', 'n', 's', 'u', 'p', 'p', 'o', 'r', 't', 'e', 'd', '\0'};
constexpr char kJsonNativeTexturePayload[] = {'n', 'a', 't', 'i', 'v', 'e', 'T', 'e', 'x', 't', 'u', 'r', 'e', 'P', 'a', 'y', 'l', 'o', 'a', 'd', '\0'};
constexpr char kJsonVariantPlan[] = {'n', 'a', 't', 'i', 'v', 'e', 'T', 'e', 'x', 't', 'u', 'r', 'e', 'P', 'a', 'y', 'l', 'o', 'a', 'd', 'V', 'a', 'r', 'i', 'a', 'n', 't', 'P', 'l', 'a', 'n', '\0'};
constexpr char kJsonName[] = {'n', 'a', 'm', 'e', '\0'};
constexpr char kJsonVkFormat[] = {'v', 'k', 'F', 'o', 'r', 'm', 'a', 't', '\0'};
constexpr char kJsonCompressionFamily[] = {'c', 'o', 'm', 'p', 'r', 'e', 's', 's', 'i', 'o', 'n', 'F', 'a', 'm', 'i', 'l', 'y', '\0'};
constexpr char kJsonEmitted[] = {'e', 'm', 'i', 't', 't', 'e', 'd', '\0'};
constexpr char kJsonSelectedPlatformTarget[] = {'s', 'e', 'l', 'e', 'c', 't', 'e', 'd', 'P', 'l', 'a', 't', 'f', 'o', 'r', 'm', 'T', 'a', 'r', 'g', 'e', 't', '\0'};
constexpr char kJsonRuntimeUsable[] = {'r', 'u', 'n', 't', 'i', 'm', 'e', 'U', 's', 'a', 'b', 'l', 'e', '\0'};
constexpr char kJsonSource[] = {'s', 'o', 'u', 'r', 'c', 'e', '\0'};
constexpr char kJsonPendingReason[] = {'p', 'e', 'n', 'd', 'i', 'n', 'g', 'R', 'e', 'a', 's', 'o', 'n', '\0'};

std::vector<NativeTexturePayloadVariantPlanEntry> nativeTexturePayloadVariantPlan(
    const TextureAsset& texture,
    VkFormat emittedVkFormat,
    const NativeTextureFormatSelection& formatSelection,
    NativeTextureCompressionPolicy compressionPolicy) {
    std::vector<NativeTexturePayloadVariantPlanEntry> plan;
    NativeTexturePayloadVariantPlanEntry emitted;
    emitted.name = kVariantEmittedNativePayload;
    emitted.vkFormat = nativeTextureFormatName(emittedVkFormat);
    emitted.compressionFamily = emittedVkFormat == formatSelection.selectedFormat
        ? formatSelection.compressionFamily
        : nativeTextureCompressionPolicyName(compressionPolicy);
    emitted.emitted = true;
    emitted.selectedPlatformTarget = emittedVkFormat == formatSelection.selectedFormat;
    emitted.runtimeUsable = emittedVkFormat != VK_FORMAT_UNDEFINED;
    emitted.source = texture.sourceContainerTranscoded
        ? kVariantSourceContainerTranscodedOutput
        : texture.sourceContainerPreserved
            ? kVariantSourceContainerPreservedOutput
            : compressionPolicy == NativeTextureCompressionPolicy::PreserveSourceContainer
                ? kVariantSourceContainerOrLoaderOutput
                : kVariantSourceCookOutput;
    plan.push_back(std::move(emitted));

    if (emittedVkFormat != formatSelection.selectedFormat) {
        NativeTexturePayloadVariantPlanEntry target;
        target.name = kVariantPlatformSelectedTarget;
        target.vkFormat = nativeTextureFormatName(formatSelection.selectedFormat);
        target.compressionFamily = formatSelection.compressionFamily;
        target.emitted = false;
        target.selectedPlatformTarget = true;
        target.runtimeUsable = false;
        target.source = kVariantSourceSelectedFormat;
        target.pendingReason = formatSelection.supported ? kVariantPendingTranscodeEmission : kVariantPendingUnsupportedTarget;
        plan.push_back(std::move(target));
    }
    return plan;
}

nlohmann::json nativeTexturePayloadVariantPlanJson(const std::vector<NativeTexturePayloadVariantPlanEntry>& plan) {
    nlohmann::json variantPlan = nlohmann::json::array();
    for (const NativeTexturePayloadVariantPlanEntry& entry : plan) {
        nlohmann::json item = nlohmann::json::object();
        item[std::string(kJsonName)] = entry.name;
        item[std::string(kJsonVkFormat)] = entry.vkFormat;
        item[std::string(kJsonCompressionFamily)] = entry.compressionFamily;
        item[std::string(kJsonEmitted)] = entry.emitted;
        item[std::string(kJsonSelectedPlatformTarget)] = entry.selectedPlatformTarget;
        item[std::string(kJsonRuntimeUsable)] = entry.runtimeUsable;
        item[std::string(kJsonSource)] = entry.source;
        item[std::string(kJsonPendingReason)] = entry.pendingReason;
        variantPlan.push_back(std::move(item));
    }
    return variantPlan;
}

std::vector<std::byte> texturePayloadBytes(const TextureAsset& texture, std::vector<RttextureMipRecord>& mips) {
    std::vector<std::byte> payload;
    if (!texture.mipData.empty() && !texture.rgba8.empty()) {
        payload = bytesOfUint8Vector(texture.rgba8);
        for (const TextureMipLevel& mip : texture.mipData) {
            mips.push_back(RttextureMipRecord{.offset = mip.offset, .size = mip.size, .width = mip.width, .height = mip.height});
        }
        return payload;
    }
    payload = bytesOfUint8Vector(texture.rgba8);
    mips.push_back(RttextureMipRecord{.offset = 0, .size = payload.size(), .width = texture.width, .height = texture.height});
    return payload;
}

uint16_t float32ToFloat16Bits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const uint32_t sign = (bits >> 16u) & 0x8000u;
    const uint32_t exponent = (bits >> 23u) & 0xffu;
    uint32_t mantissa = bits & 0x7fffffu;

    if (exponent == 0xffu) {
        const uint32_t nanPayload = mantissa == 0u ? 0u : std::max(1u, mantissa >> 13u);
        return static_cast<uint16_t>(sign | 0x7c00u | nanPayload);
    }

    int32_t halfExponent = static_cast<int32_t>(exponent) - 127 + 15;
    if (halfExponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7c00u);
    }

    if (halfExponent <= 0) {
        if (halfExponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - halfExponent);
        uint32_t halfMantissa = mantissa >> shift;
        const uint32_t roundBit = (mantissa >> (shift - 1u)) & 1u;
        const uint32_t stickyMask = (1u << (shift - 1u)) - 1u;
        if (roundBit != 0u && ((mantissa & stickyMask) != 0u || (halfMantissa & 1u) != 0u)) {
            ++halfMantissa;
        }
        return static_cast<uint16_t>(sign | halfMantissa);
    }

    uint32_t halfMantissa = mantissa >> 13u;
    const uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (halfMantissa & 1u) != 0u)) {
        ++halfMantissa;
        if (halfMantissa == 0x400u) {
            halfMantissa = 0u;
            ++halfExponent;
            if (halfExponent >= 31) {
                return static_cast<uint16_t>(sign | 0x7c00u);
            }
        }
    }

    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(halfExponent) << 10u) | halfMantissa);
}

void appendUint16Le(std::vector<std::byte>& out, uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
}

bool convertRgba32fPayloadToRgba16f(std::vector<std::byte>& payload, std::vector<RttextureMipRecord>& mips) {
    if (payload.empty() || mips.empty()) {
        return false;
    }

    uint64_t requiredBytes = 0;
    for (const RttextureMipRecord& mip : mips) {
        const uint64_t expectedMipBytes = static_cast<uint64_t>(mip.width) * static_cast<uint64_t>(mip.height) * 4ull * sizeof(float);
        if (mip.size != expectedMipBytes || mip.offset > payload.size() || mip.size > payload.size() - mip.offset) {
            return false;
        }
        requiredBytes += mip.size;
    }
    if (requiredBytes != payload.size()) {
        return false;
    }

    std::vector<std::byte> converted;
    converted.reserve(payload.size() / 2u);
    std::vector<RttextureMipRecord> convertedMips;
    convertedMips.reserve(mips.size());

    for (const RttextureMipRecord& mip : mips) {
        RttextureMipRecord convertedMip = mip;
        convertedMip.offset = converted.size();
        const uint64_t pixelCount = static_cast<uint64_t>(mip.width) * static_cast<uint64_t>(mip.height);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(payload.data()) + mip.offset;
        for (uint64_t pixel = 0; pixel < pixelCount; ++pixel) {
            for (uint32_t channel = 0; channel < 4u; ++channel) {
                float channelValue = 0.0f;
                std::memcpy(&channelValue, src + pixel * 16ull + channel * sizeof(float), sizeof(float));
                appendUint16Le(converted, float32ToFloat16Bits(channelValue));
            }
        }
        convertedMip.size = converted.size() - convertedMip.offset;
        convertedMips.push_back(convertedMip);
    }

    payload = std::move(converted);
    mips = std::move(convertedMips);
    return true;
}

void appendBc4Block(std::vector<std::byte>& out, const uint8_t values[16]) {
    uint8_t minValue = values[0];
    uint8_t maxValue = values[0];
    for (uint32_t i = 1; i < 16u; ++i) {
        minValue = std::min(minValue, values[i]);
        maxValue = std::max(maxValue, values[i]);
    }

    uint8_t palette[8] = {};
    palette[0] = maxValue;
    palette[1] = minValue;
    for (uint32_t i = 1; i <= 6u; ++i) {
        palette[i + 1u] = static_cast<uint8_t>(((7u - i) * maxValue + i * minValue + 3u) / 7u);
    }

    uint64_t indices = 0;
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t bestIndex = 0;
        uint32_t bestDistance = UINT32_MAX;
        for (uint32_t candidate = 0; candidate < 8u; ++candidate) {
            const int32_t distance = static_cast<int32_t>(values[i]) - static_cast<int32_t>(palette[candidate]);
            const uint32_t squared = static_cast<uint32_t>(distance * distance);
            if (squared < bestDistance) {
                bestDistance = squared;
                bestIndex = candidate;
            }
        }
        indices |= static_cast<uint64_t>(bestIndex) << (3u * i);
    }

    out.push_back(static_cast<std::byte>(maxValue));
    out.push_back(static_cast<std::byte>(minValue));
    for (uint32_t byteIndex = 0; byteIndex < 6u; ++byteIndex) {
        out.push_back(static_cast<std::byte>((indices >> (8u * byteIndex)) & 0xffu));
    }
}

uint32_t nativeTextureMipExtent(uint32_t base, uint32_t level) {
    return std::max(base >> level, 1u);
}

bool convertRgba8PayloadToBc7WithKtx(
    std::vector<std::byte>& payload,
    std::vector<RttextureMipRecord>& mips,
    VkFormat targetFormat) {
    if (payload.empty() || mips.empty() ||
        (targetFormat != VK_FORMAT_BC7_UNORM_BLOCK && targetFormat != VK_FORMAT_BC7_SRGB_BLOCK)) {
        return false;
    }

    uint64_t requiredBytes = 0;
    for (const RttextureMipRecord& mip : mips) {
        const uint64_t expectedMipBytes = static_cast<uint64_t>(mip.width) * static_cast<uint64_t>(mip.height) * 4ull;
        if (mip.size != expectedMipBytes || mip.offset > payload.size() || mip.size > payload.size() - mip.offset) {
            return false;
        }
        requiredBytes += mip.size;
    }
    if (requiredBytes != payload.size()) {
        return false;
    }

    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = targetFormat == VK_FORMAT_BC7_SRGB_BLOCK ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    createInfo.baseWidth = mips.front().width;
    createInfo.baseHeight = mips.front().height;
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = static_cast<ktx_uint32_t>(mips.size());
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    KTX_error_code result = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
    if (result != KTX_SUCCESS || texture == nullptr) {
        return false;
    }

    for (uint32_t level = 0; level < mips.size(); ++level) {
        const RttextureMipRecord& mip = mips[level];
        if (mip.width != nativeTextureMipExtent(createInfo.baseWidth, level) ||
            mip.height != nativeTextureMipExtent(createInfo.baseHeight, level)) {
            ktxTexture2_Destroy(texture);
            return false;
        }
        const auto* src = reinterpret_cast<const ktx_uint8_t*>(payload.data() + mip.offset);
        result = ktxTexture_SetImageFromMemory(ktxTexture(texture), level, 0, 0, src, static_cast<ktx_size_t>(mip.size));
        if (result != KTX_SUCCESS) {
            ktxTexture2_Destroy(texture);
            return false;
        }
    }

    ktxBasisParams basisParams{};
    basisParams.structSize = sizeof(basisParams);
    basisParams.uastc = KTX_TRUE;
    basisParams.threadCount = 1;
    basisParams.uastcFlags = KTX_PACK_UASTC_LEVEL_FASTEST;
    basisParams.uastcRDONoMultithreading = KTX_TRUE;
    result = ktxTexture2_CompressBasisEx(texture, &basisParams);
    if (result != KTX_SUCCESS) {
        ktxTexture2_Destroy(texture);
        return false;
    }

    result = ktxTexture2_TranscodeBasis(texture, KTX_TTF_BC7_RGBA, 0);
    if (result != KTX_SUCCESS) {
        ktxTexture2_Destroy(texture);
        return false;
    }

    ktx_uint8_t* imageData = ktxTexture_GetData(ktxTexture(texture));
    if (imageData == nullptr) {
        ktxTexture2_Destroy(texture);
        return false;
    }
    const size_t imageDataSize = static_cast<size_t>(ktxTexture_GetDataSize(ktxTexture(texture)));

    std::vector<std::byte> converted;
    std::vector<RttextureMipRecord> convertedMips;
    convertedMips.reserve(mips.size());
    for (uint32_t level = 0; level < mips.size(); ++level) {
        ktx_size_t mipOffset = 0;
        result = ktxTexture_GetImageOffset(ktxTexture(texture), level, 0, 0, &mipOffset);
        if (result != KTX_SUCCESS) {
            ktxTexture2_Destroy(texture);
            return false;
        }
        const ktx_size_t mipSize = ktxTexture_GetImageSize(ktxTexture(texture), level);
        if (static_cast<size_t>(mipOffset) > imageDataSize || static_cast<size_t>(mipSize) > imageDataSize - static_cast<size_t>(mipOffset)) {
            ktxTexture2_Destroy(texture);
            return false;
        }

        RttextureMipRecord convertedMip = mips[level];
        convertedMip.offset = converted.size();
        convertedMip.size = static_cast<uint64_t>(mipSize);
        converted.resize(converted.size() + static_cast<size_t>(mipSize));
        std::memcpy(converted.data() + convertedMip.offset, imageData + mipOffset, static_cast<size_t>(mipSize));
        convertedMips.push_back(convertedMip);
    }

    ktxTexture2_Destroy(texture);
    payload = std::move(converted);
    mips = std::move(convertedMips);
    return true;
}

bool convertRgba8PayloadToBc(std::vector<std::byte>& payload, std::vector<RttextureMipRecord>& mips, VkFormat targetFormat) {
    if (targetFormat == VK_FORMAT_BC7_UNORM_BLOCK || targetFormat == VK_FORMAT_BC7_SRGB_BLOCK) {
        return convertRgba8PayloadToBc7WithKtx(payload, mips, targetFormat);
    }
    if (payload.empty() || mips.empty() || (targetFormat != VK_FORMAT_BC4_UNORM_BLOCK && targetFormat != VK_FORMAT_BC5_UNORM_BLOCK)) {
        return false;
    }

    uint64_t requiredBytes = 0;
    for (const RttextureMipRecord& mip : mips) {
        const uint64_t expectedMipBytes = static_cast<uint64_t>(mip.width) * static_cast<uint64_t>(mip.height) * 4ull;
        if (mip.size != expectedMipBytes || mip.offset > payload.size() || mip.size > payload.size() - mip.offset) {
            return false;
        }
        requiredBytes += mip.size;
    }
    if (requiredBytes != payload.size()) {
        return false;
    }

    std::vector<std::byte> converted;
    std::vector<RttextureMipRecord> convertedMips;
    convertedMips.reserve(mips.size());

    for (const RttextureMipRecord& mip : mips) {
        RttextureMipRecord convertedMip = mip;
        convertedMip.offset = converted.size();
        const uint32_t blockCountX = (mip.width + 3u) / 4u;
        const uint32_t blockCountY = (mip.height + 3u) / 4u;
        const uint8_t* src = reinterpret_cast<const uint8_t*>(payload.data()) + mip.offset;
        for (uint32_t blockY = 0; blockY < blockCountY; ++blockY) {
            for (uint32_t blockX = 0; blockX < blockCountX; ++blockX) {
                uint8_t redValues[16] = {};
                uint8_t greenValues[16] = {};
                for (uint32_t y = 0; y < 4u; ++y) {
                    const uint32_t srcY = std::min(blockY * 4u + y, mip.height - 1u);
                    for (uint32_t x = 0; x < 4u; ++x) {
                        const uint32_t srcX = std::min(blockX * 4u + x, mip.width - 1u);
                        const uint64_t pixelOffset = (static_cast<uint64_t>(srcY) * mip.width + srcX) * 4ull;
                        const uint32_t texel = y * 4u + x;
                        redValues[texel] = src[pixelOffset];
                        greenValues[texel] = src[pixelOffset + 1u];
                    }
                }
                appendBc4Block(converted, redValues);
                if (targetFormat == VK_FORMAT_BC5_UNORM_BLOCK) {
                    appendBc4Block(converted, greenValues);
                }
            }
        }
        convertedMip.size = converted.size() - convertedMip.offset;
        convertedMips.push_back(convertedMip);
    }

    payload = std::move(converted);
    mips = std::move(convertedMips);
    return true;
}

std::string relativeOrAbsolute(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (path.empty()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    if (!ec && !relative.empty()) {
        for (const auto& part : relative) {
            if (part == "..") {
                return path.generic_string();
            }
        }
        return relative.generic_string();
    }
    return path.generic_string();
}

} // namespace

NativeAssetCooker::NativeAssetCooker()
    : textureFormatSupport_(nativeTextureOfflineFallbackFormatSupport()) {}

NativeAssetCooker::NativeAssetCooker(NativeTextureFormatSupport formatSupport)
    : textureFormatSupport_(std::move(formatSupport)) {}

NativeAssetCookResult NativeAssetCooker::cookMesh(
    const NativeAssetCookInput& input,
    const MeshAsset& mesh,
    const std::vector<AssetGuid>& materialGuids) const {
    NativeAssetWriteDesc desc = baseWriteDesc(input, NativeAssetKind::Mesh);

    RtmeshPayloadHeader header;
    header.vertexStride = sizeof(MeshVertex);
    header.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    header.indexCount = static_cast<uint32_t>(mesh.indices.size());
    header.primitiveRangeCount = static_cast<uint32_t>(mesh.primitives.size());
    header.materialSlotCount = static_cast<uint32_t>(materialGuids.size());
    header.vertexChunk = 1;
    header.indexChunk = 2;
    header.primitiveRangeChunk = 3;
    header.materialSlotChunk = 4;
    if (!mesh.vertices.empty()) {
        glm::vec3 minBounds = mesh.vertices.front().position;
        glm::vec3 maxBounds = mesh.vertices.front().position;
        for (const MeshVertex& vertex : mesh.vertices) {
            minBounds = glm::min(minBounds, vertex.position);
            maxBounds = glm::max(maxBounds, vertex.position);
        }
        header.boundsMinMax = {minBounds.x, minBounds.y, minBounds.z, maxBounds.x, maxBounds.y, maxBounds.z};
    }
    appendChunk(desc, ChunkPayloadHeader, bytesOf(header));
    appendChunk(desc, ChunkMeshVertices, bytesOfVector(mesh.vertices));
    appendChunk(desc, ChunkMeshIndices, bytesOfVector(mesh.indices));

    std::vector<RtmeshPrimitiveRecord> primitiveRecords;
    primitiveRecords.reserve(mesh.primitives.size());
    for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
        RtmeshPrimitiveRecord record;
        record.firstIndex = primitive.firstIndex;
        record.indexCount = primitive.indexCount;
        record.firstVertex = primitive.firstVertex;
        record.vertexCount = primitive.vertexCount;
        record.materialSlot = primitive.material.valid() ? primitive.material.index : UINT32_MAX;
        record.alphaMode = primitive.alphaMode;
        record.alphaCutoff = primitive.alphaCutoff;
        record.flags = (primitive.containsAlphaTestedGeometry ? 1u : 0u) | (primitive.containsBlendedGeometry ? 2u : 0u);
        primitiveRecords.push_back(record);
    }
    appendChunk(desc, ChunkMeshPrimitives, bytesOfVector(primitiveRecords));

    std::vector<uint32_t> faceMaterials;
    faceMaterials.reserve(mesh.indices.size() / 3u);
    for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
        const uint32_t triangleCount = primitive.indexCount / 3u;
        const uint32_t materialIndex = primitive.material.valid() ? primitive.material.index : 0u;
        for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
            faceMaterials.push_back(materialIndex);
        }
    }
    if (!mesh.vertices.empty() && !mesh.indices.empty() && faceMaterials.size() == mesh.indices.size() / 3u) {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec2> texcoords;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec4> tangents;
        positions.reserve(mesh.vertices.size());
        texcoords.reserve(mesh.vertices.size());
        normals.reserve(mesh.vertices.size());
        tangents.reserve(mesh.vertices.size());
        for (const MeshVertex& vertex : mesh.vertices) {
            positions.push_back(vertex.position);
            texcoords.push_back(vertex.texcoord);
            normals.push_back(vertex.normal);
            tangents.push_back(vertex.tangent);
        }
        const BvhBuildQuality quality = mesh.indices.size() / 3u >= 100000u
            ? BvhBuildQuality::MortonFast
            : BvhBuildQuality::BinnedSah;
        const BvhBuildResult localBvh = buildBvh(positions, mesh.indices, faceMaterials, &texcoords, &normals, &tangents, quality);
        appendChunk(desc, ChunkMeshLocalBvhNodes, bytesOfVector(packBvhNodesForGpu(localBvh.packedNodes)));
        appendChunk(desc, ChunkMeshLocalBvhTriangles, bytesOfVector(packTrianglesForGpu(localBvh)));
    }

    std::vector<std::array<uint8_t, 16>> materialSlots;
    materialSlots.reserve(materialGuids.size());
    for (size_t i = 0; i < materialGuids.size(); ++i) {
        const AssetGuid& guid = materialGuids[i];
        materialSlots.push_back(nativeGuidFromText(guid));
        appendUniqueDependency(desc, guid, NativeAssetKind::Material, "materialSlot." + std::to_string(i));
    }
    appendChunk(desc, ChunkMeshMaterialSlots, bytesOfVector(materialSlots));
    return finishWrite(input.outputPath, desc);
}

NativeAssetCookResult NativeAssetCooker::cookMaterial(
    const NativeAssetCookInput& input,
    const MaterialAsset& material,
    const std::vector<AssetGuid>& textureGuids) const {
    NativeAssetWriteDesc desc = baseWriteDesc(input, NativeAssetKind::Material);
    RtmaterialPayloadHeader header;
    header.baseColorFactor = {material.baseColorFactor.x, material.baseColorFactor.y, material.baseColorFactor.z, material.baseColorFactor.w};
    header.emissiveFactor = {material.emissiveFactor.x, material.emissiveFactor.y, material.emissiveFactor.z, material.emissiveStrength};
    header.metallicFactor = material.metallicFactor;
    header.roughnessFactor = material.roughnessFactor;
    header.alphaCutoff = material.alphaCutoff;
    header.occlusionStrength = material.occlusionStrength;
    header.alphaMode = material.alphaMode;
    header.shaderCompatibilityMask = material.shaderCompatibilityMask;
    const float heightScale = material.heightScale;
    uint32_t heightScaleBits = 0;
    std::memcpy(&heightScaleBits, &heightScale, sizeof(heightScaleBits));
    const uint32_t materialSemanticBits =
        ((material.materialWorkflow & 0xffu) << 0u) |
        ((material.normalMapConvention & 0xffu) << 8u) |
        ((material.specularTextureAlphaMode & 0xffu) << 16u);
    header.reserved = static_cast<uint64_t>(heightScaleBits) |
        (static_cast<uint64_t>(materialSemanticBits) << 32u);
    header.flags = material.doubleSided |
        (material.hasClearcoat << 1u) |
        (material.hasTransmission << 2u) |
        (material.hasVolume << 3u) |
        (material.hasIor << 4u) |
        (material.hasSpecular << 5u) |
        (material.hasSheen << 6u) |
        (material.hasIridescence << 7u) |
        (material.hasAnisotropy << 8u) |
        (material.hasEmissiveStrength << 9u);

    std::vector<RtmaterialTextureSlotRecord> slots;
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::BaseColor), material.baseColorTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Normal), material.normalTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::MetallicRoughness), material.metallicRoughnessTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Occlusion), material.occlusionTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Emissive), material.emissiveTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Transmission), material.transmissionTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Clearcoat), material.clearcoatTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::ClearcoatRoughness), material.clearcoatRoughnessTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::ClearcoatNormal), material.clearcoatNormalTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::VolumeThickness), material.volumeThicknessTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::SheenColor), material.sheenColorTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::SheenRoughness), material.sheenRoughnessTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Specular), material.specularTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::SpecularColor), material.specularColorTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Iridescence), material.iridescenceTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::IridescenceThickness), material.iridescenceThicknessTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Anisotropy), material.anisotropyTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Opacity), material.opacityTexture, textureGuids);
    addTextureSlot(slots, static_cast<uint32_t>(RtmaterialTextureSlot::Height), material.heightTexture, textureGuids);
    header.textureSlotCount = static_cast<uint32_t>(slots.size());
    header.textureSlotChunk = 1;
    for (const RtmaterialTextureSlotRecord& slot : slots) {
        if (slot.textureIndex < textureGuids.size()) {
            appendUniqueDependency(desc, textureGuids[slot.textureIndex], NativeAssetKind::Texture, "textureSlot." + std::to_string(slot.slot));
        }
    }

    appendChunk(desc, ChunkPayloadHeader, bytesOf(header));
    appendChunk(desc, ChunkMaterialTextureSlots, bytesOfVector(slots));
    appendChunk(desc, ChunkMaterialTextureTransforms, bytesOfVector(materialTextureTransformRecords(material)));
    return finishWrite(input.outputPath, desc);
}

NativeAssetCookResult NativeAssetCooker::cookTexture(
    const NativeAssetCookInput& input,
    const TextureAsset& texture,
    std::string_view role) const {
    NativeAssetWriteDesc desc = baseWriteDesc(input, NativeAssetKind::Texture);
    std::vector<RttextureMipRecord> mipRecords;
    std::vector<std::byte> imagePayload = texturePayloadBytes(texture, mipRecords);
    const NativeTextureRole textureRole = nativeTextureRoleFromString(role);
    const NativeTextureColorSpace colorSpace = nativeTextureColorSpaceForTexture(texture, textureRole);
    const NativeTextureCompressionPolicy compressionPolicy = nativeTextureCompressionPolicyForTexture(texture);
    VkFormat emittedVkFormat = texture.isCompressed && texture.compressedFormat != VK_FORMAT_UNDEFINED ? texture.compressedFormat : texture.format;
    const NativeTextureFormatSelection formatSelection = selectNativeTextureFormat(textureRole, colorSpace, textureFormatSupport_);
    if (!texture.isCompressed && formatSelection.supported &&
        (formatSelection.selectedFormat == VK_FORMAT_R8G8B8A8_SRGB || formatSelection.selectedFormat == VK_FORMAT_R8G8B8A8_UNORM) &&
        (texture.format == VK_FORMAT_R8G8B8A8_UNORM || texture.format == VK_FORMAT_R8G8B8A8_SRGB)) {
        emittedVkFormat = formatSelection.selectedFormat;
    }
    if (!texture.isCompressed && formatSelection.supported &&
        (formatSelection.selectedFormat == VK_FORMAT_BC4_UNORM_BLOCK ||
            formatSelection.selectedFormat == VK_FORMAT_BC5_UNORM_BLOCK ||
            formatSelection.selectedFormat == VK_FORMAT_BC7_UNORM_BLOCK ||
            formatSelection.selectedFormat == VK_FORMAT_BC7_SRGB_BLOCK) &&
        (texture.format == VK_FORMAT_R8G8B8A8_UNORM || texture.format == VK_FORMAT_R8G8B8A8_SRGB) &&
        convertRgba8PayloadToBc(imagePayload, mipRecords, formatSelection.selectedFormat)) {
        emittedVkFormat = formatSelection.selectedFormat;
    }
    if (!texture.isCompressed && formatSelection.supported &&
        formatSelection.selectedFormat == VK_FORMAT_R16G16B16A16_SFLOAT &&
        texture.format == VK_FORMAT_R32G32B32A32_SFLOAT &&
        convertRgba32fPayloadToRgba16f(imagePayload, mipRecords)) {
        emittedVkFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    const bool selectedFormatRealized = emittedVkFormat == formatSelection.selectedFormat;
    const bool compressedPayloadEmission = selectedFormatRealized && nativeTextureBlockCompressedFormat(emittedVkFormat);
    const std::string payloadVariant = nativeTexturePayloadVariantName(texture, emittedVkFormat, formatSelection, compressionPolicy);
    const std::vector<NativeTexturePayloadVariantPlanEntry> payloadVariantPlan = nativeTexturePayloadVariantPlan(texture, emittedVkFormat, formatSelection, compressionPolicy);

    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "textureRole", .value = nativeTextureRoleName(textureRole)});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "textureColorSpace", .value = nativeTextureColorSpaceName(colorSpace)});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "emittedVkFormat", .value = nativeTextureFormatName(emittedVkFormat)});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "platformFormatPolicy", .value = textureFormatSupport_.platformName});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "platformFormatSupportSource", .value = textureFormatSupport_.queriedFromVulkan ? "vulkanPhysicalDevice" : "offlineFallback"});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "platformSelectedVkFormat", .value = nativeTextureFormatName(formatSelection.selectedFormat)});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "platformFormatSelectionReason", .value = formatSelection.reason});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "compressionPolicy", .value = nativeTextureCompressionPolicyName(compressionPolicy)});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "platformSelectedVkFormatRealized", .value = selectedFormatRealized ? "true" : "false"});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "platformCompressedPayloadEmission", .value = compressedPayloadEmission ? "true" : "false"});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "nativeTexturePayloadVariant", .value = payloadVariant});
    if (!texture.sourceContainerKind.empty()) {
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "sourceContainerKind", .value = texture.sourceContainerKind});
    }
    if (!texture.nativePayloadSource.empty()) {
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "nativePayloadSource", .value = texture.nativePayloadSource});
    }
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "sourceContainerPreserved", .value = texture.sourceContainerPreserved ? "true" : "false"});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "sourceContainerTranscoded", .value = texture.sourceContainerTranscoded ? "true" : "false"});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = std::string(kJsonVariantPlan), .value = nativeTexturePayloadVariantPlanJson(payloadVariantPlan).dump()});
    if (!formatSelection.fallbackReason.empty()) {
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 2, .key = "platformFormatFallbackReason", .value = formatSelection.fallbackReason});
    }
    if (textureRole == NativeTextureRole::Unknown) {
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 2, .key = "texturePolicyWarning", .value = "low-confidence-role-inference"});
    }

    RttexturePayloadHeader header;
    header.width = texture.width;
    header.height = texture.height;
    header.mipCount = static_cast<uint32_t>(std::max(1, texture.mipLevels));
    header.vkFormat = static_cast<uint32_t>(emittedVkFormat);
    header.colorSpace = static_cast<uint32_t>(colorSpace);
    header.role = static_cast<uint32_t>(textureRole);
    header.sampler = samplerPacked(texture.sampler);
    header.compression = static_cast<uint32_t>(compressionPolicy);
    header.mipTableChunk = 1;
    header.payloadChunk = 2;
    header.flags = texture.linearColorSpace ? 1u : 0u;
    appendChunk(desc, ChunkPayloadHeader, bytesOf(header));
    appendChunk(desc, ChunkTextureMipTable, bytesOfVector(mipRecords));
    appendChunk(desc, ChunkTexturePayload, std::move(imagePayload));
    NativeAssetCookResult result = finishWrite(input.outputPath, desc);
    result.emittedVkFormat = nativeTextureFormatName(emittedVkFormat);
    result.platformSelectedVkFormat = nativeTextureFormatName(formatSelection.selectedFormat);
    result.texturePayloadVariant = payloadVariant;
    result.sourceContainerKind = texture.sourceContainerKind;
    result.nativePayloadSource = texture.nativePayloadSource;
    result.platformSelectedFormatRealized = selectedFormatRealized;
    result.platformCompressedPayloadEmission = compressedPayloadEmission;
    result.sourceContainerPreserved = texture.sourceContainerPreserved;
    result.sourceContainerTranscoded = texture.sourceContainerTranscoded;
    result.texturePayloadVariantPlan = payloadVariantPlan;
    if (textureRole == NativeTextureRole::Unknown) {
        result.warnings.push_back("Texture role inference was low-confidence; native payload role is unknown.");
    }
    return result;
}

NativeAssetCookResult NativeAssetCooker::cookAnimationController(
    const NativeAssetCookInput& input,
    const AnimationController& controller) const {
    NativeAssetCookResult result;
    result.path = input.outputPath;
    if (!controller.valid()) {
        result.errors.push_back("Animation controller native cook failed: controller has no valid initial state.");
        return result;
    }

    NativeAssetWriteDesc desc = baseWriteDesc(input, NativeAssetKind::AnimationController);
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "payloadClass", .value = "compact_rtanimcontroller"});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "controllerName", .value = controller.name()});

    std::vector<std::byte> stringTable;
    std::map<std::string, uint32_t> stringOffsets;

    std::vector<RtanimControllerParameterRecord> parameterRecords;
    parameterRecords.reserve(controller.parameters().size());
    for (const AnimationController::Parameter& parameter : controller.parameters()) {
        const NativeStringRef name = appendNativeString(stringTable, stringOffsets, parameter.name);
        RtanimControllerParameterRecord record;
        record.nameOffset = name.offset;
        record.nameSize = name.size;
        record.type = parameterTypeToNative(parameter.type);
        writeParameterValue(parameter.type, parameter.defaultValue, record.boolValue, record.intValue, record.floatValue, record.triggerValue);
        parameterRecords.push_back(record);
    }

    std::vector<RtanimControllerStateRecord> stateRecords;
    std::vector<RtanimControllerTransitionRecord> transitionRecords;
    std::vector<RtanimControllerConditionRecord> conditionRecords;
    std::vector<RtanimControllerEventRecord> eventRecords;
    std::vector<RtanimControllerBlendTreeRecord> blendTreeRecords;
    std::vector<RtanimControllerBlendTreeChildRecord> blendTreeChildRecords;
    stateRecords.reserve(controller.states().size());

    for (size_t stateIndex = 0; stateIndex < controller.states().size(); ++stateIndex) {
        const AnimationController::State& state = controller.states()[stateIndex];
        const NativeStringRef name = appendNativeString(stringTable, stringOffsets, state.name);
        const NativeStringRef clipPath = appendNativeString(stringTable, stringOffsets, state.clipPath.generic_string());
        const auto [firstStateEvent, stateEventCount] = appendControllerEvents(state.events, eventRecords, stringTable, stringOffsets);
        const uint32_t firstTransition = static_cast<uint32_t>(transitionRecords.size());

        if (!state.clipGuid.empty()) {
            appendUniqueDependency(desc, state.clipGuid, NativeAssetKind::Animation, state.name + ".clip");
        }

        for (const AnimationController::Transition& transition : state.transitions) {
            const NativeStringRef to = appendNativeString(stringTable, stringOffsets, transition.to);
            const uint32_t firstCondition = static_cast<uint32_t>(conditionRecords.size());
            for (const AnimationController::Condition& condition : transition.conditions) {
                const NativeStringRef parameter = appendNativeString(stringTable, stringOffsets, condition.parameter);
                const NativeStringRef op = appendNativeString(stringTable, stringOffsets, condition.op);
                RtanimControllerConditionRecord record;
                record.parameterOffset = parameter.offset;
                record.parameterSize = parameter.size;
                record.opOffset = op.offset;
                record.opSize = op.size;
                record.type = parameterTypeToNative(condition.value.type);
                writeParameterValue(condition.value.type, condition.value, record.boolValue, record.intValue, record.floatValue, record.triggerValue);
                conditionRecords.push_back(record);
            }
            const auto [firstTransitionEvent, transitionEventCount] = appendControllerEvents(transition.events, eventRecords, stringTable, stringOffsets);
            RtanimControllerTransitionRecord record;
            record.toOffset = to.offset;
            record.toSize = to.size;
            record.exitTimeSeconds = transition.exitTimeSeconds;
            record.firstCondition = firstCondition;
            record.conditionCount = static_cast<uint32_t>(conditionRecords.size() - firstCondition);
            record.firstEvent = firstTransitionEvent;
            record.eventCount = transitionEventCount;
            transitionRecords.push_back(record);
        }

        uint32_t blendTreeIndex = UINT32_MAX;
        if (state.hasBlendTree && !state.blendTree.children.empty()) {
            const NativeStringRef type = appendNativeString(stringTable, stringOffsets, state.blendTree.type);
            const NativeStringRef parameter = appendNativeString(stringTable, stringOffsets, state.blendTree.parameter);
            const uint32_t firstChild = static_cast<uint32_t>(blendTreeChildRecords.size());
            for (const AnimationController::BlendTreeChild& child : state.blendTree.children) {
                const NativeStringRef childName = appendNativeString(stringTable, stringOffsets, child.name);
                const NativeStringRef childPath = appendNativeString(stringTable, stringOffsets, child.clipPath.generic_string());
                RtanimControllerBlendTreeChildRecord record;
                record.nameOffset = childName.offset;
                record.nameSize = childName.size;
                record.clipGuid = nativeGuidFromText(child.clipGuid);
                record.clipPathOffset = childPath.offset;
                record.clipPathSize = childPath.size;
                record.threshold = child.threshold;
                blendTreeChildRecords.push_back(record);
                if (!child.clipGuid.empty()) {
                    appendUniqueDependency(desc, child.clipGuid, NativeAssetKind::Animation, state.name + ".blendTree." + child.name);
                }
            }
            blendTreeIndex = static_cast<uint32_t>(blendTreeRecords.size());
            RtanimControllerBlendTreeRecord record;
            record.stateIndex = static_cast<uint32_t>(stateIndex);
            record.typeOffset = type.offset;
            record.typeSize = type.size;
            record.parameterOffset = parameter.offset;
            record.parameterSize = parameter.size;
            record.firstChild = firstChild;
            record.childCount = static_cast<uint32_t>(blendTreeChildRecords.size() - firstChild);
            blendTreeRecords.push_back(record);
        }

        RtanimControllerStateRecord record;
        record.nameOffset = name.offset;
        record.nameSize = name.size;
        record.clipGuid = nativeGuidFromText(state.clipGuid);
        record.clipPathOffset = clipPath.offset;
        record.clipPathSize = clipPath.size;
        record.speed = state.speed;
        record.loop = state.loop ? 1u : 0u;
        record.defaultState = state.defaultState || state.name == controller.initialState() ? 1u : 0u;
        record.firstTransition = firstTransition;
        record.transitionCount = static_cast<uint32_t>(transitionRecords.size() - firstTransition);
        record.firstEvent = firstStateEvent;
        record.eventCount = stateEventCount;
        record.blendTreeIndex = blendTreeIndex;
        stateRecords.push_back(record);
    }

    std::vector<RtanimControllerLayerRecord> layerRecords;
    layerRecords.reserve(controller.layers().size());
    for (const AnimationController::Layer& layer : controller.layers()) {
        const NativeStringRef name = appendNativeString(stringTable, stringOffsets, layer.name);
        const NativeStringRef clipPath = appendNativeString(stringTable, stringOffsets, layer.clipPath.generic_string());
        const NativeStringRef mask = appendNativeString(stringTable, stringOffsets, layer.mask);
        RtanimControllerLayerRecord record;
        record.nameOffset = name.offset;
        record.nameSize = name.size;
        record.clipGuid = nativeGuidFromText(layer.clipGuid);
        record.clipPathOffset = clipPath.offset;
        record.clipPathSize = clipPath.size;
        record.weight = layer.weight;
        record.additive = layer.additive ? 1u : 0u;
        record.maskOffset = mask.offset;
        record.maskSize = mask.size;
        layerRecords.push_back(record);
        if (!layer.clipGuid.empty()) {
            appendUniqueDependency(desc, layer.clipGuid, NativeAssetKind::Animation, layer.name + ".layerClip");
        }
    }

    std::vector<RtanimControllerAvatarMaskRecord> maskRecords;
    std::vector<RtanimControllerAvatarMaskJointRecord> maskJointRecords;
    maskRecords.reserve(controller.avatarMasks().size());
    for (const AnimationController::AvatarMask& mask : controller.avatarMasks()) {
        const NativeStringRef name = appendNativeString(stringTable, stringOffsets, mask.name);
        const uint32_t firstIncluded = static_cast<uint32_t>(maskJointRecords.size());
        for (const std::string& joint : mask.includedJoints) {
            const NativeStringRef jointName = appendNativeString(stringTable, stringOffsets, joint);
            maskJointRecords.push_back(RtanimControllerAvatarMaskJointRecord{.nameOffset = jointName.offset, .nameSize = jointName.size});
        }
        const uint32_t firstExcluded = static_cast<uint32_t>(maskJointRecords.size());
        for (const std::string& joint : mask.excludedJoints) {
            const NativeStringRef jointName = appendNativeString(stringTable, stringOffsets, joint);
            maskJointRecords.push_back(RtanimControllerAvatarMaskJointRecord{.nameOffset = jointName.offset, .nameSize = jointName.size});
        }
        RtanimControllerAvatarMaskRecord record;
        record.nameOffset = name.offset;
        record.nameSize = name.size;
        record.firstIncludedJoint = firstIncluded;
        record.includedJointCount = static_cast<uint32_t>(firstExcluded - firstIncluded);
        record.firstExcludedJoint = firstExcluded;
        record.excludedJointCount = static_cast<uint32_t>(maskJointRecords.size() - firstExcluded);
        maskRecords.push_back(record);
    }

    RtanimControllerPayloadHeader header;
    header.parameterCount = static_cast<uint32_t>(parameterRecords.size());
    header.stateCount = static_cast<uint32_t>(stateRecords.size());
    header.transitionCount = static_cast<uint32_t>(transitionRecords.size());
    header.layerCount = static_cast<uint32_t>(layerRecords.size());
    header.parameterChunk = 2u;
    header.stateChunk = 3u;
    header.transitionChunk = 4u;
    header.blendTreeChunk = 7u;
    header.eventRouteChunk = 6u;

    appendChunk(desc, kRtanimControllerChunkPayloadHeader, bytesOf(header));
    appendChunk(desc, kRtanimControllerChunkStringTable, std::move(stringTable));
    appendChunk(desc, kRtanimControllerChunkParameters, bytesOfVector(parameterRecords));
    appendChunk(desc, kRtanimControllerChunkStates, bytesOfVector(stateRecords));
    appendChunk(desc, kRtanimControllerChunkTransitions, bytesOfVector(transitionRecords));
    appendChunk(desc, kRtanimControllerChunkConditions, bytesOfVector(conditionRecords));
    appendChunk(desc, kRtanimControllerChunkEvents, bytesOfVector(eventRecords));
    appendChunk(desc, kRtanimControllerChunkBlendTrees, bytesOfVector(blendTreeRecords));
    appendChunk(desc, kRtanimControllerChunkBlendTreeChildren, bytesOfVector(blendTreeChildRecords));
    appendChunk(desc, kRtanimControllerChunkLayers, bytesOfVector(layerRecords));
    appendChunk(desc, kRtanimControllerChunkAvatarMasks, bytesOfVector(maskRecords));
    appendChunk(desc, kRtanimControllerChunkAvatarMaskJoints, bytesOfVector(maskJointRecords));
    return finishWrite(input.outputPath, desc);
}

NativeAssetCookResult NativeAssetCooker::cookSkeletalMeshBinding(
    const NativeAssetCookInput& input,
    const AssetGuid& meshGuid,
    const AssetGuid& skeletonGuid,
    const std::vector<uint32_t>& jointRemap,
    const nlohmann::json& bindMetadata) const {
    NativeAssetCookResult result;
    result.path = input.outputPath;
    if (meshGuid.empty() || skeletonGuid.empty()) {
        result.errors.push_back("Skeletal mesh binding native cook failed: mesh and skeleton GUIDs are required.");
        return result;
    }
    if (jointRemap.empty()) {
        result.errors.push_back("Skeletal mesh binding native cook failed: joint remap must contain at least one joint.");
        return result;
    }

    NativeAssetWriteDesc desc = baseWriteDesc(input, NativeAssetKind::SkeletalMesh);
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "payloadClass", .value = "skeletal_mesh_binding"});
    appendUniqueDependency(desc, meshGuid, NativeAssetKind::Mesh, "boundMesh");
    appendUniqueDependency(desc, skeletonGuid, NativeAssetKind::Skeleton, "boundSkeleton");

    RtskeletalMeshPayloadHeader header;
    header.meshGuid = nativeGuidFromText(meshGuid);
    header.skeletonGuid = nativeGuidFromText(skeletonGuid);
    header.jointRemapCount = static_cast<uint32_t>(jointRemap.size());
    header.jointRemapChunk = kRtskeletalMeshChunkJointRemap;
    header.skinningDataChunk = kRtskeletalMeshChunkSkinningData;
    header.bindMetadataChunk = kRtskeletalMeshChunkBindMetadataJson;
    header.flags = 1u;

    RtskeletalMeshSkinningSummaryRecord skinningSummary;
    skinningSummary.jointRemapCount = static_cast<uint32_t>(jointRemap.size());
    skinningSummary.flags = 1u;

    nlohmann::json metadata = bindMetadata.is_object() ? bindMetadata : nlohmann::json::object();
    metadata["schema"] = "NativeSkeletalMeshBindingCookV1";
    metadata["meshGuid"] = meshGuid;
    metadata["skeletonGuid"] = skeletonGuid;
    metadata["jointRemapCount"] = jointRemap.size();

    appendChunk(desc, kRtskeletalMeshChunkPayloadHeader, bytesOf(header));
    appendChunk(desc, kRtskeletalMeshChunkJointRemap, bytesOfVector(jointRemap));
    appendChunk(desc, kRtskeletalMeshChunkSkinningData, bytesOf(skinningSummary));
    appendChunk(desc, kRtskeletalMeshChunkBindMetadataJson, bytesOfString(metadata.dump(2)));
    return finishWrite(input.outputPath, desc);
}

NativeAssetCookResult NativeAssetCooker::cookMetadataPayload(
    const NativeAssetCookInput& input,
    NativeAssetKind kind,
    const nlohmann::json& metadata) const {
    NativeAssetWriteDesc desc = baseWriteDesc(input, kind);
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "payloadClass", .value = "metadata_bridge"});
    appendChunk(desc, ChunkMetadataJson, bytesOfString(metadata.dump(2)));
    return finishWrite(input.outputPath, desc);
}

nlohmann::json nativeCookRuntimePayloadJson(
    const NativeAssetCookResult& result,
    NativeAssetKind kind,
    const AssetGuid& guid,
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& sourcePath,
    const std::string& sourceHash,
    const std::string& importSettingsHash) {
    nlohmann::json payload = {
        {"kind", std::string("Native") + nativeAssetKindName(kind)},
        {"assetKind", nativeAssetKindName(kind)},
        {"assetGuid", guid},
        {"cachePath", relativeOrAbsolute(result.path, workspaceRoot)},
        {"sourcePath", sourcePath.generic_string()},
        {"sourceHash", sourceHash},
        {"importSettingsHash", importSettingsHash},
        {"payloadHash", result.payloadHash},
        {"payloadBytes", result.payloadBytes},
        {"available", result.success},
        {"validForSource", result.success},
        {"nativeStandalone", true},
        {"formatVersion", 1},
    };
    if (kind == NativeAssetKind::Texture && !result.emittedVkFormat.empty()) {
        payload["nativeTexturePayload"] = {
            {"emittedVkFormat", result.emittedVkFormat},
            {"platformSelectedVkFormat", result.platformSelectedVkFormat},
            {"platformSelectedVkFormatRealized", result.platformSelectedFormatRealized},
            {"platformCompressedPayloadEmission", result.platformCompressedPayloadEmission},
            {"nativeTexturePayloadVariant", result.texturePayloadVariant},
            {"sourceContainerKind", result.sourceContainerKind},
            {"nativePayloadSource", result.nativePayloadSource},
            {"sourceContainerPreserved", result.sourceContainerPreserved},
            {"sourceContainerTranscoded", result.sourceContainerTranscoded},
        };
    }
    if (kind == NativeAssetKind::Texture && !result.texturePayloadVariantPlan.empty()) {
        payload[std::string(kJsonNativeTexturePayload)][std::string(kJsonVariantPlan)] = nativeTexturePayloadVariantPlanJson(result.texturePayloadVariantPlan);
    }
    return payload;
}

} // namespace rtv
