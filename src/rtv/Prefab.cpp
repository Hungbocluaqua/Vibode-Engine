#include "rtv/Prefab.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace rtv {

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
                node.meshGuid = item.value("meshGuid", std::string{});
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
