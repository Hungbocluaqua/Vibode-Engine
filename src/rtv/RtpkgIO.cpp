#include "rtv/RtpkgIO.h"

#include "rtv/StreamingIoBackend.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace rtv {
namespace {

constexpr uint32_t kRtpkgEmbeddedNativeAssetChunk = 0x100u;

template <typename T>
void appendPod(std::vector<std::byte>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
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

NativeBinaryError makePackageError(
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
        if (error) {
            *error = makePackageError(NativeBinaryErrorCode::IoError, path, "file", 0, 0, "Could not open file for reading");
        }
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        if (error) {
            *error = makePackageError(NativeBinaryErrorCode::IoError, path, "file", 0, 0, "Could not determine file size");
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
            *error = makePackageError(NativeBinaryErrorCode::IoError, path, "file", 0, static_cast<uint64_t>(size), "Could not read complete file");
        }
        return false;
    }
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

void appendDebugRecord(
    std::vector<NativeDebugRecord>& debugRecords,
    std::vector<std::byte>& debugDirectory,
    std::map<std::string, uint32_t>& stringOffsets,
    uint32_t type,
    const std::string& key,
    const std::string& value,
    uint32_t flags = 0) {
    NativeDebugRecord record{};
    record.type = type;
    record.keyOffset = appendString(debugDirectory, stringOffsets, key);
    record.keySize = static_cast<uint32_t>(key.size());
    record.valueOffset = appendString(debugDirectory, stringOffsets, value);
    record.valueSize = static_cast<uint32_t>(value.size());
    record.flags = flags;
    debugRecords.push_back(record);
}

bool rangeInside(uint64_t offset, uint64_t size, uint64_t fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
}

std::string lowerAscii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

const char* nativeChunkCompressionName(uint32_t compression) {
    switch (static_cast<NativeChunkCompression>(compression)) {
    case NativeChunkCompression::None: return "none";
    case NativeChunkCompression::ReservedZstd: return "zstd";
    case NativeChunkCompression::ReservedGDeflate: return "gdeflate";
    }
    return "unknown";
}

bool nativeChunkCompressionKnown(uint32_t compression) {
    return compression == static_cast<uint32_t>(NativeChunkCompression::None) ||
        compression == static_cast<uint32_t>(NativeChunkCompression::ReservedZstd) ||
        compression == static_cast<uint32_t>(NativeChunkCompression::ReservedGDeflate);
}

bool nativeChunkCompressionDirectStorageCompatible(uint32_t compression) {
    return compression == static_cast<uint32_t>(NativeChunkCompression::None) ||
        compression == static_cast<uint32_t>(NativeChunkCompression::ReservedZstd) ||
        compression == static_cast<uint32_t>(NativeChunkCompression::ReservedGDeflate);
}

bool nativeChunkCompressionRequiresCpuFallback(uint32_t compression) {
    return compression == static_cast<uint32_t>(NativeChunkCompression::ReservedZstd) ||
        compression == static_cast<uint32_t>(NativeChunkCompression::ReservedGDeflate);
}

struct RtpkgCompressionEstimate {
    std::string profile;
    std::string requiredRuntimeSupport;
    bool directStorageCompatible = false;
    bool cpuFallbackAvailable = true;
    bool gpuDecompressionAvailable = false;
    uint64_t compressedBytes = 0;
    double ratio = 1.0;
};

RtpkgCompressionEstimate estimateRtpkgCompression(std::string profile, const std::byte* bytes, size_t size) {
    profile = lowerAscii(std::move(profile));
    RtpkgCompressionEstimate out;
    out.profile = profile.empty() ? "none" : profile;
    out.compressedBytes = static_cast<uint64_t>(size);
    out.ratio = size == 0 ? 1.0 : static_cast<double>(out.compressedBytes) / static_cast<double>(size);
    out.requiredRuntimeSupport = "none";
    if (out.profile == "none" || out.profile == "uncompressed") {
        out.profile = "none";
        return out;
    }

    std::array<uint32_t, 256> histogram{};
    uint64_t repeatedPairs = 0;
    for (size_t i = 0; i < size; ++i) {
        ++histogram[static_cast<uint8_t>(bytes[i])];
        if (i > 0 && bytes[i] == bytes[i - 1]) {
            ++repeatedPairs;
        }
    }
    double entropy = 0.0;
    if (size > 0) {
        for (uint32_t count : histogram) {
            if (count == 0) {
                continue;
            }
            const double p = static_cast<double>(count) / static_cast<double>(size);
            entropy -= p * std::log2(p);
        }
    }
    const double repetition = size > 1 ? static_cast<double>(repeatedPairs) / static_cast<double>(size - 1) : 0.0;
    double ratio = std::clamp(0.18 + entropy / 8.0 * 0.78 - repetition * 0.25, 0.12, 1.0);
    if (out.profile == "zstd" || out.profile == "directstorage-zstd" || out.profile == "dstorage-zstd") {
        out.profile = "zstd";
        out.requiredRuntimeSupport = "DirectStorage 1.4 Zstd or CPU Zstd fallback";
        out.directStorageCompatible = true;
        ratio = std::clamp(ratio * 0.92, 0.10, 1.0);
    } else if (out.profile == "gdeflate" || out.profile == "directstorage-gdeflate" || out.profile == "dstorage-gdeflate") {
        out.profile = "gdeflate";
        out.requiredRuntimeSupport = "DirectStorage GDeflate or CPU decompression fallback";
        out.directStorageCompatible = true;
        ratio = std::clamp(ratio * 0.98, 0.12, 1.0);
    } else {
        out.requiredRuntimeSupport = "unknown compression profile";
        out.cpuFallbackAvailable = false;
        ratio = 1.0;
    }
    out.ratio = ratio;
    out.compressedBytes = static_cast<uint64_t>(std::ceil(static_cast<double>(size) * ratio));
    return out;
}

bool isStandaloneNativeAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerAscii(path.extension().string());
    return ext == ".rtmesh" || ext == ".rtmaterial" || ext == ".rttexture" ||
        ext == ".rtskeleton" || ext == ".rtanim" || ext == ".rtanimcontroller" || ext == ".rtskeletalmesh";
}

std::filesystem::path canonicalForSort(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical.lexically_normal();
}

std::string relativePackagePath(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (!root.empty()) {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
        if (!ec) {
            bool escapes = false;
            for (const auto& part : relative) {
                if (part == "..") {
                    escapes = true;
                    break;
                }
            }
            if (!escapes) {
                return relative.generic_string();
            }
        }
    }
    return path.filename().generic_string();
}

std::string debugStringFromPackage(const std::vector<std::byte>& bytes, const NativeAssetHeader& header, uint32_t offset, uint32_t size) {
    if (size == 0 || offset >= header.debugDirectorySize) {
        return {};
    }
    const uint64_t available = std::min<uint64_t>(size, header.debugDirectorySize - offset);
    if (!rangeInside(header.debugDirectoryOffset + offset, available, bytes.size())) {
        return {};
    }
    const char* begin = reinterpret_cast<const char*>(bytes.data() + header.debugDirectoryOffset + offset);
    return std::string(begin, begin + available);
}

std::string debugRecordValueFromPackage(
    const std::vector<std::byte>& bytes,
    const NativeAssetHeader& header,
    const std::vector<NativeDebugRecord>& records,
    const std::string& key) {
    for (const NativeDebugRecord& record : records) {
        const std::string recordKey = debugStringFromPackage(bytes, header, record.keyOffset, record.keySize);
        if (recordKey == key) {
            return debugStringFromPackage(bytes, header, record.valueOffset, record.valueSize);
        }
    }
    return {};
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

bool RtpkgWriter::write(const std::filesystem::path& path, const RtpkgWriteDesc& desc, NativeBinaryError* error) const {
    if (desc.assets.empty()) {
        if (error) {
            *error = makePackageError(NativeBinaryErrorCode::CorruptHeader, path, "assets", 0, 1, "Cannot write an empty native package");
        }
        return false;
    }

    std::vector<RtpkgEmbeddedAssetInfo> embeddedAssets;
    embeddedAssets.reserve(desc.assets.size());
    std::vector<std::vector<std::byte>> embeddedBytes;
    embeddedBytes.reserve(desc.assets.size());
    std::vector<std::vector<NativeDependencyRecord>> embeddedDependencies;
    embeddedDependencies.reserve(desc.assets.size());

    std::vector<RtpkgAssetInput> sortedInputs = desc.assets;
    std::sort(sortedInputs.begin(), sortedInputs.end(), [](const RtpkgAssetInput& a, const RtpkgAssetInput& b) {
        const std::string aKey = a.packagePath.empty() ? a.path.generic_string() : a.packagePath;
        const std::string bKey = b.packagePath.empty() ? b.path.generic_string() : b.packagePath;
        return lowerAscii(aKey) < lowerAscii(bKey);
    });

    NativeAssetReader assetReader;
    for (const RtpkgAssetInput& input : sortedInputs) {
        if (!isStandaloneNativeAssetPath(input.path)) {
            if (error) {
                *error = makePackageError(NativeBinaryErrorCode::UnsupportedPlatformFeature, input.path, "assets", 0, 0, "Package input is not a supported standalone native asset");
            }
            return false;
        }
        NativeAssetInspection inspection = assetReader.inspect(input.path, true);
        if (!inspection.ok) {
            if (error) {
                *error = inspection.errors.empty()
                    ? makePackageError(NativeBinaryErrorCode::CorruptHeader, input.path, "asset", 0, 0, "Package input failed native asset validation")
                    : inspection.errors.front();
            }
            return false;
        }
        std::vector<std::byte> bytes;
        NativeBinaryError readError;
        if (!readFileBytes(input.path, bytes, &readError)) {
            if (error) {
                *error = readError;
            }
            return false;
        }

        RtpkgEmbeddedAssetInfo info;
        info.sourcePath = input.path;
        info.packagePath = input.packagePath.empty() ? input.path.filename().generic_string() : input.packagePath;
        info.kind = static_cast<NativeAssetKind>(inspection.header.assetKind);
        info.guid = nativeGuidToText(inspection.header.assetGuid);
        info.sourceSize = bytes.size();
        info.payloadHashValid = inspection.payloadHashValid;
        embeddedAssets.push_back(std::move(info));
        embeddedBytes.push_back(std::move(bytes));
        embeddedDependencies.push_back(std::move(inspection.dependencies));
    }

    std::vector<std::byte> bytes(sizeof(NativeAssetHeader));
    std::map<std::string, uint32_t> stringOffsets;
    std::vector<std::byte> debugDirectory;
    std::vector<NativeObjectRecord> objectRecords;
    std::vector<NativeDependencyRecord> dependencyRecords;
    std::vector<NativeDebugRecord> debugRecords;
    objectRecords.reserve(embeddedAssets.size());

    const std::string debugName = desc.debugName.empty() ? path.stem().string() : desc.debugName;
    appendDebugRecord(debugRecords, debugDirectory, stringOffsets, 1, "packageName", debugName);
    appendDebugRecord(debugRecords, debugDirectory, stringOffsets, 1, "assetCount", std::to_string(embeddedAssets.size()));

    for (size_t i = 0; i < embeddedAssets.size(); ++i) {
        const RtpkgEmbeddedAssetInfo& asset = embeddedAssets[i];
        NativeObjectRecord object{};
        object.objectGuid = nativeGuidFromText(asset.guid);
        object.assetKind = static_cast<uint32_t>(asset.kind);
        object.contentVersion = 1;
        object.firstChunk = 0;
        object.chunkCount = 1;
        object.firstDependency = static_cast<uint32_t>(dependencyRecords.size());
        object.dependencyCount = static_cast<uint32_t>(embeddedDependencies[i].size());
        object.debugNameOffset = appendString(debugDirectory, stringOffsets, asset.packagePath);
        object.debugNameSize = static_cast<uint32_t>(asset.packagePath.size());
        for (size_t dependencyIndex = 0; dependencyIndex < embeddedDependencies[i].size(); ++dependencyIndex) {
            const NativeDependencyRecord& sourceDependency = embeddedDependencies[i][dependencyIndex];
            NativeDependencyRecord dependency{};
            dependency.dependencyGuid = sourceDependency.dependencyGuid;
            dependency.assetKind = sourceDependency.assetKind;
            dependency.flags = sourceDependency.flags;
            const std::string dependencyName = asset.packagePath + ".dependency." + std::to_string(dependencyIndex) + "." + nativeGuidToText(sourceDependency.dependencyGuid);
            dependency.debugNameOffset = appendString(debugDirectory, stringOffsets, dependencyName);
            dependency.debugNameSize = static_cast<uint32_t>(dependencyName.size());
            dependencyRecords.push_back(dependency);
        }
        objectRecords.push_back(object);
        appendDebugRecord(debugRecords, debugDirectory, stringOffsets, 2, "asset." + std::to_string(i) + ".path", asset.packagePath);
        appendDebugRecord(debugRecords, debugDirectory, stringOffsets, 2, "asset." + std::to_string(i) + ".source", asset.packagePath);
    }
    appendPadding(debugDirectory);

    appendPadding(bytes);
    const uint64_t objectTableOffset = bytes.size();
    for (const NativeObjectRecord& record : objectRecords) appendPod(bytes, record);
    appendPadding(bytes);
    const uint64_t chunkTableOffset = bytes.size();
    bytes.resize(bytes.size() + embeddedAssets.size() * sizeof(NativeChunkRecord));
    appendPadding(bytes);
    const uint64_t dependencyTableOffset = bytes.size();
    for (const NativeDependencyRecord& record : dependencyRecords) appendPod(bytes, record);
    appendPadding(bytes);
    const uint64_t debugRecordTableOffset = bytes.size();
    for (const NativeDebugRecord& record : debugRecords) appendPod(bytes, record);
    appendPadding(bytes);
    const uint64_t debugDirectoryOffset = bytes.size();
    appendBytes(bytes, debugDirectory);
    appendPadding(bytes);

    std::vector<NativeChunkRecord> chunkRecords;
    chunkRecords.reserve(embeddedBytes.size());
    std::vector<std::byte> payloadBytes;
    std::map<std::string, uint32_t> chunkIndexByHash;
    std::string packageManifestText;
    for (size_t i = 0; i < embeddedBytes.size(); ++i) {
        const auto payloadHash = nativeHashBytes(embeddedBytes[i]);
        const std::string payloadHashText = nativeHashHex(payloadHash);
        packageManifestText += embeddedAssets[i].packagePath + "|" + embeddedAssets[i].guid + "|" +
            nativeAssetKindName(embeddedAssets[i].kind) + "|" + payloadHashText + "\n";

        const auto existing = chunkIndexByHash.find(payloadHashText);
        if (existing != chunkIndexByHash.end()) {
            const uint32_t chunkIndex = existing->second;
            objectRecords[i].firstChunk = chunkIndex;
            embeddedAssets[i].packageOffset = chunkRecords[chunkIndex].offset;
            embeddedAssets[i].packageSize = chunkRecords[chunkIndex].size;
            continue;
        }

        appendPadding(bytes);
        NativeChunkRecord chunk{};
        chunk.type = kRtpkgEmbeddedNativeAssetChunk;
        chunk.compression = static_cast<uint32_t>(NativeChunkCompression::None);
        chunk.offset = bytes.size();
        chunk.size = embeddedBytes[i].size();
        chunk.uncompressedSize = embeddedBytes[i].size();
        chunk.payloadHash = payloadHash;
        chunk.flags = static_cast<uint32_t>(embeddedAssets[i].kind);
        embeddedAssets[i].packageOffset = chunk.offset;
        embeddedAssets[i].packageSize = chunk.size;
        const uint32_t chunkIndex = static_cast<uint32_t>(chunkRecords.size());
        objectRecords[i].firstChunk = chunkIndex;
        chunkIndexByHash.emplace(payloadHashText, chunkIndex);
        appendBytes(bytes, embeddedBytes[i]);
        appendBytes(payloadBytes, embeddedBytes[i]);
        chunkRecords.push_back(chunk);
    }
    appendPadding(bytes);

    NativeAssetHeader header{};
    header.magic = kNativeAssetMagicRtpkg;
    header.assetKind = static_cast<uint32_t>(NativeAssetKind::Package);
    header.assetGuid = nativeGuidFromText("rtpkg:" + nativeHashHex(nativeHashText(packageManifestText)));
    header.sourceHash = nativeHashText(packageManifestText);
    header.importSettingsHash = nativeHashText("rtpkg-writer-v1-uncompressed-dedup");
    header.payloadHash = nativeHashBytes(payloadBytes);
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
    header.migrationTableCount = 0;
    header.migrationTableStride = sizeof(NativeMigrationRecord);
    header.fileSize = bytes.size();

    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!objectRecords.empty()) {
        std::memcpy(bytes.data() + objectTableOffset, objectRecords.data(), objectRecords.size() * sizeof(NativeObjectRecord));
    }
    if (!chunkRecords.empty()) {
        std::memcpy(bytes.data() + chunkTableOffset, chunkRecords.data(), chunkRecords.size() * sizeof(NativeChunkRecord));
    }

    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) {
                *error = makePackageError(NativeBinaryErrorCode::IoError, path, "file", 0, 0, "Could not create native package output directory: " + ec.message());
            }
            return false;
        }
    }
    const std::filesystem::path tempPath = path.string() + ".tmp";
    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            if (error) {
                *error = makePackageError(NativeBinaryErrorCode::IoError, tempPath, "file", 0, bytes.size(), "Could not open temporary native package for writing");
            }
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file.good()) {
            if (error) {
                *error = makePackageError(NativeBinaryErrorCode::IoError, tempPath, "file", 0, bytes.size(), "Could not write complete native package");
            }
            return false;
        }
    }

    RtpkgReader reader;
    RtpkgInspection validation = reader.inspect(tempPath, true);
    if (!validation.native.ok) {
        if (error) {
            *error = validation.native.errors.empty()
                ? makePackageError(NativeBinaryErrorCode::CorruptHeader, tempPath, "file", 0, bytes.size(), "Native package self-validation failed")
                : validation.native.errors.front();
        }
        return false;
    }

    if (!replaceFileAtomically(tempPath, path, ec)) {
        if (error) {
            *error = makePackageError(NativeBinaryErrorCode::IoError, path, "file", 0, bytes.size(), "Could not atomically replace native package destination: " + ec.message());
        }
        return false;
    }
    return true;
}

