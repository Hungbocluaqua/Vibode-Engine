#include "rtv/NativeBinaryIO.h"

#include <Volk/volk.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <type_traits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace rtv {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t fnv1a(const std::byte* data, size_t size, uint64_t seed) {
    uint64_t value = kFnvOffset ^ seed;
    for (size_t i = 0; i < size; ++i) {
        value ^= static_cast<uint8_t>(data[i]);
        value *= kFnvPrime;
    }
    return value;
}

void fnv1aUpdate(uint64_t& value, const std::byte* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        value ^= static_cast<uint8_t>(data[i]);
        value *= kFnvPrime;
    }
}

std::array<uint8_t, 32> nativeHashPartsToBytes(const std::array<uint64_t, 4>& parts) {
    std::array<uint8_t, 32> hash{};
    for (size_t i = 0; i < parts.size(); ++i) {
        for (size_t b = 0; b < 8; ++b) {
            hash[i * 8 + b] = static_cast<uint8_t>((parts[i] >> (b * 8)) & 0xffu);
        }
    }
    return hash;
}

template <typename T>
void appendPod(std::vector<std::byte>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
std::vector<std::byte> bytesOfPod(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<std::byte> out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

template <typename T>
std::vector<std::byte> bytesOfPodVector(const std::vector<T>& values) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<std::byte> out(sizeof(T) * values.size());
    if (!out.empty()) {
        std::memcpy(out.data(), values.data(), out.size());
    }
    return out;
}

struct FixtureRttextureMipRecord {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct FixtureRtmaterialTextureSlotRecord {
    uint32_t slot = 0;
    std::array<uint8_t, 16> textureGuid{};
    uint32_t textureIndex = UINT32_MAX;
    uint32_t flags = 0;
};

const char* fixtureVkFormatName(uint32_t format) {
    switch (static_cast<VkFormat>(format)) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return "BC1_RGB_UNORM";
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return "BC1_RGB_SRGB";
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1_RGBA_UNORM";
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "BC1_RGBA_SRGB";
    case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3_UNORM";
    case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7_UNORM";
    case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7_SRGB";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5_UNORM";
    case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4_UNORM";
    default: return "fixture_custom_vk_format";
    }
}

bool fixtureBlockCompressedFormat(uint32_t format) {
    switch (static_cast<VkFormat>(format)) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

bool fixtureHdrDecodedFormat(uint32_t format) {
    const VkFormat vkFormat = static_cast<VkFormat>(format);
    return vkFormat == VK_FORMAT_R16G16B16A16_SFLOAT || vkFormat == VK_FORMAT_R32G32B32A32_SFLOAT;
}

std::string lowerFixtureAscii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

NativeTextureRole fixtureTextureRoleFromString(std::string_view value) {
    if (value.empty()) {
        return NativeTextureRole::BaseColor;
    }
    const std::string role = lowerFixtureAscii(value);
    if (role == "basecolor" || role == "base_color" || role == "albedo" || role == "diffuse") return NativeTextureRole::BaseColor;
    if (role == "normal" || role == "norm" || role == "nrm") return NativeTextureRole::Normal;
    if (role == "metallicroughness" || role == "metallic_roughness" || role == "metalrough" || role == "orm" || role == "rma" || role == "mra" || role == "mrao" || role == "rmao") return NativeTextureRole::MetallicRoughness;
    if (role == "metallic" || role == "metalness" || role == "metal") return NativeTextureRole::Metallic;
    if (role == "roughness" || role == "rough") return NativeTextureRole::Roughness;
    if (role == "occlusion" || role == "ambientocclusion" || role == "ao") return NativeTextureRole::Occlusion;
    if (role == "emissive" || role == "emission" || role == "emit") return NativeTextureRole::Emissive;
    if (role == "opacity" || role == "alpha" || role == "transparency" || role == "mask") return NativeTextureRole::Opacity;
    if (role == "height" || role == "displacement" || role == "disp" || role == "bump") return NativeTextureRole::Height;
    if (role == "thickness" || role == "volume_thickness" || role == "volumethickness") return NativeTextureRole::Thickness;
    if (role == "environmenthdr" || role == "environment" || role == "hdr" || role == "hdri") return NativeTextureRole::EnvironmentHdr;
    if (role == "data") return NativeTextureRole::Data;
    if (role == "specular") return NativeTextureRole::Specular;
    if (role == "specularcolor" || role == "specular_color") return NativeTextureRole::SpecularColor;
    if (role == "transmission") return NativeTextureRole::Transmission;
    if (role == "clearcoat") return NativeTextureRole::Clearcoat;
    if (role == "clearcoatroughness" || role == "clearcoat_roughness") return NativeTextureRole::ClearcoatRoughness;
    if (role == "clearcoatnormal" || role == "clearcoat_normal") return NativeTextureRole::ClearcoatNormal;
    if (role == "sheen") return NativeTextureRole::Sheen;
    if (role == "sheencolor" || role == "sheen_color") return NativeTextureRole::SheenColor;
    if (role == "sheenroughness" || role == "sheen_roughness") return NativeTextureRole::SheenRoughness;
    if (role == "iridescence") return NativeTextureRole::Iridescence;
    if (role == "iridescencethickness" || role == "iridescence_thickness") return NativeTextureRole::IridescenceThickness;
    if (role == "anisotropy") return NativeTextureRole::Anisotropy;
    return NativeTextureRole::Unknown;
}

const char* fixtureTextureRoleName(NativeTextureRole role) {
    switch (role) {
    case NativeTextureRole::BaseColor: return "baseColor";
    case NativeTextureRole::Normal: return "normal";
    case NativeTextureRole::MetallicRoughness: return "metallicRoughness";
    case NativeTextureRole::Metallic: return "metallic";
    case NativeTextureRole::Roughness: return "roughness";
    case NativeTextureRole::Occlusion: return "occlusion";
    case NativeTextureRole::Emissive: return "emissive";
    case NativeTextureRole::Opacity: return "opacity";
    case NativeTextureRole::Height: return "height";
    case NativeTextureRole::EnvironmentHdr: return "environmentHDR";
    case NativeTextureRole::Data: return "data";
    case NativeTextureRole::Specular: return "specular";
    case NativeTextureRole::SpecularColor: return "specularColor";
    case NativeTextureRole::Transmission: return "transmission";
    case NativeTextureRole::Clearcoat: return "clearcoat";
    case NativeTextureRole::ClearcoatRoughness: return "clearcoatRoughness";
    case NativeTextureRole::ClearcoatNormal: return "clearcoatNormal";
    case NativeTextureRole::Sheen: return "sheen";
    case NativeTextureRole::SheenColor: return "sheenColor";
    case NativeTextureRole::SheenRoughness: return "sheenRoughness";
    case NativeTextureRole::Iridescence: return "iridescence";
    case NativeTextureRole::IridescenceThickness: return "iridescenceThickness";
    case NativeTextureRole::Anisotropy: return "anisotropy";
    case NativeTextureRole::Thickness: return "thickness";
    case NativeTextureRole::Unknown:
    default: return "unknown";
    }
}

std::vector<std::byte> fixtureTexturePayload(uint32_t format) {
    const bool blockCompressed = fixtureBlockCompressedFormat(format);
    size_t byteCount = 64u;
    if (blockCompressed) {
        byteCount = static_cast<VkFormat>(format) == VK_FORMAT_BC4_UNORM_BLOCK ? 8u : 16u;
    } else if (static_cast<VkFormat>(format) == VK_FORMAT_R16G16B16A16_SFLOAT) {
        byteCount = 4u * 4u * 4u * sizeof(uint16_t);
    } else if (static_cast<VkFormat>(format) == VK_FORMAT_R32G32B32A32_SFLOAT) {
        byteCount = 4u * 4u * 4u * sizeof(float);
    }
    std::vector<std::byte> payload(byteCount);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>((i * 37u + static_cast<uint32_t>(format)) & 0xffu);
    }
    return payload;
}

void appendBytes(std::vector<std::byte>& out, const std::vector<std::byte>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendPadding(std::vector<std::byte>& out, uint64_t alignment = kNativeAssetAlignment) {
    const uint64_t aligned = nativeAlignUp(static_cast<uint64_t>(out.size()), alignment);
    while (out.size() < aligned) {
        out.push_back(std::byte{0});
    }
}

bool writeRaw(std::ofstream& file, const std::byte* data, uint64_t size) {
    if (size == 0) {
        return true;
    }
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file.good();
}

bool writeVector(std::ofstream& file, const std::vector<std::byte>& bytes) {
    return writeRaw(file, bytes.data(), static_cast<uint64_t>(bytes.size()));
}

bool writePadding(std::ofstream& file, uint64_t& cursor, uint64_t alignment = kNativeAssetAlignment) {
    const uint64_t aligned = nativeAlignUp(cursor, alignment);
    if (aligned == cursor) {
        return true;
    }
    std::array<std::byte, kNativeAssetAlignment> zeros{};
    uint64_t remaining = aligned - cursor;
    while (remaining > 0) {
        const uint64_t chunk = std::min<uint64_t>(remaining, zeros.size());
        if (!writeRaw(file, zeros.data(), chunk)) {
            return false;
        }
        remaining -= chunk;
        cursor += chunk;
    }
    return true;
}

bool readFileBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes, NativeBinaryError* error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (error) {
            error->code = NativeBinaryErrorCode::IoError;
            error->path = path;
            error->message = "Could not open native asset file for reading";
        }
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        if (error) {
            error->code = NativeBinaryErrorCode::IoError;
            error->path = path;
            error->message = "Could not determine native asset file size";
        }
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!file.good() && size != 0) {
        if (error) {
            error->code = NativeBinaryErrorCode::IoError;
            error->path = path;
            error->message = "Could not read complete native asset file";
        }
        return false;
    }
    return true;
}

