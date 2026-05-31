#include "rtv/AssetImport.h"

#include "rtv/AssetManager.h"
#include "rtv/GltfLoader.h"

#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <exception>

#include <nlohmann/json.hpp>

namespace rtv {

namespace {

std::string lowerString(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string safeStem(std::filesystem::path path) {
    std::string stem = path.stem().string();
    if (stem.empty()) {
        stem = "ImportedAsset";
    }
    for (char& c : stem) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return stem;
}

std::string genericRelativeOrValue(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    return ec ? path.generic_string() : relative.generic_string();
}

std::string timestampString() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

std::string pathHashString(const std::filesystem::path& path) {
    const std::string value = path.lexically_normal().generic_string();
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::filesystem::path destinationFolderForType(AssetType type) {
    switch (type) {
    case AssetType::HDRI: return "HDRI";
    case AssetType::Texture: return "Textures";
    case AssetType::Material: return "Materials";
    case AssetType::Mesh:
    case AssetType::Prefab: return "Models";
    case AssetType::Scene: return "Scenes";
    case AssetType::Unknown:
    default: return "Models";
    }
}

std::string textureColorSpaceLabel(const TextureAsset& texture) {
    if (texture.srgb) return "sRGB";
    return texture.linearColorSpace ? "Linear" : "DataLinear";
}

std::string materialAlphaModeLabel(uint32_t alphaMode) {
    switch (alphaMode) {
    case kMaterialAlphaModeMask: return "Mask";
    case kMaterialAlphaModeBlend: return "Blend";
    case kMaterialAlphaModeOpaque:
    default: return "Opaque";
    }
}

} // namespace

AssetType assetTypeForSourcePath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".hdr" || ext == ".exr") return AssetType::HDRI;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".ktx" || ext == ".ktx2") return AssetType::Texture;
    if (ext == ".rtlevel") return AssetType::Scene;
    if (ext == ".gltf" || ext == ".glb") return AssetType::Prefab;
    return AssetType::Unknown;
}

std::string assetSourceHashForPath(const std::filesystem::path& path) {
    return pathHashString(path);
}

std::string assetImportSettingsHashForRequest(const AssetImportRequest& request) {
    return pathHashString(request.sourcePath.generic_string() + request.mode + request.settings.coordinateConversion);
}

AssetGuid importedAssetGuidFor(
    std::string_view sourceHash,
    std::string_view settingsHash,
    std::string_view kind,
    size_t index) {
    std::ostringstream key;
    key << sourceHash << ':' << settingsHash << ':' << kind << ':' << index;
    return "asset-" + pathHashString(key.str());
}

