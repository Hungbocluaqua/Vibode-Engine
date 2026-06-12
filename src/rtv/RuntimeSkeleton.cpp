#include "rtv/RuntimeSkeleton.h"

#include "rtv/NativeBinaryIO.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace rtv {
namespace {

constexpr uint32_t kRtskeletonMetadataJsonChunk = 100u;

void addWarning(std::vector<std::string>* warnings, std::string message) {
    if (warnings != nullptr) {
        warnings->push_back(std::move(message));
    }
}

bool readFileBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    return file.good() || size == 0;
}

bool chunkRangeInside(const NativeChunkRecord& chunk, size_t fileSize) {
    return chunk.offset <= fileSize && chunk.size <= fileSize - chunk.offset;
}

std::array<float, 16> matrixFromJson(const nlohmann::json& json) {
    std::array<float, 16> result{};
    if (!json.is_array()) {
        return result;
    }
    for (size_t i = 0; i < std::min<size_t>(16, json.size()); ++i) {
        if (json[i].is_number()) {
            result[i] = json[i].get<float>();
        }
    }
    return result;
}

int32_t indexFromJsonValue(const nlohmann::json& value, int32_t fallback = -1) {
    if (value.is_number_integer()) {
        return value.get<int32_t>();
    }
    if (value.is_object()) {
        if (value.contains("index") && value["index"].is_number_integer()) {
            return value["index"].get<int32_t>();
        }
        if (value.contains("nodeIndex") && value["nodeIndex"].is_number_integer()) {
            return value["nodeIndex"].get<int32_t>();
        }
    }
    return fallback;
}

std::string nameFromJsonValue(const nlohmann::json& value, std::string fallback = {}) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_object()) {
        if (value.contains("name") && value["name"].is_string()) {
            return value["name"].get<std::string>();
        }
        if (value.contains("jointName") && value["jointName"].is_string()) {
            return value["jointName"].get<std::string>();
        }
    }
    return fallback;
}

const nlohmann::json* skeletonPayloadFromRoot(const nlohmann::json& root) {
    if (root.contains("skeleton") && root["skeleton"].is_object()) {
        return &root["skeleton"];
    }
    if (root.contains("skin") && root["skin"].is_object()) {
        return &root;
    }
    if (root.contains("joints") && root["joints"].is_array()) {
        return &root;
    }
    return nullptr;
}

} // namespace

RuntimeSkeleton RuntimeSkeleton::fromJson(const nlohmann::json& root, std::vector<std::string>* warnings) {
    RuntimeSkeleton skeleton;
    const nlohmann::json* payload = skeletonPayloadFromRoot(root);
    if (payload == nullptr) {
        addWarning(warnings, "Runtime skeleton load failed: JSON does not contain a skeleton payload.");
        return skeleton;
    }

    skeleton.name_ = payload->value("name", std::string{});
    if (payload->contains("skeletonRoot")) {
        skeleton.skeletonRoot_ = indexFromJsonValue((*payload)["skeletonRoot"], -1);
    }

    if (payload->contains("joints") && (*payload)["joints"].is_array()) {
        for (const nlohmann::json& jointJson : (*payload)["joints"]) {
            if (!jointJson.is_object()) {
                continue;
            }
            RuntimeSkeletonJoint joint;
            joint.index = jointJson.value("index", -1);
            joint.name = jointJson.value("name", std::string{});
            joint.parentIndex = jointJson.value("parentIndex", -1);
            joint.parentName = jointJson.value("parentName", std::string{});
            if (jointJson.contains("inverseBindMatrix")) {
                joint.inverseBindMatrix = matrixFromJson(jointJson["inverseBindMatrix"]);
                joint.hasInverseBindMatrix = true;
            }
            if (joint.index < 0 || joint.name.empty()) {
                addWarning(warnings, "Runtime skeleton skipped an invalid joint.");
                continue;
            }
            skeleton.joints_.push_back(std::move(joint));
        }
    } else if (payload->contains("skin") && (*payload)["skin"].is_object()) {
        const nlohmann::json& skin = (*payload)["skin"];
        const int32_t rootIndex = skin.contains("skeletonRoot") ? indexFromJsonValue(skin["skeletonRoot"], -1) : -1;
        skeleton.skeletonRoot_ = rootIndex;
        if (skin.contains("joints") && skin["joints"].is_array()) {
            int32_t index = 0;
            for (const nlohmann::json& jointValue : skin["joints"]) {
                RuntimeSkeletonJoint joint;
                joint.index = indexFromJsonValue(jointValue, index);
                ++index;
                joint.name = nameFromJsonValue(jointValue);
                if (joint.name.empty() && joint.index >= 0) {
                    joint.name = "joint_" + std::to_string(joint.index);
                }
                joint.parentIndex = joint.index == rootIndex ? -1 : joint.index - 1;
                joint.parentName = joint.parentIndex >= 0 && joint.parentIndex < static_cast<int32_t>(skeleton.joints_.size())
                    ? skeleton.joints_[static_cast<size_t>(joint.parentIndex)].name
                    : std::string{};
                if (joint.index < 0 || joint.name.empty()) {
                    addWarning(warnings, "Runtime skeleton skipped an invalid skin joint.");
                    continue;
                }
                skeleton.joints_.push_back(std::move(joint));
            }
        }
    }

    if (skeleton.skeletonRoot_ < 0 && !skeleton.joints_.empty()) {
        for (const RuntimeSkeletonJoint& joint : skeleton.joints_) {
            if (joint.parentIndex < 0) {
                skeleton.skeletonRoot_ = joint.index;
                break;
            }
        }
    }
    return skeleton;
}

