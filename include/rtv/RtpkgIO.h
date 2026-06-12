#pragma once

#include "rtv/NativeBinaryIO.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

struct RtpkgAssetInput {
    std::filesystem::path path;
    std::string packagePath;
};

struct RtpkgWriteDesc {
    std::string debugName;
    std::filesystem::path root;
    std::vector<RtpkgAssetInput> assets;
};

struct RtpkgEmbeddedAssetInfo {
    std::filesystem::path sourcePath;
    std::string packagePath;
    NativeAssetKind kind = NativeAssetKind::Unknown;
    std::string guid;
    uint64_t sourceSize = 0;
    uint64_t packageOffset = 0;
    uint64_t packageSize = 0;
    bool payloadHashValid = false;
};

struct RtpkgInspection {
    NativeAssetInspection native;
    std::vector<RtpkgEmbeddedAssetInfo> embeddedAssets;
};

class RtpkgWriter {
public:
    [[nodiscard]] bool write(const std::filesystem::path& path, const RtpkgWriteDesc& desc, NativeBinaryError* error = nullptr) const;
};

class RtpkgReader {
public:
    [[nodiscard]] RtpkgInspection inspect(const std::filesystem::path& path, bool validatePayloadHash = true) const;
};

[[nodiscard]] std::vector<RtpkgAssetInput> collectRtpkgAssetInputs(const std::vector<std::filesystem::path>& inputs, const std::filesystem::path& root = {});
[[nodiscard]] nlohmann::json rtpkgInspectionToJson(const RtpkgInspection& inspection, const std::filesystem::path& path);
[[nodiscard]] int writeRtpkgCommand(const std::filesystem::path& packagePath, const std::vector<std::filesystem::path>& inputs, const std::filesystem::path& root = {});
[[nodiscard]] int inspectRtpkgCommand(const std::filesystem::path& path, const std::filesystem::path& jsonOut = {});

} // namespace rtv