RtpkgInspection RtpkgReader::inspect(const std::filesystem::path& path, bool validatePayloadHash) const {
    RtpkgInspection result;
    NativeAssetReader reader;
    result.native = reader.inspect(path, validatePayloadHash);
    if (!result.native.ok) {
        return result;
    }
    if (result.native.header.magic != kNativeAssetMagicRtpkg || result.native.header.assetKind != static_cast<uint32_t>(NativeAssetKind::Package)) {
        result.native.ok = false;
        result.native.errors.push_back(makePackageError(NativeBinaryErrorCode::CorruptHeader, path, "header", 0, sizeof(NativeAssetHeader), "Native file is not an .rtpkg package"));
        return result;
    }

    std::vector<std::byte> bytes;
    NativeBinaryError readError;
    if (!readFileBytes(path, bytes, &readError)) {
        result.native.ok = false;
        result.native.errors.push_back(readError);
        return result;
    }
    NativeAssetReader embeddedReader;
    for (size_t i = 0; i < result.native.objects.size(); ++i) {
        const NativeObjectRecord& object = result.native.objects[i];
        if (object.chunkCount != 1 || object.firstChunk >= result.native.chunks.size()) {
            result.native.ok = false;
            result.native.errors.push_back(makePackageError(
                NativeBinaryErrorCode::CorruptTable,
                path,
                "packageObject",
                result.native.header.objectTableOffset + i * sizeof(NativeObjectRecord),
                sizeof(NativeObjectRecord),
                "Package object must reference exactly one valid embedded native asset chunk"));
            continue;
        }
        const NativeChunkRecord& chunk = result.native.chunks[object.firstChunk];
        if (chunk.type != kRtpkgEmbeddedNativeAssetChunk || !rangeInside(chunk.offset, chunk.size, bytes.size())) {
            result.native.ok = false;
            result.native.errors.push_back(makePackageError(
                NativeBinaryErrorCode::CorruptTable,
                path,
                "packageChunk",
                chunk.offset,
                chunk.size,
                "Package object chunk is not a valid embedded native asset payload"));
            continue;
        }
        RtpkgEmbeddedAssetInfo info;
        info.packagePath = debugStringFromPackage(bytes, result.native.header, object.debugNameOffset, object.debugNameSize);
        const std::string sourcePath = debugRecordValueFromPackage(
            bytes,
            result.native.header,
            result.native.debugRecords,
            "asset." + std::to_string(i) + ".source");
        if (!sourcePath.empty()) {
            info.sourcePath = sourcePath;
        }
        info.kind = static_cast<NativeAssetKind>(object.assetKind);
        info.guid = nativeGuidToText(object.objectGuid);
        info.packageOffset = chunk.offset;
        info.packageSize = chunk.size;
        info.packageChunkIndex = object.firstChunk;
        info.compression = chunk.compression;
        info.directStorageCompatible = nativeChunkCompressionDirectStorageCompatible(chunk.compression);
        info.cpuFallbackRequired = nativeChunkCompressionRequiresCpuFallback(chunk.compression);
        info.runtimePayloadDecodeReady = chunk.compression == static_cast<uint32_t>(NativeChunkCompression::None);
        info.sourceSize = chunk.uncompressedSize;
        if (!nativeChunkCompressionKnown(chunk.compression)) {
            result.native.ok = false;
            result.native.errors.push_back(makePackageError(
                NativeBinaryErrorCode::UnsupportedPlatformFeature,
                path,
                "packageChunk",
                chunk.offset,
                chunk.size,
                "Package chunk uses an unknown compression metadata value"));
        }
        if (validatePayloadHash) {
            info.payloadHashValid = nativeHashBytes(bytes.data() + chunk.offset, static_cast<size_t>(chunk.size)) == chunk.payloadHash;
            if (!info.payloadHashValid) {
                result.native.ok = false;
                result.native.errors.push_back(makePackageError(NativeBinaryErrorCode::HashMismatch, path, "packageChunk", chunk.offset, chunk.size, "Embedded native asset chunk hash does not match package chunk table"));
            }
        }
        if (info.runtimePayloadDecodeReady) {
            std::vector<std::byte> embeddedBytes(bytes.begin() + static_cast<size_t>(chunk.offset), bytes.begin() + static_cast<size_t>(chunk.offset + chunk.size));
            const NativeAssetInspection embeddedInspection = embeddedReader.inspectBytes(info.packagePath.empty() ? path : std::filesystem::path(info.packagePath), embeddedBytes, validatePayloadHash);
            if (!embeddedInspection.ok || embeddedInspection.header.assetGuid != object.objectGuid || embeddedInspection.header.assetKind != object.assetKind) {
                result.native.ok = false;
                result.native.errors.push_back(makePackageError(
                    embeddedInspection.ok ? NativeBinaryErrorCode::CorruptTable : (embeddedInspection.errors.empty() ? NativeBinaryErrorCode::CorruptHeader : embeddedInspection.errors.front().code),
                    path,
                    "embeddedAsset",
                    chunk.offset,
                    chunk.size,
                    "Embedded native asset payload failed validation or does not match package object GUID/kind"));
            }
        }
        result.embeddedAssets.push_back(std::move(info));
    }
    if (result.embeddedAssets.size() != result.native.objects.size()) {
        result.native.ok = false;
    }
    return result;
}