StagedAssetImportResult stagePlaceholderAssetImport(
    const AssetImportRequest& request,
    const AssetImportWorkspace& workspace) {
    StagedAssetImportResult result;
    if (request.sourcePath.empty()) {
        result.errors.push_back("Import source path is empty");
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::exists(request.sourcePath, ec)) {
        result.errors.push_back("Import source path does not exist: " + request.sourcePath.string());
        return result;
    }
    if (workspace.contentRoot.empty() || workspace.cacheRoot.empty() || workspace.registryPath.empty()) {
        result.errors.push_back("Import workspace is incomplete");
        return result;
    }

    const AssetType type = assetTypeForSourcePath(request.sourcePath);
    const std::string name = safeStem(request.sourcePath);
    const std::filesystem::path destinationFolder = request.destinationFolder.empty()
        ? destinationFolderForType(type)
        : request.destinationFolder;
    const std::filesystem::path importedDir = workspace.contentRoot / destinationFolder / name;
    const std::filesystem::path cacheDir = workspace.cacheRoot / destinationFolder / name;
    std::filesystem::create_directories(importedDir, ec);
    if (ec) {
        result.errors.push_back("Could not create import destination: " + ec.message());
        return result;
    }
    std::filesystem::create_directories(cacheDir, ec);
    if (ec) {
        result.errors.push_back("Could not create import cache folder: " + ec.message());
        return result;
    }

    const bool sourceIsGltf = type == AssetType::Prefab;
    const std::filesystem::path importedPath = importedDir / (name + (sourceIsGltf ? ".rtprefab.json" : ".rtasset.json"));
    const std::filesystem::path cachePath = cacheDir / (name + ".rtimportcache.json");
    const std::filesystem::path reportPath = importedDir / (name + ".import_report.json");
    const std::string sourceHash = assetSourceHashForPath(request.sourcePath);
    const std::string importSettingsHash = assetImportSettingsHashForRequest(request);
    const AssetGuid guid = importedAssetGuidFor(sourceHash, importSettingsHash, assetTypeName(type), 0);
    if (workspace.compatibilityMode) {
        result.warnings.push_back("Imported in no-project compatibility mode; create/open a project for normal asset workflows.");
    }

    const auto writeJson = [&](const std::filesystem::path& path, const nlohmann::json& json) -> bool {
        std::ofstream file(path);
        if (!file.is_open()) {
            result.errors.push_back("Could not write " + path.string());
            return false;
        }
        file << json.dump(2);
        result.generatedFiles.push_back(path);
        return true;
    };

    nlohmann::json placeholder = {
        {"version", 1},
        {"kind", sourceIsGltf ? "ImportedGltfPrefabRoot" : "PlaceholderImportedAsset"},
        {"guid", guid},
        {"type", assetTypeName(type)},
        {"displayName", name},
        {"sourcePath", request.sourcePath.generic_string()},
        {"mode", request.mode},
        {"nonMutatingSkeleton", true},
    };
    nlohmann::json cache = {
        {"version", 1},
        {"kind", sourceIsGltf ? "GltfImportCacheSummary" : "PlaceholderImportCache"},
        {"guid", guid},
        {"sourceHash", sourceHash},
        {"importSettingsHash", importSettingsHash},
    };

    std::vector<AssetRecord> records;
    std::vector<AssetGuid> rootDependencies;
    if (sourceIsGltf) {
        try {
            AssetManager importedAssets;
            GltfLoader loader(importedAssets);
            SceneAsset scene = loader.load(request.sourcePath);
            placeholder["nodeCount"] = scene.nodes.size();
            placeholder["rootNodes"] = scene.rootNodes;
            placeholder["lightCount"] = scene.lights.size();
            placeholder["textureCount"] = importedAssets.textures().size();
            placeholder["materialCount"] = importedAssets.materials().size();
            placeholder["meshCount"] = importedAssets.meshes().size();

            nlohmann::json nodes = nlohmann::json::array();
            for (size_t i = 0; i < scene.nodes.size(); ++i) {
                const SceneNodeAsset& node = scene.nodes[i];
                nodes.push_back({
                    {"index", i},
                    {"name", node.name.empty() ? ("Node_" + std::to_string(i)) : node.name},
                    {"parent", node.parent},
                    {"mesh", node.mesh.valid() ? static_cast<int>(node.mesh.index) : -1},
                    {"children", node.children},
                });
            }
            placeholder["sourceHierarchy"] = nodes;

            std::vector<AssetGuid> textureGuids;
            const auto& textures = importedAssets.textures();
            textureGuids.reserve(textures.size());
            for (size_t i = 0; i < textures.size(); ++i) {
                const TextureAsset& texture = textures[i];
                const AssetGuid textureGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Texture", i);
                textureGuids.push_back(textureGuid);
                rootDependencies.push_back(textureGuid);
                const std::string textureName = safeStem(texture.name.empty() ? ("Texture_" + std::to_string(i)) : texture.name);
                const std::filesystem::path texturePath = importedDir / "Textures" / (textureName + ".rttexture.json");
                const std::filesystem::path textureCache = cacheDir / "Textures" / (textureName + ".rttexturecache.json");
                std::filesystem::create_directories(texturePath.parent_path(), ec);
                std::filesystem::create_directories(textureCache.parent_path(), ec);
                (void)writeJson(texturePath, {
                    {"version", 1},
                    {"kind", "ImportedGltfTexture"},
                    {"guid", textureGuid},
                    {"sourcePath", texture.sourcePath.generic_string()},
                    {"width", texture.width},
                    {"height", texture.height},
                    {"channels", texture.channels},
                    {"colorSpace", textureColorSpaceLabel(texture)},
                    {"rule", texture.srgb ? "baseColor/emissive textures import as sRGB" : "normal/metallicRoughness/occlusion data imports as linear"},
                });
                (void)writeJson(textureCache, {
                    {"version", 1},
                    {"kind", "TextureCachePlaceholder"},
                    {"guid", textureGuid},
                    {"sourceHash", sourceHash},
                });
                AssetRecord record;
                record.guid = textureGuid;
                record.type = AssetType::Texture;
                record.displayName = textureName;
                record.sourcePath = texture.sourcePath.empty() ? request.sourcePath.generic_string() : texture.sourcePath.generic_string();
                record.importedPath = genericRelativeOrValue(texturePath, workspace.root);
                record.cachePath = genericRelativeOrValue(textureCache, workspace.root);
                record.sourceHash = sourceHash;
                record.importSettingsHash = importSettingsHash;
                record.lastModifiedTimestamp = timestampString();
                record.importSettings = request.settings;
                record.status = AssetImportStatus::Imported;
                records.push_back(std::move(record));
            }

            std::vector<AssetGuid> materialGuids;
            const auto& materials = importedAssets.materials();
            materialGuids.reserve(materials.size());
            for (size_t i = 0; i < materials.size(); ++i) {
                const MaterialAsset& material = materials[i];
                const AssetGuid materialGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Material", i);
                materialGuids.push_back(materialGuid);
                rootDependencies.push_back(materialGuid);
                const std::string materialName = safeStem(material.name.empty() ? ("Material_" + std::to_string(i)) : material.name);
                const std::filesystem::path materialPath = importedDir / "Materials" / (materialName + ".rtmaterial.json");
                std::filesystem::create_directories(materialPath.parent_path(), ec);
                nlohmann::json textureDependencies = nlohmann::json::array();
                auto addTextureDependency = [&](TextureAssetHandle handle, const char* role, const char* colorSpace) {
                    if (handle.valid() && handle.index < textureGuids.size()) {
                        textureDependencies.push_back({{"guid", textureGuids[handle.index]}, {"role", role}, {"colorSpace", colorSpace}});
                    }
                };
                addTextureDependency(material.baseColorTexture, "baseColor", "sRGB");
                addTextureDependency(material.emissiveTexture, "emissive", "sRGB");
                addTextureDependency(material.normalTexture, "normal", "Linear");
                addTextureDependency(material.metallicRoughnessTexture, "metallicRoughness", "Linear; metallic=B, roughness=G");
                addTextureDependency(material.occlusionTexture, "occlusion", "Linear; occlusion=R");
                (void)writeJson(materialPath, {
                    {"version", 1},
                    {"kind", "ImportedGltfMaterial"},
                    {"guid", materialGuid},
                    {"alphaMode", materialAlphaModeLabel(material.alphaMode)},
                    {"doubleSided", material.doubleSided != 0},
                    {"metallicRoughnessRule", "glTF metallic-roughness texture uses G=roughness and B=metallic"},
                    {"textureDependencies", textureDependencies},
                });
                AssetRecord record;
                record.guid = materialGuid;
                record.type = AssetType::Material;
                record.displayName = materialName;
                record.sourcePath = request.sourcePath.generic_string();
                record.importedPath = genericRelativeOrValue(materialPath, workspace.root);
                for (const auto& dep : textureDependencies) {
                    record.dependencies.push_back(AssetDependency{dep.value("guid", std::string{}), dep.value("role", std::string{})});
                }
                record.sourceHash = sourceHash;
                record.importSettingsHash = importSettingsHash;
                record.lastModifiedTimestamp = timestampString();
                record.importSettings = request.settings;
                record.status = AssetImportStatus::Imported;
                records.push_back(std::move(record));
            }

            std::vector<AssetGuid> meshGuids;
            const auto& meshes = importedAssets.meshes();
            meshGuids.reserve(meshes.size());
            for (size_t i = 0; i < meshes.size(); ++i) {
                const MeshAsset& mesh = meshes[i];
                const AssetGuid meshGuid = importedAssetGuidFor(sourceHash, importSettingsHash, "Mesh", i);
                meshGuids.push_back(meshGuid);
                rootDependencies.push_back(meshGuid);
                const std::string meshName = safeStem(mesh.name.empty() ? ("Mesh_" + std::to_string(i)) : mesh.name);
                const std::filesystem::path meshPath = importedDir / "Meshes" / (meshName + ".rtmesh.json");
                const std::filesystem::path meshCache = cacheDir / "Meshes" / (meshName + ".rtmeshcache.json");
                std::filesystem::create_directories(meshPath.parent_path(), ec);
                std::filesystem::create_directories(meshCache.parent_path(), ec);
                nlohmann::json primitiveMaterials = nlohmann::json::array();
                for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
                    if (primitive.material.valid() && primitive.material.index < materialGuids.size()) {
                        primitiveMaterials.push_back(materialGuids[primitive.material.index]);
                    }
                }
                (void)writeJson(meshPath, {
                    {"version", 1},
                    {"kind", "ImportedGltfMesh"},
                    {"guid", meshGuid},
                    {"vertexCount", mesh.vertices.size()},
                    {"indexCount", mesh.indices.size()},
                    {"primitiveCount", mesh.primitives.size()},
                    {"materialDependencies", primitiveMaterials},
                });
                (void)writeJson(meshCache, {
                    {"version", 1},
                    {"kind", "MeshCachePlaceholder"},
                    {"guid", meshGuid},
                    {"blasCacheRequested", request.settings.buildBlasCache},
                });
                AssetRecord record;
                record.guid = meshGuid;
                record.type = AssetType::Mesh;
                record.displayName = meshName;
                record.sourcePath = request.sourcePath.generic_string();
                record.importedPath = genericRelativeOrValue(meshPath, workspace.root);
                record.cachePath = genericRelativeOrValue(meshCache, workspace.root);
                for (const auto& dep : primitiveMaterials) {
                    record.dependencies.push_back(AssetDependency{dep.get<std::string>(), "material"});
                }
                record.sourceHash = sourceHash;
                record.importSettingsHash = importSettingsHash;
                record.lastModifiedTimestamp = timestampString();
                record.importSettings = request.settings;
                record.status = AssetImportStatus::Imported;
                records.push_back(std::move(record));
            }

            nlohmann::json prefabNodes = nlohmann::json::array();
            for (size_t i = 0; i < scene.nodes.size(); ++i) {
                const SceneNodeAsset& node = scene.nodes[i];
                nlohmann::json materialGuidsForNode = nlohmann::json::array();
                if (node.mesh.valid() && node.mesh.index < meshes.size()) {
                    for (const MeshPrimitiveAsset& primitive : meshes[node.mesh.index].primitives) {
                        if (primitive.material.valid() && primitive.material.index < materialGuids.size()) {
                            materialGuidsForNode.push_back(materialGuids[primitive.material.index]);
                        }
                    }
                }
                prefabNodes.push_back({
                    {"index", i},
                    {"name", node.name.empty() ? ("Node_" + std::to_string(i)) : node.name},
                    {"parent", node.parent},
                    {"children", node.children},
                    {"mesh", node.mesh.valid() ? static_cast<int>(node.mesh.index) : -1},
                    {"meshGuid", node.mesh.valid() && node.mesh.index < meshGuids.size() ? meshGuids[node.mesh.index] : std::string{}},
                    {"materialGuids", materialGuidsForNode},
                });
            }
            placeholder["sourceHierarchy"] = prefabNodes;
            placeholder["prefab"] = {
                {"version", 1},
                {"guid", guid},
                {"name", name},
                {"sourcePath", request.sourcePath.generic_string()},
                {"rootNodes", scene.rootNodes},
                {"nodes", prefabNodes},
            };

            cache["textureCount"] = textures.size();
            cache["materialCount"] = materials.size();
            cache["meshCount"] = meshes.size();
            cache["nodeCount"] = scene.nodes.size();
        } catch (const std::exception& ex) {
            result.errors.push_back(std::string("glTF import inspection failed: ") + ex.what());
            return result;
        }
    }
    if (!result.errors.empty()) {
        return result;
    }
    placeholder["dependencies"] = rootDependencies;
    nlohmann::json generatedFilesJson = nlohmann::json::array();
    for (const std::filesystem::path& path : result.generatedFiles) {
        generatedFilesJson.push_back(path.generic_string());
    }
    generatedFilesJson.push_back(importedPath.generic_string());
    generatedFilesJson.push_back(cachePath.generic_string());
    const nlohmann::json report = {
        {"version", 1},
        {"kind", "ImportReport"},
        {"guid", guid},
        {"sourcePath", request.sourcePath.generic_string()},
        {"generatedFiles", generatedFilesJson},
        {"warnings", result.warnings},
        {"errors", result.errors},
        {"sceneMutation", false},
        {"rendererResourcesCreated", false},
    };

    if (!writeJson(importedPath, placeholder) || !writeJson(cachePath, cache) || !writeJson(reportPath, report)) {
        return result;
    }

    AssetRecord record;
    record.guid = guid;
    record.type = type;
    record.displayName = name;
    record.sourcePath = request.sourcePath.generic_string();
    record.importedPath = genericRelativeOrValue(importedPath, workspace.root);
    record.cachePath = genericRelativeOrValue(cachePath, workspace.root);
    record.sourceHash = sourceHash;
    record.importSettingsHash = importSettingsHash;
    record.lastModifiedTimestamp = timestampString();
    record.importSettings = request.settings;
    record.status = AssetImportStatus::Imported;
    for (const AssetGuid& dependency : rootDependencies) {
        record.dependencies.push_back(AssetDependency{dependency, "source"});
    }
    result.record = std::move(record);
    result.records = std::move(records);
    result.records.insert(result.records.begin(), result.record);
    result.importReportPath = reportPath;
    result.success = true;
    return result;
}

} // namespace rtv