NativeBinaryError makeError(
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

bool rangeInside(uint64_t offset, uint64_t size, uint64_t fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
}

template <typename T>
bool readRecordRange(
    const std::vector<std::byte>& bytes,
    const std::filesystem::path& path,
    const char* tableName,
    uint64_t offset,
    uint32_t count,
    uint32_t stride,
    std::vector<T>& out,
    std::vector<NativeBinaryError>& errors) {
    if (count == 0) {
        return true;
    }
    if (stride != sizeof(T)) {
        errors.push_back(makeError(
            NativeBinaryErrorCode::CorruptTable,
            path,
            tableName,
            offset,
            sizeof(T),
            "Native table stride does not match this reader"));
        return false;
    }
    const uint64_t totalSize = static_cast<uint64_t>(count) * static_cast<uint64_t>(stride);
    if (!rangeInside(offset, totalSize, bytes.size())) {
        errors.push_back(makeError(
            NativeBinaryErrorCode::CorruptTable,
            path,
            tableName,
            offset,
            totalSize,
            "Native table offset/count extends outside the file"));
        return false;
    }
    out.resize(count);
    std::memcpy(out.data(), bytes.data() + offset, static_cast<size_t>(totalSize));
    return true;
}

uint32_t appendString(std::vector<std::byte>& debugDirectory, std::map<std::string, uint32_t>& offsets, const std::string& value) {
    const auto existing = offsets.find(value);
    if (existing != offsets.end()) {
        return existing->second;
    }
    const uint32_t offset = static_cast<uint32_t>(debugDirectory.size());
    offsets.emplace(value, offset);
    for (char c : value) {
        debugDirectory.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    debugDirectory.push_back(std::byte{0});
    return offset;
}

std::string debugString(const std::vector<std::byte>& bytes, uint64_t debugOffset, uint64_t debugSize, uint32_t offset, uint32_t size) {
    if (size == 0 || !rangeInside(debugOffset + offset, size, bytes.size()) || offset >= debugSize) {
        return {};
    }
    const uint64_t available = std::min<uint64_t>(size, debugSize - offset);
    const char* begin = reinterpret_cast<const char*>(bytes.data() + debugOffset + offset);
    return std::string(begin, begin + available);
}

std::string debugDirectoryString(const std::vector<std::byte>& debugDirectory, uint32_t offset, uint32_t size) {
    if (size == 0 || offset >= debugDirectory.size()) {
        return {};
    }
    const uint64_t available = std::min<uint64_t>(size, static_cast<uint64_t>(debugDirectory.size()) - offset);
    const char* begin = reinterpret_cast<const char*>(debugDirectory.data() + offset);
    return std::string(begin, begin + available);
}

nlohmann::json hashJson(const std::array<uint8_t, 32>& hash) {
    return nativeHashHex(hash);
}

const char* nativeChunkCompressionNameLocal(uint32_t compression) {
    switch (static_cast<NativeChunkCompression>(compression)) {
    case NativeChunkCompression::None: return "none";
    case NativeChunkCompression::ReservedZstd: return "zstd";
    case NativeChunkCompression::ReservedGDeflate: return "gdeflate";
    }
    return "unknown";
}

bool replaceFileAtomically(const std::filesystem::path& tempPath, const std::filesystem::path& destination, std::error_code& ec) {
    ec.clear();
#if defined(_WIN32)
    if (MoveFileExW(tempPath.wstring().c_str(), destination.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
#else
    std::filesystem::rename(tempPath, destination, ec);
    return !ec;
#endif
}

} // namespace

uint64_t nativeAlignUp(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    const uint64_t remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

std::array<uint8_t, 32> nativeHashBytes(const std::byte* data, size_t size) {
    const std::array<uint64_t, 4> parts{
        fnv1a(data, size, 0x9ae16a3b2f90404full),
        fnv1a(data, size, 0xc949d7c7509e6557ull),
        fnv1a(data, size, 0xff51afd7ed558ccdull),
        fnv1a(data, size, 0xc4ceb9fe1a85ec53ull),
    };
    return nativeHashPartsToBytes(parts);
}

std::array<uint8_t, 32> nativeHashBytes(const std::vector<std::byte>& data) {
    return nativeHashBytes(data.data(), data.size());
}

std::array<uint8_t, 32> nativeHashText(std::string_view text) {
    return nativeHashBytes(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

std::string nativeHex(const uint8_t* data, size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<uint32_t>(data[i]);
    }
    return out.str();
}

std::string nativeHashHex(const std::array<uint8_t, 32>& hash) {
    return nativeHex(hash.data(), hash.size());
}

std::array<uint8_t, 16> nativeGuidFromText(std::string_view text) {
    std::string hex;
    hex.reserve(32);
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (hex.size() < 32) {
        const auto hash = nativeHashText(text.empty() ? "native-asset-fixture" : text);
        std::array<uint8_t, 16> guid{};
        std::copy(hash.begin(), hash.begin() + 16, guid.begin());
        return guid;
    }
    std::array<uint8_t, 16> guid{};
    for (size_t i = 0; i < guid.size(); ++i) {
        const std::string byteText = hex.substr(i * 2, 2);
        guid[i] = static_cast<uint8_t>(std::stoul(byteText, nullptr, 16));
    }
    return guid;
}

std::string nativeGuidToText(const std::array<uint8_t, 16>& guid) {
    const std::string hex = nativeHex(guid.data(), guid.size());
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
        hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

const char* nativeAssetKindName(NativeAssetKind kind) {
    switch (kind) {
    case NativeAssetKind::Mesh: return "mesh";
    case NativeAssetKind::Material: return "material";
    case NativeAssetKind::Texture: return "texture";
    case NativeAssetKind::Skeleton: return "skeleton";
    case NativeAssetKind::Animation: return "animation";
    case NativeAssetKind::AnimationController: return "animation_controller";
    case NativeAssetKind::SkeletalMesh: return "skeletal_mesh";
    case NativeAssetKind::Package: return "package";
    case NativeAssetKind::Unknown: break;
    }
    return "unknown";
}

NativeAssetKind nativeAssetKindFromExtension(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    if (ext == ".rtmesh") return NativeAssetKind::Mesh;
    if (ext == ".rtmaterial") return NativeAssetKind::Material;
    if (ext == ".rttexture") return NativeAssetKind::Texture;
    if (ext == ".rtskeleton") return NativeAssetKind::Skeleton;
    if (ext == ".rtanim") return NativeAssetKind::Animation;
    if (ext == ".rtanimcontroller") return NativeAssetKind::AnimationController;
    if (ext == ".rtskeletalmesh") return NativeAssetKind::SkeletalMesh;
    if (ext == ".rtpkg") return NativeAssetKind::Package;
    return NativeAssetKind::Unknown;
}

uint32_t nativeAssetMagicForKind(NativeAssetKind kind) {
    switch (kind) {
    case NativeAssetKind::Mesh: return kNativeAssetMagicRtmesh;
    case NativeAssetKind::Material: return kNativeAssetMagicRtmaterial;
    case NativeAssetKind::Texture: return kNativeAssetMagicRttexture;
    case NativeAssetKind::Skeleton: return kNativeAssetMagicRtskeleton;
    case NativeAssetKind::Animation: return kNativeAssetMagicRtanim;
    case NativeAssetKind::AnimationController: return kNativeAssetMagicRtanimController;
    case NativeAssetKind::SkeletalMesh: return kNativeAssetMagicRtskeletalMesh;
    case NativeAssetKind::Package: return kNativeAssetMagicRtpkg;
    case NativeAssetKind::Unknown: break;
    }
    return 0;
}

std::string nativeBinaryErrorCodeName(NativeBinaryErrorCode code) {
    switch (code) {
    case NativeBinaryErrorCode::None: return "none";
    case NativeBinaryErrorCode::UnsupportedVersion: return "unsupported_version";
    case NativeBinaryErrorCode::CorruptHeader: return "corrupt_header";
    case NativeBinaryErrorCode::CorruptTable: return "corrupt_table";
    case NativeBinaryErrorCode::MissingDependency: return "missing_dependency";
    case NativeBinaryErrorCode::HashMismatch: return "hash_mismatch";
    case NativeBinaryErrorCode::MigrationRequired: return "migration_required";
    case NativeBinaryErrorCode::MigrationFailed: return "migration_failed";
    case NativeBinaryErrorCode::UnsupportedPlatformFeature: return "unsupported_platform_feature";
    case NativeBinaryErrorCode::IoError: return "io_error";
    }
    return "unknown";
}

bool NativeAssetWriter::write(
    const std::filesystem::path& path,
    const NativeAssetWriteDesc& desc,
    NativeBinaryError* error,
    NativeAssetWriteResult* result) const {
    if (desc.kind == NativeAssetKind::Unknown || desc.kind == NativeAssetKind::Package) {
        if (error) {
            *error = makeError(NativeBinaryErrorCode::CorruptHeader, path, "header", 0, sizeof(NativeAssetHeader), "Unsupported standalone native asset kind");
        }
        return false;
    }
    std::vector<std::byte> metadataBytes;
    metadataBytes.resize(sizeof(NativeAssetHeader));

    std::map<std::string, uint32_t> stringOffsets;
    std::vector<std::byte> debugDirectory;
    const uint32_t debugNameOffset = appendString(debugDirectory, stringOffsets, desc.debugName);

    std::vector<NativeDependencyRecord> dependencyRecords;
    dependencyRecords.reserve(desc.dependencies.size());
    for (const auto& dependency : desc.dependencies) {
        NativeDependencyRecord record{};
        record.dependencyGuid = dependency.guid;
        record.assetKind = static_cast<uint32_t>(dependency.kind);
        record.flags = dependency.flags;
        record.debugNameOffset = appendString(debugDirectory, stringOffsets, dependency.debugName);
        record.debugNameSize = static_cast<uint32_t>(dependency.debugName.size());
        dependencyRecords.push_back(record);
    }

    std::vector<NativeDebugRecord> debugRecords;
    debugRecords.reserve(desc.debugRecords.size() + 2u);
    auto appendDebugRecord = [&](uint32_t type, const std::string& key, const std::string& value, uint32_t flags) {
        NativeDebugRecord record{};
        record.type = type;
        record.keyOffset = appendString(debugDirectory, stringOffsets, key);
        record.keySize = static_cast<uint32_t>(key.size());
        record.valueOffset = appendString(debugDirectory, stringOffsets, value);
        record.valueSize = static_cast<uint32_t>(value.size());
        record.flags = flags;
        debugRecords.push_back(record);
    };
    appendDebugRecord(1, "assetGuid", nativeGuidToText(desc.assetGuid), 0);
    appendDebugRecord(1, "debugName", desc.debugName, 0);
    for (const auto& input : desc.debugRecords) {
        appendDebugRecord(input.type, input.key, input.value, input.flags);
    }
    appendPadding(debugDirectory);

    NativeObjectRecord object{};
    object.objectGuid = desc.assetGuid;
    object.assetKind = static_cast<uint32_t>(desc.kind);
    object.contentVersion = desc.contentVersion;
    object.firstChunk = 0;
    object.chunkCount = static_cast<uint32_t>(desc.chunks.size());
    object.firstDependency = 0;
    object.dependencyCount = static_cast<uint32_t>(dependencyRecords.size());
    object.debugNameOffset = debugNameOffset;
    object.debugNameSize = static_cast<uint32_t>(desc.debugName.size());
    std::vector<NativeObjectRecord> objectRecords{object};

    appendPadding(metadataBytes);
    const uint64_t objectTableOffset = metadataBytes.size();
    for (const auto& record : objectRecords) appendPod(metadataBytes, record);
    appendPadding(metadataBytes);
    const uint64_t chunkTableOffset = metadataBytes.size();
    const uint64_t chunkTableSize = static_cast<uint64_t>(desc.chunks.size()) * sizeof(NativeChunkRecord);
    metadataBytes.resize(metadataBytes.size() + static_cast<size_t>(chunkTableSize));
    appendPadding(metadataBytes);
    const uint64_t dependencyTableOffset = metadataBytes.size();
    for (const auto& record : dependencyRecords) appendPod(metadataBytes, record);
    appendPadding(metadataBytes);
    const uint64_t debugRecordTableOffset = metadataBytes.size();
    for (const auto& record : debugRecords) appendPod(metadataBytes, record);
    for (const auto& record : desc.migrationRecords) appendPod(metadataBytes, record);
    appendPadding(metadataBytes);
    const uint64_t debugDirectoryOffset = metadataBytes.size();
    appendBytes(metadataBytes, debugDirectory);
    appendPadding(metadataBytes);

    std::vector<NativeChunkRecord> chunkRecords;
    chunkRecords.reserve(desc.chunks.size());
    std::array<uint64_t, 4> payloadHashParts{
        kFnvOffset ^ 0x9ae16a3b2f90404full,
        kFnvOffset ^ 0xc949d7c7509e6557ull,
        kFnvOffset ^ 0xff51afd7ed558ccdull,
        kFnvOffset ^ 0xc4ceb9fe1a85ec53ull,
    };
    uint64_t fileSize = metadataBytes.size();
    for (const auto& input : desc.chunks) {
        fileSize = nativeAlignUp(fileSize);
        NativeChunkRecord record{};
        record.type = input.type;
        record.compression = static_cast<uint32_t>(input.compression);
        record.offset = fileSize;
        record.size = input.payload.size();
        record.uncompressedSize = input.payload.size();
        record.payloadHash = nativeHashBytes(input.payload);
        record.flags = input.flags;
        fileSize += input.payload.size();
        for (uint64_t& part : payloadHashParts) {
            fnv1aUpdate(part, input.payload.data(), input.payload.size());
        }
        chunkRecords.push_back(record);
    }
    fileSize = nativeAlignUp(fileSize);

    NativeAssetHeader header{};
    header.magic = desc.magic != 0 ? desc.magic : nativeAssetMagicForKind(desc.kind);
    header.assetKind = static_cast<uint32_t>(desc.kind);
    header.contentVersion = desc.contentVersion;
    header.assetGuid = desc.assetGuid;
    header.sourceHash = desc.sourceHash;
    header.importSettingsHash = desc.importSettingsHash;
    header.payloadHash = nativeHashPartsToBytes(payloadHashParts);
    header.objectTableOffset = objectTableOffset;
    header.objectTableCount = static_cast<uint32_t>(objectRecords.size());
    header.objectTableStride = sizeof(NativeObjectRecord);
    header.chunkTableOffset = chunkTableOffset;
    header.chunkTableCount = static_cast<uint32_t>(chunkRecords.size());
    header.chunkTableStride = sizeof(NativeChunkRecord);
    header.dependencyTableOffset = dependencyTableOffset;
    header.dependencyTableCount = static_cast<uint32_t>(dependencyRecords.size());
    header.dependencyTableStride = sizeof(NativeDependencyRecord);
    header.debugDirectoryOffset = debugDirectoryOffset;
    header.debugDirectorySize = debugDirectory.size();
    header.migrationTableOffset = debugRecordTableOffset + static_cast<uint64_t>(debugRecords.size()) * sizeof(NativeDebugRecord);
    header.migrationTableCount = static_cast<uint32_t>(desc.migrationRecords.size());
    header.migrationTableStride = sizeof(NativeMigrationRecord);
    header.fileSize = fileSize;

    std::memcpy(metadataBytes.data(), &header, sizeof(header));
    if (!chunkRecords.empty()) {
        std::memcpy(metadataBytes.data() + chunkTableOffset, chunkRecords.data(), chunkRecords.size() * sizeof(NativeChunkRecord));
    }
    if (result != nullptr) {
        result->fileSize = header.fileSize;
        result->payloadHash = header.payloadHash;
    }

    const std::filesystem::path tempPath = path.string() + ".tmp";
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) {
                *error = makeError(NativeBinaryErrorCode::IoError, path, "file", 0, 0, "Could not create native asset output directory: " + ec.message());
            }
            return false;
        }
    }
    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            if (error) {
                *error = makeError(NativeBinaryErrorCode::IoError, tempPath, "file", 0, header.fileSize, "Could not open temporary native asset file for writing");
            }
            return false;
        }
        uint64_t cursor = 0;
        if (!writeVector(file, metadataBytes)) {
            if (error) {
                *error = makeError(NativeBinaryErrorCode::IoError, tempPath, "file", cursor, header.fileSize, "Could not write native asset metadata");
            }
            return false;
        }
        cursor += metadataBytes.size();
        for (const auto& input : desc.chunks) {
            if (!writePadding(file, cursor)) {
                if (error) {
                    *error = makeError(NativeBinaryErrorCode::IoError, tempPath, "file", cursor, header.fileSize, "Could not write native asset chunk padding");
                }
                return false;
            }
            if (!writeVector(file, input.payload)) {
                if (error) {
                    *error = makeError(NativeBinaryErrorCode::IoError, tempPath, "file", cursor, input.payload.size(), "Could not write native asset chunk payload");
                }
                return false;
            }
            cursor += input.payload.size();
        }
        if (!writePadding(file, cursor)) {
            if (error) {
                *error = makeError(NativeBinaryErrorCode::IoError, tempPath, "file", cursor, header.fileSize, "Could not write final native asset padding");
            }
            return false;
        }
        if (cursor != header.fileSize) {
            if (error) {
                *error = makeError(NativeBinaryErrorCode::IoError, tempPath, "file", cursor, header.fileSize, "Native asset streaming writer size mismatch");
            }
            return false;
        }
    }

    if (!replaceFileAtomically(tempPath, path, ec)) {
        if (error) {
            *error = makeError(NativeBinaryErrorCode::IoError, path, "file", 0, header.fileSize, "Could not atomically replace native asset destination: " + ec.message());
        }
        return false;
    }
    return true;
}

NativeAssetInspection NativeAssetReader::inspectBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, bool validatePayloadHash) const {
    NativeAssetInspection inspection{};
    if (bytes.size() < sizeof(NativeAssetHeader)) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptHeader, pathHint, "header", 0, sizeof(NativeAssetHeader), "Native asset file is smaller than the shared header"));
        return inspection;
    }
    std::memcpy(&inspection.header, bytes.data(), sizeof(NativeAssetHeader));
    const NativeAssetHeader& header = inspection.header;

    if (header.endianMarker != kNativeAssetEndianMarker) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptHeader, pathHint, "header", offsetof(NativeAssetHeader, endianMarker), sizeof(header.endianMarker), "Native asset endian marker is invalid"));
    }
    if (header.headerSize != sizeof(NativeAssetHeader)) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptHeader, pathHint, "header", offsetof(NativeAssetHeader, headerSize), sizeof(header.headerSize), "Native asset header size is not supported by this reader"));
    }
    const NativeAssetKind expectedKind = nativeAssetKindFromExtension(pathHint);
    if (expectedKind != NativeAssetKind::Unknown && header.magic != nativeAssetMagicForKind(expectedKind)) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptHeader, pathHint, "header", offsetof(NativeAssetHeader, magic), sizeof(header.magic), "Native asset magic does not match the file extension"));
    }
    if (header.headerVersion > kNativeAssetReadableVersionMax) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::UnsupportedVersion, pathHint, "header", offsetof(NativeAssetHeader, headerVersion), sizeof(header.headerVersion), "Native asset header version is newer than this reader supports"));
    }
    if (header.minimumReaderVersion > kNativeAssetReadableVersionMax) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::UnsupportedVersion, pathHint, "header", offsetof(NativeAssetHeader, minimumReaderVersion), sizeof(header.minimumReaderVersion), "Native asset requires a newer reader"));
    }
    if (header.contentVersion > kNativeAssetReadableVersionMax) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::UnsupportedVersion, pathHint, "header", offsetof(NativeAssetHeader, contentVersion), sizeof(header.contentVersion), "Native asset content version is newer than this reader supports"));
    }
    if (header.contentVersion < kNativeAssetReadableVersionMax) {
        inspection.migrationRequired = true;
        inspection.migrationAvailable = header.contentVersion == 0 && header.minimumReaderVersion <= kNativeAssetReadableVersionMax;
        inspection.warnings.push_back("Native asset content version is older than the current writer version; migration is required before mutation.");
    }
    if (header.fileSize != bytes.size()) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptHeader, pathHint, "header", offsetof(NativeAssetHeader, fileSize), bytes.size(), "Native asset file size field does not match the actual file size"));
    }

    readRecordRange(bytes, pathHint, "objectTable", header.objectTableOffset, header.objectTableCount, header.objectTableStride, inspection.objects, inspection.errors);
    readRecordRange(bytes, pathHint, "chunkTable", header.chunkTableOffset, header.chunkTableCount, header.chunkTableStride, inspection.chunks, inspection.errors);
    readRecordRange(bytes, pathHint, "dependencyTable", header.dependencyTableOffset, header.dependencyTableCount, header.dependencyTableStride, inspection.dependencies, inspection.errors);
    readRecordRange(bytes, pathHint, "migrationTable", header.migrationTableOffset, header.migrationTableCount, header.migrationTableStride, inspection.migrations, inspection.errors);

    if (!rangeInside(header.debugDirectoryOffset, header.debugDirectorySize, bytes.size())) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptTable, pathHint, "debugDirectory", header.debugDirectoryOffset, header.debugDirectorySize, "Debug directory is outside the file"));
    } else if (header.debugDirectorySize > 0) {
        inspection.debugDirectory.assign(
            bytes.begin() + static_cast<size_t>(header.debugDirectoryOffset),
            bytes.begin() + static_cast<size_t>(header.debugDirectoryOffset + header.debugDirectorySize));
    }
    for (size_t i = 0; i < inspection.objects.size(); ++i) {
        const NativeObjectRecord& object = inspection.objects[i];
        if (object.firstChunk > inspection.chunks.size() || object.chunkCount > inspection.chunks.size() - object.firstChunk) {
            inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptTable, pathHint, "objectTable", header.objectTableOffset + i * sizeof(NativeObjectRecord), sizeof(NativeObjectRecord), "Object chunk range is outside the chunk table"));
        }
        if (object.firstDependency > inspection.dependencies.size() || object.dependencyCount > inspection.dependencies.size() - object.firstDependency) {
            inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptTable, pathHint, "objectTable", header.objectTableOffset + i * sizeof(NativeObjectRecord), sizeof(NativeObjectRecord), "Object dependency range is outside the dependency table"));
        }
        if (object.debugNameSize > 0 && !rangeInside(object.debugNameOffset, object.debugNameSize, header.debugDirectorySize)) {
            inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptTable, pathHint, "objectDebugName", object.debugNameOffset, object.debugNameSize, "Object debug name is outside the debug directory"));
        }
    }
    const uint64_t debugRecordsEnd = header.migrationTableOffset;
    if (debugRecordsEnd > header.debugDirectoryOffset) {
        inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptTable, pathHint, "debugRecords", debugRecordsEnd, 0, "Debug record table overlaps the debug string directory"));
    } else if (debugRecordsEnd > 0) {
        const uint64_t start = nativeAlignUp(header.dependencyTableOffset + static_cast<uint64_t>(header.dependencyTableCount) * header.dependencyTableStride);
        const uint64_t debugRecordBytes = debugRecordsEnd > start ? debugRecordsEnd - start : 0;
        if (debugRecordBytes % sizeof(NativeDebugRecord) == 0 && rangeInside(start, debugRecordBytes, bytes.size())) {
            const uint64_t count = debugRecordBytes / sizeof(NativeDebugRecord);
            inspection.debugRecords.resize(static_cast<size_t>(count));
            std::memcpy(inspection.debugRecords.data(), bytes.data() + start, static_cast<size_t>(debugRecordBytes));
        }
    }

    std::array<uint64_t, 4> payloadHashParts{
        kFnvOffset ^ 0x9ae16a3b2f90404full,
        kFnvOffset ^ 0xc949d7c7509e6557ull,
        kFnvOffset ^ 0xff51afd7ed558ccdull,
        kFnvOffset ^ 0xc4ceb9fe1a85ec53ull,
    };
    for (const NativeChunkRecord& chunk : inspection.chunks) {
        if (!rangeInside(chunk.offset, chunk.size, bytes.size())) {
            inspection.errors.push_back(makeError(NativeBinaryErrorCode::CorruptTable, pathHint, "chunkPayload", chunk.offset, chunk.size, "Chunk payload is outside the file"));
            continue;
        }
        if (validatePayloadHash) {
            const auto hash = nativeHashBytes(bytes.data() + chunk.offset, static_cast<size_t>(chunk.size));
            if (hash != chunk.payloadHash) {
                inspection.errors.push_back(makeError(NativeBinaryErrorCode::HashMismatch, pathHint, "chunkPayload", chunk.offset, chunk.size, "Chunk payload hash does not match the chunk table"));
            }
        }
        if (validatePayloadHash) {
            for (uint64_t& part : payloadHashParts) {
                fnv1aUpdate(part, bytes.data() + chunk.offset, static_cast<size_t>(chunk.size));
            }
        }
    }
    if (validatePayloadHash) {
        const auto payloadHash = nativeHashPartsToBytes(payloadHashParts);
        inspection.payloadHashValid = payloadHash == header.payloadHash;
        if (!inspection.payloadHashValid) {
            inspection.errors.push_back(makeError(NativeBinaryErrorCode::HashMismatch, pathHint, "payloadHash", offsetof(NativeAssetHeader, payloadHash), header.payloadHash.size(), "Header payload hash does not match concatenated chunk payloads"));
        }
    }
    inspection.ok = inspection.errors.empty();
    return inspection;
}