std::vector<RtpkgAssetInput> collectRtpkgAssetInputs(const std::vector<std::filesystem::path>& inputs, const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::path& input : inputs) {
        std::error_code ec;
        if (std::filesystem::is_directory(input, ec)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(input, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) {
                    break;
                }
                std::error_code entryError;
                if (entry.is_regular_file(entryError) && isStandaloneNativeAssetPath(entry.path())) {
                    files.push_back(canonicalForSort(entry.path()));
                }
            }
        } else if (std::filesystem::is_regular_file(input, ec) && isStandaloneNativeAssetPath(input)) {
            files.push_back(canonicalForSort(input));
        }
    }
    std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return lowerAscii(a.generic_string()) < lowerAscii(b.generic_string());
    });
    files.erase(std::unique(files.begin(), files.end()), files.end());

    const std::filesystem::path canonicalRoot = root.empty() ? std::filesystem::path{} : canonicalForSort(root);
    std::vector<RtpkgAssetInput> result;
    result.reserve(files.size());
    for (const std::filesystem::path& file : files) {
        result.push_back(RtpkgAssetInput{.path = file, .packagePath = relativePackagePath(file, canonicalRoot)});
    }
    return result;
}

nlohmann::json rtpkgInspectionToJson(const RtpkgInspection& inspection, const std::filesystem::path& path) {
    nlohmann::json report = nativeAssetInspectionToJson(inspection.native, path);
    nlohmann::json embedded = nlohmann::json::array();
    uint32_t uncompressedChunks = 0;
    uint32_t zstdChunks = 0;
    uint32_t gdeflateChunks = 0;
    uint32_t unsupportedCompressionChunks = 0;
    uint32_t directStorageCompatibleChunks = 0;
    uint32_t cpuFallbackRequiredChunks = 0;
    uint32_t runtimePayloadDecodeReadyChunks = 0;
    for (const NativeChunkRecord& chunk : inspection.native.chunks) {
        if (chunk.compression == static_cast<uint32_t>(NativeChunkCompression::None)) {
            ++uncompressedChunks;
            ++runtimePayloadDecodeReadyChunks;
        } else if (chunk.compression == static_cast<uint32_t>(NativeChunkCompression::ReservedZstd)) {
            ++zstdChunks;
            ++cpuFallbackRequiredChunks;
        } else if (chunk.compression == static_cast<uint32_t>(NativeChunkCompression::ReservedGDeflate)) {
            ++gdeflateChunks;
            ++cpuFallbackRequiredChunks;
        } else {
            ++unsupportedCompressionChunks;
        }
        if (nativeChunkCompressionDirectStorageCompatible(chunk.compression)) {
            ++directStorageCompatibleChunks;
        }
    }

    for (size_t i = 0; i < inspection.embeddedAssets.size(); ++i) {
        const RtpkgEmbeddedAssetInfo& asset = inspection.embeddedAssets[i];
        embedded.push_back({
            {"index", i},
            {"source_path", asset.sourcePath.empty() ? std::string{} : asset.sourcePath.generic_string()},
            {"package_path", asset.packagePath},
            {"kind", nativeAssetKindName(asset.kind)},
            {"guid", asset.guid},
            {"source_size", asset.sourceSize},
            {"package_chunk_index", asset.packageChunkIndex == UINT32_MAX ? nlohmann::json(nullptr) : nlohmann::json(asset.packageChunkIndex)},
            {"package_offset", asset.packageOffset},
            {"package_size", asset.packageSize},
            {"compression", asset.compression},
            {"compression_name", nativeChunkCompressionName(asset.compression)},
            {"directstorage_compatible", asset.directStorageCompatible},
            {"cpu_fallback_required", asset.cpuFallbackRequired},
            {"runtime_payload_decode_ready", asset.runtimePayloadDecodeReady},
            {"payload_hash_valid", asset.payloadHashValid},
        });
    }
    report["embedded_assets"] = std::move(embedded);
    report["package"] = {
        {"asset_count", inspection.embeddedAssets.size()},
        {"object_table_valid", inspection.native.ok && inspection.native.header.objectTableCount == inspection.embeddedAssets.size()},
        {"chunk_table_valid", inspection.native.ok && inspection.native.header.chunkTableCount > 0 && inspection.native.header.chunkTableCount <= inspection.embeddedAssets.size()},
        {"dependency_table_count", inspection.native.header.dependencyTableCount},
        {"compression", {
            {"chunk_count", inspection.native.chunks.size()},
            {"uncompressed_chunks", uncompressedChunks},
            {"zstd_metadata_chunks", zstdChunks},
            {"gdeflate_metadata_chunks", gdeflateChunks},
            {"unsupported_compression_chunks", unsupportedCompressionChunks},
            {"directstorage_compatible_chunks", directStorageCompatibleChunks},
            {"cpu_fallback_required_chunks", cpuFallbackRequiredChunks},
            {"runtime_payload_decode_ready_chunks", runtimePayloadDecodeReadyChunks},
            {"directstorage_ready", inspection.native.ok && unsupportedCompressionChunks == 0 && directStorageCompatibleChunks == inspection.native.chunks.size()},
            {"runtime_payload_decode_ready", inspection.native.ok && runtimePayloadDecodeReadyChunks == inspection.native.chunks.size()},
        }},
    };
    return report;
}