RuntimeSkeleton RuntimeSkeleton::loadJson(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    std::ifstream file(path);
    if (!file.is_open()) {
        addWarning(warnings, "Runtime skeleton load failed: could not open " + path.string());
        return {};
    }
    try {
        nlohmann::json root;
        file >> root;
        return fromJson(root, warnings);
    } catch (const std::exception& ex) {
        addWarning(warnings, std::string("Runtime skeleton load failed: ") + ex.what());
    }
    return {};
}

RuntimeSkeleton RuntimeSkeleton::loadNativeBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, std::vector<std::string>* warnings) {
    NativeAssetReader reader;
    const NativeAssetInspection inspection = reader.inspectBytes(pathHint, bytes, true);
    if (!inspection.ok) {
        addWarning(warnings, inspection.errors.empty()
            ? "Runtime skeleton native load failed: invalid native asset " + pathHint.string()
            : "Runtime skeleton native load failed: " + inspection.errors.front().message);
        return {};
    }
    if (static_cast<NativeAssetKind>(inspection.header.assetKind) != NativeAssetKind::Skeleton) {
        addWarning(warnings, "Runtime skeleton native load failed: asset is not .rtskeleton " + pathHint.string());
        return {};
    }
    const auto chunkIt = std::find_if(inspection.chunks.begin(), inspection.chunks.end(), [](const NativeChunkRecord& chunk) {
        return chunk.type == kRtskeletonMetadataJsonChunk;
    });
    if (chunkIt == inspection.chunks.end() || !chunkRangeInside(*chunkIt, bytes.size())) {
        addWarning(warnings, "Runtime skeleton native load failed: missing metadata JSON chunk in " + pathHint.string());
        return {};
    }
    std::string metadata;
    metadata.resize(static_cast<size_t>(chunkIt->size));
    if (!metadata.empty()) {
        std::memcpy(metadata.data(), bytes.data() + chunkIt->offset, metadata.size());
    }
    try {
        return fromJson(nlohmann::json::parse(metadata), warnings);
    } catch (const std::exception& ex) {
        addWarning(warnings, std::string("Runtime skeleton native load failed: ") + ex.what());
    }
    return {};
}

RuntimeSkeleton RuntimeSkeleton::loadNative(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    std::vector<std::byte> bytes;
    if (!readFileBytes(path, bytes)) {
        addWarning(warnings, "Runtime skeleton load failed: could not open native " + path.string());
        return {};
    }
    return loadNativeBytes(path, bytes, warnings);
}

RuntimeSkeleton RuntimeSkeleton::load(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    if (nativeAssetKindFromExtension(path) == NativeAssetKind::Skeleton) {
        return loadNative(path, warnings);
    }
    return loadJson(path, warnings);
}

nlohmann::json runtimeSkeletonInspectionJson(const RuntimeSkeleton& skeleton, const std::vector<std::string>& warnings) {
    nlohmann::json joints = nlohmann::json::array();
    for (const RuntimeSkeletonJoint& joint : skeleton.joints()) {
        joints.push_back({
            {"index", joint.index},
            {"name", joint.name},
            {"parentIndex", joint.parentIndex},
            {"parentName", joint.parentName},
            {"hasInverseBindMatrix", joint.hasInverseBindMatrix},
        });
    }
    return {
        {"schema", "RuntimeSkeletonInspectionV1"},
        {"ok", skeleton.valid()},
        {"name", skeleton.name()},
        {"skeletonRoot", skeleton.skeletonRoot()},
        {"jointCount", skeleton.joints().size()},
        {"joints", joints},
        {"warnings", warnings},
    };
}

int inspectRuntimeSkeletonCommand(const std::filesystem::path& path, const std::filesystem::path& jsonOut) {
    std::vector<std::string> warnings;
    const RuntimeSkeleton skeleton = RuntimeSkeleton::load(path, &warnings);
    const nlohmann::json report = runtimeSkeletonInspectionJson(skeleton, warnings);
    if (!jsonOut.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(jsonOut.parent_path(), ec);
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Failed to write runtime skeleton inspection: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return skeleton.valid() ? 0 : 1;
}

} // namespace rtv