NativeAssetInspection NativeAssetReader::inspect(const std::filesystem::path& path, bool validatePayloadHash) const {
    std::vector<std::byte> bytes;
    NativeBinaryError ioError;
    if (!readFileBytes(path, bytes, &ioError)) {
        NativeAssetInspection inspection{};
        inspection.errors.push_back(ioError);
        return inspection;
    }
    return inspectBytes(path, bytes, validatePayloadHash);
}

nlohmann::json nativeAssetInspectionToJson(const NativeAssetInspection& inspection, const std::filesystem::path& path) {
    const NativeAssetHeader& h = inspection.header;
    nlohmann::json errors = nlohmann::json::array();
    for (const NativeBinaryError& error : inspection.errors) {
        errors.push_back({
            {"code", nativeBinaryErrorCodeName(error.code)},
            {"path", error.path.empty() ? path.generic_string() : error.path.generic_string()},
            {"table", error.table},
            {"offset", error.offset},
            {"expected_size", error.expectedSize},
            {"message", error.message},
        });
    }

    nlohmann::json chunks = nlohmann::json::array();
    for (size_t i = 0; i < inspection.chunks.size(); ++i) {
        const NativeChunkRecord& c = inspection.chunks[i];
        chunks.push_back({
            {"index", i},
            {"type", c.type},
            {"compression", c.compression},
            {"compression_name", nativeChunkCompressionNameLocal(c.compression)},
            {"offset", c.offset},
            {"size", c.size},
            {"uncompressed_size", c.uncompressedSize},
            {"payload_hash", hashJson(c.payloadHash)},
            {"flags", c.flags},
        });
    }

    nlohmann::json objects = nlohmann::json::array();
    for (size_t i = 0; i < inspection.objects.size(); ++i) {
        const NativeObjectRecord& o = inspection.objects[i];
        objects.push_back({
            {"index", i},
            {"guid", nativeGuidToText(o.objectGuid)},
            {"kind", nativeAssetKindName(static_cast<NativeAssetKind>(o.assetKind))},
            {"content_version", o.contentVersion},
            {"first_chunk", o.firstChunk},
            {"chunk_count", o.chunkCount},
            {"first_dependency", o.firstDependency},
            {"dependency_count", o.dependencyCount},
            {"debug_name_offset", o.debugNameOffset},
            {"debug_name_size", o.debugNameSize},
            {"flags", o.flags},
        });
    }

    nlohmann::json dependencies = nlohmann::json::array();
    for (size_t i = 0; i < inspection.dependencies.size(); ++i) {
        const NativeDependencyRecord& d = inspection.dependencies[i];
        dependencies.push_back({
            {"index", i},
            {"guid", nativeGuidToText(d.dependencyGuid)},
            {"kind", nativeAssetKindName(static_cast<NativeAssetKind>(d.assetKind))},
            {"flags", d.flags},
            {"debug_name_offset", d.debugNameOffset},
            {"debug_name_size", d.debugNameSize},
        });
    }

    nlohmann::json debugRecords = nlohmann::json::array();
    for (size_t i = 0; i < inspection.debugRecords.size(); ++i) {
        const NativeDebugRecord& d = inspection.debugRecords[i];
        debugRecords.push_back({
            {"index", i},
            {"type", d.type},
            {"key", debugDirectoryString(inspection.debugDirectory, d.keyOffset, d.keySize)},
            {"key_offset", d.keyOffset},
            {"key_size", d.keySize},
            {"value", debugDirectoryString(inspection.debugDirectory, d.valueOffset, d.valueSize)},
            {"value_offset", d.valueOffset},
            {"value_size", d.valueSize},
            {"flags", d.flags},
        });
    }

    return {
        {"path", path.generic_string()},
        {"ok", inspection.ok},
        {"migration_required", inspection.migrationRequired},
        {"migration_available", inspection.migrationAvailable},
        {"payload_hash_valid", inspection.payloadHashValid},
        {"header", {
            {"magic", h.magic},
            {"header_version", h.headerVersion},
            {"asset_kind", nativeAssetKindName(static_cast<NativeAssetKind>(h.assetKind))},
            {"endian_marker", h.endianMarker},
            {"header_size", h.headerSize},
            {"content_version", h.contentVersion},
            {"minimum_reader_version", h.minimumReaderVersion},
            {"asset_guid", nativeGuidToText(h.assetGuid)},
            {"source_hash", hashJson(h.sourceHash)},
            {"import_settings_hash", hashJson(h.importSettingsHash)},
            {"payload_hash", hashJson(h.payloadHash)},
            {"object_table_offset", h.objectTableOffset},
            {"object_table_count", h.objectTableCount},
            {"chunk_table_offset", h.chunkTableOffset},
            {"chunk_table_count", h.chunkTableCount},
            {"dependency_table_offset", h.dependencyTableOffset},
            {"dependency_table_count", h.dependencyTableCount},
            {"debug_directory_offset", h.debugDirectoryOffset},
            {"debug_directory_size", h.debugDirectorySize},
            {"migration_table_offset", h.migrationTableOffset},
            {"migration_table_count", h.migrationTableCount},
            {"file_size", h.fileSize},
        }},
        {"objects", objects},
        {"chunks", chunks},
        {"dependencies", dependencies},
        {"debug_records", debugRecords},
        {"warnings", inspection.warnings},
        {"errors", errors},
    };
}