nlohmann::json rtpkgValidationIssueJson(
    std::string severity,
    std::string code,
    std::string message,
    nlohmann::json details = nlohmann::json::object()) {
    nlohmann::json issue = {
        {"severity", std::move(severity)},
        {"code", std::move(code)},
        {"message", std::move(message)},
    };
    if (!details.empty()) {
        issue["details"] = std::move(details);
    }
    return issue;
}

nlohmann::json rtpkgNativeErrorIssueJson(const NativeBinaryError& error) {
    return rtpkgValidationIssueJson(
        "error",
        nativeBinaryErrorCodeName(error.code),
        error.message,
        {
            {"path", error.path.empty() ? std::string{} : error.path.generic_string()},
            {"table", error.table},
            {"offset", error.offset},
            {"expected_size", error.expectedSize},
        });
}

nlohmann::json validateRtpkgInspectionToJson(const RtpkgInspection& inspection, const std::filesystem::path& path) {
    nlohmann::json failures = nlohmann::json::array();
    nlohmann::json warnings = nlohmann::json::array();
    nlohmann::json requirements = nlohmann::json::object();
    auto fail = [&](std::string code, std::string message, nlohmann::json details = nlohmann::json::object()) {
        failures.push_back(rtpkgValidationIssueJson("error", std::move(code), std::move(message), std::move(details)));
    };
    auto warn = [&](std::string code, std::string message, nlohmann::json details = nlohmann::json::object()) {
        warnings.push_back(rtpkgValidationIssueJson("warning", std::move(code), std::move(message), std::move(details)));
    };

    for (const NativeBinaryError& error : inspection.native.errors) {
        failures.push_back(rtpkgNativeErrorIssueJson(error));
    }
    for (const std::string& warning : inspection.native.warnings) {
        warnings.push_back(rtpkgValidationIssueJson("warning", "native_warning", warning));
    }

    const bool isPackage =
        inspection.native.header.magic == kNativeAssetMagicRtpkg &&
        inspection.native.header.assetKind == static_cast<uint32_t>(NativeAssetKind::Package);
    const bool hasAssets = !inspection.embeddedAssets.empty();
    const bool objectTableMatches = inspection.native.header.objectTableCount == inspection.embeddedAssets.size();
    const bool oneChunkPerAsset =
        inspection.native.header.chunkTableCount == inspection.embeddedAssets.size() &&
        inspection.native.chunks.size() == inspection.embeddedAssets.size();

    uint32_t hashValidAssets = 0;
    uint32_t directStorageCompatibleAssets = 0;
    uint32_t runtimeDecodeReadyAssets = 0;
    uint32_t cpuFallbackRequiredAssets = 0;
    uint32_t unsupportedCompressionAssets = 0;
    uint64_t packagePayloadBytes = 0;
    std::set<std::string> guids;
    std::set<std::string> duplicateGuids;
    std::set<std::string> validDuplicateTextureVariantGuids;
    std::set<std::string> invalidDuplicateGuids;
    std::map<std::string, std::vector<const RtpkgEmbeddedAssetInfo*>> assetsByGuid;
    std::set<std::string> packagePaths;
    std::set<std::string> duplicatePackagePaths;
    for (const RtpkgEmbeddedAssetInfo& asset : inspection.embeddedAssets) {
        packagePayloadBytes += asset.packageSize;
        if (asset.payloadHashValid) {
            ++hashValidAssets;
        }
        if (asset.directStorageCompatible) {
            ++directStorageCompatibleAssets;
        }
        if (asset.runtimePayloadDecodeReady) {
            ++runtimeDecodeReadyAssets;
        }
        if (asset.cpuFallbackRequired) {
            ++cpuFallbackRequiredAssets;
        }
        if (!nativeChunkCompressionKnown(asset.compression)) {
            ++unsupportedCompressionAssets;
        }
        if (!asset.guid.empty() && !guids.insert(asset.guid).second) {
            duplicateGuids.insert(asset.guid);
        }
        if (!asset.guid.empty()) {
            assetsByGuid[asset.guid].push_back(&asset);
        }
        const std::string packagePath = lowerAscii(asset.packagePath);
        if (!packagePath.empty() && !packagePaths.insert(packagePath).second) {
            duplicatePackagePaths.insert(asset.packagePath);
        }
    }
    for (const auto& [guid, assets] : assetsByGuid) {
        if (assets.size() <= 1) {
            continue;
        }
        bool allTextureVariants = true;
        std::set<std::string> variantPackagePaths;
        std::set<std::string> variantPayloadHashes;
        for (const RtpkgEmbeddedAssetInfo* asset : assets) {
            allTextureVariants = allTextureVariants && asset->kind == NativeAssetKind::Texture;
            allTextureVariants = allTextureVariants && !asset->packagePath.empty();
            allTextureVariants = allTextureVariants && variantPackagePaths.insert(lowerAscii(asset->packagePath)).second;
            if (asset->packageChunkIndex == UINT32_MAX || asset->packageChunkIndex >= inspection.native.chunks.size()) {
                allTextureVariants = false;
            } else {
                allTextureVariants = allTextureVariants && variantPayloadHashes.insert(nativeHashHex(inspection.native.chunks[asset->packageChunkIndex].payloadHash)).second;
            }
        }
        if (allTextureVariants) {
            validDuplicateTextureVariantGuids.insert(guid);
        } else {
            invalidDuplicateGuids.insert(guid);
        }
    }

    const bool allHashesValid = hasAssets && hashValidAssets == inspection.embeddedAssets.size();
    const bool allDirectStorageCompatible = hasAssets && directStorageCompatibleAssets == inspection.embeddedAssets.size();
    const bool allRuntimeDecodeReady = hasAssets && runtimeDecodeReadyAssets == inspection.embeddedAssets.size();
    const bool noUnsupportedCompression = unsupportedCompressionAssets == 0;
    const bool runtimeIndependent = hasAssets && oneChunkPerAsset && packagePayloadBytes > 0;

    requirements = {
        {"native_container_ok", inspection.native.ok},
        {"is_rtpkg_package", isPackage},
        {"has_embedded_assets", hasAssets},
        {"object_table_matches_embedded_assets", objectTableMatches},
        {"one_payload_chunk_per_embedded_asset", oneChunkPerAsset},
        {"all_payload_hashes_valid", allHashesValid},
        {"no_duplicate_embedded_guids", invalidDuplicateGuids.empty()},
        {"duplicate_embedded_guid_count", duplicateGuids.size()},
        {"valid_duplicate_texture_variant_guid_count", validDuplicateTextureVariantGuids.size()},
        {"no_duplicate_package_paths", duplicatePackagePaths.empty()},
        {"no_unsupported_compression", noUnsupportedCompression},
        {"directstorage_ready", allDirectStorageCompatible && noUnsupportedCompression},
        {"runtime_payload_decode_ready", allRuntimeDecodeReady},
        {"runtime_independent_of_source_files", runtimeIndependent},
    };

    if (!inspection.native.ok) {
        fail("native_container_invalid", "Native package container inspection failed");
    }
    if (!isPackage) {
        fail("not_rtpkg_package", "File is not a valid .rtpkg package");
    }
    if (!hasAssets) {
        fail("empty_package", "Package contains no embedded native assets");
    }
    if (!objectTableMatches) {
        fail("object_table_mismatch", "Package object table does not match embedded asset count", {
            {"object_table_count", inspection.native.header.objectTableCount},
            {"embedded_asset_count", inspection.embeddedAssets.size()},
        });
    }
    if (!oneChunkPerAsset) {
        fail("chunk_table_mismatch", "Package must contain exactly one payload chunk per embedded asset", {
            {"chunk_table_count", inspection.native.header.chunkTableCount},
            {"chunk_count", inspection.native.chunks.size()},
            {"embedded_asset_count", inspection.embeddedAssets.size()},
        });
    }
    if (!allHashesValid) {
        fail("payload_hash_validation_failed", "Not every embedded payload hash validates", {
            {"hash_valid_assets", hashValidAssets},
            {"embedded_asset_count", inspection.embeddedAssets.size()},
        });
    }
    if (!invalidDuplicateGuids.empty()) {
        fail("duplicate_embedded_guids", "Package contains duplicate embedded asset GUIDs that are not valid texture target variants", {
            {"guids", invalidDuplicateGuids},
            {"valid_texture_variant_guids", validDuplicateTextureVariantGuids},
        });
    }
    if (!duplicatePackagePaths.empty()) {
        fail("duplicate_package_paths", "Package contains duplicate embedded package paths", {
            {"package_paths", duplicatePackagePaths},
        });
    }
    if (!noUnsupportedCompression) {
        fail("unsupported_compression", "Package contains chunks with unsupported compression metadata", {
            {"unsupported_compression_assets", unsupportedCompressionAssets},
        });
    }
    if (!allDirectStorageCompatible) {
        fail("directstorage_incompatible_chunks", "Package contains chunks that are not compatible with DirectStorage metadata", {
            {"directstorage_compatible_assets", directStorageCompatibleAssets},
            {"embedded_asset_count", inspection.embeddedAssets.size()},
        });
    }
    if (!allRuntimeDecodeReady) {
        fail("runtime_payload_decode_unavailable", "Package contains compressed payloads without an implemented runtime decode path", {
            {"runtime_decode_ready_assets", runtimeDecodeReadyAssets},
            {"cpu_fallback_required_assets", cpuFallbackRequiredAssets},
            {"embedded_asset_count", inspection.embeddedAssets.size()},
        });
    }
    if (!runtimeIndependent) {
        fail("runtime_source_dependency_risk", "Package payload table is not self-contained enough for source-file-independent runtime loading", {
            {"payload_bytes", packagePayloadBytes},
            {"chunk_count", inspection.native.chunks.size()},
            {"embedded_asset_count", inspection.embeddedAssets.size()},
        });
    }
    if (cpuFallbackRequiredAssets > 0) {
        warn("cpu_decompression_required", "Package contains DirectStorage-compatible compressed chunks that currently require CPU fallback", {
            {"cpu_fallback_required_assets", cpuFallbackRequiredAssets},
        });
    }

    const bool ok = failures.empty();
    return {
        {"schema", "RtpkgValidationReportV1"},
        {"ok", ok},
        {"package_path", path.generic_string()},
        {"asset_count", inspection.embeddedAssets.size()},
        {"chunk_count", inspection.native.chunks.size()},
        {"payload_bytes", packagePayloadBytes},
        {"hash_valid_assets", hashValidAssets},
        {"directstorage_compatible_assets", directStorageCompatibleAssets},
        {"runtime_decode_ready_assets", runtimeDecodeReadyAssets},
        {"requirements", requirements},
        {"failures", failures},
        {"warnings", warnings},
        {"inspection", rtpkgInspectionToJson(inspection, path)},
    };
}

