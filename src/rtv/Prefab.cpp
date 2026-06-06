#include "rtv/Prefab.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace rtv {

namespace {

glm::vec3 vec3FromJson(const nlohmann::json& json, glm::vec3 fallback) {
    if (!json.is_array() || json.size() < 3) {
        return fallback;
    }
    return {
        json[0].get<float>(),
        json[1].get<float>(),
        json[2].get<float>(),
    };
}

glm::mat4 matrixFromJsonArray(const nlohmann::json& json, const glm::mat4& fallback) {
    if (!json.is_array() || json.size() < 16) {
        return fallback;
    }
    glm::mat4 result{1.0f};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            result[col][row] = json[static_cast<size_t>(col * 4 + row)].get<float>();
        }
    }
    return result;
}

glm::mat4 matrixFromTransformJson(const nlohmann::json& json, const glm::mat4& fallback) {
    if (!json.is_object()) {
        return fallback;
    }
    const glm::vec3 position = vec3FromJson(json.value("position", nlohmann::json::array()), glm::vec3{0.0f});
    const glm::vec3 rotationEuler = vec3FromJson(json.value("rotationEuler", nlohmann::json::array()), glm::vec3{0.0f});
    const glm::vec3 scale = vec3FromJson(json.value("scale", nlohmann::json::array()), glm::vec3{1.0f});
    return glm::translate(glm::mat4{1.0f}, position) * glm::mat4_cast(glm::quat(rotationEuler)) * glm::scale(glm::mat4{1.0f}, scale);
}

glm::mat4 prefabNodeTransformFromJson(const nlohmann::json& json) {
    glm::mat4 transform{1.0f};
    if (json.contains("transform")) {
        transform = matrixFromTransformJson(json["transform"], transform);
    }
    if (json.contains("matrix")) {
        transform = matrixFromJsonArray(json["matrix"], transform);
    }
    return transform;
}

std::vector<float> floatVectorFromJson(const nlohmann::json& json) {
    std::vector<float> values;
    if (!json.is_array()) {
        return values;
    }
    values.reserve(json.size());
    for (const nlohmann::json& item : json) {
        if (item.is_number()) {
            values.push_back(item.get<float>());
        }
    }
    return values;
}

} // namespace

bool loadPrefabAsset(const std::filesystem::path& path, PrefabAsset& outPrefab, std::string* error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        if (error != nullptr) *error = "Could not open prefab asset";
        return false;
    }
    try {
        nlohmann::json json;
        file >> json;
        const nlohmann::json* prefab = &json;
        if (json.contains("prefab") && json["prefab"].is_object()) {
            prefab = &json["prefab"];
        }
        outPrefab = PrefabAsset{};
        outPrefab.guid = prefab->value("guid", json.value("guid", std::string{}));
        outPrefab.name = prefab->value("name", json.value("displayName", path.stem().string()));
        outPrefab.sourcePath = prefab->value("sourcePath", json.value("sourcePath", std::string{}));
        const nlohmann::json* runtimePayload = nullptr;
        if (prefab->contains("runtimePayload") && (*prefab)["runtimePayload"].is_object()) {
            runtimePayload = &(*prefab)["runtimePayload"];
        } else if (json.contains("runtimePayload") && json["runtimePayload"].is_object()) {
            runtimePayload = &json["runtimePayload"];
        }
        if (runtimePayload != nullptr) {
            outPrefab.runtimePayloadKind = runtimePayload->value("kind", std::string{});
            outPrefab.runtimeCachePath = runtimePayload->value(
                "cachePath",
                runtimePayload->value("sceneCachePath", runtimePayload->value("path", std::string{})));
            outPrefab.runtimePayloadHash = runtimePayload->value("payloadHash", runtimePayload->value("hash", std::string{}));
            outPrefab.runtimeSourceHash = runtimePayload->value("sourceHash", std::string{});
            outPrefab.runtimeImportSettingsHash = runtimePayload->value("importSettingsHash", std::string{});
        }
        if (prefab->contains("rootNodes") && (*prefab)["rootNodes"].is_array()) {
            for (const nlohmann::json& root : (*prefab)["rootNodes"]) {
                outPrefab.rootNodes.push_back(root.get<uint32_t>());
            }
        }
        const nlohmann::json nodes = prefab->contains("nodes") ? (*prefab)["nodes"] : json.value("sourceHierarchy", nlohmann::json::array());
        if (nodes.is_array()) {
            for (const nlohmann::json& item : nodes) {
                PrefabNodeAsset node;
                node.name = item.value("name", std::string{});
                node.parent = item.value("parent", -1);
                node.sourceNodeIndex = item.value("sourceNodeIndex", -1);
                node.transform = prefabNodeTransformFromJson(item);
                node.meshGuid = item.value("meshGuid", std::string{});
                node.morphWeights = floatVectorFromJson(item.value("morphWeights", nlohmann::json::array()));
                node.hasCamera = item.value("hasCamera", false);
                node.cameraProjection = item.value("cameraProjection", node.cameraProjection);
                node.cameraYfov = item.value("cameraYfov", node.cameraYfov);
                node.cameraAspectRatio = item.value("cameraAspectRatio", node.cameraAspectRatio);
                node.cameraOrthoXmag = item.value("cameraOrthoXmag", node.cameraOrthoXmag);
                node.cameraOrthoYmag = item.value("cameraOrthoYmag", node.cameraOrthoYmag);
                node.cameraNear = item.value("cameraNear", node.cameraNear);
                node.cameraFar = item.value("cameraFar", node.cameraFar);
                node.hasLight = item.value("hasLight", false);
                node.lightType = item.value("lightType", node.lightType);
                node.lightColor = vec3FromJson(item.value("lightColor", nlohmann::json::array()), node.lightColor);
                node.lightIntensity = item.value("lightIntensity", node.lightIntensity);
                node.lightSizeOrRadius = item.value("lightSizeOrRadius", node.lightSizeOrRadius);
                node.lightInnerConeRadians = item.value("lightInnerConeRadians", node.lightInnerConeRadians);
                node.lightOuterConeRadians = item.value("lightOuterConeRadians", node.lightOuterConeRadians);
                node.lightEnabled = item.value("lightEnabled", node.lightEnabled);
                if (item.contains("materialGuids") && item["materialGuids"].is_array()) {
                    for (const nlohmann::json& material : item["materialGuids"]) {
                        if (material.is_string()) {
                            node.materialGuids.push_back(material.get<std::string>());
                        }
                    }
                }
                if (item.contains("children") && item["children"].is_array()) {
                    for (const nlohmann::json& child : item["children"]) {
                        node.children.push_back(child.get<uint32_t>());
                    }
                }
                outPrefab.nodes.push_back(std::move(node));
            }
        }
        if (outPrefab.rootNodes.empty() && !outPrefab.nodes.empty()) {
            for (uint32_t i = 0; i < outPrefab.nodes.size(); ++i) {
                if (outPrefab.nodes[i].parent < 0) {
                    outPrefab.rootNodes.push_back(i);
                }
            }
        }
        return !outPrefab.guid.empty();
    } catch (const std::exception& ex) {
        if (error != nullptr) *error = ex.what();
        return false;
    }
}

} // namespace rtv