void buildCompactAnimationControllerFixture(NativeAssetWriteDesc& desc) {
    std::vector<std::byte> stringTable;
    std::map<std::string, uint32_t> stringOffsets;
    auto stringRef = [&](const std::string& value) {
        return std::pair<uint32_t, uint32_t>{appendString(stringTable, stringOffsets, value), static_cast<uint32_t>(value.size())};
    };

    std::vector<RtanimControllerParameterRecord> parameters;
    {
        auto [nameOffset, nameSize] = stringRef("moving");
        RtanimControllerParameterRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.type = static_cast<uint32_t>(0u);
        record.boolValue = 1u;
        parameters.push_back(record);
    }
    {
        auto [nameOffset, nameSize] = stringRef("speed");
        RtanimControllerParameterRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.type = static_cast<uint32_t>(2u);
        record.floatValue = 0.75f;
        parameters.push_back(record);
    }

    std::vector<RtanimControllerConditionRecord> conditions;
    {
        auto [parameterOffset, parameterSize] = stringRef("moving");
        auto [opOffset, opSize] = stringRef("equals");
        RtanimControllerConditionRecord record;
        record.parameterOffset = parameterOffset;
        record.parameterSize = parameterSize;
        record.opOffset = opOffset;
        record.opSize = opSize;
        record.type = static_cast<uint32_t>(0u);
        record.boolValue = 1u;
        conditions.push_back(record);
    }

    std::vector<RtanimControllerEventRecord> events;
    {
        auto [nameOffset, nameSize] = stringRef("controller.walk.enter");
        auto [payloadOffset, payloadSize] = stringRef("{\"state\":\"Walk\"}");
        RtanimControllerEventRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.payloadOffset = payloadOffset;
        record.payloadSize = payloadSize;
        events.push_back(record);
    }

    std::vector<RtanimControllerTransitionRecord> transitions;
    {
        auto [toOffset, toSize] = stringRef("Walk");
        RtanimControllerTransitionRecord record;
        record.toOffset = toOffset;
        record.toSize = toSize;
        record.firstCondition = 0u;
        record.conditionCount = 1u;
        transitions.push_back(record);
    }

    std::vector<RtanimControllerBlendTreeChildRecord> blendTreeChildren;
    {
        auto [nameOffset, nameSize] = stringRef("WalkSlow");
        auto [pathOffset, pathSize] = stringRef("walk_slow.rtanim.json");
        RtanimControllerBlendTreeChildRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.clipGuid = nativeGuidFromText("00000000-0000-0000-0000-000000000103");
        record.clipPathOffset = pathOffset;
        record.clipPathSize = pathSize;
        record.threshold = 0.0f;
        blendTreeChildren.push_back(record);
    }
    {
        auto [nameOffset, nameSize] = stringRef("WalkFast");
        auto [pathOffset, pathSize] = stringRef("walk_fast.rtanim.json");
        RtanimControllerBlendTreeChildRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.clipGuid = nativeGuidFromText("00000000-0000-0000-0000-000000000104");
        record.clipPathOffset = pathOffset;
        record.clipPathSize = pathSize;
        record.threshold = 1.0f;
        blendTreeChildren.push_back(record);
    }

    std::vector<RtanimControllerBlendTreeRecord> blendTrees;
    {
        auto [typeOffset, typeSize] = stringRef("1d");
        auto [parameterOffset, parameterSize] = stringRef("speed");
        RtanimControllerBlendTreeRecord record;
        record.stateIndex = 1u;
        record.typeOffset = typeOffset;
        record.typeSize = typeSize;
        record.parameterOffset = parameterOffset;
        record.parameterSize = parameterSize;
        record.firstChild = 0u;
        record.childCount = static_cast<uint32_t>(blendTreeChildren.size());
        blendTrees.push_back(record);
    }

    std::vector<RtanimControllerStateRecord> states;
    {
        auto [nameOffset, nameSize] = stringRef("Idle");
        auto [pathOffset, pathSize] = stringRef("idle.rtanim.json");
        RtanimControllerStateRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.clipGuid = nativeGuidFromText("00000000-0000-0000-0000-000000000101");
        record.clipPathOffset = pathOffset;
        record.clipPathSize = pathSize;
        record.loop = 1u;
        record.defaultState = 1u;
        record.firstTransition = 0u;
        record.transitionCount = 1u;
        states.push_back(record);
    }
    {
        auto [nameOffset, nameSize] = stringRef("Walk");
        auto [pathOffset, pathSize] = stringRef("walk.rtanim.json");
        RtanimControllerStateRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.clipGuid = nativeGuidFromText("00000000-0000-0000-0000-000000000102");
        record.clipPathOffset = pathOffset;
        record.clipPathSize = pathSize;
        record.loop = 1u;
        record.firstEvent = 0u;
        record.eventCount = 1u;
        record.blendTreeIndex = 0u;
        states.push_back(record);
    }

    std::vector<RtanimControllerLayerRecord> layers;
    {
        auto [nameOffset, nameSize] = stringRef("Base");
        RtanimControllerLayerRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.weight = 1.0f;
        layers.push_back(record);
    }
    {
        auto [nameOffset, nameSize] = stringRef("UpperBodyAdditive");
        auto [pathOffset, pathSize] = stringRef("upper_body_additive.rtanim.json");
        auto [maskOffset, maskSize] = stringRef("UpperBody");
        RtanimControllerLayerRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.clipGuid = nativeGuidFromText("00000000-0000-0000-0000-000000000105");
        record.clipPathOffset = pathOffset;
        record.clipPathSize = pathSize;
        record.weight = 0.5f;
        record.additive = 1u;
        record.maskOffset = maskOffset;
        record.maskSize = maskSize;
        layers.push_back(record);
    }

    std::vector<RtanimControllerAvatarMaskJointRecord> joints;
    for (const std::string& jointName : {"Spine", "Chest", "Head", "LeftLeg", "RightLeg"}) {
        auto [nameOffset, nameSize] = stringRef(jointName);
        RtanimControllerAvatarMaskJointRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        joints.push_back(record);
    }
    std::vector<RtanimControllerAvatarMaskRecord> masks;
    {
        auto [nameOffset, nameSize] = stringRef("UpperBody");
        RtanimControllerAvatarMaskRecord record;
        record.nameOffset = nameOffset;
        record.nameSize = nameSize;
        record.firstIncludedJoint = 0u;
        record.includedJointCount = 3u;
        record.firstExcludedJoint = 3u;
        record.excludedJointCount = 2u;
        masks.push_back(record);
    }

    RtanimControllerPayloadHeader header;
    header.parameterCount = static_cast<uint32_t>(parameters.size());
    header.stateCount = static_cast<uint32_t>(states.size());
    header.transitionCount = static_cast<uint32_t>(transitions.size());
    header.layerCount = static_cast<uint32_t>(layers.size());
    header.parameterChunk = 2u;
    header.stateChunk = 3u;
    header.transitionChunk = 4u;
    header.blendTreeChunk = 7u;
    header.eventRouteChunk = 6u;

    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkPayloadHeader, .payload = bytesOfPod(header)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkStringTable, .payload = std::move(stringTable)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkParameters, .payload = bytesOfPodVector(parameters)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkStates, .payload = bytesOfPodVector(states)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkTransitions, .payload = bytesOfPodVector(transitions)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkConditions, .payload = bytesOfPodVector(conditions)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkEvents, .payload = bytesOfPodVector(events)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkBlendTrees, .payload = bytesOfPodVector(blendTrees)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkBlendTreeChildren, .payload = bytesOfPodVector(blendTreeChildren)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkLayers, .payload = bytesOfPodVector(layers)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkAvatarMasks, .payload = bytesOfPodVector(masks)});
    desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtanimControllerChunkAvatarMaskJoints, .payload = bytesOfPodVector(joints)});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "payloadClass", .value = "compact_rtanimcontroller"});
}