std::string rtpkgPatchAssetKey(const RtpkgEmbeddedAssetInfo& asset) {
    if (!asset.guid.empty()) {
        return "guid:" + lowerAscii(asset.guid);
    }
    return "path:" + lowerAscii(asset.packagePath);
}

std::string rtpkgPatchPayloadHashHex(const RtpkgInspection& inspection, const RtpkgEmbeddedAssetInfo& asset) {
    if (asset.packageChunkIndex == UINT32_MAX || asset.packageChunkIndex >= inspection.native.chunks.size()) {
        return {};
    }
    return nativeHashHex(inspection.native.chunks[asset.packageChunkIndex].payloadHash);
}

nlohmann::json rtpkgPatchAssetJson(
    std::string action,
    std::string key,
    std::string reason,
    const RtpkgInspection* baseInspection,
    const RtpkgEmbeddedAssetInfo* baseAsset,
    const RtpkgInspection* updatedInspection,
    const RtpkgEmbeddedAssetInfo* updatedAsset) {
    const RtpkgEmbeddedAssetInfo* primary = updatedAsset ? updatedAsset : baseAsset;
    nlohmann::json item = {
        {"action", std::move(action)},
        {"key", std::move(key)},
        {"reason", std::move(reason)},
        {"guid", primary ? primary->guid : std::string{}},
        {"package_path", primary ? primary->packagePath : std::string{}},
        {"kind", primary ? nativeAssetKindName(primary->kind) : std::string{}},
        {"base_package_size", baseAsset ? baseAsset->packageSize : 0},
        {"updated_package_size", updatedAsset ? updatedAsset->packageSize : 0},
        {"base_source_size", baseAsset ? baseAsset->sourceSize : 0},
        {"updated_source_size", updatedAsset ? updatedAsset->sourceSize : 0},
        {"base_compression", baseAsset ? nativeChunkCompressionName(baseAsset->compression) : std::string{}},
        {"updated_compression", updatedAsset ? nativeChunkCompressionName(updatedAsset->compression) : std::string{}},
        {"base_payload_hash", (baseInspection && baseAsset) ? rtpkgPatchPayloadHashHex(*baseInspection, *baseAsset) : std::string{}},
        {"updated_payload_hash", (updatedInspection && updatedAsset) ? rtpkgPatchPayloadHashHex(*updatedInspection, *updatedAsset) : std::string{}},
    };
    if (baseAsset) {
        item["base_package_path"] = baseAsset->packagePath;
        item["base_guid"] = baseAsset->guid;
        item["base_kind"] = nativeAssetKindName(baseAsset->kind);
    }
    if (updatedAsset) {
        item["updated_package_path"] = updatedAsset->packagePath;
        item["updated_guid"] = updatedAsset->guid;
        item["updated_kind"] = nativeAssetKindName(updatedAsset->kind);
    }
    return item;
}

