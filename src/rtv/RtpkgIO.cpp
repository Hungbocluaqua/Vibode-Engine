#include "rtv/RtpkgIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
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
        info.sourceSize = chunk.uncompressedSize;
        if (validatePayloadHash) {
            info.payloadHashValid = nativeHashBytes(bytes.data() + chunk.offset, static_cast<size_t>(chunk.size)) == chunk.payloadHash;
            if (!info.payloadHashValid) {
                result.native.ok = false;
                result.native.errors.push_back(makePackageError(NativeBinaryErrorCode::HashMismatch, path, "packageChunk", chunk.offset, chunk.size, "Embedded native asset chunk hash does not match package chunk table"));
            }
        }
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
    for (size_t i = 0; i < inspection.embeddedAssets.size(); ++i) {
        const RtpkgEmbeddedAssetInfo& asset = inspection.embeddedAssets[i];
        embedded.push_back({
            {"index", i},
            {"source_path", asset.sourcePath.empty() ? std::string{} : asset.sourcePath.generic_string()},
            {"package_path", asset.packagePath},
            {"kind", nativeAssetKindName(asset.kind)},
            {"guid", asset.guid},
            {"source_size", asset.sourceSize},
            {"package_offset", asset.packageOffset},
            {"package_size", asset.packageSize},
            {"payload_hash_valid", asset.payloadHashValid},
        });
    }
    report["embedded_assets"] = std::move(embedded);
    report["package"] = {
        {"asset_count", inspection.embeddedAssets.size()},
        {"object_table_valid", inspection.native.ok && inspection.native.header.objectTableCount == inspection.embeddedAssets.size()},
        {"chunk_table_valid", inspection.native.ok && inspection.native.header.chunkTableCount > 0 && inspection.native.header.chunkTableCount <= inspection.embeddedAssets.size()},
        {"dependency_table_count", inspection.native.header.dependencyTableCount},
    };
    return report;
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

} // namespace rtv
