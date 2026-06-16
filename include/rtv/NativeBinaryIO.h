#pragma once

#include "rtv/NativeAssetFormat.h"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

struct NativeBinaryError {
    NativeBinaryErrorCode code = NativeBinaryErrorCode::None;
    std::filesystem::path path;
    std::string table;
    uint64_t offset = 0;
    uint64_t expectedSize = 0;
    std::string message;
};

struct NativeBinaryChunkInput {
    uint32_t type = 0;
    std::vector<std::byte> payload;
    NativeChunkCompression compression = NativeChunkCompression::None;
    uint32_t flags = 0;
};

struct NativeBinaryDependencyInput {
    std::array<uint8_t, 16> guid{};
    NativeAssetKind kind = NativeAssetKind::Unknown;
    uint32_t flags = static_cast<uint32_t>(NativeDependencyFlags::Required);
    std::string debugName;
};

struct NativeBinaryDebugInput {
    uint32_t type = 0;
    std::string key;
    std::string value;
    uint32_t flags = 0;
};

struct NativeAssetWriteDesc {
    NativeAssetKind kind = NativeAssetKind::Unknown;
    uint32_t magic = 0;
    uint32_t contentVersion = 1;
    std::array<uint8_t, 16> assetGuid{};
    std::array<uint8_t, 32> sourceHash{};
    std::array<uint8_t, 32> importSettingsHash{};
    std::string debugName;
    std::vector<NativeBinaryChunkInput> chunks;
    std::vector<NativeBinaryDependencyInput> dependencies;
    std::vector<NativeBinaryDebugInput> debugRecords;
    std::vector<NativeMigrationRecord> migrationRecords;
};

struct NativeAssetWriteResult {
    uint64_t fileSize = 0;
    std::array<uint8_t, 32> payloadHash{};
};

struct NativeAssetInspection {
    bool ok = false;
    bool migrationRequired = false;
    bool migrationAvailable = false;
    bool payloadHashValid = false;
    NativeAssetHeader header{};
    std::vector<NativeObjectRecord> objects;
    std::vector<NativeChunkRecord> chunks;
    std::vector<NativeDependencyRecord> dependencies;
    std::vector<NativeDebugRecord> debugRecords;
    std::vector<std::byte> debugDirectory;
    std::vector<NativeMigrationRecord> migrations;
    std::vector<std::string> warnings;
    std::vector<NativeBinaryError> errors;
};

[[nodiscard]] uint64_t nativeAlignUp(uint64_t value, uint64_t alignment = kNativeAssetAlignment);
[[nodiscard]] std::array<uint8_t, 32> nativeHashBytes(const std::byte* data, size_t size);
[[nodiscard]] std::array<uint8_t, 32> nativeHashBytes(const std::vector<std::byte>& data);
[[nodiscard]] std::array<uint8_t, 32> nativeHashText(std::string_view text);
[[nodiscard]] std::string nativeHex(const uint8_t* data, size_t size);
[[nodiscard]] std::string nativeHashHex(const std::array<uint8_t, 32>& hash);
[[nodiscard]] std::array<uint8_t, 16> nativeGuidFromText(std::string_view text);
[[nodiscard]] std::string nativeGuidToText(const std::array<uint8_t, 16>& guid);
[[nodiscard]] const char* nativeAssetKindName(NativeAssetKind kind);
[[nodiscard]] NativeAssetKind nativeAssetKindFromExtension(const std::filesystem::path& path);
[[nodiscard]] uint32_t nativeAssetMagicForKind(NativeAssetKind kind);
[[nodiscard]] std::string nativeBinaryErrorCodeName(NativeBinaryErrorCode code);

class NativeAssetWriter {
public:
    [[nodiscard]] bool write(
        const std::filesystem::path& path,
        const NativeAssetWriteDesc& desc,
        NativeBinaryError* error = nullptr,
        NativeAssetWriteResult* result = nullptr) const;
};

class NativeAssetReader {
public:
    [[nodiscard]] NativeAssetInspection inspect(const std::filesystem::path& path, bool validatePayloadHash = true) const;
    [[nodiscard]] NativeAssetInspection inspectBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, bool validatePayloadHash = true) const;
    // Reads only the header and metadata tables (object/chunk/dependency/migration tables plus the
    // debug directory) without loading any payload chunk bytes. This is the bounded, streaming-friendly
    // read used by the native asset catalog to answer "which chunks does this asset need" cheaply.
    // Chunk payload offset/size ranges are validated against the header file-size field rather than the
    // in-memory buffer, and payload hashes are not verified.
    [[nodiscard]] NativeAssetInspection inspectTablesOnly(const std::filesystem::path& path) const;
};

[[nodiscard]] nlohmann::json nativeAssetInspectionToJson(const NativeAssetInspection& inspection, const std::filesystem::path& path);
[[nodiscard]] int inspectNativeAssetCommand(const std::filesystem::path& path, const std::filesystem::path& jsonOut = {});
[[nodiscard]] int emitNativeAssetFixtureCommand(
    const std::filesystem::path& path,
    NativeAssetKind kind,
    std::string_view guidText = {},
    uint32_t fixtureTextureVkFormat = 0,
    std::string_view fixtureMaterialTextureGuid = {},
    std::string_view fixtureTextureRole = {});

} // namespace rtv