bool rtpkgPatchSameAssetContent(
    const RtpkgInspection& baseInspection,
    const RtpkgEmbeddedAssetInfo& baseAsset,
    const RtpkgInspection& updatedInspection,
    const RtpkgEmbeddedAssetInfo& updatedAsset) {
    return baseAsset.kind == updatedAsset.kind &&
        baseAsset.sourceSize == updatedAsset.sourceSize &&
        baseAsset.packageSize == updatedAsset.packageSize &&
        baseAsset.compression == updatedAsset.compression &&
        rtpkgPatchPayloadHashHex(baseInspection, baseAsset) == rtpkgPatchPayloadHashHex(updatedInspection, updatedAsset);
}

int writeRtpkgCommand(const std::filesystem::path& packagePath, const std::vector<std::filesystem::path>& inputs, const std::filesystem::path& root) {
    RtpkgWriteDesc desc;
    desc.debugName = packagePath.stem().string();
    desc.root = root;
    desc.assets = collectRtpkgAssetInputs(inputs, root);
    if (desc.assets.empty()) {
        std::cerr << "No standalone native assets found for package input.\n";
        return 1;
    }
    RtpkgWriter writer;
    NativeBinaryError error;
    if (!writer.write(packagePath, desc, &error)) {
        std::cerr << "Failed to write native package: " << error.message << '\n';
        return 1;
    }
    return 0;
}