int inspectNativeAssetCommand(const std::filesystem::path& path, const std::filesystem::path& jsonOut) {
    NativeAssetReader reader;
    const NativeAssetInspection inspection = reader.inspect(path, true);
    const nlohmann::json report = nativeAssetInspectionToJson(inspection, path);
    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
        std::ofstream out(jsonOut);
        if (!out.is_open()) {
            std::cerr << "Failed to write native asset inspection JSON: " << jsonOut << '\n';
            return 1;
        }
        out << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return inspection.ok ? 0 : 1;
}

int emitNativeAssetFixtureCommand(const std::filesystem::path& path, NativeAssetKind kind, std::string_view guidText, uint32_t fixtureTextureVkFormat, std::string_view fixtureMaterialTextureGuid, std::string_view fixtureTextureRole) {
    NativeAssetWriteDesc desc;
    desc.kind = kind == NativeAssetKind::Unknown ? nativeAssetKindFromExtension(path) : kind;
    desc.magic = nativeAssetMagicForKind(desc.kind);
    desc.assetGuid = nativeGuidFromText(guidText.empty() ? path.generic_string() : guidText);
    desc.sourceHash = nativeHashText("native-fixture-source");
    desc.importSettingsHash = nativeHashText("native-fixture-settings-v1");
    desc.debugName = path.stem().string();
    if (desc.kind == NativeAssetKind::Mesh) {
        desc.dependencies.push_back(NativeBinaryDependencyInput{
            .guid = nativeGuidFromText("native-fixture-missing-material-dependency"),
            .kind = NativeAssetKind::Material,
            .flags = static_cast<uint32_t>(NativeDependencyFlags::Required) | static_cast<uint32_t>(NativeDependencyFlags::Runtime),
            .debugName = "fixture missing material dependency",
        });
    }
    if (desc.kind == NativeAssetKind::Material) {
        RtmaterialPayloadHeader header;
        std::vector<FixtureRtmaterialTextureSlotRecord> slots;
        if (!fixtureMaterialTextureGuid.empty()) {
            FixtureRtmaterialTextureSlotRecord slot;
            slot.slot = static_cast<uint32_t>(RtmaterialTextureSlot::BaseColor);
            slot.textureGuid = nativeGuidFromText(fixtureMaterialTextureGuid);
            slot.textureIndex = 0u;
            slots.push_back(slot);
            desc.dependencies.push_back(NativeBinaryDependencyInput{
                .guid = slot.textureGuid,
                .kind = NativeAssetKind::Texture,
                .flags = static_cast<uint32_t>(NativeDependencyFlags::Required) | static_cast<uint32_t>(NativeDependencyFlags::Runtime),
                .debugName = "fixture baseColor texture dependency",
            });
        }
        header.textureSlotCount = static_cast<uint32_t>(slots.size());
        header.textureSlotChunk = slots.empty() ? 0u : 20u;
        desc.chunks.push_back(NativeBinaryChunkInput{.type = 1u, .payload = bytesOfPod(header)});
        if (!slots.empty()) {
            desc.chunks.push_back(NativeBinaryChunkInput{.type = 20u, .payload = bytesOfPodVector(slots)});
        }
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "fixture", .value = "true"});
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "payloadClass", .value = "fixture_rtmaterial"});
        NativeBinaryError error;
        NativeAssetWriter writer;
        if (!writer.write(path, desc, &error)) {
            std::cerr << "Failed to write native fixture: " << error.message << '\n';
            return 1;
        }
        return 0;
    }
    if (desc.kind == NativeAssetKind::Texture) {
        const uint32_t vkFormat = fixtureTextureVkFormat == 0u ? static_cast<uint32_t>(VK_FORMAT_R8G8B8A8_SRGB) : fixtureTextureVkFormat;
        std::vector<std::byte> payload = fixtureTexturePayload(vkFormat);
        const NativeTextureRole textureRole = fixtureTextureRoleFromString(fixtureTextureRole);
        RttexturePayloadHeader header;
        header.width = 4;
        header.height = 4;
        header.mipCount = 1;
        header.vkFormat = vkFormat;
        header.colorSpace = fixtureHdrDecodedFormat(vkFormat)
            ? static_cast<uint32_t>(NativeTextureColorSpace::HdrLinear)
            : vkFormat == static_cast<uint32_t>(VK_FORMAT_R8G8B8A8_SRGB) || vkFormat == static_cast<uint32_t>(VK_FORMAT_BC7_SRGB_BLOCK)
            ? static_cast<uint32_t>(NativeTextureColorSpace::Srgb)
            : static_cast<uint32_t>(NativeTextureColorSpace::Linear);
        header.role = static_cast<uint32_t>(textureRole);
        header.flags = fixtureHdrDecodedFormat(vkFormat) ? 1u : 0u;
        header.compression = fixtureBlockCompressedFormat(vkFormat)
            ? static_cast<uint32_t>(NativeTextureCompressionPolicy::PreserveSourceContainer)
            : fixtureHdrDecodedFormat(vkFormat)
                ? static_cast<uint32_t>(NativeTextureCompressionPolicy::DecodedHdr)
            : static_cast<uint32_t>(NativeTextureCompressionPolicy::DecodedRgba8);
        header.mipTableChunk = 30u;
        header.payloadChunk = 31u;
        const FixtureRttextureMipRecord mip{.offset = 0, .size = static_cast<uint64_t>(payload.size()), .width = 4, .height = 4};
        desc.chunks.push_back(NativeBinaryChunkInput{.type = 1u, .payload = bytesOfPod(header)});
        desc.chunks.push_back(NativeBinaryChunkInput{.type = 30u, .payload = bytesOfPodVector(std::vector<FixtureRttextureMipRecord>{mip})});
        desc.chunks.push_back(NativeBinaryChunkInput{.type = 31u, .payload = std::move(payload)});
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "fixture", .value = "true"});
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "emittedVkFormat", .value = fixtureVkFormatName(vkFormat)});
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "textureRole", .value = fixtureTextureRoleName(textureRole)});
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "nativeTexturePayloadVariant", .value = fixtureBlockCompressedFormat(vkFormat) ? "fixture-compressed-payload" : fixtureHdrDecodedFormat(vkFormat) ? "fixture-hdr-payload" : "fixture-rgba-payload"});
        NativeBinaryError error;
        NativeAssetWriter writer;
        if (!writer.write(path, desc, &error)) {
            std::cerr << "Failed to write native fixture: " << error.message << '\n';
            return 1;
        }
        return 0;
    }
    if (desc.kind == NativeAssetKind::SkeletalMesh) {
        const std::array<uint8_t, 16> meshGuid = nativeGuidFromText("00000000-0000-0000-0000-000000000201");
        const std::array<uint8_t, 16> skeletonGuid = nativeGuidFromText("00000000-0000-0000-0000-000000000202");
        desc.dependencies.push_back(NativeBinaryDependencyInput{
            .guid = meshGuid,
            .kind = NativeAssetKind::Mesh,
            .flags = static_cast<uint32_t>(NativeDependencyFlags::Optional) | static_cast<uint32_t>(NativeDependencyFlags::Runtime),
            .debugName = "fixture mesh dependency",
        });
        desc.dependencies.push_back(NativeBinaryDependencyInput{
            .guid = skeletonGuid,
            .kind = NativeAssetKind::Skeleton,
            .flags = static_cast<uint32_t>(NativeDependencyFlags::Optional) | static_cast<uint32_t>(NativeDependencyFlags::Runtime),
            .debugName = "fixture skeleton dependency",
        });

        std::vector<uint32_t> jointRemap = {0u, 1u};
        RtskeletalMeshPayloadHeader header;
        header.meshGuid = meshGuid;
        header.skeletonGuid = skeletonGuid;
        header.jointRemapCount = static_cast<uint32_t>(jointRemap.size());
        header.jointRemapChunk = kRtskeletalMeshChunkJointRemap;
        header.skinningDataChunk = kRtskeletalMeshChunkSkinningData;
        header.bindMetadataChunk = kRtskeletalMeshChunkBindMetadataJson;
        header.flags = 1u;

        std::vector<std::byte> skinningData = {
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
        };
        const nlohmann::json bindMetadata = {
            {"schema", "NativeFixtureSkeletalMeshBindingV1"},
            {"meshGuid", nativeGuidToText(meshGuid)},
            {"skeletonGuid", nativeGuidToText(skeletonGuid)},
            {"jointRemapCount", jointRemap.size()},
        };
        const std::string bindMetadataText = bindMetadata.dump(2);
        std::vector<std::byte> bindMetadataBytes;
        bindMetadataBytes.reserve(bindMetadataText.size());
        for (char c : bindMetadataText) {
            bindMetadataBytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }

        desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtskeletalMeshChunkPayloadHeader, .payload = bytesOfPod(header)});
        desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtskeletalMeshChunkJointRemap, .payload = bytesOfPodVector(jointRemap)});
        desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtskeletalMeshChunkSkinningData, .payload = std::move(skinningData)});
        desc.chunks.push_back(NativeBinaryChunkInput{.type = kRtskeletalMeshChunkBindMetadataJson, .payload = std::move(bindMetadataBytes)});
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "payloadClass", .value = "skeletal_mesh_binding"});

        NativeBinaryError error;
        NativeAssetWriter writer;
        if (!writer.write(path, desc, &error)) {
            std::cerr << "Failed to write native fixture: " << error.message << '\n';
            return 1;
        }
        return 0;
    }
    if (desc.kind == NativeAssetKind::AnimationController) {
        buildCompactAnimationControllerFixture(desc);
        NativeBinaryError error;
        NativeAssetWriter writer;
        if (!writer.write(path, desc, &error)) {
            std::cerr << "Failed to write native fixture: " << error.message << '\n';
            return 1;
        }
        return 0;
    }
    if (desc.kind == NativeAssetKind::Skeleton) {
        const nlohmann::json metadata = {
            {"schema", "NativeFixtureRuntimeSkeletonV1"},
            {"skeleton", {
                {"name", "Fixture Skeleton"},
                {"skeletonRoot", 0},
                {"joints", nlohmann::json::array({
                    nlohmann::json{{"index", 0}, {"name", "Root"}, {"parentIndex", -1}, {"parentName", ""}},
                    nlohmann::json{{"index", 1}, {"name", "Spine"}, {"parentIndex", 0}, {"parentName", "Root"}},
                })},
            }},
        };
        const std::string metadataText = metadata.dump(2);
        std::vector<std::byte> payload;
        payload.reserve(metadataText.size());
        for (char c : metadataText) {
            payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
        desc.chunks.push_back(NativeBinaryChunkInput{.type = 100u, .payload = std::move(payload)});
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "payloadClass", .value = "metadata_bridge"});
        NativeBinaryError error;
        NativeAssetWriter writer;
        if (!writer.write(path, desc, &error)) {
            std::cerr << "Failed to write native fixture: " << error.message << '\n';
            return 1;
        }
        return 0;
    }
    if (desc.kind == NativeAssetKind::Animation) {
        const nlohmann::json metadata = {
            {"schema", "NativeFixtureRuntimeAnimationV1"},
            {"animation", {
                {"name", "Fixture Animation"},
                {"clip", {{"startTime", 0.0}, {"endTime", 1.0}, {"duration", 1.0}}},
                {"events", nlohmann::json::array({nlohmann::json{{"timeSeconds", 0.5}, {"name", "fixture.step"}, {"payload", nlohmann::json{{"foot", "left"}}}}})},
                {"channels", nlohmann::json::array({
                    nlohmann::json{
                        {"target", {{"node", 1}, {"nodeName", "Joint0"}, {"path", "rotation"}}},
                        {"decodedTrack", {
                            {"decoded", true},
                            {"targetPath", "rotation"},
                            {"interpolation", "LINEAR"},
                            {"times", nlohmann::json::array({0.0, 1.0})},
                            {"values", nlohmann::json::array({nlohmann::json::array({0.0, 0.0, 0.0, 1.0}), nlohmann::json::array({0.0, 0.7071068, 0.0, 0.7071068})})},
                        }},
                    },
                })},
            }},
        };
        const std::string metadataText = metadata.dump(2);
        std::vector<std::byte> payload;
        payload.reserve(metadataText.size());
        for (char c : metadataText) {
            payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
        desc.chunks.push_back(NativeBinaryChunkInput{.type = 100u, .payload = std::move(payload)});
        desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "payloadClass", .value = "metadata_bridge"});
        NativeBinaryError error;
        NativeAssetWriter writer;
        if (!writer.write(path, desc, &error)) {
            std::cerr << "Failed to write native fixture: " << error.message << '\n';
            return 1;
        }
        return 0;
    }
    std::string payloadText = std::string("fixture payload for ") + nativeAssetKindName(desc.kind);
    std::vector<std::byte> payload;
    payload.reserve(payloadText.size());
    for (char c : payloadText) {
        payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    desc.chunks.push_back(NativeBinaryChunkInput{.type = 1, .payload = std::move(payload)});
    desc.debugRecords.push_back(NativeBinaryDebugInput{.type = 1, .key = "fixture", .value = "true"});

    NativeBinaryError error;
    NativeAssetWriter writer;
    if (!writer.write(path, desc, &error)) {
        std::cerr << "Failed to write native fixture: " << error.message << '\n';
        return 1;
    }
    return 0;
}

} // namespace rtv