int inspectRtpkgCommand(const std::filesystem::path& path, const std::filesystem::path& jsonOut) {
    RtpkgReader reader;
    const RtpkgInspection inspection = reader.inspect(path, true);
    const nlohmann::json report = rtpkgInspectionToJson(inspection, path);
    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
        std::ofstream out(jsonOut);
        if (!out.is_open()) {
            std::cerr << "Failed to write native package inspection JSON: " << jsonOut << '\n';
            return 1;
        }
        out << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return inspection.native.ok ? 0 : 1;
}

int validateRtpkgCommand(const std::filesystem::path& path, const std::filesystem::path& jsonOut) {
    RtpkgReader reader;
    const RtpkgInspection inspection = reader.inspect(path, true);
    const nlohmann::json report = validateRtpkgInspectionToJson(inspection, path);
    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Failed to create native package validation report directory: " << parent << " (" << ec.message() << ")\n";
                return 1;
            }
        }
        std::ofstream out(jsonOut);
        if (!out.is_open()) {
            std::cerr << "Failed to write native package validation JSON: " << jsonOut << '\n';
            return 1;
        }
        out << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return report.value("ok", false) ? 0 : 1;
}

int planRtpkgPatchCommand(
    const std::filesystem::path& basePath,
    const std::filesystem::path& updatedPath,
    const std::filesystem::path& jsonOut) {
    RtpkgReader reader;
    const RtpkgInspection baseInspection = reader.inspect(basePath, true);
    const RtpkgInspection updatedInspection = reader.inspect(updatedPath, true);
    const nlohmann::json baseValidation = validateRtpkgInspectionToJson(baseInspection, basePath);
    const nlohmann::json updatedValidation = validateRtpkgInspectionToJson(updatedInspection, updatedPath);

    std::map<std::string, const RtpkgEmbeddedAssetInfo*> baseAssets;
    std::map<std::string, const RtpkgEmbeddedAssetInfo*> updatedAssets;
    uint64_t basePayloadBytes = 0;
    uint64_t updatedPayloadBytes = 0;
    for (const RtpkgEmbeddedAssetInfo& asset : baseInspection.embeddedAssets) {
        basePayloadBytes += asset.packageSize;
        baseAssets.emplace(rtpkgPatchAssetKey(asset), &asset);
    }
    for (const RtpkgEmbeddedAssetInfo& asset : updatedInspection.embeddedAssets) {
        updatedPayloadBytes += asset.packageSize;
        updatedAssets.emplace(rtpkgPatchAssetKey(asset), &asset);
    }

    nlohmann::json actions = nlohmann::json::array();
    uint32_t addedAssets = 0;
    uint32_t removedAssets = 0;
    uint32_t changedAssets = 0;
    uint32_t unchangedAssets = 0;
    uint64_t patchPayloadBytes = 0;
    uint64_t removedPayloadBytes = 0;
    uint64_t reusedPayloadBytes = 0;

    std::set<std::string> keys;
    for (const auto& [key, asset] : baseAssets) {
        (void)asset;
        keys.insert(key);
    }
    for (const auto& [key, asset] : updatedAssets) {
        (void)asset;
        keys.insert(key);
    }

    for (const std::string& key : keys) {
        const auto baseIt = baseAssets.find(key);
        const auto updatedIt = updatedAssets.find(key);
        const RtpkgEmbeddedAssetInfo* baseAsset = baseIt == baseAssets.end() ? nullptr : baseIt->second;
        const RtpkgEmbeddedAssetInfo* updatedAsset = updatedIt == updatedAssets.end() ? nullptr : updatedIt->second;
        if (baseAsset && updatedAsset) {
            if (rtpkgPatchSameAssetContent(baseInspection, *baseAsset, updatedInspection, *updatedAsset)) {
                ++unchangedAssets;
                reusedPayloadBytes += updatedAsset->packageSize;
                actions.push_back(rtpkgPatchAssetJson("unchanged", key, "payload_reusable", &baseInspection, baseAsset, &updatedInspection, updatedAsset));
            } else {
                ++changedAssets;
                patchPayloadBytes += updatedAsset->packageSize;
                actions.push_back(rtpkgPatchAssetJson("changed", key, "payload_or_metadata_changed", &baseInspection, baseAsset, &updatedInspection, updatedAsset));
            }
        } else if (updatedAsset) {
            ++addedAssets;
            patchPayloadBytes += updatedAsset->packageSize;
            actions.push_back(rtpkgPatchAssetJson("added", key, "new_asset", nullptr, nullptr, &updatedInspection, updatedAsset));
        } else if (baseAsset) {
            ++removedAssets;
            removedPayloadBytes += baseAsset->packageSize;
            actions.push_back(rtpkgPatchAssetJson("removed", key, "removed_asset", &baseInspection, baseAsset, nullptr, nullptr));
        }
    }

    const bool baseOk = baseValidation.value("ok", false);
    const bool updatedOk = updatedValidation.value("ok", false);
    const bool ok = baseOk && updatedOk;
    const double reuseRatio = updatedPayloadBytes == 0
        ? 0.0
        : static_cast<double>(reusedPayloadBytes) / static_cast<double>(updatedPayloadBytes);
    const nlohmann::json report = {
        {"schema", "RtpkgPatchPlanV1"},
        {"ok", ok},
        {"patch_ready", ok},
        {"requires_full_rewrite", !ok},
        {"base_package_path", basePath.generic_string()},
        {"updated_package_path", updatedPath.generic_string()},
        {"base_asset_count", baseInspection.embeddedAssets.size()},
        {"updated_asset_count", updatedInspection.embeddedAssets.size()},
        {"added_assets", addedAssets},
        {"removed_assets", removedAssets},
        {"changed_assets", changedAssets},
        {"unchanged_assets", unchangedAssets},
        {"base_payload_bytes", basePayloadBytes},
        {"updated_payload_bytes", updatedPayloadBytes},
        {"patch_payload_bytes", patchPayloadBytes},
        {"removed_payload_bytes", removedPayloadBytes},
        {"reused_payload_bytes", reusedPayloadBytes},
        {"reuse_ratio", reuseRatio},
        {"base_validation", {
            {"ok", baseOk},
            {"failure_count", baseValidation.value("failures", nlohmann::json::array()).size()},
            {"requirements", baseValidation.value("requirements", nlohmann::json::object())},
        }},
        {"updated_validation", {
            {"ok", updatedOk},
            {"failure_count", updatedValidation.value("failures", nlohmann::json::array()).size()},
            {"requirements", updatedValidation.value("requirements", nlohmann::json::object())},
        }},
        {"actions", actions},
    };

    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Failed to create native package patch-plan report directory: " << parent << " (" << ec.message() << ")\n";
                return 1;
            }
        }
        std::ofstream out(jsonOut);
        if (!out.is_open()) {
            std::cerr << "Failed to write native package patch-plan JSON: " << jsonOut << '\n';
            return 1;
        }
        out << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return ok ? 0 : 1;
}

int simulateRtpkgCompressionCommand(
    const std::filesystem::path& path,
    std::string_view profile,
    const std::filesystem::path& jsonOut) {
    RtpkgReader reader;
    const RtpkgInspection inspection = reader.inspect(path, true);
    std::vector<std::byte> bytes;
    NativeBinaryError readError;
    if (inspection.native.ok && !readFileBytes(path, bytes, &readError)) {
        std::cerr << "Failed to read native package for compression simulation: " << readError.message << '\n';
        return 1;
    }

    nlohmann::json chunks = nlohmann::json::array();
    uint64_t totalSourceBytes = 0;
    uint64_t totalEstimatedBytes = 0;
    uint32_t directStorageCompatibleChunks = 0;
    uint32_t cpuFallbackChunks = 0;
    uint32_t gpuDecompressionChunks = 0;
    const std::string selectedProfile = std::string(profile.empty() ? "none" : profile);
    for (size_t i = 0; i < inspection.native.chunks.size(); ++i) {
        const NativeChunkRecord& chunk = inspection.native.chunks[i];
        const bool rangeValid = rangeInside(chunk.offset, chunk.size, bytes.size());
        RtpkgCompressionEstimate estimate;
        if (rangeValid) {
            estimate = estimateRtpkgCompression(
                selectedProfile,
                bytes.data() + static_cast<size_t>(chunk.offset),
                static_cast<size_t>(chunk.size));
        } else {
            estimate.profile = lowerAscii(selectedProfile);
        }
        totalSourceBytes += chunk.uncompressedSize;
        totalEstimatedBytes += estimate.compressedBytes;
        directStorageCompatibleChunks += estimate.directStorageCompatible ? 1u : 0u;
        cpuFallbackChunks += estimate.cpuFallbackAvailable ? 1u : 0u;
        gpuDecompressionChunks += estimate.gpuDecompressionAvailable ? 1u : 0u;
        chunks.push_back({
            {"index", i},
            {"type", chunk.type},
            {"existing_compression", nativeChunkCompressionName(chunk.compression)},
            {"package_offset", chunk.offset},
            {"package_size", chunk.size},
            {"uncompressed_size", chunk.uncompressedSize},
            {"range_valid", rangeValid},
            {"simulated_compression", estimate.profile},
            {"estimated_compressed_size", estimate.compressedBytes},
            {"estimated_ratio", estimate.ratio},
            {"required_runtime_support", estimate.requiredRuntimeSupport},
            {"directstorage_compatible", estimate.directStorageCompatible},
            {"cpu_fallback_available", estimate.cpuFallbackAvailable},
            {"gpu_decompression_available", estimate.gpuDecompressionAvailable},
        });
    }

    const nlohmann::json report = {
        {"schema", "RtpkgCompressionSimulationV1"},
        {"ok", inspection.native.ok},
        {"package_path", path.generic_string()},
        {"profile", lowerAscii(selectedProfile)},
        {"asset_count", inspection.embeddedAssets.size()},
        {"chunk_count", inspection.native.chunks.size()},
        {"source_bytes", totalSourceBytes},
        {"estimated_compressed_bytes", totalEstimatedBytes},
        {"estimated_ratio", totalSourceBytes == 0 ? 1.0 : static_cast<double>(totalEstimatedBytes) / static_cast<double>(totalSourceBytes)},
        {"directstorage_compatible_chunks", directStorageCompatibleChunks},
        {"cpu_fallback_chunks", cpuFallbackChunks},
        {"gpu_decompression_chunks", gpuDecompressionChunks},
        {"compression_metadata_ready", inspection.native.ok && !inspection.native.chunks.empty()},
        {"payloads_rewritten", false},
        {"chunks", chunks},
        {"inspection", rtpkgInspectionToJson(inspection, path)},
    };

    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
        std::ofstream out(jsonOut);
        if (!out.is_open()) {
            std::cerr << "Failed to write native package compression simulation JSON: " << jsonOut << '\n';
            return 1;
        }
        out << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return inspection.native.ok ? 0 : 1;
}

int simulateRtpkgStreamingIoCommand(
    const std::filesystem::path& path,
    const StreamingRuntimeOptions& options,
    const std::filesystem::path& jsonOut) {
    RtpkgReader reader;
    const RtpkgInspection inspection = reader.inspect(path, true);
    std::unique_ptr<StreamingIoBackend> backend = makeStreamingIoBackend(options);

    nlohmann::json chunkReads = nlohmann::json::array();
    bool ok = inspection.native.ok;
    uint64_t hashValidChunks = 0;
    uint64_t bytesRead = 0;
    const auto benchmarkStart = std::chrono::steady_clock::now();
    if (inspection.native.ok) {
        for (size_t i = 0; i < inspection.native.chunks.size(); ++i) {
            const NativeChunkRecord& chunk = inspection.native.chunks[i];
            StreamingIoReadRequest request;
            request.path = path;
            request.offset = chunk.offset;
            request.size = chunk.size;
            request.label = "rtpkg chunk " + std::to_string(i);
            StreamingIoReadResult result = backend->read(request);
            const bool sizeMatches = result.ok && result.completedBytes == chunk.size && result.bytes.size() == chunk.size;
            const bool hashValid = sizeMatches && nativeHashBytes(result.bytes) == chunk.payloadHash;
            ok = ok && result.ok && hashValid;
            hashValidChunks += hashValid ? 1ull : 0ull;
            bytesRead += result.completedBytes;
            chunkReads.push_back({
                {"index", i},
                {"type", chunk.type},
                {"compression", chunk.compression},
                {"compression_name", nativeChunkCompressionName(chunk.compression)},
                {"offset", chunk.offset},
                {"requested_bytes", chunk.size},
                {"completed_bytes", result.completedBytes},
                {"backend", streamingIoBackendKindName(result.backend)},
                {"effective_backend", backend->name()},
                {"ok", result.ok},
                {"size_matches", sizeMatches},
                {"payload_hash_valid", hashValid},
                {"fallback_reason", result.fallbackReason},
                {"error", result.error},
            });
        }
    }
    const auto benchmarkEnd = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(benchmarkEnd - benchmarkStart).count();
    const StreamingIoMetrics metrics = backend->metrics();
    const double throughputMiBPerSec = elapsedMs <= 0.0
        ? 0.0
        : (static_cast<double>(metrics.bytesCompleted) / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);

    const nlohmann::json report = {
        {"schema", "RtpkgStreamingIoProbeV1"},
        {"ok", ok},
        {"package_path", path.generic_string()},
        {"options", streamingRuntimeOptionsToJson(options)},
        {"availability", streamingIoBackendAvailabilityJson(options)},
        {"metrics", streamingIoMetricsJson(metrics)},
        {"elapsed_ms", elapsedMs},
        {"throughput_mib_per_sec", throughputMiBPerSec},
        {"chunk_count", inspection.native.chunks.size()},
        {"hash_valid_chunks", hashValidChunks},
        {"bytes_read", bytesRead},
        {"reads", chunkReads},
        {"package", rtpkgInspectionToJson(inspection, path)},
    };

    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Failed to create native package streaming I/O report directory: " << parent << " (" << ec.message() << ")\n";
                return 1;
            }
        }
        std::ofstream out(jsonOut);
        if (!out.is_open()) {
            std::cerr << "Failed to write native package streaming I/O report: " << jsonOut << '\n';
            return 1;
        }
        out << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return ok ? 0 : 1;
}

} // namespace rtv
